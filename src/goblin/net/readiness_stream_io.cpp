#include "goblin/net/stream_io.hpp"

#if defined(__linux__)

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>

#if GOBLIN_HAVE_EXASOCK
#include <dlfcn.h>
#include <exasock/extensions.h>
#include <exasock/socket.h>
#endif

namespace goblin::net {
namespace {

#if GOBLIN_HAVE_EXASOCK
template <class Function>
Function process_symbol(const char* name) noexcept {
    void* symbol = ::dlsym(RTLD_DEFAULT, name);
    Function function = nullptr;
    static_assert(sizeof function == sizeof symbol);
    std::memcpy(&function, &symbol, sizeof function);
    return function;
}

Status require_exasock_preload() {
    const auto loaded = process_symbol<decltype(&::exasock_loaded)>("exasock_loaded");
    if (!loaded || loaded() == 0)
        return err(Errc::unsupported,
                   "ExaSock runtime is not active (run with: exasock --no-auto "
                   "goblin-store ...)");
    return {};
}

int exasock_connection_error(int fd) noexcept {
    static const auto get_device =
        process_symbol<decltype(&::exasock_tcp_get_device)>("exasock_tcp_get_device");
    if (!get_device) return ENOSYS;

    // Device names returned by the public extension API are short (for example, exanic0), but do
    // not couple the check to Linux's interface-name limit.
    std::array<char, 64> device{};
    int port = -1;
    errno = 0;
    if (get_device(fd, device.data(), device.size(), &port) == 0) return 0;
    return errno != 0 ? errno : EOPNOTSUPP;
}
#endif

class ReadinessStreamIo final : public StreamIo {
public:
    static Result<std::unique_ptr<StreamIo>> create(core::Reactor& disk,
                                                    bool require_exasock_connections) {
        const int epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd < 0) return err(Errc::io_error, "epoll_create1");
        const int completion_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (completion_fd < 0) {
            ::close(epoll_fd);
            return err(Errc::io_error, "eventfd");
        }
        if (auto status = disk.register_completion_eventfd(completion_fd); !status) {
            ::close(completion_fd);
            ::close(epoll_fd);
            return std::unexpected(status.error());
        }
        epoll_event event{};
        event.events = EPOLLIN;
        event.data.u64 = kDiskEvent;
        if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, completion_fd, &event) < 0) {
            disk.unregister_completion_eventfd();
            ::close(completion_fd);
            ::close(epoll_fd);
            return err(Errc::io_error, "epoll_ctl completion eventfd");
        }
        return std::unique_ptr<StreamIo>(new ReadinessStreamIo(
            disk, epoll_fd, completion_fd, require_exasock_connections));
    }

    ~ReadinessStreamIo() override {
        disk_.unregister_completion_eventfd();
        ::close(completion_fd_);
        ::close(epoll_fd_);
    }

    bool submit_read(int fd, std::uint64_t offset, MutBytes buf,
                     std::uint64_t user_data) override {
        return disk_.submit_read(fd, offset, buf, user_data);
    }
    bool submit_recv(int fd, MutBytes buf, std::uint64_t user_data) override {
        Pending p;
        p.kind = Kind::recv;
        p.fd = fd;
        p.user_data = user_data;
        p.mutable_bytes = buf;
        return start(std::move(p));
    }
    bool submit_send(int fd, ByteView buf, std::uint64_t user_data, int flags) override {
        Pending p;
        p.kind = Kind::send;
        p.fd = fd;
        p.user_data = user_data;
        p.bytes = buf;
        p.flags = flags;
        return start(std::move(p));
    }
    bool submit_sendmsg(int fd, msghdr* msg, std::uint64_t user_data, int flags) override {
        if (!msg) return false;
        Pending p;
        p.kind = Kind::sendmsg;
        p.fd = fd;
        p.user_data = user_data;
        p.message = msg;
        p.flags = flags;
        return start(std::move(p));
    }
    bool submit_accept(int listen_fd, std::uint64_t user_data) override {
        Pending p;
        p.kind = Kind::accept;
        p.fd = listen_fd;
        p.user_data = user_data;
        return start(std::move(p));
    }
    bool submit_poll(int fd, unsigned poll_mask, std::uint64_t user_data) override {
        Pending p;
        p.kind = Kind::poll;
        p.fd = fd;
        p.user_data = user_data;
        p.poll_mask = poll_mask;
        return start(std::move(p));
    }

    unsigned submission_space() const noexcept override { return disk_.submission_space(); }
    int submit() override { return disk_.submit(); }

    void submit_and_wait_timeout(unsigned timeout_ms) override {
        (void)disk_.submit();
        collect_disk();
        if (!ready_.empty()) return;

        std::array<epoll_event, 256> events{};
        const int timeout = static_cast<int>(std::min<unsigned>(timeout_ms, INT_MAX));
        const int count = ::epoll_wait(epoll_fd_, events.data(), events.size(), timeout);
        if (count < 0) return; // EINTR and transient wait failures simply produce no completions.

        bool disk_ready = false;
        for (int i = 0; i < count; ++i) {
            if (events[static_cast<std::size_t>(i)].data.u64 == kDiskEvent) {
                disk_ready = true;
                continue;
            }
            const int fd = static_cast<int>(
                static_cast<std::uint32_t>(events[static_cast<std::size_t>(i)].data.u64));
            retry(fd);
        }
        if (disk_ready) drain_completion_eventfd();
        // Always inspect the CQ. This also covers a completion racing with epoll_wait and prevents
        // leftover CQEs from sleeping after the eventfd counter was drained.
        collect_disk();
    }

    unsigned reap(std::span<core::Completion> out) override {
        collect_disk();
        unsigned count = 0;
        while (count < out.size() && !ready_.empty()) {
            out[count++] = ready_.front();
            ready_.pop_front();
        }
        return count;
    }

    void close_fd(int fd) noexcept override {
        if (const auto it = pending_.find(fd); it != pending_.end()) {
            ready_.push_back({it->second.user_data, -ECANCELED});
            pending_.erase(it);
        }
        if (registered_.erase(fd) != 0)
            (void)::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        ::close(fd);
    }

