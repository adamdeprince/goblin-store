#include "goblin/net/connection_dispatcher.hpp"

#include "goblin/net/stream_io.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <thread>
#include <utility>
#include <unistd.h>

#if defined(__linux__)
#include <sys/eventfd.h>
#include <sys/socket.h>
#endif

namespace goblin::net {

ConnectionRouter::ConnectionRouter(std::vector<int> worker_cpus)
    : worker_cpus_(std::move(worker_cpus)) {}

ConnectionRoute ConnectionRouter::choose(std::span<const std::uint64_t> loads,
                                         int incoming_cpu, int napi_id) {
    if (loads.empty() || loads.size() != worker_cpus_.size()) return {};

    const std::uint64_t minimum = *std::min_element(loads.begin(), loads.end());
    const auto eligible = [&](std::size_t worker) { return loads[worker] == minimum; };
    const auto cyclic_match = [&](const auto& predicate) -> std::size_t {
        for (std::size_t offset = 0; offset < loads.size(); ++offset) {
            const std::size_t worker = (next_ + offset) % loads.size();
            if (eligible(worker) && predicate(worker)) return worker;
        }
        return loads.size();
    };

    ConnectionRoute route;
    if (incoming_cpu >= 0) {
        route.worker = cyclic_match(
            [&](std::size_t worker) { return worker_cpus_[worker] == incoming_cpu; });
        route.incoming_cpu_match = route.worker != loads.size();
    } else {
        route.worker = loads.size();
    }

    if (route.worker == loads.size() && napi_id > 0) {
        if (const auto it = napi_owner_.find(napi_id);
            it != napi_owner_.end() && eligible(it->second)) {
            route.worker = it->second;
            route.napi_affinity_match = true;
        }
    }
    if (route.worker == loads.size()) route.worker = cyclic_match([](std::size_t) { return true; });
    if (route.worker == loads.size()) route.worker = 0; // defensive; a nonempty load set has a min

    if (napi_id > 0) napi_owner_[napi_id] = route.worker;
    next_ = (route.worker + 1) % loads.size();
    return route;
}

ConnectionInbox::ConnectionInbox(std::size_t worker_id, int worker_cpu,
                                 int notification_fd) noexcept
    : worker_id_(worker_id), worker_cpu_(worker_cpu), notification_fd_(notification_fd) {}

Result<std::unique_ptr<ConnectionInbox>> ConnectionInbox::create(std::size_t worker_id,
                                                                 int worker_cpu) {
#if defined(__linux__)
    const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0)
        return err(Errc::io_error,
                   std::string("connection-dispatch eventfd: ") + std::strerror(errno));
    return std::unique_ptr<ConnectionInbox>(new ConnectionInbox(worker_id, worker_cpu, fd));
#else
    (void)worker_id;
    (void)worker_cpu;
    return err(Errc::unsupported, "connection dispatch is supported only on Linux");
#endif
}

ConnectionInbox::~ConnectionInbox() {
    discard_connections();
    if (notification_fd_ >= 0) ::close(notification_fd_);
}

bool ConnectionInbox::enqueue(int fd) noexcept {
    try {
        std::lock_guard lock(queue_mutex_);
        queue_.push_back(fd);
        // Charge while the queue lock prevents the consumer from closing the fd first.
        current_connections_.fetch_add(1, std::memory_order_relaxed);
        accepted_connections_.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
        return false;
    }

#if defined(__linux__)
    const std::uint64_t one = 1;
    const ssize_t written = ::write(notification_fd_, &one, sizeof one);
    // EAGAIN means the eventfd is already saturated and therefore already readable. Other failures
    // can occur only during teardown; the queued fd remains owned by this inbox and is closed there.
    (void)written;
#endif
    return true;
}

std::deque<int> ConnectionInbox::take_connections() {
#if defined(__linux__)
    std::uint64_t count = 0;
    while (::read(notification_fd_, &count, sizeof count) ==
           static_cast<ssize_t>(sizeof count)) {}
#endif
    std::deque<int> ready;
    std::lock_guard lock(queue_mutex_);
    ready.swap(queue_);
    return ready;
}

void ConnectionInbox::release_connection() noexcept {
    std::uint64_t current = current_connections_.load(std::memory_order_relaxed);
    while (current != 0 &&
           !current_connections_.compare_exchange_weak(current, current - 1,
                                                       std::memory_order_relaxed)) {}
}

void ConnectionInbox::discard_connections() noexcept {
    auto pending = take_connections();
    for (const int fd : pending) {
        ::close(fd);
        release_connection();
    }
}

ConnectionDispatcher::ConnectionDispatcher(int listener_fd, std::vector<int> worker_cpus,
                                           std::string label,
                                           bool require_exasock_connections)
    : listener_fd_(listener_fd), label_(std::move(label)),
      require_exasock_connections_(require_exasock_connections),
      router_(std::move(worker_cpus)) {}

