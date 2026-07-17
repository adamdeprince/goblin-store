#include "exasock_transport_test.hpp"
#include "goblin/store/client.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <sys/socket.h>
#include <unistd.h>

namespace {

using goblin::client::ConnectionError;
using goblin::client::ExasockOptions;
using goblin::client::TimeoutError;
using goblin::client::Transport;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void check(bool condition, std::string_view message) {
    if (!condition) fail(message);
}

template <class Expected, class Function>
void check_throws(Function&& function, std::string_view message) {
    try {
        std::forward<Function>(function)();
    } catch (const Expected&) {
        return;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << message << ": wrong exception: " << error.what() << '\n';
        std::exit(1);
    }
    fail(message);
}

class Descriptor {
public:
    explicit Descriptor(int fd = -1) noexcept : fd_(fd) {}
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Descriptor& operator=(Descriptor&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    ~Descriptor() {
        if (fd_ != -1) (void)::close(fd_);
    }

    int get() const noexcept { return fd_; }
    int release() noexcept { return std::exchange(fd_, -1); }
    void close() noexcept {
        if (fd_ != -1) (void)::close(fd_);
        fd_ = -1;
    }

private:
    int fd_;
};

struct SocketPair {
    std::unique_ptr<Transport> transport;
    Descriptor peer;
};

SocketPair socket_pair() {
    int descriptors[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                     descriptors) == -1) {
        throw std::runtime_error("socketpair failed");
    }

    Descriptor sender(descriptors[0]);
    Descriptor peer(descriptors[1]);
    const int send_buffer = 4096;
    if (::setsockopt(sender.get(), SOL_SOCKET, SO_SNDBUF, &send_buffer,
                     sizeof(send_buffer)) == -1) {
        throw std::runtime_error("setsockopt(SO_SNDBUF) failed");
    }

    auto transport = goblin::client::detail::make_exasock_transport_for_test(
        sender.release());
    return {std::move(transport), std::move(peer)};
}

void drain_peer(int fd, std::string& output) {
    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t count = ::recv(fd, buffer.data(), buffer.size(), MSG_DONTWAIT);
        if (count > 0) {
            output.append(buffer.data(), static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) return;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        throw std::runtime_error("recv from socketpair failed");
    }
}

void test_partial_send_preserves_stream_order() {
    auto pair = socket_pair();
    constexpr std::size_t kBodyBytes = 256 * 1024;
    const std::string header = "set ordered 7 0 262144\r\n";
    std::string body(kBodyBytes, '\0');
    for (std::size_t index = 0; index < body.size(); ++index)
        body[index] = static_cast<char>((index * 37U) & 0xffU);
    const std::string trailer = "\r\n";
    const std::string expected = header + body + trailer;

    check(pair.transport->try_send(header), "small inline header was not accepted");
    check(pair.transport->try_send_bulk(body), "bulk body was not accepted");
    check(!pair.transport->try_send(trailer),
          "tiny send buffer did not force pending-body backpressure");

    std::string received;
    received.reserve(expected.size());
    bool trailer_owned = false;
    constexpr std::size_t kMaximumPasses = 100000;
    for (std::size_t pass = 0;
         pass < kMaximumPasses && (!trailer_owned || received.size() < expected.size());
         ++pass) {
        drain_peer(pair.peer.get(), received);
        pair.transport->wait();
        if (!trailer_owned) trailer_owned = pair.transport->try_send(trailer);
        if (received.size() < expected.size()) std::this_thread::yield();
    }
    drain_peer(pair.peer.get(), received);

    check(trailer_owned, "trailer never became transport-owned after body drain");
    check(!pair.transport->failed(), "partial send marked the transport failed");
    check(!pair.transport->disconnected(), "partial send disconnected the transport");
    check(received == expected,
          "partial send duplicated, dropped, or reordered header/body/trailer bytes");
}

void test_receive_eagain_and_peer_disconnect() {
    auto pair = socket_pair();
    check(!pair.transport->peek(), "empty nonblocking socket unexpectedly had input");
    check(!pair.transport->failed(), "EAGAIN marked receive as failed");

    constexpr std::string_view message = "VALUE key 0 3\r\nbug\r\nEND\r\n";
    const ssize_t sent = ::send(pair.peer.get(), message.data(), message.size(), MSG_NOSIGNAL);
    check(sent == static_cast<ssize_t>(message.size()), "socketpair test message was not sent");
    std::string received;
    for (std::size_t pass = 0; pass < 1000 && received.size() < message.size(); ++pass) {
        if (const auto fragment = pair.transport->peek()) {
            received.append(fragment->data(), fragment->size());
            pair.transport->pop();
        } else {
            pair.transport->wait();
        }
    }
    check(received == message, "receive fragments did not reconstruct the peer bytes");

    pair.peer.close();
    for (std::size_t pass = 0; pass < 1000 && !pair.transport->disconnected(); ++pass) {
        (void)pair.transport->peek();
        pair.transport->wait();
    }
    check(pair.transport->disconnected(), "peer EOF was not reported as a disconnect");
    check(!pair.transport->failed(), "orderly peer EOF was reported as an I/O failure");
}

void test_connect_failure_exception_types() {
    check_throws<TimeoutError>(
        [] { goblin::client::detail::throw_exasock_open_failure_for_test(true); },
        "ExaSock connect timeout did not preserve TimeoutError");
    check_throws<ConnectionError>(
        [] { goblin::client::detail::throw_exasock_open_failure_for_test(false); },
        "ExaSock non-timeout connect failure did not use ConnectionError");
}

void test_public_endpoint_rejects_non_ipv4_before_preload_check() {
    constexpr std::array<std::string_view, 2> addresses{"::1", "localhost"};
    for (const std::string_view address : addresses) {
        ExasockOptions options;
        options.address = std::string(address);
        check_throws<std::invalid_argument>(
            [&] { (void)goblin::client::Client::connect_exasock(options); },
            "ExaSock accepted a non-IPv4 endpoint");
    }
}

} // namespace

int main() {
    test_partial_send_preserves_stream_order();
    test_receive_eagain_and_peer_disconnect();
    test_connect_failure_exception_types();
    test_public_endpoint_rejects_non_ipv4_before_preload_check();
    std::cout << "goblin_store_exasock_transport_tests: all tests passed\n";
}