private:
    enum class Kind { accept, recv, send, sendmsg, poll };
    struct Pending {
        Kind kind = Kind::recv;
        int fd = -1;
        std::uint64_t user_data = 0;
        MutBytes mutable_bytes{};
        ByteView bytes{};
        msghdr* message = nullptr;
        int flags = 0;
        unsigned poll_mask = 0;
    };

    static constexpr std::uint64_t kDiskEvent = std::numeric_limits<std::uint64_t>::max();

    ReadinessStreamIo(core::Reactor& disk, int epoll_fd, int completion_fd,
                      bool require_exasock_connections) noexcept
        : disk_(disk), epoll_fd_(epoll_fd), completion_fd_(completion_fd),
          require_exasock_connections_(require_exasock_connections) {}

    static bool would_block(int error) noexcept {
        return error == EAGAIN || error == EWOULDBLOCK;
    }

    std::optional<int> perform(Pending& p) {
        switch (p.kind) {
            case Kind::accept: {
                const int accepted = ::accept(p.fd, nullptr, nullptr);
                if (accepted < 0) {
                    if (would_block(errno)) return std::nullopt;
                    return -errno;
                }
                const int current = ::fcntl(accepted, F_GETFL, 0);
                if (current < 0 || ::fcntl(accepted, F_SETFL, current | O_NONBLOCK) < 0) {
                    const int error = errno;
                    ::close(accepted);
                    return -error;
                }
#if GOBLIN_HAVE_EXASOCK
                if (require_exasock_connections_) {
                    if (const int error = exasock_connection_error(accepted); error != 0) {
                        if (!reported_exasock_rejection_) {
                            std::fprintf(stderr,
                                         "ExaSock connection rejected: socket is not accelerated "
                                         "(%s)\n",
                                         std::strerror(error));
                            reported_exasock_rejection_ = true;
                        }
                        ::close(accepted);
                        return -error;
                    }
                }
#else
                if (require_exasock_connections_) {
                    ::close(accepted);
                    return -ENOTSUP;
                }
#endif
                return accepted;
            }
            case Kind::recv: {
                const ssize_t result =
                    ::recv(p.fd, p.mutable_bytes.data(), p.mutable_bytes.size(), 0);
                if (result < 0) {
                    if (would_block(errno)) return std::nullopt;
                    return -errno;
                }
                return static_cast<int>(result);
            }
            case Kind::send: {
                const ssize_t result = ::send(p.fd, p.bytes.data(), p.bytes.size(),
                                              MSG_NOSIGNAL | p.flags);
                if (result < 0) {
                    if (would_block(errno)) return std::nullopt;
                    return -errno;
                }
                return static_cast<int>(result);
            }
            case Kind::sendmsg: {
                const ssize_t result = ::sendmsg(p.fd, p.message, MSG_NOSIGNAL | p.flags);
                if (result < 0) {
                    if (would_block(errno)) return std::nullopt;
                    return -errno;
                }
                return static_cast<int>(result);
            }
            case Kind::poll: {
                pollfd descriptor{p.fd, static_cast<short>(p.poll_mask), 0};
                const int result = ::poll(&descriptor, 1, 0);
                if (result < 0) return -errno;
                if (result == 0) return std::nullopt;
                return descriptor.revents;
            }
        }
        return -EINVAL;
    }

    std::uint32_t interest(const Pending& p) const noexcept {
        switch (p.kind) {
            case Kind::accept:
            case Kind::recv: return EPOLLIN;
            case Kind::send:
            case Kind::sendmsg: return EPOLLOUT;
            case Kind::poll: {
                std::uint32_t events = 0;
                if (p.poll_mask & POLLIN) events |= EPOLLIN;
                if (p.poll_mask & POLLOUT) events |= EPOLLOUT;
                return events;
            }
        }
        return 0;
    }

    bool arm(const Pending& p) {
        epoll_event event{};
        event.events = interest(p) | EPOLLERR | EPOLLHUP | EPOLLRDHUP | EPOLLONESHOT;
        event.data.u64 = static_cast<std::uint32_t>(p.fd);
        const bool known = registered_.contains(p.fd);
        const int op = known ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
        if (::epoll_ctl(epoll_fd_, op, p.fd, &event) == 0) {
            if (!known) registered_.insert(p.fd);
            return true;
        }
        return false;
    }

    bool start(Pending p) {
        if (p.fd < 0 || pending_.contains(p.fd)) return false;
        if (auto result = perform(p)) {
            ready_.push_back({p.user_data, *result});
            return true;
        }
        if (!arm(p)) return false;
        pending_.emplace(p.fd, std::move(p));
        return true;
    }

    void retry(int fd) {
        const auto it = pending_.find(fd);
        if (it == pending_.end()) return;
        if (auto result = perform(it->second)) {
            ready_.push_back({it->second.user_data, *result});
            pending_.erase(it);
            return;
        }
        // EPOLLONESHOT consumed this notification; EAGAIN means rearm the same operation.
        if (!arm(it->second)) {
            ready_.push_back({it->second.user_data, -errno});
            pending_.erase(it);
        }
    }

    void drain_completion_eventfd() noexcept {
        std::uint64_t value = 0;
        while (::read(completion_fd_, &value, sizeof value) ==
               static_cast<ssize_t>(sizeof value)) {}
    }

    void collect_disk() {
        std::array<core::Completion, 256> completions{};
        for (;;) {
            const unsigned count = disk_.reap(completions);
            for (unsigned i = 0; i < count; ++i) ready_.push_back(completions[i]);
            if (count < completions.size()) return;
        }
    }

    core::Reactor& disk_;
    int epoll_fd_ = -1;
    int completion_fd_ = -1;
    std::unordered_map<int, Pending> pending_;
    std::unordered_set<int> registered_;
    std::deque<core::Completion> ready_;
    bool require_exasock_connections_ = false;
    bool reported_exasock_rejection_ = false;
};

} // namespace

