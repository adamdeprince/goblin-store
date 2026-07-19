// Application-owned TCP connection placement for the per-core stream loops.
//
// One coordinator-owned listener feeds a small acceptor.  The acceptor assigns each new fd to the
// least-loaded worker, using the incoming CPU/NAPI identity only to break equal-load ties.  Bytes
// never pass through the acceptor: after the fd integer is queued, one worker owns the connection
// for its complete lifetime.  This avoids SO_REUSEPORT hash imbalance for small sets of persistent
// connections while retaining NIC receive-queue locality whenever it does not conflict with load.
#pragma once

#include "goblin/common/error.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace goblin::net {

struct ConnectionRoute {
    std::size_t worker = 0;
    bool incoming_cpu_match = false;
    bool napi_affinity_match = false;
};

// Pure connection-placement policy, public so its fairness and locality rules can be tested without
// sockets, privileges, a NIC, or io_uring. `loads` must have one entry per worker CPU.
class ConnectionRouter {
public:
    explicit ConnectionRouter(std::vector<int> worker_cpus);

    ConnectionRoute choose(std::span<const std::uint64_t> loads, int incoming_cpu = -1,
                           int napi_id = 0);

private:
    std::vector<int> worker_cpus_;
    std::unordered_map<int, std::size_t> napi_owner_;
    std::size_t next_ = 0;
};

// One acceptor producer and one event-loop consumer touch this queue. The mutex is intentionally on
// the connection-establishment path, never the request or byte-streaming path. Keeping the queue
// dynamically sized means a connection burst cannot be discarded merely because a fixed handoff
// ring was undersized.
class alignas(64) ConnectionInbox {
public:
    ~ConnectionInbox();

    ConnectionInbox(const ConnectionInbox&) = delete;
    ConnectionInbox& operator=(const ConnectionInbox&) = delete;

    int notification_fd() const noexcept { return notification_fd_; }
    std::size_t worker_id() const noexcept { return worker_id_; }
    int worker_cpu() const noexcept { return worker_cpu_; }
    std::uint64_t current_connections() const noexcept {
        return current_connections_.load(std::memory_order_relaxed);
    }
    std::uint64_t accepted_connections() const noexcept {
        return accepted_connections_.load(std::memory_order_relaxed);
    }

    // Called only by the owning stream loop after its eventfd poll completes.
    std::deque<int> take_connections();
    // Balances the count charged before an fd was made visible in the queue.
    void release_connection() noexcept;

private:
    friend class ConnectionDispatcher;

    ConnectionInbox(std::size_t worker_id, int worker_cpu, int notification_fd) noexcept;
    static Result<std::unique_ptr<ConnectionInbox>> create(std::size_t worker_id,
                                                            int worker_cpu);
    bool enqueue(int fd) noexcept;
    void discard_connections() noexcept;

    std::size_t worker_id_ = 0;
    int worker_cpu_ = -1;
    int notification_fd_ = -1;
    mutable std::mutex queue_mutex_;
    std::deque<int> queue_;
    std::atomic<std::uint64_t> current_connections_{0};
    std::atomic<std::uint64_t> accepted_connections_{0};
};

class ConnectionDispatcher {
public:
    // Takes ownership of listener_fd on both success and failure. The listener must already be
    // listening and nonblocking. One worker is created for every entry in worker_cpus; -1 means the
    // worker has no explicit CPU affinity and therefore cannot be an incoming-CPU locality match.
    static Result<std::unique_ptr<ConnectionDispatcher>> create(
        int listener_fd, std::vector<int> worker_cpus, std::string label,
        bool require_exasock_connections = false);

    ~ConnectionDispatcher();

    ConnectionDispatcher(const ConnectionDispatcher&) = delete;
    ConnectionDispatcher& operator=(const ConnectionDispatcher&) = delete;

    std::size_t worker_count() const noexcept { return inboxes_.size(); }
    ConnectionInbox& inbox(std::size_t worker) noexcept { return *inboxes_[worker]; }
    const ConnectionInbox& inbox(std::size_t worker) const noexcept { return *inboxes_[worker]; }

    // Blocks until shutdown is set. Fatal listener errors set shutdown so sibling workers drain.
    void run(std::atomic<bool>& shutdown) noexcept;
    // Intended for shutdown/benchmark diagnostics after all workers have joined.
    void report() const noexcept;

private:
    ConnectionDispatcher(int listener_fd, std::vector<int> worker_cpus, std::string label,
                         bool require_exasock_connections);

    bool dispatch(int fd) noexcept;

    int listener_fd_ = -1;
    std::string label_;
    bool require_exasock_connections_ = false;
    bool reported_exasock_rejection_ = false;
    ConnectionRouter router_;
    std::vector<std::unique_ptr<ConnectionInbox>> inboxes_;
    std::vector<std::uint64_t> load_snapshot_;
    std::atomic<std::uint64_t> cpu_local_assignments_{0};
    std::atomic<std::uint64_t> napi_local_assignments_{0};
};

} // namespace goblin::net
