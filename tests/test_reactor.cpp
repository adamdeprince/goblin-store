#include "mini_test.hpp"

#include "goblin/core/reactor.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace goblin;
using namespace goblin::core;

TEST("reactor: io_uring read returns the file's bytes") {
    if (!Reactor::available()) {
        std::println("    (skipped: built without liburing)");
        return;
    }

    const std::string path =
        (fs::temp_directory_path() / ("goblin-reactor-" + std::to_string(::getpid()))).string();
    const std::string contents = "goblin reactor io_uring test payload";
    { std::ofstream f(path); f << contents; }

    const int fd = ::open(path.c_str(), O_RDONLY);
    CHECK(fd >= 0);

    auto r = Reactor::create();
    if (!r) {
        // io_uring may be unavailable at runtime (e.g. blocked by seccomp); don't fail the suite.
        std::println("    (skipped: io_uring unavailable at runtime: {})", r.error().detail);
        ::close(fd);
        fs::remove(path);
        return;
    }

    std::array<std::byte, 128> buf{};
    const auto n = r->read_sync(fd, 0, MutBytes(buf.data(), buf.size()));
    CHECK(n.has_value());
    if (n) {
        CHECK_EQ(*n, contents.size());
        const std::string got(reinterpret_cast<const char*>(buf.data()), *n);
        CHECK(got == contents);
    }

    ::close(fd);
    fs::remove(path);
}

TEST("reactor net: socketpair recv + send round-trip via the ring") {
    if (!Reactor::available()) {
        std::println("    (skipped: built without liburing)");
        return;
    }
    auto r = Reactor::create();
    if (!r) {
        std::println("    (skipped: io_uring unavailable: {})", r.error().detail);
        return;
    }

    int sp[2];
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);

    const char msg[] = "goblin-uring-net";
    constexpr std::uint64_t UD_RECV = 0x10, UD_SEND = 0x20;
    std::array<std::byte, 64> rbuf{};
    CHECK(r->submit_recv(sp[1], MutBytes(rbuf.data(), rbuf.size()), UD_RECV));
    CHECK(r->submit_send(sp[0], ByteView(reinterpret_cast<const std::byte*>(msg), sizeof msg - 1),
                         UD_SEND));
    r->submit_and_wait(2);

    std::array<Completion, 8> cqes{};
    const unsigned n = r->reap(cqes);
    CHECK_EQ(n, 2u);
    int recv_res = -1, send_res = -1;
    for (unsigned i = 0; i < n; ++i) {
        if (cqes[i].user_data == UD_RECV) recv_res = cqes[i].res;       // dispatch by user_data
        else if (cqes[i].user_data == UD_SEND) send_res = cqes[i].res;
    }
    CHECK_EQ(send_res, int(sizeof msg - 1));
    CHECK_EQ(recv_res, int(sizeof msg - 1));
    CHECK(std::memcmp(rbuf.data(), msg, sizeof msg - 1) == 0);

    ::close(sp[0]);
    ::close(sp[1]);
}

TEST("reactor net: a 1 MiB transfer drains across multiple short send/recv completions") {
    if (!Reactor::available()) {
        std::println("    (skipped: built without liburing)");
        return;
    }
    auto r = Reactor::create();
    if (!r) {
        std::println("    (skipped: io_uring unavailable: {})", r.error().detail);
        return;
    }

    int sp[2];
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);

    constexpr std::size_t N = 1u << 20; // 1 MiB >> socket buffer => partial sends + many recvs
    std::vector<std::byte> src(N), dst(N, std::byte{0});
    for (std::size_t i = 0; i < N; ++i) src[i] = static_cast<std::byte>((i * 31 + 7) & 0xFF);

    constexpr std::uint64_t UD_RECV = 1, UD_SEND = 2;
    std::size_t sent = 0, recvd = 0, last_send_len = 0;
    bool send_inflight = false, recv_inflight = false, short_send_seen = false;
    int guard = 0;
    while (recvd < N && guard++ < 1'000'000) {
        if (!send_inflight && sent < N) {
            last_send_len = N - sent;
            CHECK(r->submit_send(sp[0], ByteView(src.data() + sent, last_send_len), UD_SEND));
            send_inflight = true;
        }
        if (!recv_inflight && recvd < N) {
            const std::size_t want = std::min<std::size_t>(64u * 1024, N - recvd);
            CHECK(r->submit_recv(sp[1], MutBytes(dst.data() + recvd, want), UD_RECV));
            recv_inflight = true;
        }
        r->submit_and_wait(1);
        std::array<Completion, 8> cqes{};
        const unsigned n = r->reap(cqes);
        for (unsigned i = 0; i < n; ++i) {
            if (cqes[i].user_data == UD_SEND) {
                send_inflight = false;
                if (cqes[i].res > 0) {
                    if (static_cast<std::size_t>(cqes[i].res) < last_send_len) short_send_seen = true;
                    sent += static_cast<std::size_t>(cqes[i].res);
                }
            } else if (cqes[i].user_data == UD_RECV) {
                recv_inflight = false;
                if (cqes[i].res > 0) recvd += static_cast<std::size_t>(cqes[i].res);
            }
        }
    }
    CHECK_EQ(recvd, N);
    CHECK(short_send_seen); // a 1 MiB send can't complete in one shot -> res is reported short
    CHECK(std::memcmp(dst.data(), src.data(), N) == 0);

    ::close(sp[0]);
    ::close(sp[1]);
}
