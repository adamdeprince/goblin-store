#include "mini_test.hpp"

#include "goblin/core/reactor.hpp"
#include "goblin/net/stream_io.hpp"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <print>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

using namespace goblin;

namespace {

#if defined(__linux__)
bool make_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

std::optional<core::Completion> wait_one(net::StreamIo& io, unsigned attempts = 20) {
    for (unsigned i = 0; i < attempts; ++i) {
        io.submit_and_wait_timeout(25);
        std::array<core::Completion, 1> completion{};
        if (io.reap(completion) == 1) return completion[0];
    }
    return std::nullopt;
}
#endif

} // namespace

TEST("readiness stream I/O reports socket receive completions") {
#if !defined(__linux__)
    std::println("    (skipped: readiness backend is Linux-only)");
    return;
#else
    if (!core::Reactor::available()) {
        std::println("    (skipped: built without liburing)");
        return;
    }
    auto reactor = core::Reactor::create();
    if (!reactor) {
        std::println("    (skipped: io_uring unavailable: {})", reactor.error().detail);
        return;
    }
    auto io = net::make_readiness_stream_io(*reactor);
    CHECK(io.has_value());
    if (!io) return;

    int sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    if (sockets[0] < 0) return;
    CHECK(make_nonblocking(sockets[0]));
    std::array<std::byte, 16> buffer{};
    constexpr std::uint64_t token = 0x1234;
    CHECK((*io)->submit_recv(sockets[0], buffer, token));
    CHECK(::send(sockets[1], "hello", 5, 0) == 5);
    const auto completion = wait_one(**io);
    CHECK(completion.has_value());
    if (completion) {
        CHECK(completion->user_data == token);
        CHECK(completion->res == 5);
        CHECK(std::memcmp(buffer.data(), "hello", 5) == 0);
    }
    (*io)->close_fd(sockets[0]);
    ::close(sockets[1]);
#endif
}

TEST("readiness stream I/O cancels a pending operation on close") {
#if !defined(__linux__)
    std::println("    (skipped: readiness backend is Linux-only)");
    return;
#else
    if (!core::Reactor::available()) {
        std::println("    (skipped: built without liburing)");
        return;
    }
    auto reactor = core::Reactor::create();
    if (!reactor) {
        std::println("    (skipped: io_uring unavailable: {})", reactor.error().detail);
        return;
    }
    auto io = net::make_readiness_stream_io(*reactor);
    CHECK(io.has_value());
    if (!io) return;

    int sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    if (sockets[0] < 0) return;
    CHECK(make_nonblocking(sockets[0]));
    std::array<std::byte, 8> buffer{};
    constexpr std::uint64_t token = 0x5678;
    CHECK((*io)->submit_recv(sockets[0], buffer, token));
    (*io)->close_fd(sockets[0]);
    const auto completion = wait_one(**io);
    CHECK(completion.has_value());
    if (completion) {
        CHECK(completion->user_data == token);
        CHECK(completion->res == -ECANCELED);
    }
    ::close(sockets[1]);
#endif
}

