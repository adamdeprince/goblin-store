#include "goblin/store/client.hpp"

#if !defined(__linux__)
#error "The Goblin Store ExaSock transport is Linux-only"
#endif

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <dlfcn.h>

// ExaSock is an external, system-provided dependency. No part of its SDK is
// copied into or distributed with Goblin Store.
#include <exasock/extensions.h>
#include <exasock/socket.h>

namespace goblin::client {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kInlineFragmentBytes = 64 * 1024;
constexpr std::size_t kBulkFragmentBytes = 256 * 1024;
constexpr std::size_t kReceiveBufferBytes = 256 * 1024;

using ExasockLoaded = int (*)();
using ExasockTcpGetDevice = int (*)(int, char*, std::size_t, int*);

template <class Function>
Function resolve_exasock_symbol(const char* name) noexcept {
    void* symbol = ::dlsym(RTLD_DEFAULT, name);
    if (!symbol) return nullptr;
    static_assert(sizeof(Function) == sizeof(symbol));
    Function function = nullptr;
    std::memcpy(&function, &symbol, sizeof(function));
    return function;
}

void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    asm volatile("yield" ::: "memory");
#elif defined(__loongarch__)
    asm volatile("dbar 0" ::: "memory");
#else
    asm volatile("" ::: "memory");
#endif
}

std::string errno_message(std::string_view operation, int error = errno) {
    return std::string(operation) + ": " + std::strerror(error);
}

int remaining_ms(Clock::time_point deadline) noexcept {
    const auto now = Clock::now();
    if (now >= deadline) return 0;
    const auto remaining =
        std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
    if (remaining > std::numeric_limits<int>::max())
        return std::numeric_limits<int>::max();
    return static_cast<int>(remaining);
}

bool set_descriptor_flags(int fd, std::string& error) {
    const int status = ::fcntl(fd, F_GETFL, 0);
    if (status == -1 || ::fcntl(fd, F_SETFL, status | O_NONBLOCK) == -1) {
        error = errno_message("make ExaSock socket nonblocking");
        return false;
    }
    const int descriptor = ::fcntl(fd, F_GETFD, 0);
    if (descriptor == -1 || ::fcntl(fd, F_SETFD, descriptor | FD_CLOEXEC) == -1) {
        error = errno_message("make ExaSock socket close-on-exec");
        return false;
    }
    return true;
}

bool wait_for_connect(int fd, Clock::time_point deadline, std::string& error,
                      bool& timed_out) {
    timed_out = false;
    for (;;) {
        const int timeout = remaining_ms(deadline);
        if (timeout == 0) {
            error = "ExaSock TCP connect timed out";
            timed_out = true;
            return false;
        }
        pollfd descriptor{.fd = fd, .events = POLLOUT, .revents = 0};
        const int ready = ::poll(&descriptor, 1, timeout);
        if (ready == -1 && errno == EINTR) continue;
        if (ready == 0) {
            error = "ExaSock TCP connect timed out";
            timed_out = true;
            return false;
        }
        if (ready == -1) {
            error = errno_message("poll ExaSock TCP connect");
            return false;
        }

        int socket_error = 0;
        socklen_t length = sizeof(socket_error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) == -1) {
            error = errno_message("read ExaSock TCP connect result");
            return false;
        }
        if (socket_error != 0) {
            error = errno_message("connect ExaSock TCP socket", socket_error);
            return false;
        }
        return true;
    }
}

class ExasockTransport final : public Transport {
public:
    explicit ExasockTransport(int fd) : fd_(fd), receive_(kReceiveBufferBytes) {}
    ~ExasockTransport() override { close(); }

    std::size_t max_fragment_size() const noexcept override {
        return kInlineFragmentBytes;
    }
    std::size_t max_bulk_fragment_size() const noexcept override {
        return kBulkFragmentBytes;
    }

    bool try_send(std::string_view fragment) noexcept override {
        return try_send_common(fragment);
    }
    bool try_send_bulk(std::string_view fragment) noexcept override {
        return try_send_common(fragment);
    }

