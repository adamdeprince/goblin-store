// goblin-bench: large-object GET load generator. Pre-stores one object via memcache, then GETs it
// from N keep-alive connections for a fixed duration and reports throughput + TTFB + completion-time
// percentiles. Pure socket client (no server deps), multi-threaded so the client can saturate the
// server. Use it to A/B the read-ahead pipeline (run the server with vs without --no-read-ahead) and
// to sweep --io-buffers / --io-chunk. Protocols: memcache (default) and HTTP.
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;
using ns = std::chrono::nanoseconds;

struct Args {
    std::string host = "127.0.0.1";
    std::uint16_t port = 11211; // memcache port (used to populate, and to GET in memcache mode)
    std::uint16_t http_port = 8080;
    std::string proto = "memcache"; // memcache | http
    std::string key = "benchobj";
    std::size_t size = 1u << 20; // object size in bytes
    int conns = 8;
    int duration_s = 10;
    bool no_store = false;
};

std::size_t parse_size(std::string_view s) {
    std::size_t mult = 1;
    if (!s.empty()) {
        switch (s.back()) {
            case 'k': case 'K': mult = 1024; s.remove_suffix(1); break;
            case 'm': case 'M': mult = 1024 * 1024; s.remove_suffix(1); break;
            case 'g': case 'G': mult = 1024ull * 1024 * 1024; s.remove_suffix(1); break;
            default: break;
        }
    }
    std::size_t v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v * mult;
}

