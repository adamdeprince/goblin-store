#include "mini_test.hpp"

#include "goblin/core/buffer_pool.hpp"
#include "goblin/core/reactor.hpp"
#include "goblin/crypto/sha256.hpp"
#include "goblin/protocol/memcache/event_loop.hpp"
#include "goblin/storage/index.hpp"
#include "goblin/storage/tier_manager.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <netinet/in.h>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace goblin;
using namespace goblin::core;
using goblin::crypto::hash_key;
using goblin::memcache::EventLoop;
using goblin::storage::Index;
using goblin::storage::TierManager;

static std::pair<int, std::uint16_t> make_loopback_listener() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return {-1, 0};
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) { ::close(fd); return {-1, 0}; }
    socklen_t al = sizeof a;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &al);
    if (::listen(fd, 256) < 0) { ::close(fd); return {-1, 0}; }
    return {fd, ntohs(a.sin_port)};
}

static int client_connect(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    timeval tv{8, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) { ::close(fd); return -1; }
    return fd;
}

static bool write_all(int fd, const void* p, std::size_t n) {
    const char* c = static_cast<const char*>(p);
    for (std::size_t s = 0; s < n;) {
        const ssize_t w = ::send(fd, c + s, n - s, 0);
        if (w < 0 && errno == EINTR) continue; // a signal interrupted the syscall — retry
        if (w <= 0) return false;
        s += static_cast<std::size_t>(w);
    }
    return true;
}