    std::optional<std::string_view> peek() noexcept override {
        progress_send();
        if (failed_ || disconnected_ || fd_ == -1) return std::nullopt;
        if (receive_exposed_)
            return std::string_view(receive_.data(), receive_size_);

        for (;;) {
            const ssize_t received =
                ::recv(fd_, receive_.data(), receive_.size(), MSG_DONTWAIT);
            if (received > 0) {
                receive_size_ = static_cast<std::size_t>(received);
                receive_exposed_ = true;
                return std::string_view(receive_.data(), receive_size_);
            }
            if (received == 0) {
                disconnected_ = true;
                return std::nullopt;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return std::nullopt;
            fail(errno_message("receive from ExaSock TCP socket"));
            return std::nullopt;
        }
    }

    void pop() noexcept override {
        receive_exposed_ = false;
        receive_size_ = 0;
    }

    bool failed() const noexcept override { return failed_; }
    bool disconnected() const noexcept override { return disconnected_; }
    std::string_view error() const noexcept override { return error_; }

    void wait() noexcept override {
        progress_send();
        cpu_relax();
    }

    void close() noexcept override {
        if (fd_ == -1) return;
        (void)::shutdown(fd_, SHUT_RDWR);
        (void)::close(fd_);
        fd_ = -1;
        disconnected_ = true;
        pending_.clear();
        pending_offset_ = 0;
        receive_exposed_ = false;
        receive_size_ = 0;
    }

private:
    int fd_ = -1;
    std::vector<char> receive_;
    std::string pending_;
    std::size_t pending_offset_ = 0;
    std::size_t receive_size_ = 0;
    bool receive_exposed_ = false;
    bool failed_ = false;
    bool disconnected_ = false;
    std::string error_;

    void fail(std::string message) noexcept {
        if (!failed_) error_ = std::move(message);
        failed_ = true;
    }

    void progress_send() noexcept {
        while (!failed_ && !disconnected_ && pending_offset_ < pending_.size()) {
            const char* data = pending_.data() + pending_offset_;
            const std::size_t available = pending_.size() - pending_offset_;
            const ssize_t sent = ::send(fd_, data, available, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (sent > 0) {
                pending_offset_ += static_cast<std::size_t>(sent);
                continue;
            }
            if (sent == 0) {
                fail("send to ExaSock TCP socket made no progress");
                return;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EPIPE || errno == ECONNRESET) disconnected_ = true;
            fail(errno_message("send to ExaSock TCP socket"));
            return;
        }
        if (pending_offset_ == pending_.size()) {
            pending_.clear();
            pending_offset_ = 0;
        }
    }

    bool try_send_common(std::string_view fragment) noexcept {
        if (fragment.empty() || fragment.size() > kBulkFragmentBytes) return false;
        progress_send();
        if (failed_ || disconnected_ || fd_ == -1 || !pending_.empty()) return false;

        for (;;) {
            const ssize_t sent =
                ::send(fd_, fragment.data(), fragment.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
            if (sent > 0) {
                const std::size_t consumed = static_cast<std::size_t>(sent);
                if (consumed < fragment.size()) {
                    try {
                        pending_.assign(fragment.data() + consumed,
                                        fragment.size() - consumed);
                    } catch (...) {
                        fail("retain partially sent ExaSock TCP fragment: out of memory");
                        return false;
                    }
                }
                // A true result means the transport owns the complete fragment;
                // any unsent suffix is retained above and drained before the
                // next inline or bulk fragment can enter the socket stream.
                return true;
            }
            if (sent == 0) {
                fail("send to ExaSock TCP socket made no progress");
                return false;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
            if (errno == EPIPE || errno == ECONNRESET) disconnected_ = true;
            fail(errno_message("send to ExaSock TCP socket"));
            return false;
        }
    }
};

std::unique_ptr<Transport> open_exasock(const ExasockOptions& options,
                                        std::string& error, bool& timed_out) {
    timed_out = false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(options.port);
    if (::inet_pton(AF_INET, options.address.c_str(), &address.sin_addr) != 1) {
        error = "ExaSock address must be a numeric IPv4 address";
        return nullptr;
    }

    const auto deadline = Clock::now() + options.connect_timeout;

    const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == -1) {
        error = errno_message("create ExaSock TCP socket");
        return nullptr;
    }

    auto close_fd = [&] { (void)::close(fd); };
    if (!set_descriptor_flags(fd, error)) {
        close_fd();
        return nullptr;
    }

    const int enabled = 0;
    if (::setsockopt(fd, SOL_EXASOCK, SO_EXA_NO_ACCEL, &enabled,
                     sizeof(enabled)) == -1) {
        error = errno_message("request ExaSock acceleration");
        close_fd();
        return nullptr;
    }

    const int no_delay = 1;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay,
                     sizeof(no_delay)) == -1) {
        error = errno_message("enable TCP_NODELAY on ExaSock socket");
        close_fd();
        return nullptr;
    }

    const int connected = ::connect(
        fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (connected == -1 && errno != EINPROGRESS) {
        error = errno_message("connect ExaSock TCP socket");
        close_fd();
        return nullptr;
    }
    if (connected == -1 && !wait_for_connect(fd, deadline, error, timed_out)) {
        close_fd();
        return nullptr;
    }

    std::array<char, 64> device{};
    int port = -1;
    const auto get_device =
        resolve_exasock_symbol<ExasockTcpGetDevice>("exasock_tcp_get_device");
    if (!get_device) {
        error = "active ExaSock library does not publish exasock_tcp_get_device";
        close_fd();
        return nullptr;
    }
    if (get_device(fd, device.data(), device.size(), &port) == -1) {
        error = errno_message(
            "ExaSock connected the TCP socket without SmartNIC acceleration");
        close_fd();
        return nullptr;
    }

    try {
        return std::make_unique<ExasockTransport>(fd);
    } catch (...) {
        close_fd();
        throw;
    }
}

[[noreturn]] void throw_open_failure(bool timed_out, std::string error) {
    if (timed_out) throw TimeoutError(std::move(error));
    throw ConnectionError(std::move(error));
}

} // namespace

bool exasock_available() noexcept { return true; }

bool exasock_active() noexcept {
    const auto loaded = resolve_exasock_symbol<ExasockLoaded>("exasock_loaded");
    return loaded && loaded() != 0;
}

std::unique_ptr<Transport> connect_exasock(const ExasockOptions& options) {
    if (options.address.empty())
        throw std::invalid_argument("ExaSock address must not be empty");
    if (options.port == 0)
        throw std::invalid_argument("ExaSock port must not be zero");
    if (options.connect_timeout.count() < 0 || options.operation_timeout.count() < 0)
        throw std::invalid_argument("timeouts must not be negative");
    in_addr numeric_address{};
    if (::inet_pton(AF_INET, options.address.c_str(), &numeric_address) != 1)
        throw std::invalid_argument("ExaSock address must be a numeric IPv4 address");
    if (!exasock_active()) {
        throw ConnectionError(
            "ExaSock support is built, but its interception library is not active; "
            "start the application with the system 'exasock' launcher");
    }

    std::string error;
    bool timed_out = false;
    auto transport = open_exasock(options, error, timed_out);
    if (!transport) throw_open_failure(timed_out, std::move(error));
    return transport;
}

#if defined(GOBLIN_STORE_CLIENT_TESTING) && GOBLIN_STORE_CLIENT_TESTING
namespace detail {

std::unique_ptr<Transport> make_exasock_transport_for_test(int fd) {
    return std::make_unique<ExasockTransport>(fd);
}

void throw_exasock_open_failure_for_test(bool timed_out) {
    throw_open_failure(timed_out, timed_out ? "test connect timed out"
                                           : "test connect failed");
}

} // namespace detail
#endif

} // namespace goblin::client