Result<std::unique_ptr<ConnectionDispatcher>> ConnectionDispatcher::create(
    int listener_fd, std::vector<int> worker_cpus, std::string label,
    bool require_exasock_connections) {
    if (listener_fd < 0)
        return err(Errc::invalid_argument, "connection dispatcher listener is invalid");
    if (worker_cpus.empty()) {
        ::close(listener_fd);
        return err(Errc::invalid_argument, "connection dispatcher requires at least one worker");
    }

    auto dispatcher = std::unique_ptr<ConnectionDispatcher>(new ConnectionDispatcher(
        listener_fd, worker_cpus, std::move(label), require_exasock_connections));
    dispatcher->inboxes_.reserve(worker_cpus.size());
    for (std::size_t worker = 0; worker < worker_cpus.size(); ++worker) {
        auto inbox = ConnectionInbox::create(worker, worker_cpus[worker]);
        if (!inbox) return std::unexpected(inbox.error());
        dispatcher->inboxes_.push_back(std::move(*inbox));
    }
    dispatcher->load_snapshot_.resize(worker_cpus.size());
    return dispatcher;
}

ConnectionDispatcher::~ConnectionDispatcher() {
    if (listener_fd_ >= 0) ::close(listener_fd_);
}

namespace {

int incoming_cpu(int fd) noexcept {
#if defined(__linux__) && defined(SO_INCOMING_CPU)
    int value = -1;
    socklen_t length = sizeof value;
    if (::getsockopt(fd, SOL_SOCKET, SO_INCOMING_CPU, &value, &length) == 0) return value;
#else
    (void)fd;
#endif
    return -1;
}

int incoming_napi_id(int fd) noexcept {
#if defined(__linux__) && defined(SO_INCOMING_NAPI_ID)
    int value = 0;
    socklen_t length = sizeof value;
    if (::getsockopt(fd, SOL_SOCKET, SO_INCOMING_NAPI_ID, &value, &length) == 0) return value;
#else
    (void)fd;
#endif
    return 0;
}

bool make_cloexec(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFD, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

bool make_nonblocking(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

} // namespace

bool ConnectionDispatcher::dispatch(int fd) noexcept {
    if (!make_cloexec(fd)) return false;
    if (require_exasock_connections_) {
        if (!make_nonblocking(fd)) return false;
        if (const auto status = validate_exasock_connection(fd); !status) {
            if (!reported_exasock_rejection_) {
                std::fprintf(stderr, "%s connection rejected: %s\n", label_.c_str(),
                             status.error().detail.c_str());
                reported_exasock_rejection_ = true;
            }
            return false;
        }
    }

    for (std::size_t worker = 0; worker < inboxes_.size(); ++worker)
        load_snapshot_[worker] = inboxes_[worker]->current_connections();
    const ConnectionRoute route =
        router_.choose(load_snapshot_, incoming_cpu(fd), incoming_napi_id(fd));
    if (!inboxes_[route.worker]->enqueue(fd)) return false;
    if (route.incoming_cpu_match)
        cpu_local_assignments_.fetch_add(1, std::memory_order_relaxed);
    if (route.napi_affinity_match)
        napi_local_assignments_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void ConnectionDispatcher::run(std::atomic<bool>& shutdown) noexcept {
#if defined(__linux__)
    while (!shutdown.load(std::memory_order_relaxed)) {
        pollfd descriptor{listener_fd_, POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, 200);
        if (ready < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "%s dispatcher poll: %s\n", label_.c_str(),
                         std::strerror(errno));
            shutdown.store(true, std::memory_order_relaxed);
            return;
        }
        if (ready == 0) continue;
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            std::fprintf(stderr, "%s dispatcher listener failed (revents=%d)\n", label_.c_str(),
                         descriptor.revents);
            shutdown.store(true, std::memory_order_relaxed);
            return;
        }

        while (!shutdown.load(std::memory_order_relaxed)) {
            const int fd = ::accept(listener_fd_, nullptr, nullptr);
            if (fd >= 0) {
                if (!dispatch(fd)) ::close(fd);
                continue;
            }
            if (errno == EINTR || errno == ECONNABORTED) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                break;
            }
            std::fprintf(stderr, "%s dispatcher accept: %s\n", label_.c_str(),
                         std::strerror(errno));
            shutdown.store(true, std::memory_order_relaxed);
            return;
        }
    }
#else
    (void)shutdown;
#endif
}

void ConnectionDispatcher::report() const noexcept {
    std::fprintf(stderr,
                 "%s dispatch summary: workers=%zu cpu_local=%llu napi_local=%llu\n",
                 label_.c_str(), inboxes_.size(),
                 static_cast<unsigned long long>(
                     cpu_local_assignments_.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     napi_local_assignments_.load(std::memory_order_relaxed)));
    for (const auto& inbox : inboxes_)
        std::fprintf(stderr, "%s dispatch worker=%zu cpu=%d accepted=%llu current=%llu\n",
                     label_.c_str(), inbox->worker_id(), inbox->worker_cpu(),
                     static_cast<unsigned long long>(inbox->accepted_connections()),
                     static_cast<unsigned long long>(inbox->current_connections()));
}

} // namespace goblin::net