int connect_to(const std::string& host, std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    timeval tv{10, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    ::inet_pton(AF_INET, host.c_str(), &a.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) { ::close(fd); return -1; }
    return fd;
}

bool write_all(int fd, const char* p, std::size_t n) {
    while (n) {
        const ssize_t w = ::send(fd, p, n, 0);
        if (w <= 0) return false;
        p += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}

// Store the object once via memcache SET (HTTP has no write path). store_key is the exact key.
bool populate(const Args& a, const std::string& store_key) {
    const int fd = connect_to(a.host, a.port);
    if (fd < 0) return false;
    const std::string val(a.size, 'x');
    const std::string hdr = "set " + store_key + " 0 0 " + std::to_string(a.size) + "\r\n";
    const bool wrote = write_all(fd, hdr.data(), hdr.size()) &&
                       write_all(fd, val.data(), val.size()) && write_all(fd, "\r\n", 2);
    char buf[64];
    const ssize_t r = wrote ? ::recv(fd, buf, sizeof buf, 0) : -1;
    ::close(fd);
    return r > 0 && std::string_view(buf, static_cast<std::size_t>(r)).starts_with("STORED");
}

// One memcache GET. Response is "VALUE <key> 0 <size>\r\n<size bytes>\r\nEND\r\n"; total length is
// known up front, so we just count bytes to completion. Records ttfb (first byte) + total (last).
bool mc_get(int fd, const std::string& key, std::size_t expect_total, ns& ttfb, ns& total) {
    const std::string req = "get " + key + "\r\n";
    const auto t0 = Clock::now();
    if (!write_all(fd, req.data(), req.size())) return false;
    char buf[1u << 16];
    std::size_t got = 0;
    bool first = true;
    while (got < expect_total) {
        const ssize_t r = ::recv(fd, buf, sizeof buf, 0);
        if (r <= 0) return false;
        if (first) { ttfb = Clock::now() - t0; first = false; }
        got += static_cast<std::size_t>(r);
    }
    total = Clock::now() - t0;
    return got == expect_total;
}

// One HTTP/1.1 GET. Read headers (small), parse Content-Length, then count the body to completion.
bool http_get(int fd, const std::string& path, std::size_t expect_body, ns& ttfb, ns& total) {
    const std::string req = "GET " + path + " HTTP/1.1\r\nHost: b\r\n\r\n";
    const auto t0 = Clock::now();
    if (!write_all(fd, req.data(), req.size())) return false;
    char buf[1u << 16];
    std::string head;
    std::size_t body_got = 0, clen = 0;
    bool first = true, have_head = false, ok_status = false;
    for (;;) {
        const ssize_t r = ::recv(fd, buf, sizeof buf, 0);
        if (r <= 0) return false;
        if (first) { ttfb = Clock::now() - t0; first = false; }
        if (!have_head) {
            head.append(buf, static_cast<std::size_t>(r));
            const auto he = head.find("\r\n\r\n");
            if (he == std::string::npos) continue;
            have_head = true;
            ok_status = head.starts_with("HTTP/1.1 200");
            std::string lower = head;
            for (char& c : lower) c = static_cast<char>(c | 0x20);
            if (const auto p = lower.find("content-length:"); p != std::string::npos)
                std::from_chars(lower.data() + p + 15, lower.data() + lower.find("\r\n", p), clen);
            body_got = head.size() - (he + 4); // body bytes already pulled in with the header read
        } else {
            body_got += static_cast<std::size_t>(r);
        }
        if (have_head && body_got >= clen) break;
    }
    total = Clock::now() - t0;
    return ok_status && clen == expect_body;
}

struct Stat {
    std::uint64_t ops = 0;
    std::uint64_t bytes = 0;
    std::vector<ns> ttfb;
    std::vector<ns> total;
};

void worker(const Args& a, std::string req, std::size_t expect_total, Clock::time_point deadline,
            Stat& out) {
    const std::uint16_t port = a.proto == "http" ? a.http_port : a.port;
    const int fd = connect_to(a.host, port);
    if (fd < 0) return;
    out.ttfb.reserve(1 << 16);
    out.total.reserve(1 << 16);
    while (Clock::now() < deadline) {
        ns ttfb{}, total{};
        const bool ok = a.proto == "http" ? http_get(fd, req, a.size, ttfb, total)
                                          : mc_get(fd, req, expect_total, ttfb, total);
        if (!ok) break;
        ++out.ops;
        out.bytes += a.size;
        out.ttfb.push_back(ttfb);
        out.total.push_back(total);
    }
    ::close(fd);
}

double us(ns v) { return std::chrono::duration<double, std::micro>(v).count(); }
ns pct(std::vector<ns>& v, double p) {
    if (v.empty()) return ns{0};
    const std::size_t i = static_cast<std::size_t>(p / 100.0 * static_cast<double>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(i), v.end());
    return v[i];
}

} // namespace

int main(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string_view s = argv[i];
        auto next = [&]() -> std::string_view { return (i + 1 < argc) ? argv[++i] : ""; };
        if (s == "--host") a.host = next();
        else if (s == "--port") a.port = static_cast<std::uint16_t>(std::atoi(next().data()));
        else if (s == "--http-port") a.http_port = static_cast<std::uint16_t>(std::atoi(next().data()));
        else if (s == "--proto") a.proto = next();
        else if (s == "--key") a.key = next();
        else if (s == "--size") a.size = parse_size(next());
        else if (s == "--conns") a.conns = std::atoi(next().data());
        else if (s == "--duration") a.duration_s = std::atoi(next().data());
        else if (s == "--no-store") a.no_store = true;
        else if (s == "--help") {
            std::puts("goblin-bench --proto memcache|http --size 8M --conns 16 --duration 10\n"
                      "  --host H --port N (memcache) --http-port N --key K --no-store");
            return 0;
        } else { std::fprintf(stderr, "unknown arg '%.*s'\n", (int)s.size(), s.data()); return 2; }
    }

    // In HTTP path mode the key carries the URL's leading '/', so store + request under "/<key>".
    const bool http = a.proto == "http";
    const std::string store_key = http ? "/" + a.key : a.key;
    const std::string req = store_key; // memcache GET key == http GET path here
    if (!a.no_store && !populate(a, store_key)) {
        std::fprintf(stderr, "populate failed (is the server up on %s:%u?)\n", a.host.c_str(), a.port);
        return 1;
    }
    // memcache response length = "VALUE <key> 0 <size>\r\n" + size + "\r\nEND\r\n".
    const std::size_t mc_total =
        std::string("VALUE " + req + " 0 " + std::to_string(a.size) + "\r\n").size() + a.size + 7;

    std::printf("goblin-bench: proto=%s size=%zu conns=%d duration=%ds key=%s\n", a.proto.c_str(),
                a.size, a.conns, a.duration_s, req.c_str());
    std::vector<Stat> stats(static_cast<std::size_t>(a.conns));
    std::vector<std::thread> ts;
    const auto t0 = Clock::now();
    const auto deadline = t0 + std::chrono::seconds(a.duration_s);
    for (int i = 0; i < a.conns; ++i)
        ts.emplace_back([&, i] { worker(a, req, mc_total, deadline, stats[static_cast<std::size_t>(i)]); });
    for (auto& t : ts) t.join();
    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();

    std::uint64_t ops = 0, bytes = 0;
    std::vector<ns> ttfb, total;
    for (auto& s : stats) {
        ops += s.ops;
        bytes += s.bytes;
        ttfb.insert(ttfb.end(), s.ttfb.begin(), s.ttfb.end());
        total.insert(total.end(), s.total.begin(), s.total.end());
    }
    if (ops == 0) { std::fprintf(stderr, "no successful GETs\n"); return 1; }
    const double mibps = static_cast<double>(bytes) / (1024.0 * 1024.0) / secs;
    std::printf("  ops          : %llu  (%.0f ops/s)\n", (unsigned long long)ops, ops / secs);
    std::printf("  throughput   : %.1f MiB/s  (%.2f Gib/s)\n", mibps, mibps * 8 / 1024.0);
    std::printf("  TTFB    us   : p50 %.1f  p99 %.1f\n", us(pct(ttfb, 50)), us(pct(ttfb, 99)));
    std::printf("  complete us  : p50 %.1f  p99 %.1f\n", us(pct(total, 50)), us(pct(total, 99)));
    return 0;
}