TEST("readiness stream I/O accepts a nonblocking TCP connection") {
#if !defined(__linux__)
    std::println("    (skipped: readiness backend is Linux-only)");
    return;
#else
    if (!core::Reactor::available()) {
        std::println("    (skipped: built without liburing)");
        return;
    }
    auto reactor = core::Reactor::create();
    if (!reactor) {
        std::println("    (skipped: io_uring unavailable: {})", reactor.error().detail);
        return;
    }
    auto io = net::make_readiness_stream_io(*reactor);
    CHECK(io.has_value());
    if (!io) return;

    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    if (listener < 0) return;
    CHECK(make_nonblocking(listener));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    CHECK(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
    CHECK(::listen(listener, 1) == 0);
    socklen_t address_length = sizeof address;
    CHECK(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_length) == 0);

    constexpr std::uint64_t token = 0x9abc;
    CHECK((*io)->submit_accept(listener, token));
    const int client = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(client >= 0);
    if (client < 0) {
        (*io)->close_fd(listener);
        return;
    }
    CHECK(::connect(client, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);

    const auto completion = wait_one(**io);
    CHECK(completion.has_value());
    if (completion) {
        CHECK(completion->user_data == token);
        CHECK(completion->res >= 0);
        if (completion->res >= 0) {
            const int flags = ::fcntl(completion->res, F_GETFL, 0);
            CHECK(flags >= 0);
            CHECK((flags & O_NONBLOCK) != 0);
            (*io)->close_fd(completion->res);
        }
    }
    (*io)->close_fd(listener);
    ::close(client);
#endif
}

TEST("readiness stream I/O preserves send and sendmsg completions") {
#if !defined(__linux__)
    std::println("    (skipped: readiness backend is Linux-only)");
    return;
#else
    if (!core::Reactor::available()) {
        std::println("    (skipped: built without liburing)");
        return;
    }
    auto reactor = core::Reactor::create();
    if (!reactor) {
        std::println("    (skipped: io_uring unavailable: {})", reactor.error().detail);
        return;
    }
    auto io = net::make_readiness_stream_io(*reactor);
    CHECK(io.has_value());
    if (!io) return;

    int sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    if (sockets[0] < 0) return;
    CHECK(make_nonblocking(sockets[0]));

    const std::array<std::byte, 4> first{
        std::byte{'h'}, std::byte{'e'}, std::byte{'a'}, std::byte{'d'}};
    CHECK((*io)->submit_send(sockets[0], first, 0x1111));
    auto completion = wait_one(**io);
    CHECK(completion.has_value());
    if (completion) {
        CHECK(completion->user_data == 0x1111);
        CHECK(completion->res == static_cast<int>(first.size()));
    }
    std::array<std::byte, 8> received{};
    CHECK(::recv(sockets[1], received.data(), received.size(), 0) ==
          static_cast<ssize_t>(first.size()));
    CHECK(std::memcmp(received.data(), first.data(), first.size()) == 0);

    std::array<char, 2> second{'t', 'a'};
    std::array<char, 2> third{'i', 'l'};
    std::array<iovec, 2> vectors{{
        {second.data(), second.size()},
        {third.data(), third.size()},
    }};
    msghdr message{};
    message.msg_iov = vectors.data();
    message.msg_iovlen = vectors.size();
    CHECK((*io)->submit_sendmsg(sockets[0], &message, 0x2222));
    completion = wait_one(**io);
    CHECK(completion.has_value());
    if (completion) {
        CHECK(completion->user_data == 0x2222);
        CHECK(completion->res == 4);
    }
    CHECK(::recv(sockets[1], received.data(), received.size(), 0) == 4);
    CHECK(std::memcmp(received.data(), "tail", 4) == 0);

    (*io)->close_fd(sockets[0]);
    ::close(sockets[1]);
#endif
}

TEST("readiness stream I/O wakes for disk reactor completions") {
#if !defined(__linux__)
    std::println("    (skipped: readiness backend is Linux-only)");
    return;
#else
    if (!core::Reactor::available()) {
        std::println("    (skipped: built without liburing)");
        return;
    }
    auto reactor = core::Reactor::create();
    if (!reactor) {
        std::println("    (skipped: io_uring unavailable: {})", reactor.error().detail);
        return;
    }
    auto io = net::make_readiness_stream_io(*reactor);
    CHECK(io.has_value());
    if (!io) return;

    char path[] = "/var/tmp/goblin-readiness-XXXXXX";
    const int file = ::mkstemp(path);
    CHECK(file >= 0);
    if (file < 0) return;
    ::unlink(path);
    constexpr char payload[] = "disk completion";
    CHECK(::write(file, payload, sizeof payload) == static_cast<ssize_t>(sizeof payload));

    std::array<std::byte, sizeof payload> buffer{};
    constexpr std::uint64_t token = 0x3333;
    CHECK((*io)->submit_read(file, 0, buffer, token));
    const auto completion = wait_one(**io);
    CHECK(completion.has_value());
    if (completion) {
        CHECK(completion->user_data == token);
        CHECK(completion->res == static_cast<int>(sizeof payload));
        CHECK(std::memcmp(buffer.data(), payload, sizeof payload) == 0);
    }
    ::close(file);
#endif
}
