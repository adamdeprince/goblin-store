// Per-core io_uring reactor (ADR-0001/0002). Each worker core owns one Reactor; it submits reads
// against shared drive fds on its own ring and reaps completions. Gated by GOBLIN_HAVE_URING:
// without liburing the build still works and create() returns Errc::unsupported (stub).
//
// This first cut exposes the async primitives (submit_read / submit / reap) plus a blocking
// read_sync() used to bootstrap and test the ring. Registered buffers/files, multishot accept,
// and the event loop layer build on top of this.
#pragma once

#include "goblin/common/error.hpp"
#include "goblin/common/types.hpp"

#include <cstdint>
#include <span>
#include <sys/socket.h> // msghdr

#ifndef GOBLIN_HAVE_URING
#define GOBLIN_HAVE_URING 0
#endif

#if GOBLIN_HAVE_URING
#include <liburing.h>
#include <memory>
#endif

namespace goblin::core {

// One reaped completion: `res` is bytes transferred (>= 0) or a negative errno.
struct Completion {
    std::uint64_t user_data = 0;
    int res = 0;
};

class Reactor {
public:
    static Result<Reactor> create(unsigned entries = 256);

    // Queue a read of `buf.size()` bytes from `fd` at `offset`. Returns false if the submission
    // queue is full (call submit() to drain, then retry). `user_data` is echoed in the Completion.
    bool submit_read(int fd, std::uint64_t offset, MutBytes buf, std::uint64_t user_data);

    // Network ops on the same ring (ADR-0002). `user_data` is echoed in the Completion so the event
    // loop can dispatch on it. submit_accept's completion `res` is the new connection fd.
    bool submit_recv(int fd, MutBytes buf, std::uint64_t user_data);
    // `flags` is OR'd with MSG_NOSIGNAL. Pass MSG_MORE for intermediate pieces so the stack can
    // coalesce with the next send (header+head, mid-tail pieces).
    bool submit_send(int fd, ByteView buf, std::uint64_t user_data, int flags = 0);
    // Scatter-gather send. `msg` (and its iov) must remain valid until the next ring submit drains
    // the SQE (caller typically owns them on Conn). CQE `res` is total bytes across iovecs.
    bool submit_sendmsg(int fd, msghdr* msg, std::uint64_t user_data, int flags = 0);
    bool submit_accept(int listen_fd, std::uint64_t user_data);
    // One-shot readiness poll (POLLIN/POLLOUT): the Completion's `res` is the ready events. Used to
    // drive OpenSSL's non-blocking handshake/read on the loop (ADR-0005).
    bool submit_poll(int fd, unsigned poll_mask, std::uint64_t user_data);

    // Remaining userspace SQ slots. A multi-segment operation can preflight the complete batch so
    // it never half-queues buffers that must be consumed atomically by the caller's state machine.
    unsigned submission_space() const noexcept;
    int submit();                          // flush queued SQEs; returns count submitted
    int submit_and_wait(unsigned min_complete); // flush + block until >= min_complete completions
    void submit_and_wait_timeout(unsigned timeout_ms); // flush + wait up to timeout for >=1 (then reap)
    unsigned reap(std::span<Completion> out); // drain ready completions; returns count written

    // Bootstrap/test helper: one read, submit, wait, return bytes read.
    Result<std::size_t> read_sync(int fd, std::uint64_t offset, MutBytes buf);

    static constexpr bool available() noexcept { return GOBLIN_HAVE_URING; }

private:
#if GOBLIN_HAVE_URING
    struct RingDeleter {
        void operator()(io_uring* r) const noexcept {
            if (r) { io_uring_queue_exit(r); delete r; }
        }
    };
    using RingPtr = std::unique_ptr<io_uring, RingDeleter>;
    explicit Reactor(RingPtr ring) : ring_(std::move(ring)) {}
    RingPtr ring_;
#else
    Reactor() = default;
#endif
};

} // namespace goblin::core