static std::string read_until(int fd, std::string_view marker) {
    std::string s;
    char buf[1024];
    while (s.find(marker) == std::string::npos) {
        const ssize_t k = ::recv(fd, buf, sizeof buf, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        s.append(buf, static_cast<std::size_t>(k));
    }
    return s;
}

// Send "get <key>", parse "VALUE k f n\r\n<n bytes>\r\nEND\r\n"; nullopt on miss/error.
static std::optional<std::string> get_value(int fd, const std::string& key) {
    const std::string req = "get " + key + "\r\n";
    if (!write_all(fd, req.data(), req.size())) return std::nullopt;
    std::string buf;
    char tmp[4096];
    while (buf.find("\r\n") == std::string::npos) {
        const ssize_t k = ::recv(fd, tmp, sizeof tmp, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return std::nullopt;
        buf.append(tmp, static_cast<std::size_t>(k));
    }
    const auto hdr_end = buf.find("\r\n");
    const std::string hdr = buf.substr(0, hdr_end);
    if (hdr.rfind("VALUE ", 0) != 0) return std::nullopt; // miss ("END") or error
    const auto sp = hdr.rfind(' ');
    const std::size_t size = static_cast<std::size_t>(std::stoul(hdr.substr(sp + 1)));
    const std::size_t need = hdr_end + 2 + size + 7; // header + value + "\r\nEND\r\n"
    while (buf.size() < need) {
        const ssize_t k = ::recv(fd, tmp, sizeof tmp, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return std::nullopt;
        buf.append(tmp, static_cast<std::size_t>(k));
    }
    return buf.substr(hdr_end + 2, size);
}

static std::string pattern(int seed, std::size_t n) {
    std::string s(n, '\0');
    for (std::size_t j = 0; j < n; ++j)
        s[j] = static_cast<char>((j * 7 + static_cast<std::size_t>(seed) * 131 + 3) & 0xFF);
    return s;
}

static Result<TierManager> open_tm(const std::string& base, Index& index, bool three_layer,
                                   unsigned write_buffers = 8) {
    PoolConfig ssd, hdd;
    ssd.stripe_unit = 64 * KiB;
    ssd.dirs = {base + "/s0", base + "/s1"};
    for (const auto& d : ssd.dirs) fs::create_directories(d);
    if (three_layer) {
        hdd.stripe_unit = 64 * KiB;
        hdd.dirs = {base + "/h0", base + "/h1"};
        for (const auto& d : hdd.dirs) fs::create_directories(d);
    }
    TierSizes tiers;
    tiers.ram_head = 4 * KiB;
    tiers.ssd_prefix = three_layer ? 64 * KiB : 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 64 * MiB;
    mem.block_bytes = 1 * MiB;
    mem.lock_memory = false;
    EvictionConfig ev;
    return TierManager::open(tiers, mem, ev, ssd, hdd, index, 256 * KiB, write_buffers, false);
}

static bool store_obj(TierManager& tm, const std::string& key, const std::string& val) {
    return tm.store(hash_key(key), ByteView(reinterpret_cast<const std::byte*>(val.data()), val.size()), 0)
        .has_value();
}

static std::string tmp_base(const char* tag) {
    return (fs::temp_directory_path() / ("goblin-evl-" + std::string(tag) + std::to_string(::getpid())))
        .string();
}

static Result<IoBufferPool> make_iopool() { return IoBufferPool::create(128 * KiB, 64, false); }

// Send "<verb> key 0 0 <len>\r\n<val>\r\n"; return the reply line ("STORED\r\n", etc.).
static std::string set_value(int fd, const std::string& key, const std::string& val,
                             const char* verb = "set") {
    std::string req = std::string(verb) + " " + key + " 0 0 " + std::to_string(val.size()) + "\r\n";
    req += val;
    req += "\r\n";
    if (!write_all(fd, req.data(), req.size())) return {};
    return read_until(fd, "\r\n");
}

// Like set_value but with a caller-chosen data-block terminator (to test a bad CRLF).
static std::string set_bad(int fd, const std::string& key, const std::string& val,
                           const std::string& trailer) {
    const std::string req =
        "set " + key + " 0 0 " + std::to_string(val.size()) + "\r\n" + val + trailer;
    if (!write_all(fd, req.data(), req.size())) return {};
    return read_until(fd, "\r\n");
}

// ----------------------------------------------------------------------------- Step 2: verbs

TEST("event loop: version + delete, pipelined in one request") {
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc = Reactor::create();
    if (!rc) { std::println("    (skipped: io_uring unavailable: {})", rc.error().detail); return; }
    const std::string base = tmp_base("verb");
    Index index;
    auto tm = open_tm(base, index, false);
    auto io = make_iopool();
    CHECK(tm.has_value() && io.has_value());
    if (!tm || !io) { fs::remove_all(base); return; }
    CHECK(store_obj(*tm, "foo", std::string(16, 'x')));

    auto [lfd, port] = make_loopback_listener();
    CHECK(lfd >= 0);
    if (lfd < 0) { fs::remove_all(base); return; }
    EventLoop loop(*rc, lfd, *tm, index, *io);
    std::thread th([&] { loop.run(); });

    bool ok = false;
    const int c = client_connect(port);
    if (c >= 0) {
        const std::string req = "version\r\ndelete foo\r\ndelete foo\r\n";
        if (write_all(c, req.data(), req.size())) {
            const std::string r = read_until(c, "NOT_FOUND\r\n");
            const auto pv = r.find("VERSION "), pd = r.find("DELETED\r\n"), pn = r.find("NOT_FOUND\r\n");
            ok = pv == 0 && pd != std::string::npos && pn != std::string::npos && pv < pd && pd < pn;
        }
        ::close(c);
    }
    loop.stop();
    th.join();
    ::close(lfd);
    fs::remove_all(base);
    CHECK(ok);
}

TEST("event loop: a command split across two recvs still parses") {
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc = Reactor::create();
    if (!rc) { std::println("    (skipped: io_uring unavailable: {})", rc.error().detail); return; }
    const std::string base = tmp_base("split");
    Index index;
    auto tm = open_tm(base, index, false);
    auto io = make_iopool();
    CHECK(tm.has_value() && io.has_value());
    if (!tm || !io) { fs::remove_all(base); return; }

    auto [lfd, port] = make_loopback_listener();
    CHECK(lfd >= 0);
    if (lfd < 0) { fs::remove_all(base); return; }
    EventLoop loop(*rc, lfd, *tm, index, *io);
    std::thread th([&] { loop.run(); });

    bool ok = false;
    const int c = client_connect(port);
    if (c >= 0) {
        write_all(c, "vers", 4);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        write_all(c, "ion\r\n", 5);
        ok = read_until(c, "\r\n").find("VERSION ") == 0;
        ::close(c);
    }
    loop.stop();
    th.join();
    ::close(lfd);
    fs::remove_all(base);
    CHECK(ok);
}

TEST("event loop: an unknown command returns ERROR") {
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc = Reactor::create();
    if (!rc) { std::println("    (skipped: io_uring unavailable: {})", rc.error().detail); return; }
    const std::string base = tmp_base("bad");
    Index index;
    auto tm = open_tm(base, index, false);
    auto io = make_iopool();
    CHECK(tm.has_value() && io.has_value());
    if (!tm || !io) { fs::remove_all(base); return; }

    auto [lfd, port] = make_loopback_listener();
    CHECK(lfd >= 0);
    if (lfd < 0) { fs::remove_all(base); return; }
    EventLoop loop(*rc, lfd, *tm, index, *io);
    std::thread th([&] { loop.run(); });

    bool ok = false;
    const int c = client_connect(port);
    if (c >= 0) {
        write_all(c, "bogus-command\r\n", 15);
        ok = read_until(c, "\r\n").find("ERROR") == 0;
        ::close(c);
    }
    loop.stop();
    th.join();
    ::close(lfd);
    fs::remove_all(base);
    CHECK(ok);
}

TEST("event loop: 1000 connect/close churn, then still serves (lifecycle / ASan)") {
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc = Reactor::create();
    if (!rc) { std::println("    (skipped: io_uring unavailable: {})", rc.error().detail); return; }
    const std::string base = tmp_base("churn");
    Index index;
    auto tm = open_tm(base, index, false);
    auto io = make_iopool();
    CHECK(tm.has_value() && io.has_value());
    if (!tm || !io) { fs::remove_all(base); return; }

    auto [lfd, port] = make_loopback_listener();
    CHECK(lfd >= 0);
    if (lfd < 0) { fs::remove_all(base); return; }
    EventLoop loop(*rc, lfd, *tm, index, *io);
    std::thread th([&] { loop.run(); });

    for (int i = 0; i < 1000; ++i) {
        const int c = client_connect(port);
        if (c >= 0) ::close(c);
    }
    bool ok = false;
    const int c = client_connect(port);
    if (c >= 0) {
        if (write_all(c, "version\r\n", 9)) ok = read_until(c, "\r\n").find("VERSION ") == 0;
        ::close(c);
    }
    loop.stop();
    th.join();
    ::close(lfd);
    fs::remove_all(base);
    CHECK(ok);
}

// ----------------------------------------------------------------------------- Step 3: async GET

TEST("event loop: GET head-resident / multi-piece / miss") {
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc = Reactor::create();
    if (!rc) { std::println("    (skipped: io_uring unavailable: {})", rc.error().detail); return; }
    const std::string base = tmp_base("get2");
    Index index;
    auto tm = open_tm(base, index, false);
    auto io = make_iopool();
    CHECK(tm.has_value() && io.has_value());
    if (!tm || !io) { fs::remove_all(base); return; }
    const std::string small = pattern(1, 2 * 1024);   // fully head-resident (<= ram_head)
    const std::string big = pattern(2, 512 * 1024);   // 4 pieces of 128 KiB, multi-segment
    CHECK(store_obj(*tm, "small", small));
    CHECK(store_obj(*tm, "big", big));

    auto [lfd, port] = make_loopback_listener();
    CHECK(lfd >= 0);
    if (lfd < 0) { fs::remove_all(base); return; }
    EventLoop loop(*rc, lfd, *tm, index, *io);
    std::thread th([&] { loop.run(); });

    bool ok = false;
    const int c = client_connect(port);
    if (c >= 0) {
        const auto a = get_value(c, "small");
        const auto b = get_value(c, "big");
        const auto m = get_value(c, "nope");
        ok = a && *a == small && b && *b == big && !m;
        ::close(c);
    }
    loop.stop();
    th.join();
    ::close(lfd);
    fs::remove_all(base);
    CHECK(ok);
}

TEST("event loop: GET spanning SSD prefix -> HDD tail (3-layer)") {
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc = Reactor::create();
    if (!rc) { std::println("    (skipped: io_uring unavailable: {})", rc.error().detail); return; }
    const std::string base = tmp_base("get3");
    Index index;
    auto tm = open_tm(base, index, true); // 3-layer, ssd_prefix 64 KiB
    auto io = make_iopool();
    CHECK(tm.has_value() && io.has_value());
    if (!tm || !io) { fs::remove_all(base); return; }
    const std::string v = pattern(7, 256 * 1024); // 4 KiB head + 60 KiB SSD + 192 KiB HDD
    CHECK(store_obj(*tm, "big3", v));

    auto [lfd, port] = make_loopback_listener();
    CHECK(lfd >= 0);
    if (lfd < 0) { fs::remove_all(base); return; }
    EventLoop loop(*rc, lfd, *tm, index, *io);
    std::thread th([&] { loop.run(); });

    bool ok = false;
    const int c = client_connect(port);
    if (c >= 0) {
        const auto g = get_value(c, "big3");
        ok = g && *g == v;
        ::close(c);
    }
    loop.stop();
    th.join();
    ::close(lfd);
    fs::remove_all(base);
    CHECK(ok);
}

TEST("event loop: two loops share storage; 64 concurrent GETs across both (TSan)") {
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc0 = Reactor::create();
    auto rc1 = Reactor::create();
    if (!rc0 || !rc1) { std::println("    (skipped: io_uring unavailable)"); return; }
    const std::string base = tmp_base("conc");
    Index index;
    auto tm = open_tm(base, index, false); // shared across both loops
    auto io0 = make_iopool();
    auto io1 = make_iopool();
    CHECK(tm.has_value() && io0.has_value() && io1.has_value());
    if (!tm || !io0 || !io1) { fs::remove_all(base); return; }

    constexpr int kN = 64;
    std::vector<std::string> vals(kN);
    for (int i = 0; i < kN; ++i) {
        vals[i] = pattern(i, 8 * 1024 + static_cast<std::size_t>(i) * 64);
        CHECK(store_obj(*tm, "k" + std::to_string(i), vals[i]));
    }

    auto [lfd0, port0] = make_loopback_listener();
    auto [lfd1, port1] = make_loopback_listener();
    CHECK(lfd0 >= 0 && lfd1 >= 0);
    if (lfd0 < 0 || lfd1 < 0) { fs::remove_all(base); return; }
    EventLoop loop0(*rc0, lfd0, *tm, index, *io0);
    EventLoop loop1(*rc1, lfd1, *tm, index, *io1);
    std::thread t0([&] { loop0.run(); });
    std::thread t1([&] { loop1.run(); });

    std::atomic<int> good{0};
    std::vector<std::thread> clients;
    for (int i = 0; i < kN; ++i) {
        clients.emplace_back([&, i] {
            const int c = client_connect(i % 2 ? port1 : port0);
            if (c < 0) return;
            const auto g = get_value(c, "k" + std::to_string(i));
            if (g && *g == vals[i]) good.fetch_add(1);
            ::close(c);
        });
    }
    for (auto& t : clients) t.join();
    loop0.stop();
    loop1.stop();
    t0.join();
    t1.join();
    ::close(lfd0);
    ::close(lfd1);
    fs::remove_all(base);
    CHECK_EQ(good.load(), kN);
}

// ----------------------------------------------------------------------------- Step 4: async SET

TEST("event loop: SET then GET round-trips (small + >> recv-buffer body)") {
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc = Reactor::create();
    if (!rc) { std::println("    (skipped: io_uring unavailable: {})", rc.error().detail); return; }
    const std::string base = tmp_base("set");
    Index index;
    auto tm = open_tm(base, index, false);
    auto io = make_iopool();
    CHECK(tm.has_value() && io.has_value());
    if (!tm || !io) { fs::remove_all(base); return; }

    auto [lfd, port] = make_loopback_listener();
    CHECK(lfd >= 0);
    if (lfd < 0) { fs::remove_all(base); return; }
    EventLoop loop(*rc, lfd, *tm, index, *io);
    std::thread th([&] { loop.run(); });

    bool ok = false;
    const int c = client_connect(port);
    if (c >= 0) {
        const std::string small = pattern(3, 100);
        const std::string big = pattern(4, 512 * 1024); // many 16 KiB recv chunks
        const bool s1 = set_value(c, "a", small) == "STORED\r\n";
        const bool s2 = set_value(c, "b", big) == "STORED\r\n";
        const auto g1 = get_value(c, "a");
        const auto g2 = get_value(c, "b");
        ok = s1 && s2 && g1 && *g1 == small && g2 && *g2 == big;
        ::close(c);
    }
    loop.stop();
    th.join();
    ::close(lfd);
    fs::remove_all(base);
    CHECK(ok);
}

TEST("event loop: add / replace admission") {
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc = Reactor::create();
    if (!rc) { std::println("    (skipped: io_uring unavailable: {})", rc.error().detail); return; }
    const std::string base = tmp_base("addrep");
    Index index;
    auto tm = open_tm(base, index, false);
    auto io = make_iopool();
    CHECK(tm.has_value() && io.has_value());
    if (!tm || !io) { fs::remove_all(base); return; }

    auto [lfd, port] = make_loopback_listener();
    CHECK(lfd >= 0);
    if (lfd < 0) { fs::remove_all(base); return; }
    EventLoop loop(*rc, lfd, *tm, index, *io);
    std::thread th([&] { loop.run(); });

    bool ok = false;
    const int c = client_connect(port);
    if (c >= 0) {
        const std::string v = pattern(5, 64);
        const bool a1 = set_value(c, "x", v, "add") == "STORED\r\n";         // new
        const bool a2 = set_value(c, "x", v, "add") == "NOT_STORED\r\n";     // already exists
        const bool r1 = set_value(c, "x", v, "replace") == "STORED\r\n";     // exists
        const bool r2 = set_value(c, "y", v, "replace") == "NOT_STORED\r\n"; // missing
        ok = a1 && a2 && r1 && r2;
        ::close(c);
    }
    loop.stop();
    th.join();
    ::close(lfd);
    fs::remove_all(base);
    CHECK(ok);
}

TEST("event loop: a bad data-chunk terminator -> CLIENT_ERROR") {
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc = Reactor::create();
    if (!rc) { std::println("    (skipped: io_uring unavailable: {})", rc.error().detail); return; }
    const std::string base = tmp_base("badchunk");
    Index index;
    auto tm = open_tm(base, index, false);
    auto io = make_iopool();
    CHECK(tm.has_value() && io.has_value());
    if (!tm || !io) { fs::remove_all(base); return; }

    auto [lfd, port] = make_loopback_listener();
    CHECK(lfd >= 0);
    if (lfd < 0) { fs::remove_all(base); return; }
    EventLoop loop(*rc, lfd, *tm, index, *io);
    std::thread th([&] { loop.run(); });

    bool ok = false;
    const int c = client_connect(port);
    if (c >= 0) {
        const std::string r = set_bad(c, "k", "hello", "XX"); // body fine, terminator isn't \r\n
        ok = r.rfind("CLIENT_ERROR", 0) == 0;
        ::close(c);
    }
    loop.stop();
    th.join();
    ::close(lfd);
    fs::remove_all(base);
    CHECK(ok);
}

TEST("event loop: two loops share storage; 32 concurrent SET+GET round-trips (TSan)") {
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc0 = Reactor::create();
    auto rc1 = Reactor::create();
    if (!rc0 || !rc1) { std::println("    (skipped: io_uring unavailable)"); return; }
    const std::string base = tmp_base("rw");
    Index index;
    auto tm = open_tm(base, index, false);
    auto io0 = make_iopool();
    auto io1 = make_iopool();
    CHECK(tm.has_value() && io0.has_value() && io1.has_value());
    if (!tm || !io0 || !io1) { fs::remove_all(base); return; }

    auto [lfd0, port0] = make_loopback_listener();
    auto [lfd1, port1] = make_loopback_listener();
    CHECK(lfd0 >= 0 && lfd1 >= 0);
    if (lfd0 < 0 || lfd1 < 0) { fs::remove_all(base); return; }
    EventLoop loop0(*rc0, lfd0, *tm, index, *io0);
    EventLoop loop1(*rc1, lfd1, *tm, index, *io1);
    std::thread t0([&] { loop0.run(); });
    std::thread t1([&] { loop1.run(); });

    constexpr int kN = 32;
    std::atomic<int> good{0};
    std::vector<std::thread> clients;
    for (int i = 0; i < kN; ++i) {
        clients.emplace_back([&, i] {
            const int c = client_connect(i % 2 ? port1 : port0);
            if (c < 0) return;
            const std::string key = "wk" + std::to_string(i);
            const std::string v = pattern(1000 + i, 4096 + static_cast<std::size_t>(i) * 32);
            const bool stored = set_value(c, key, v) == "STORED\r\n";
            const auto g = get_value(c, key);
            if (stored && g && *g == v) good.fetch_add(1);
            ::close(c);
        });
    }
    for (auto& t : clients) t.join();
    loop0.stop();
    loop1.stop();
    t0.join();
    t1.join();
    ::close(lfd0);
    ::close(lfd1);
    fs::remove_all(base);
    CHECK_EQ(good.load(), kN);
}

TEST("event loop: SET backpressure — one staging buffer, many concurrent SETs all stored") {
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc = Reactor::create();
    if (!rc) { std::println("    (skipped: io_uring unavailable: {})", rc.error().detail); return; }
    const std::string base = tmp_base("setbp");
    Index index;
    // One write-staging buffer: with many concurrent SETs, all but one must park on would_block and
    // resume when a buffer frees (ADR-0011 backpressure) — none may be dropped or NOT_STORED.
    auto tm = open_tm(base, index, false, /*write_buffers=*/1);
    auto io = make_iopool();
    CHECK(tm.has_value() && io.has_value());
    if (!tm || !io) { fs::remove_all(base); return; }

    auto [lfd, port] = make_loopback_listener();
    CHECK(lfd >= 0);
    if (lfd < 0) { fs::remove_all(base); return; }
    EventLoop loop(*rc, lfd, *tm, index, *io);
    std::thread th([&] { loop.run(); });

    constexpr int kN = 16;
    std::atomic<int> good{0};
    std::vector<std::thread> clients;
    for (int i = 0; i < kN; ++i) {
        clients.emplace_back([&, i] {
            const int c = client_connect(port);
            if (c < 0) return;
            const std::string key = "bp" + std::to_string(i);
            // Body > the 16 KiB recv buffer, so the holder keeps the lone staging buffer across
            // several recvs — guaranteeing the other connections' SETs park while it streams.
            const std::string v = pattern(2000 + i, 64 * 1024 + static_cast<std::size_t>(i) * 128);
            const bool stored = set_value(c, key, v) == "STORED\r\n";
            const auto g = get_value(c, key);
            if (stored && g && *g == v) good.fetch_add(1);
            ::close(c);
        });
    }
    for (auto& t : clients) t.join();
    loop.stop();
    th.join();
    ::close(lfd);
    fs::remove_all(base);
    CHECK_EQ(good.load(), kN);
}

TEST("event loop: GET backpressure — one read buffer, many concurrent disk-tail GETs all served") {
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc = Reactor::create();
    if (!rc) { std::println("    (skipped: io_uring unavailable: {})", rc.error().detail); return; }
    const std::string base = tmp_base("getbp");
    Index index;
    auto tm = open_tm(base, index, false);
    // One read I/O buffer: concurrent GETs with a disk tail must park (queue) and drain when it frees
    // (ADR-0011 backpressure) — never dropped/closed mid-protocol.
    auto io = IoBufferPool::create(64 * KiB, /*count=*/1, false);
    CHECK(tm.has_value() && io.has_value());
    if (!tm || !io) { fs::remove_all(base); return; }

    constexpr int kN = 16;
    std::vector<std::string> vals(kN);
    for (int i = 0; i < kN; ++i) {
        // > ram_head (4 KiB) so each GET needs the buffer, and > the 64 KiB buffer so it streams in
        // several pieces — the holder keeps the lone buffer across ticks, forcing the others to park.
        vals[i] = pattern(3000 + i, 150 * 1024 + static_cast<std::size_t>(i) * 128);
        CHECK(store_obj(*tm, "gp" + std::to_string(i), vals[i]));
    }

    auto [lfd, port] = make_loopback_listener();
    CHECK(lfd >= 0);
    if (lfd < 0) { fs::remove_all(base); return; }
    EventLoop loop(*rc, lfd, *tm, index, *io);
    std::thread th([&] { loop.run(); });

    std::atomic<int> good{0};
    std::vector<std::thread> clients;
    for (int i = 0; i < kN; ++i) {
        clients.emplace_back([&, i] {
            const int c = client_connect(port);
            if (c < 0) return;
            const auto g = get_value(c, "gp" + std::to_string(i));
            if (g && *g == vals[i]) good.fetch_add(1);
            ::close(c);
        });
    }
    for (auto& t : clients) t.join();
    loop.stop();
    th.join();
    ::close(lfd);
    fs::remove_all(base);
    CHECK_EQ(good.load(), kN);
}

TEST("event loop: a stalled slow reader is disconnected, freeing its buffer for a waiting GET") {
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc = Reactor::create();
    if (!rc) { std::println("    (skipped: io_uring unavailable: {})", rc.error().detail); return; }
    const std::string base = tmp_base("slowrd");
    Index index;
    auto tm = open_tm(base, index, false);
    auto io = IoBufferPool::create(64 * KiB, /*count=*/1, false); // a single read buffer
    CHECK(tm.has_value() && io.has_value());
    if (!tm || !io) { fs::remove_all(base); return; }
    const std::string val = pattern(42, 1 * MiB); // disk tail, far larger than any socket buffer
    CHECK(store_obj(*tm, "obj", val));

    auto [lfd, port] = make_loopback_listener();
    CHECK(lfd >= 0);
    if (lfd < 0) { fs::remove_all(base); return; }
    EventLoop loop(*rc, lfd, *tm, index, *io, /*io_timeout_ms=*/300);
    std::thread th([&] { loop.run(); });

    // Slow reader: ask for the object but never read the response. With a tiny receive buffer its
    // send stalls almost immediately, pinning the lone read buffer.
    const int slow = client_connect(port);
    if (slow >= 0) {
        int rcvbuf = 2048;
        ::setsockopt(slow, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
        const std::string req = "get obj\r\n";
        write_all(slow, req.data(), req.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // let its GET grab the buffer
    }

    // A normal reader: its GET parks (no buffer) until the slow reader is swept (~300 ms), then is
    // served. The 8 s client recv timeout is the safety net if the buffer were never reclaimed.
    bool ok = false;
    const int c = client_connect(port);
    if (c >= 0) {
        const auto g = get_value(c, "obj");
        ok = g && *g == val;
        ::close(c);
    }
    if (slow >= 0) ::close(slow);
    loop.stop();
    th.join();
    ::close(lfd);
    fs::remove_all(base);
    CHECK(ok);
}
