#include "mini_test.hpp"

#include "goblin/core/buffer_pool.hpp"
#include "goblin/core/reactor.hpp"
#include "goblin/crypto/sha256.hpp"
#include "goblin/http/http_loop.hpp"
#include "goblin/http/key_derivation.hpp"
#include "goblin/storage/index.hpp"
#include "goblin/storage/tier_manager.hpp"

#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
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

namespace fs = std::filesystem;
using namespace goblin;
using namespace goblin::core;
using goblin::crypto::hash_key;
using goblin::http::HttpLoop;
using goblin::http::KeyOptions;
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

static bool write_all(int fd, std::string_view s) {
    for (std::size_t off = 0; off < s.size();) {
        const ssize_t w = ::send(fd, s.data() + off, s.size() - off, 0);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        off += static_cast<std::size_t>(w);
    }
    return true;
}

struct HttpResp {
    int status = 0;
    std::string headers; // the header block (for Content-Range etc.)
    std::string body;
};

// Read one HTTP response: status code + (case-insensitive) Content-Length. For a HEAD response the
// headers carry Content-Length but no body, so the caller passes read_body=false.
static std::optional<HttpResp> read_http(int fd, bool read_body) {
    std::string buf;
    char tmp[4096];
    std::size_t hdr_end;
    while ((hdr_end = buf.find("\r\n\r\n")) == std::string::npos) {
        const ssize_t k = ::recv(fd, tmp, sizeof tmp, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return std::nullopt;
        buf.append(tmp, static_cast<std::size_t>(k));
    }
    HttpResp r;
    const auto sp = buf.find(' ');
    if (sp == std::string::npos) return std::nullopt;
    r.status = std::atoi(buf.c_str() + sp + 1);
    r.headers = buf.substr(0, hdr_end);
    if (!read_body) return r;

    std::string head = buf.substr(0, hdr_end);
    for (char& ch : head) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    std::size_t clen = 0;
    if (const auto p = head.find("content-length:"); p != std::string::npos)
        clen = std::strtoul(buf.c_str() + p + 15, nullptr, 10);

    const std::size_t body_start = hdr_end + 4;
    while (buf.size() < body_start + clen) {
        const ssize_t k = ::recv(fd, tmp, sizeof tmp, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return std::nullopt;
        buf.append(tmp, static_cast<std::size_t>(k));
    }
    r.body = buf.substr(body_start, clen);
    return r;
}

static std::optional<HttpResp> http_req(int fd, std::string_view request, bool read_body = true) {
    if (!write_all(fd, request)) return std::nullopt;
    return read_http(fd, read_body);
}

static std::string pattern(int seed, std::size_t n) {
    std::string s(n, '\0');
    for (std::size_t j = 0; j < n; ++j)
        s[j] = static_cast<char>((j * 7 + static_cast<std::size_t>(seed) * 131 + 3) & 0xFF);
    return s;
}

static Result<TierManager> open_tm(const std::string& base, Index& index) {
    PoolConfig ssd, hdd;
    ssd.stripe_unit = 64 * KiB;
    ssd.dirs = {base + "/s0", base + "/s1"};
    for (const auto& d : ssd.dirs) fs::create_directories(d);
    TierSizes tiers;
    tiers.ram_head = 4 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 64 * MiB;
    mem.block_bytes = 1 * MiB;
    mem.lock_memory = false;
    EvictionConfig ev;
    return TierManager::open(tiers, mem, ev, ssd, hdd, index);
}

static bool store_http(TierManager& tm, const std::string& path, const std::string& body,
                       const KeyOptions& opt) {
    const auto key = http::derive_key("h", path, opt); // path mode: host ignored
    if (!key) return false;
    return tm.store(hash_key(*key), ByteView(reinterpret_cast<const std::byte*>(body.data()), body.size()), 0)
        .has_value();
}

static std::string tmp_base(const char* tag) {
    return (fs::temp_directory_path() / ("goblin-http-" + std::string(tag) + std::to_string(::getpid())))
        .string();
}

// One ranged GET on a fresh connection (Connection: close).
static std::optional<HttpResp> ranged_get(std::uint16_t port, std::string_view path,
                                          std::string_view range) {
    const int c = client_connect(port);
    if (c < 0) return std::nullopt;
    std::string req = "GET ";
    req += path;
    req += " HTTP/1.1\r\nHost: h\r\nRange: ";
    req += range;
    req += "\r\nConnection: close\r\n\r\n";
    const auto r = http_req(c, req);
    ::close(c);
    return r;
}

TEST("http loop: GET streams the body; HEAD is headers-only; a miss is 404") {
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc = Reactor::create();
    if (!rc) { std::println("    (skipped: io_uring unavailable: {})", rc.error().detail); return; }
    const std::string base = tmp_base("get");
    Index index;
    auto tm = open_tm(base, index);
    auto io = IoBufferPool::create(128 * KiB, 8, false);
    CHECK(tm.has_value() && io.has_value());
    if (!tm || !io) { fs::remove_all(base); return; }
    KeyOptions opt; // path mode (default)
    const std::string body = pattern(9, 50 * 1024); // 4 KiB head + ~46 KiB disk tail
    CHECK(store_http(*tm, "/big/obj", body, opt));

    auto [lfd, port] = make_loopback_listener();
    CHECK(lfd >= 0);
    if (lfd < 0) { fs::remove_all(base); return; }
    HttpLoop loop(*rc, lfd, *tm, index, *io, opt, 0);
    std::thread th([&] { loop.run(); });

    bool get_ok = false, head_ok = false, miss_ok = false;
    if (const int c = client_connect(port); c >= 0) {
        const auto r = http_req(c, "GET /big/obj HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");
        get_ok = r && r->status == 200 && r->body == body;
        ::close(c);
    }
    if (const int c = client_connect(port); c >= 0) {
        const auto r =
            http_req(c, "HEAD /big/obj HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n", false);
        head_ok = r && r->status == 200 && r->body.empty(); // 200 + Content-Length, no body
        ::close(c);
    }
    if (const int c = client_connect(port); c >= 0) {
        const auto r = http_req(c, "GET /nope HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");
        miss_ok = r && r->status == 404 && r->body.empty();
        ::close(c);
    }
    loop.stop();
    th.join();
    ::close(lfd);
    fs::remove_all(base);
    CHECK(get_ok);
    CHECK(head_ok);
    CHECK(miss_ok);
}

TEST("http loop: keep-alive serves two GETs on one connection, then 405 for PUT") {
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc = Reactor::create();
    if (!rc) { std::println("    (skipped: io_uring unavailable: {})", rc.error().detail); return; }
    const std::string base = tmp_base("ka");
    Index index;
    auto tm = open_tm(base, index);
    auto io = IoBufferPool::create(128 * KiB, 8, false);
    CHECK(tm.has_value() && io.has_value());
    if (!tm || !io) { fs::remove_all(base); return; }
    KeyOptions opt;
    const std::string a = pattern(1, 200), b = pattern(2, 9000);
    CHECK(store_http(*tm, "/a", a, opt));
    CHECK(store_http(*tm, "/b", b, opt));

    auto [lfd, port] = make_loopback_listener();
    CHECK(lfd >= 0);
    if (lfd < 0) { fs::remove_all(base); return; }
    HttpLoop loop(*rc, lfd, *tm, index, *io, opt, 0);
    std::thread th([&] { loop.run(); });

    bool ok = false;
    if (const int c = client_connect(port); c >= 0) {
        const auto r1 = http_req(c, "GET /a HTTP/1.1\r\nHost: h\r\n\r\n"); // keep-alive
        const auto r2 = http_req(c, "GET /b HTTP/1.1\r\nHost: h\r\n\r\n"); // same connection
        const auto r3 = http_req(c, "PUT /a HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");
        ok = r1 && r1->status == 200 && r1->body == a && //
             r2 && r2->status == 200 && r2->body == b && //
             r3 && r3->status == 405;
        ::close(c);
    }
    loop.stop();
    th.join();
    ::close(lfd);
    fs::remove_all(base);
    CHECK(ok);
}

TEST("http loop: Range -> 206 (head / disk / spanning / suffix / open-ended) and 416") {
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc = Reactor::create();
    if (!rc) { std::println("    (skipped: io_uring unavailable: {})", rc.error().detail); return; }
    const std::string base = tmp_base("range");
    Index index;
    auto tm = open_tm(base, index);
    auto io = IoBufferPool::create(128 * KiB, 8, false);
    CHECK(tm.has_value() && io.has_value());
    if (!tm || !io) { fs::remove_all(base); return; }
    KeyOptions opt;
    const std::string body = pattern(7, 50 * 1024); // 51200: 4 KiB head + disk tail
    CHECK(store_http(*tm, "/obj", body, opt));

    auto [lfd, port] = make_loopback_listener();
    CHECK(lfd >= 0);
    if (lfd < 0) { fs::remove_all(base); return; }
    HttpLoop loop(*rc, lfd, *tm, index, *io, opt, 0);
    std::thread th([&] { loop.run(); });

    bool ok = true;
    auto expect = [&](std::string_view range, std::size_t off, std::size_t len, std::string_view cr) {
        const auto r = ranged_get(port, "/obj", range);
        ok = ok && r && r->status == 206 && r->body == body.substr(off, len) &&
             r->headers.find(cr) != std::string::npos;
    };
    expect("bytes=0-99", 0, 100, "Content-Range: bytes 0-99/51200");      // within the head
    expect("bytes=10000-10099", 10000, 100, "bytes 10000-10099/51200");   // disk region
    expect("bytes=4000-4200", 4000, 201, "bytes 4000-4200/51200");        // spans head -> disk
    expect("bytes=-100", 51100, 100, "bytes 51100-51199/51200");          // suffix
    expect("bytes=51100-", 51100, 100, "bytes 51100-51199/51200");        // open-ended
    {
        const auto r = ranged_get(port, "/obj", "bytes=999999-");         // unsatisfiable
        ok = ok && r && r->status == 416 && r->headers.find("bytes */51200") != std::string::npos;
    }

    loop.stop();
    th.join();
    ::close(lfd);
    fs::remove_all(base);
    CHECK(ok);
}
