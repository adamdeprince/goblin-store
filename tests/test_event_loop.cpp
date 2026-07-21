#include "mini_test.hpp"

#include "goblin/core/buffer_pool.hpp"
#include "goblin/core/reactor.hpp"
#include "goblin/core/stats.hpp"
#include "goblin/crypto/sha256.hpp"
#include "goblin/net/connection_dispatcher.hpp"
#include "goblin/protocol/memcache/event_loop.hpp"
#include "goblin/protocol/memcache/auth.hpp"
#include "goblin/protocol/memcache/protocol.hpp"
#include "goblin/storage/index.hpp"
#include "goblin/storage/tier_manager.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <netinet/in.h>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
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
                                   unsigned write_buffers = 8, Size ram_head = 4 * KiB) {
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
    tiers.ram_head = ram_head;
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

// Observe the connection immediately after EventLoop::process() has queued its work but before the
// outer run_once() can submit or reap any of those SQEs. This makes the read-vs-head ordering test
// deterministic without a timing hook in production code.
class PrefetchProbeLoop : public EventLoop {
public:
    using EventLoop::EventLoop;

    bool observed() const noexcept { return observed_.load(std::memory_order_acquire); }
    bool valid() const noexcept { return valid_.load(std::memory_order_relaxed); }
    Size tail_start() const noexcept { return tail_start_.load(std::memory_order_relaxed); }

protected:
    void process(Conn* c) override {
        EventLoop::process(c);
        if (observed_.load(std::memory_order_relaxed) || c->state != St::get_header ||
            c->get_size <= c->send_pos)
            return;

        const int lane = c->read_lane;
        const bool read_queued = lane >= 0 && c->lane[static_cast<std::size_t>(lane)].reads > 0;
        tail_start_.store(c->send_pos, std::memory_order_relaxed);
        valid_.store(c->head_sent == 0 && c->send_lane == -1 && c->coalesced_send &&
                         read_queued && c->plan_pos > c->send_pos && c->inflight >= 2,
                     std::memory_order_relaxed);
        observed_.store(true, std::memory_order_release);
    }

private:
    std::atomic<bool> observed_{false};
    std::atomic<bool> valid_{false};
    std::atomic<Size> tail_start_{0};
};

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

TEST("event loop: consumes dispatcher handoff and owns the connection") {
#if !defined(__linux__)
    return;
#else
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc = Reactor::create();
    if (!rc) { std::println("    (skipped: io_uring unavailable: {})", rc.error().detail); return; }
    const std::string base = tmp_base("dispatch");
    Index index;
    auto tm = open_tm(base, index, false);
    auto io = make_iopool();
    CHECK(tm.has_value() && io.has_value());
    if (!tm || !io) { fs::remove_all(base); return; }

    auto [lfd, port] = make_loopback_listener();
    CHECK(lfd >= 0);
    if (lfd < 0) { fs::remove_all(base); return; }
    const int flags = ::fcntl(lfd, F_GETFL, 0);
    CHECK(flags >= 0 && ::fcntl(lfd, F_SETFL, flags | O_NONBLOCK) == 0);
    auto dispatcher = goblin::net::ConnectionDispatcher::create(lfd, {-1}, "event-loop test");
    CHECK(dispatcher.has_value());
    if (!dispatcher) { fs::remove_all(base); return; } // dispatcher owns lfd on failure too

    std::atomic<bool> dispatcher_shutdown{false};
    EventLoop loop(*rc, (*dispatcher)->inbox(0), *tm, index, *io);
    std::thread loop_thread([&] { loop.run(); });
    std::thread dispatcher_thread([&] { (*dispatcher)->run(dispatcher_shutdown); });

    bool version_ok = false;
    if (const int client = client_connect(port); client >= 0) {
        version_ok = write_all(client, "version\r\n", 9) &&
                     read_until(client, "\r\n").find("VERSION ") == 0;
        ::close(client);
    }
    dispatcher_shutdown.store(true, std::memory_order_relaxed);
    dispatcher_thread.join();
    loop.stop();
    loop_thread.join();

    CHECK(version_ok);
    CHECK_EQ((*dispatcher)->inbox(0).accepted_connections(), 1u);
    CHECK_EQ((*dispatcher)->inbox(0).current_connections(), 0u);
    fs::remove_all(base);
#endif
}

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

TEST("event loop: ASCII authentication gates a pipelined connection") {
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc = Reactor::create();
    if (!rc) { std::println("    (skipped: io_uring unavailable: {})", rc.error().detail); return; }
    const std::string base = tmp_base("auth");
    fs::create_directories(base);
    const std::string credentials = base + "/users";
    { std::ofstream file(credentials); file << "alice:secret\n"; }
    ::chmod(credentials.c_str(), 0600);
    auto auth = goblin::memcache::Authenticator::load(credentials);
    Index index;
    auto tm = open_tm(base, index, false);
    auto io = make_iopool();
    CHECK(auth && tm && io);
    if (!auth || !tm || !io) { fs::remove_all(base); return; }

    auto [lfd, port] = make_loopback_listener();
    CHECK(lfd >= 0);
    if (lfd < 0) { fs::remove_all(base); return; }
    EventLoop loop(*rc, lfd, *tm, index, *io);
    loop.set_authenticator(&*auth);
    std::thread th([&] { loop.run(); });

    bool ok = false;
    const int client = client_connect(port);
    if (client >= 0) {
        constexpr std::string_view request =
            "version\r\nset ignored 0 0 12\r\nalice secret\r\nversion\r\n";
        if (write_all(client, request.data(), request.size())) {
            const std::string response = read_until(client, goblin::memcache::kVersion);
            ok = response == "CLIENT_ERROR unauthenticated\r\nSTORED\r\n" +
                               std::string(goblin::memcache::kVersion);
        }
        ::close(client);
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

TEST("event loop: unknown commands use ERROR and malformed known commands use CLIENT_ERROR") {
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
        const auto exchange = [&](std::string_view request) {
            if (!write_all(c, request.data(), request.size())) return std::string{};
            return read_until(c, "\r\n");
        };
        const std::string unknown = exchange("bogus-command\r\n");
        const std::string malformed = exchange("set key 0 nope 1\r\n");
        const std::string bad_key = exchange("delete bad\tkey\r\n");
        const std::string settings = exchange("stats settings\r\n");
        ok = unknown == "ERROR\r\n" &&
             malformed == "CLIENT_ERROR bad command line format\r\n" &&
             bad_key == "CLIENT_ERROR bad command line format\r\n" &&
             settings == "END\r\n";
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

TEST("event loop: multi-key get and gets return every hit in order and one END") {
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc = Reactor::create();
    if (!rc) { std::println("    (skipped: io_uring unavailable: {})", rc.error().detail); return; }
    const std::string base = tmp_base("multiget");
    Index index;
    auto tm = open_tm(base, index, false);
    auto io = make_iopool();
    CHECK(tm.has_value() && io.has_value());
    if (!tm || !io) { fs::remove_all(base); return; }
    const std::string small = pattern(31, 127);       // inline-head fast path
    const std::string big = pattern(32, 96 * KiB);   // async disk-tail path
    CHECK(store_obj(*tm, "small", small));
    CHECK(store_obj(*tm, "big", big));
    const auto small_meta = index.lookup(hash_key("small"));
    const auto big_meta = index.lookup(hash_key("big"));
    CHECK(small_meta.has_value() && big_meta.has_value());
    if (!small_meta || !big_meta) { fs::remove_all(base); return; }

    auto [lfd, port] = make_loopback_listener();
    CHECK(lfd >= 0);
    if (lfd < 0) { fs::remove_all(base); return; }
    EventLoop loop(*rc, lfd, *tm, index, *io);
    std::thread th([&] { loop.run(); });

    bool ok = false;
    const int c = client_connect(port);
    if (c >= 0) {
        const std::string request =
            "get absent small big missing\r\ngets big nowhere small\r\nversion\r\n";
        if (write_all(c, request.data(), request.size())) {
            const std::string response = read_until(c, goblin::memcache::kVersion);
            std::string expected;
            expected += goblin::memcache::value_header("small", 0, small.size());
            expected += small;
            expected += "\r\n";
            expected += goblin::memcache::value_header("big", 0, big.size());
            expected += big;
            expected += "\r\nEND\r\n";
            expected += goblin::memcache::value_header_cas(
                "big", 0, big.size(), big_meta->etag);
            expected += big;
            expected += "\r\n";
            expected += goblin::memcache::value_header_cas(
                "small", 0, small.size(), small_meta->etag);
            expected += small;
            expected += "\r\nEND\r\n";
            expected += goblin::memcache::kVersion;
            ok = response == expected;
        }
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

TEST("event loop: initial response send queues the disk tail behind the configured head") {
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc = Reactor::create();
    if (!rc) { std::println("    (skipped: io_uring unavailable: {})", rc.error().detail); return; }
    const std::string base = tmp_base("earlytail");
    Index index;
    constexpr Size kHead = 32 * KiB; // deliberately not the 256 KiB product default
    auto tm = open_tm(base, index, false, /*write_buffers=*/8, kHead);
    auto io = make_iopool();
    CHECK(tm.has_value() && io.has_value());
    if (!tm || !io) { fs::remove_all(base); return; }
    const std::string value = pattern(71, 96 * KiB); // configured head plus a 64 KiB disk tail
    CHECK(store_obj(*tm, "early", value));

    auto [lfd, port] = make_loopback_listener();
    CHECK(lfd >= 0);
    if (lfd < 0) { fs::remove_all(base); return; }
    PrefetchProbeLoop loop(*rc, lfd, *tm, index, *io);
    std::thread th([&] { loop.run(); });

    bool served = false;
    const int c = client_connect(port);
    if (c >= 0) {
        const auto got = get_value(c, "early");
        served = got && *got == value;
        ::close(c);
    }
    loop.stop();
    th.join();
    ::close(lfd);
    fs::remove_all(base);

    CHECK(served);
    CHECK(loop.observed());
    CHECK(loop.valid());
    CHECK_EQ(loop.tail_start(), kHead);
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

TEST("event loop: add and replace conditions are rechecked when the value publishes") {
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc = Reactor::create();
    if (!rc) { std::println("    (skipped: io_uring unavailable: {})", rc.error().detail); return; }
    const std::string base = tmp_base("atomic-addrep");
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

    constexpr unsigned kWriters = 6;
    const std::string value(64, 'a');
    const std::string add_header = "add contested 0 0 " + std::to_string(value.size()) + "\r\n";
    std::vector<int> clients;
    for (unsigned i = 0; i < kWriters; ++i) {
        const int client = client_connect(port);
        if (client >= 0 && write_all(client, add_header.data(), add_header.size()))
            clients.push_back(client);
        else if (client >= 0)
            ::close(client);
    }
    // Every command header reaches set_body while the key is still absent. Sending bodies only after
    // that point deterministically exercises the old check-then-publish race.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    unsigned stored = 0, rejected = 0;
    const std::string body = value + "\r\n";
    for (const int client : clients) {
        if (write_all(client, body.data(), body.size())) {
            const std::string response = read_until(client, "\r\n");
            if (response == "STORED\r\n") ++stored;
            else if (response == "NOT_STORED\r\n") ++rejected;
        }
        ::close(client);
    }

    bool replace_rejected = false;
    const int replacement = client_connect(port);
    if (replacement >= 0) {
        const std::string header =
            "replace contested 0 0 " + std::to_string(value.size()) + "\r\n";
        if (write_all(replacement, header.data(), header.size())) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const int deleter = client_connect(port);
            bool deleted = false;
            if (deleter >= 0) {
                const std::string request = "delete contested\r\n";
                deleted = write_all(deleter, request.data(), request.size()) &&
                          read_until(deleter, "\r\n") == "DELETED\r\n";
                ::close(deleter);
            }
            replace_rejected = deleted && write_all(replacement, body.data(), body.size()) &&
                               read_until(replacement, "\r\n") == "NOT_STORED\r\n";
        }
        ::close(replacement);
    }

    loop.stop();
    th.join();
    ::close(lfd);
    CHECK_EQ(clients.size(), std::size_t(kWriters));
    CHECK_EQ(stored, 1u);
    CHECK_EQ(rejected, kWriters - 1);
    CHECK(replace_rejected);
    CHECK(!index.contains(hash_key("contested")));
    fs::remove_all(base);
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

static std::optional<std::uint64_t> stat_u64(const std::string& reply,
                                             std::string_view name);

TEST("event loop: idle keepalives expire and increment the idle-drop counter") {
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc = Reactor::create();
    if (!rc) { std::println("    (skipped: io_uring unavailable: {})", rc.error().detail); return; }
    const std::string base = tmp_base("idlelimit");
    Index index;
    auto tm = open_tm(base, index, false);
    auto io = make_iopool();
    CHECK(tm.has_value() && io.has_value());
    if (!tm || !io) { fs::remove_all(base); return; }
    auto [lfd, port] = make_loopback_listener();
    CHECK(lfd >= 0);
    if (lfd < 0) { fs::remove_all(base); return; }
    StatsRegistry reg;
    EventLoop loop(*rc, lfd, *tm, index, *io, /*io_timeout_ms=*/0, &reg);
    loop.set_overload_limits(/*idle_timeout_ms=*/100, /*queue_timeout_ms=*/0, 64, 64);
    std::thread th([&] { loop.run(); });

    const int idle = client_connect(port);
    bool expired = false;
    if (idle >= 0) {
        // Complete one request so the deadline is measured from an established server-side
        // keepalive, rather than racing accept scheduling against the client safety timeout.
        CHECK(write_all(idle, "version\r\n", 9));
        CHECK(read_until(idle, "\r\n").starts_with("VERSION "));
        timeval timeout{2, 0};
        ::setsockopt(idle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        char byte = 0;
        ssize_t received;
        do {
            received = ::recv(idle, &byte, 1, 0);
        } while (received < 0 && errno == EINTR);
        expired = received == 0 || (received < 0 && errno == ECONNRESET);
        ::close(idle);
    }

    std::string reply;
    const int observer = client_connect(port);
    if (observer >= 0) {
        if (write_all(observer, "stats\r\n", 7)) reply = read_until(observer, "END\r\n");
        ::close(observer);
    }
    loop.stop();
    th.join();
    ::close(lfd);
    fs::remove_all(base);
    CHECK(expired);
    CHECK(stat_u64(reply, "idle_drops").value_or(0) >= 1);
}

TEST("event loop: SET waiter count and queue deadline shed blocked connections") {
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc = Reactor::create();
    if (!rc) { std::println("    (skipped: io_uring unavailable: {})", rc.error().detail); return; }
    const std::string base = tmp_base("setlimits");
    Index index;
    auto tm = open_tm(base, index, false, /*write_buffers=*/1);
    auto io = make_iopool();
    CHECK(tm.has_value() && io.has_value());
    if (!tm || !io) { fs::remove_all(base); return; }
    auto [lfd, port] = make_loopback_listener();
    CHECK(lfd >= 0);
    if (lfd < 0) { fs::remove_all(base); return; }
    StatsRegistry reg;
    EventLoop loop(*rc, lfd, *tm, index, *io, /*io_timeout_ms=*/0, &reg);
    loop.set_overload_limits(/*idle_timeout_ms=*/0, /*queue_timeout_ms=*/100,
                             /*max_get_waiters=*/64, /*max_set_waiters=*/1);
    std::thread th([&] { loop.run(); });

    const int holder = client_connect(port);
    const int waiter = client_connect(port);
    const int excess = client_connect(port);
    CHECK(holder >= 0 && waiter >= 0 && excess >= 0);
    const std::string header1 = "set holder 0 0 65536\r\n";
    const std::string header2 = "set waiter 0 0 65536\r\n";
    const std::string header3 = "set excess 0 0 65536\r\n";
    if (holder >= 0) write_all(holder, header1.data(), header1.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (waiter >= 0) write_all(waiter, header2.data(), header2.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    if (excess >= 0) write_all(excess, header3.data(), header3.size());

    bool excess_dropped = false;
    bool waiter_expired = false;
    if (excess >= 0) {
        timeval timeout{2, 0};
        ::setsockopt(excess, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        char byte = 0;
        excess_dropped = ::recv(excess, &byte, 1, 0) <= 0;
    }
    if (waiter >= 0) {
        timeval timeout{2, 0};
        ::setsockopt(waiter, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        char byte = 0;
        waiter_expired = ::recv(waiter, &byte, 1, 0) <= 0;
    }

    std::string reply;
    const int observer = client_connect(port);
    if (observer >= 0) {
        if (write_all(observer, "stats\r\n", 7)) reply = read_until(observer, "END\r\n");
        ::close(observer);
    }
    if (holder >= 0) ::close(holder);
    if (waiter >= 0) ::close(waiter);
    if (excess >= 0) ::close(excess);
    loop.stop();
    th.join();
    ::close(lfd);
    fs::remove_all(base);
    CHECK(excess_dropped);
    CHECK(waiter_expired);
    CHECK(stat_u64(reply, "queue_drops").value_or(0) >= 2);
}

TEST("event loop: GET waiter bound sheds excess disk-tail readers") {
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc = Reactor::create();
    if (!rc) { std::println("    (skipped: io_uring unavailable: {})", rc.error().detail); return; }
    const std::string base = tmp_base("getlimit");
    Index index;
    auto tm = open_tm(base, index, false);
    auto io = IoBufferPool::create(64 * KiB, /*count=*/1, false);
    CHECK(tm.has_value() && io.has_value());
    if (!tm || !io) { fs::remove_all(base); return; }
    const std::string value = pattern(711, 8 * MiB);
    CHECK(store_obj(*tm, "large", value));
    auto [lfd, port] = make_loopback_listener();
    CHECK(lfd >= 0);
    if (lfd < 0) { fs::remove_all(base); return; }
    StatsRegistry reg;
    EventLoop loop(*rc, lfd, *tm, index, *io, /*io_timeout_ms=*/0, &reg);
    loop.set_overload_limits(/*idle_timeout_ms=*/0, /*queue_timeout_ms=*/0,
                             /*max_get_waiters=*/0, /*max_set_waiters=*/64);
    std::thread th([&] { loop.run(); });

    const int holder = client_connect(port);
    if (holder >= 0) {
        int receive_buffer = 2048;
        ::setsockopt(holder, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof receive_buffer);
        write_all(holder, "get large\r\n", 11);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const int excess = client_connect(port);
    bool dropped = false;
    if (excess >= 0) {
        timeval timeout{2, 0};
        ::setsockopt(excess, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        write_all(excess, "get large\r\n", 11);
        char byte = 0;
        dropped = ::recv(excess, &byte, 1, 0) <= 0;
    }
    std::string reply;
    const int observer = client_connect(port);
    if (observer >= 0) {
        if (write_all(observer, "stats\r\n", 7)) reply = read_until(observer, "END\r\n");
        ::close(observer);
    }
    if (holder >= 0) ::close(holder);
    if (excess >= 0) ::close(excess);
    loop.stop();
    th.join();
    ::close(lfd);
    fs::remove_all(base);
    CHECK(dropped);
    CHECK(stat_u64(reply, "queue_drops").value_or(0) >= 1);
}

// ----------------------------------------------------------------------------- Step 5: stats

// Pull the integer value of a `STAT <name> <value>` line out of a stats reply (nullopt if absent).
static std::optional<std::uint64_t> stat_u64(const std::string& reply, std::string_view name) {
    const std::string key = "STAT " + std::string(name) + " ";
    const auto p = reply.find(key);
    if (p == std::string::npos) return std::nullopt;
    const auto vs = p + key.size();
    const auto ve = reply.find("\r\n", vs);
    if (ve == std::string::npos) return std::nullopt;
    return std::stoull(reply.substr(vs, ve - vs));
}

TEST("event loop: stats reports connections, hits/misses, sets and bytes") {
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc = Reactor::create();
    if (!rc) { std::println("    (skipped: io_uring unavailable: {})", rc.error().detail); return; }
    const std::string base = tmp_base("stats");
    Index index;
    auto tm = open_tm(base, index, false);
    auto io = make_iopool();
    CHECK(tm.has_value() && io.has_value());
    if (!tm || !io) { fs::remove_all(base); return; }

    auto [lfd, port] = make_loopback_listener();
    CHECK(lfd >= 0);
    if (lfd < 0) { fs::remove_all(base); return; }
    StatsRegistry reg; // the loop registers its slot; the `stats` command aggregates it
    EventLoop loop(*rc, lfd, *tm, index, *io, /*io_timeout_ms=*/0, &reg);
    std::thread th([&] { loop.run(); });

    bool ok = false;
    const int c = client_connect(port);
    if (c >= 0) {
        const std::string val = pattern(9, 1000);
        const bool stored = set_value(c, "skey", val) == "STORED\r\n";
        const auto hit = get_value(c, "skey");  // one hit
        const auto miss = get_value(c, "nope"); // one miss
        std::string reply;
        if (write_all(c, "stats\r\n", 7)) reply = read_until(c, "END\r\n");
        std::string reset_reply;
        if (write_all(c, "stats reset\r\n", 13)) reset_reply = read_until(c, "\r\n");
        std::string after_reset;
        if (write_all(c, "stats\r\n", 7)) after_reset = read_until(c, "END\r\n");
        ok = stored && hit && *hit == val && !miss &&
             reply.find("STAT pid ") != std::string::npos &&
             reply.find("STAT version goblin-store 0.0.3\r\n") != std::string::npos &&
             stat_u64(reply, "get_hits") == 1 && stat_u64(reply, "get_misses") == 1 &&
             stat_u64(reply, "cmd_get") == 2 && stat_u64(reply, "sets_stored") == 1 &&
             stat_u64(reply, "cmd_set") == 1 && stat_u64(reply, "bytes_stored") == val.size() &&
             stat_u64(reply, "total_connections").value_or(0) >= 1 &&
             stat_u64(reply, "curr_connections").value_or(0) >= 1 &&
             stat_u64(reply, "bytes_served").value_or(0) >= val.size() &&
             stat_u64(reply, "goblin_ssd_filesystem_count") == 1 &&
             stat_u64(reply, "goblin_hdd_filesystem_count") == 0 &&
             stat_u64(reply, "goblin_ssd_filesystem_0_capacity_bytes").value_or(0) > 0 &&
             stat_u64(reply, "goblin_ssd_filesystem_0_inodes").value_or(0) > 0 &&
             stat_u64(reply, "goblin_ssd_filesystem_0_inodes_free").has_value() &&
             reset_reply == "RESET\r\n" && stat_u64(after_reset, "get_hits") == 0 &&
             stat_u64(after_reset, "get_misses") == 0 &&
             stat_u64(after_reset, "sets_stored") == 0 &&
             stat_u64(after_reset, "curr_connections").value_or(0) >= 1;
        ::close(c);
    }
    loop.stop();
    th.join();
    ::close(lfd);
    fs::remove_all(base);
    CHECK(ok);
}

// ----------------------------------------------------------------------------- Step 6: TTL

// "set <key> 0 <exptime> <len>\r\n<val>\r\n" — exercises memcache TTLs.
static std::string set_ttl(int fd, const std::string& key, const std::string& val,
                           std::int64_t exptime) {
    std::string req = "set " + key + " 0 " + std::to_string(exptime) + " " +
                      std::to_string(val.size()) + "\r\n";
    req += val;
    req += "\r\n";
    if (!write_all(fd, req.data(), req.size())) return {};
    return read_until(fd, "\r\n");
}

TEST("event loop: TTL — expired GET misses, live hits; add over an expired key succeeds") {
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc = Reactor::create();
    if (!rc) { std::println("    (skipped: io_uring unavailable: {})", rc.error().detail); return; }
    const std::string base = tmp_base("ttl");
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
        const std::string v = pattern(11, 64);
        // exptime > 30 days is an absolute Unix time: 1e9 is the past (2001), 4e9 the future (2096).
        const bool se = set_ttl(c, "exp", v, 1000000000u) == "STORED\r\n";
        const bool sl = set_ttl(c, "live", v, 4000000000u) == "STORED\r\n";
        const bool sn = set_value(c, "never", v) == "STORED\r\n"; // exptime 0 = never
        const bool si = set_ttl(c, "immediate", v, -1) == "STORED\r\n";
        const auto ge = get_value(c, "exp");    // expired -> lazy miss
        const auto gl = get_value(c, "live");   // hit
        const auto gn = get_value(c, "never");  // hit
        const auto gi = get_value(c, "immediate"); // negative exptime -> immediate lazy miss
        const bool add = set_value(c, "exp", v, "add") == "STORED\r\n"; // expired counts as absent
        const auto gr = get_value(c, "exp");    // re-added (no TTL) -> hit
        ok = se && sl && sn && si && !ge && gl && *gl == v && gn && *gn == v && !gi &&
             add && gr && *gr == v;
        ::close(c);
    }
    loop.stop();
    th.join();
    ::close(lfd);
    fs::remove_all(base);
    CHECK(ok);
}

TEST("tier_manager: reap_expired drops past-TTL objects, keeps live ones") {
    const std::string base = tmp_base("reap");
    Index index;
    auto tm = open_tm(base, index, false);
    CHECK(tm.has_value());
    if (!tm) { fs::remove_all(base); return; }
    const std::string v = pattern(12, 128);
    auto bytes = [&](const std::string& s) {
        return ByteView(reinterpret_cast<const std::byte*>(s.data()), s.size());
    };
    CHECK(tm->store(hash_key("a"), bytes(v), 0, 1u).has_value());          // expiry=1 -> long expired
    CHECK(tm->store(hash_key("b"), bytes(v), 0, 0u).has_value());          // never
    CHECK(tm->store(hash_key("c"), bytes(v), 0, 4000000000u).has_value()); // future
    CHECK_EQ(index.size(), std::size_t(3));
    CHECK_EQ(tm->reap_expired(), std::size_t(1)); // only "a" is past its TTL
    CHECK_EQ(index.size(), std::size_t(2));
    CHECK(!index.lookup(hash_key("a")).has_value());
    CHECK(index.lookup(hash_key("b")).has_value());
    CHECK(index.lookup(hash_key("c")).has_value());
    fs::remove_all(base);
}

// ----------------------------------------------------------------------------- Step 7: CAS

// `gets <key>` -> parse "VALUE k f n <cas>\r\n<n bytes>\r\nEND\r\n"; returns the CAS (nullopt on miss).
static std::optional<std::uint64_t> gets_cas(int fd, const std::string& key) {
    const std::string req = "gets " + key + "\r\n";
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
    if (hdr.rfind("VALUE ", 0) != 0) return std::nullopt; // miss (just "END")
    const auto sp_cas = hdr.rfind(' ');                   // VALUE key flags bytes cas
    const std::uint64_t cas = std::stoull(hdr.substr(sp_cas + 1));
    const auto sp_bytes = hdr.rfind(' ', sp_cas - 1);
    const std::size_t bytes = std::stoul(hdr.substr(sp_bytes + 1, sp_cas - sp_bytes - 1));
    const std::size_t need = hdr_end + 2 + bytes + 7; // drain header CRLF + data + "\r\nEND\r\n"
    while (buf.size() < need) {
        const ssize_t k = ::recv(fd, tmp, sizeof tmp, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return std::nullopt;
        buf.append(tmp, static_cast<std::size_t>(k));
    }
    return cas;
}

// "cas <key> 0 0 <len> <cas>\r\n<val>\r\n" -> reply line.
static std::string cas_cmd(int fd, const std::string& key, const std::string& val, std::uint64_t cas) {
    std::string req = "cas " + key + " 0 0 " + std::to_string(val.size()) + " " + std::to_string(cas) +
                      "\r\n" + val + "\r\n";
    if (!write_all(fd, req.data(), req.size())) return {};
    return read_until(fd, "\r\n");
}

// ----------------------------------------------------------------------------- Step 8: meta protocol

// Send a meta command line, read one response line (no value); returns it without the CRLF.
static std::string meta_cmd(int fd, const std::string& cmd) {
    const std::string req = cmd + "\r\n";
    if (!write_all(fd, req.data(), req.size())) return {};
    const std::string r = read_until(fd, "\r\n");
    return r.substr(0, r.find("\r\n"));
}

// "ms <key> <len> [flags]\r\n<val>\r\n" -> the response line.
static std::string meta_set(int fd, const std::string& key, const std::string& val,
                            const std::string& flags) {
    std::string req = "ms " + key + " " + std::to_string(val.size());
    if (!flags.empty()) req += " " + flags;
    req += "\r\n" + val + "\r\n";
    if (!write_all(fd, req.data(), req.size())) return {};
    const std::string r = read_until(fd, "\r\n");
    return r.substr(0, r.find("\r\n"));
}

// "mg <key> v" -> the value (nullopt on EN miss). Reads "VA <size> ...\r\n<size bytes>\r\n".
static std::optional<std::string> meta_mg_value(int fd, const std::string& key) {
    const std::string req = "mg " + key + " v\r\n";
    if (!write_all(fd, req.data(), req.size())) return std::nullopt;
    std::string buf;
    char tmp[4096];
    while (buf.find("\r\n") == std::string::npos) {
        const ssize_t k = ::recv(fd, tmp, sizeof tmp, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return std::nullopt;
        buf.append(tmp, static_cast<std::size_t>(k));
    }
    const auto eol = buf.find("\r\n");
    if (buf.rfind("VA ", 0) != 0) return std::nullopt; // miss (EN)
    const std::size_t size = std::stoul(buf.substr(3)); // "VA <size> ..."
    const std::size_t need = eol + 2 + size + 2;        // VA line + data + CRLF
    while (buf.size() < need) {
        const ssize_t k = ::recv(fd, tmp, sizeof tmp, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return std::nullopt;
        buf.append(tmp, static_cast<std::size_t>(k));
    }
    return buf.substr(eol + 2, size);
}

// Extract the numeric value of a " <f><digits>" meta flag (e.g. ' c' -> the CAS).
static std::optional<std::uint64_t> meta_flag_u64(const std::string& line, char f) {
    const std::string tok = std::string(" ") + f;
    const auto p = line.find(tok);
    if (p == std::string::npos) return std::nullopt;
    return std::stoull(line.substr(p + 2));
}

TEST("event loop: meta protocol — mn / ms / mg / md, flags, modes, CAS") {
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc = Reactor::create();
    if (!rc) { std::println("    (skipped: io_uring unavailable: {})", rc.error().detail); return; }
    const std::string base = tmp_base("meta");
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
        const std::string v = "HELLOWORLD"; // 10 bytes
        const bool mn = meta_cmd(c, "mn") == "MN";
        const bool store = meta_set(c, "k", v, "F7") == "HD";              // ms store
        const std::string hd = meta_cmd(c, "mg k f s t");                  // HD f7 s10 t-1 (never)
        const bool meta_hd = hd.rfind("HD", 0) == 0 && hd.find(" f7") != std::string::npos &&
                             hd.find(" s10") != std::string::npos && hd.find(" t-1") != std::string::npos;
        const bool opaque = meta_cmd(c, "mg k Oabc") == "HD Oabc";         // O echoed on the reply
        const auto val = meta_mg_value(c, "k");                            // mg v -> value
        const bool got_val = val && *val == v;
        const bool add = meta_set(c, "k", v, "ME") == "NS";                // add over existing -> NS
        const bool repl = meta_set(c, "ghost", v, "MR") == "NS";          // replace missing -> NS
        const auto cas = meta_flag_u64(meta_cmd(c, "mg k c"), 'c');        // current CAS
        const bool cas_match = cas && meta_set(c, "k", v, "C" + std::to_string(*cas)) == "HD"; // stores
        const bool cas_stale = cas && meta_set(c, "k", v, "C" + std::to_string(*cas)) == "EX"; // CAS bumped
        const bool del = meta_cmd(c, "md k") == "HD";                      // delete
        const bool miss = meta_cmd(c, "mg k") == "EN";                     // now a miss
        const bool del_nf = meta_cmd(c, "md k") == "NF";                   // already gone
        const bool expired_store = meta_set(c, "expired", "x", "T-1") == "HD";
        const auto before_touch = index.lookup(hash_key("expired"));
        const bool expired_touch_misses = meta_cmd(c, "mg expired T300") == "EN";
        const auto after_touch = index.lookup(hash_key("expired"));
        const bool expired_not_revived = before_touch && after_touch &&
                                         before_touch->expiry == 1 && after_touch->expiry == 1;
        ok = mn && store && meta_hd && opaque && got_val && add && repl && cas && cas_match &&
             cas_stale && del && miss && del_nf && expired_store && expired_touch_misses &&
             expired_not_revived;
        ::close(c);
    }
    loop.stop();
    th.join();
    ::close(lfd);
    fs::remove_all(base);
    CHECK(ok);
}

TEST("event loop: cas — gets returns a CAS; match stores, stale -> EXISTS, missing -> NOT_FOUND") {
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc = Reactor::create();
    if (!rc) { std::println("    (skipped: io_uring unavailable: {})", rc.error().detail); return; }
    const std::string base = tmp_base("cas");
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
        const std::string v = pattern(13, 64);
        const bool stored = set_value(c, "k", v) == "STORED\r\n";
        const auto cas1 = gets_cas(c, "k");                  // current CAS
        const auto miss = gets_cas(c, "nope");               // miss -> no CAS
        const bool match = cas1 && cas_cmd(c, "k", v, *cas1) == "STORED\r\n";   // matching CAS stores
        const auto cas2 = gets_cas(c, "k");                  // CAS changed after the store
        const bool stale = cas1 && cas_cmd(c, "k", v, *cas1) == "EXISTS\r\n";   // old CAS -> EXISTS
        const bool nf = cas_cmd(c, "ghost", v, 1) == "NOT_FOUND\r\n";           // cas on a missing key
        ok = stored && cas1 && !miss && match && cas2 && *cas2 != *cas1 && stale && nf;
        ::close(c);
    }
    loop.stop();
    th.join();
    ::close(lfd);
    fs::remove_all(base);
    CHECK(ok);
}

// ----------------------------------------------------------------------------- Step 9: graceful shutdown

TEST("event loop: graceful shutdown drains the idle keepalive conn, run() returns") {
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc = Reactor::create();
    if (!rc) { std::println("    (skipped: io_uring unavailable: {})", rc.error().detail); return; }
    const std::string base = tmp_base("shutdown");
    Index index;
    auto tm = open_tm(base, index, false);
    auto io = make_iopool();
    CHECK(tm.has_value() && io.has_value());
    if (!tm || !io) { fs::remove_all(base); return; }
    const std::string val = pattern(15, 2048);
    CHECK(store_obj(*tm, "obj", val));

    auto [lfd, port] = make_loopback_listener();
    CHECK(lfd >= 0);
    if (lfd < 0) { fs::remove_all(base); return; }
    std::atomic<bool> shutdown{false};
    EventLoop loop(*rc, lfd, *tm, index, *io);
    loop.set_shutdown(&shutdown, /*grace_ms=*/1000);
    std::thread th([&] { loop.run(); }); // returns once shutdown is observed + drained

    bool got = false, conn_closed = false;
    const int c = client_connect(port);
    if (c >= 0) {
        const auto g = get_value(c, "obj"); // a GET completes; the conn stays open (keepalive, idle)
        got = g && *g == val;
        shutdown.store(true, std::memory_order_relaxed); // trigger graceful shutdown
        th.join();                                       // run() drains the idle conn, then returns
        char buf[1];
        conn_closed = ::recv(c, buf, 1, 0) <= 0; // the drain RST-closed our idle keepalive conn
        ::close(c);
    } else {
        shutdown.store(true);
        th.join();
    }
    ::close(lfd);
    fs::remove_all(base);
    CHECK(got);
    CHECK(conn_closed);
}