Result<std::unique_ptr<StreamIo>> make_readiness_stream_io(core::Reactor& disk_reactor,
                                                           bool require_exasock_connections) {
    return ReadinessStreamIo::create(disk_reactor, require_exasock_connections);
}

Status enable_exasock_socket(int fd) {
#if GOBLIN_HAVE_EXASOCK
    if (auto status = require_exasock_preload(); !status) return status;
    int no_acceleration = 0;
    if (::setsockopt(fd, SOL_EXASOCK, SO_EXA_NO_ACCEL, &no_acceleration,
                     sizeof no_acceleration) < 0)
        return err(Errc::io_error,
                   "ExaSock opt-in failed (run with: exasock --no-auto goblin-store ...)");
    return {};
#else
    (void)fd;
    return err(Errc::unsupported,
               "built without ExaSock (configure with -DGOBLIN_ENABLE_EXASOCK=ON)");
#endif
}

Status validate_exasock_connection(int fd) {
#if GOBLIN_HAVE_EXASOCK
    if (const int error = exasock_connection_error(fd); error != 0)
        return err(Errc::io_error,
                   "socket is not ExaSock-accelerated (" +
                       std::string(std::strerror(error)) + ")");
    return {};
#else
    (void)fd;
    return err(Errc::unsupported,
               "built without ExaSock (configure with -DGOBLIN_ENABLE_EXASOCK=ON)");
#endif
}

} // namespace goblin::net

#else // !__linux__

namespace goblin::net {

Result<std::unique_ptr<StreamIo>> make_readiness_stream_io(core::Reactor&, bool) {
    return err(Errc::unsupported, "readiness stream I/O is Linux-only");
}

Status enable_exasock_socket(int) {
    return err(Errc::unsupported, "ExaSock is Linux-only");
}

Status validate_exasock_connection(int) {
    return err(Errc::unsupported, "ExaSock is Linux-only");
}

} // namespace goblin::net

#endif
