// goblin-bench: large-object GET load generator. Pre-stores one or more objects via memcache, then
// GETs a selectable key range from N keep-alive connections for a fixed duration and reports
// throughput + response-TTFB + first-payload-byte + completion-time percentiles. Pure socket client
// (no server deps),
// multi-threaded so the client can saturate the server. A multi-key remote hot range can exercise
// NUMA promotion; one key remains the default for read-ahead and I/O-buffer A/B tests.
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;
using ns = std::chrono::nanoseconds;

#if defined(CLOCK_MONOTONIC_RAW)
constexpr clockid_t kLatencyClock = CLOCK_MONOTONIC_RAW;
constexpr std::string_view kLatencyClockName = "CLOCK_MONOTONIC_RAW";
#else
constexpr clockid_t kLatencyClock = CLOCK_MONOTONIC;
constexpr std::string_view kLatencyClockName = "CLOCK_MONOTONIC";
#endif

std::int64_t latency_now_ns() {
    timespec t{};
    ::clock_gettime(kLatencyClock, &t);
    return static_cast<std::int64_t>(t.tv_sec) * 1'000'000'000LL + t.tv_nsec;
}

struct Args {
    std::string host = "127.0.0.1";
    std::uint16_t port = 11211; // memcache port (used to populate, and to GET in memcache mode)
    std::uint16_t http_port = 8080;
    std::string proto = "memcache"; // memcache | http
    std::string key = "benchobj";
    std::size_t size = 1u << 20; // object size in bytes
    std::size_t keys = 1;
    std::size_t hot_start = 0;
    std::size_t hot_keys = 0; // zero means every key at or after hot_start
    int conns = 8;
    int duration_s = 10;
    std::uint64_t request_count = 0; // zero means stop only at the duration deadline
    std::uint64_t seed = 0x6a09e667f3bcc909ULL;
    std::string raw_path;
    bool no_store = false;
    bool populate_only = false;
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

std::optional<std::uint64_t> parse_u64(std::string_view s) {
    int base = 10;
    if (s.starts_with("0x") || s.starts_with("0X")) {
        base = 16;
        s.remove_prefix(2);
    }
    if (s.empty()) return std::nullopt;
    std::uint64_t value = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value, base);
    if (ec != std::errc{} || end != s.data() + s.size()) return std::nullopt;
    return value;
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

std::string numbered_key(const Args& a, std::size_t index) {
    return a.keys == 1 ? a.key : a.key + std::to_string(index);
}

bool read_line(int fd, std::string& line) {
    line.clear();
    while (line.size() < 128) {
        char c = 0;
        if (::recv(fd, &c, 1, 0) != 1) return false;
        line.push_back(c);
        if (c == '\n') return true;
    }
    return false;
}

// Store every object over one memcache connection (HTTP has no write path).
bool populate(const Args& a) {
    const int fd = connect_to(a.host, a.port);
    if (fd < 0) return false;
    const std::string val(a.size, 'x');
    std::string response;
    for (std::size_t i = 0; i < a.keys; ++i) {
        std::string store_key = numbered_key(a, i);
        if (a.proto == "http") store_key.insert(store_key.begin(), '/');
        const std::string hdr =
            "set " + store_key + " 0 0 " + std::to_string(a.size) + "\r\n";
        if (!write_all(fd, hdr.data(), hdr.size()) || !write_all(fd, val.data(), val.size()) ||
            !write_all(fd, "\r\n", 2) || !read_line(fd, response) || response != "STORED\r\n") {
            ::close(fd);
            return false;
        }
        if (a.keys > 1 && ((i + 1) % 128 == 0 || i + 1 == a.keys)) {
            std::fprintf(stderr, "\r  populated %zu/%zu", i + 1, a.keys);
            std::fflush(stderr);
        }
    }
    if (a.keys > 1) std::fputc('\n', stderr);
    ::close(fd);
    return true;
}

// One memcache GET. Response is "VALUE <key> 0 <size>\r\n<size bytes>\r\nEND\r\n"; total length is
// known up front. Read exactly the response header before reading payload so a large recv cannot
// coalesce the header and body and turn "first object byte" into "first protocol byte".
bool mc_get(int fd, const std::string& key, std::size_t header_bytes, std::size_t expect_total,
            std::int64_t& query_start_ns, ns& response_ttfb, ns& payload_ttfb, ns& total) {
    const std::string req = "get " + key + "\r\n";
    const std::int64_t t0 = latency_now_ns();
    query_start_ns = t0;
    if (!write_all(fd, req.data(), req.size())) return false;
    char buf[1u << 16];
    std::size_t got = 0;
    bool first_response = true;
    while (got < header_bytes) {
        const std::size_t want = std::min(sizeof buf, header_bytes - got);
        const ssize_t r = ::recv(fd, buf, want, 0);
        if (r <= 0) return false;
        if (first_response) {
            response_ttfb = ns{latency_now_ns() - t0};
            first_response = false;
        }
        got += static_cast<std::size_t>(r);
    }

    bool first_payload = true;
    while (got < expect_total) {
        const ssize_t r = ::recv(fd, buf, sizeof buf, 0);
        if (r <= 0) return false;
        if (first_payload) {
            payload_ttfb = ns{latency_now_ns() - t0};
            first_payload = false;
        }
        got += static_cast<std::size_t>(r);
    }
    total = ns{latency_now_ns() - t0};
    return got == expect_total && !first_response && !first_payload;
}

// One HTTP/1.1 GET. Read headers (small), parse Content-Length, then count the body to completion.
bool http_get(int fd, const std::string& path, std::size_t expect_body,
              std::int64_t& query_start_ns, ns& response_ttfb, ns& payload_ttfb, ns& total) {
    const std::string req = "GET " + path + " HTTP/1.1\r\nHost: b\r\n\r\n";
    const std::int64_t t0 = latency_now_ns();
    query_start_ns = t0;
    if (!write_all(fd, req.data(), req.size())) return false;
    char buf[1u << 16];
    std::string head;
    std::size_t body_got = 0, clen = 0;
    bool first_response = true, first_payload = true, have_head = false, ok_status = false;
    for (;;) {
        const ssize_t r = ::recv(fd, buf, sizeof buf, 0);
        if (r <= 0) return false;
        const std::int64_t received = latency_now_ns();
        if (first_response) {
            response_ttfb = ns{received - t0};
            first_response = false;
        }
        if (!have_head) {
            head.append(buf, static_cast<std::size_t>(r));
            const auto he = head.find("\r\n\r\n");
            if (he == std::string::npos) continue;
            have_head = true;
            ok_status = head.starts_with("HTTP/1.1 200");
            std::string lower = head;
            for (char& c : lower)
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
            const auto p = lower.find("content-length:");
            if (p == std::string::npos) {
                std::fputs("HTTP response has no Content-Length\n", stderr);
                return false;
            }
            const auto line_end = lower.find("\r\n", p);
            if (line_end == std::string::npos) {
                std::fputs("unterminated HTTP Content-Length header\n", stderr);
                return false;
            }
            auto value = p + 15;
            while (value < line_end && (lower[value] == ' ' || lower[value] == '\t')) ++value;
            const auto [end, ec] =
                std::from_chars(lower.data() + value, lower.data() + line_end, clen);
            if (ec != std::errc{} || end != lower.data() + line_end) {
                std::fprintf(stderr, "invalid HTTP Content-Length: '%.*s'\n",
                             static_cast<int>(line_end - value), lower.data() + value);
                return false;
            }
            body_got = head.size() - (he + 4); // body bytes already pulled in with the header read
            if (body_got > 0) {
                payload_ttfb = ns{received - t0};
                first_payload = false;
            }
        } else {
            if (first_payload) {
                payload_ttfb = ns{received - t0};
                first_payload = false;
            }
            body_got += static_cast<std::size_t>(r);
        }
        if (have_head && body_got >= clen) break;
    }
    total = ns{latency_now_ns() - t0};
    const bool valid = ok_status && clen == expect_body && !first_response && !first_payload;
    if (!valid)
        std::fprintf(stderr,
                     "HTTP response mismatch: status=%d content-length=%zu expected=%zu "
                     "body=%zu payload-seen=%d\n",
                     ok_status, clen, expect_body, body_got, !first_payload);
    return valid;
}

struct Stat {
    std::uint64_t ops = 0;
    std::uint64_t bytes = 0;
    std::uint64_t sequence_hash = 1469598103934665603ULL;
    bool failed = false;
    std::vector<std::size_t> key_indices;
    std::vector<std::int64_t> query_start_ns;
    std::vector<ns> response_ttfb;
    std::vector<ns> payload_ttfb;
    std::vector<ns> total;
};

void prepare_stat(Stat& out, std::size_t reserve) {
    // The latency experiment keeps millions of samples. Fault every backing page before the timer
    // starts so client-side anonymous-memory faults do not become periodic first-byte outliers.
    const auto prefault = [reserve](auto& values) {
        using Value = typename std::remove_reference_t<decltype(values)>::value_type;
        values.resize(reserve);
        auto* bytes = reinterpret_cast<volatile unsigned char*>(values.data());
        const std::size_t byte_count = values.size() * sizeof(Value);
        for (std::size_t offset = 0; offset < byte_count; offset += 4096) bytes[offset] = 0;
        if (byte_count != 0) bytes[byte_count - 1] = 0;
        values.clear();
    };
    prefault(out.key_indices);
    prefault(out.query_start_ns);
    prefault(out.response_ttfb);
    prefault(out.payload_ttfb);
    prefault(out.total);
}

void worker(const Args& a, const std::vector<std::string>& requests,
            const std::vector<std::size_t>& header_bytes,
            const std::vector<std::size_t>& expected_totals,
            std::size_t worker_index, std::uint64_t request_quota,
            Clock::time_point deadline, Stat& out) {
    const std::uint16_t port = a.proto == "http" ? a.http_port : a.port;
    const int fd = connect_to(a.host, port);
    if (fd < 0) {
        out.failed = true;
        return;
    }
    std::uint64_t random = a.seed + 0x9e3779b97f4a7c15ULL * (worker_index + 1);
    if (random == 0) random = 0x2545f4914f6cdd1dULL;
    const auto next_random = [&]() {
        random ^= random >> 12;
        random ^= random << 25;
        random ^= random >> 27;
        return random * 0x2545f4914f6cdd1dULL;
    };
    while ((request_quota == 0 || out.ops < request_quota) && Clock::now() < deadline) {
        const std::size_t request_index =
            static_cast<std::size_t>(next_random() % requests.size());
        std::int64_t query_start_ns = 0;
        ns response_ttfb{}, payload_ttfb{}, total{};
        const bool ok = a.proto == "http"
            ? http_get(fd, requests[request_index], a.size, query_start_ns, response_ttfb,
                       payload_ttfb, total)
            : mc_get(fd, requests[request_index], header_bytes[request_index],
                     expected_totals[request_index], query_start_ns, response_ttfb,
                     payload_ttfb, total);
        if (!ok) {
            out.failed = true;
            break;
        }
        ++out.ops;
        out.bytes += a.size;
        const std::size_t absolute_key_index = a.hot_start + request_index;
        out.sequence_hash ^= static_cast<std::uint64_t>(absolute_key_index);
        out.sequence_hash *= 1099511628211ULL;
        out.key_indices.push_back(absolute_key_index);
        out.query_start_ns.push_back(query_start_ns);
        out.response_ttfb.push_back(response_ttfb);
        out.payload_ttfb.push_back(payload_ttfb);
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
    bool duration_set = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view s = argv[i];
        auto next = [&]() -> std::string_view { return (i + 1 < argc) ? argv[++i] : ""; };
        if (s == "--host") a.host = next();
        else if (s == "--port") a.port = static_cast<std::uint16_t>(std::atoi(next().data()));
        else if (s == "--http-port") a.http_port = static_cast<std::uint16_t>(std::atoi(next().data()));
        else if (s == "--proto") a.proto = next();
        else if (s == "--key") a.key = next();
        else if (s == "--size") a.size = parse_size(next());
        else if (s == "--keys") a.keys = parse_size(next());
        else if (s == "--hot-start") a.hot_start = parse_size(next());
        else if (s == "--hot-keys") a.hot_keys = parse_size(next());
        else if (s == "--conns") a.conns = std::atoi(next().data());
        else if (s == "--duration") { a.duration_s = std::atoi(next().data()); duration_set = true; }
        else if (s == "--requests") {
            const auto value = parse_u64(next());
            if (!value) { std::fputs("invalid --requests\n", stderr); return 2; }
            a.request_count = *value;
        }
        else if (s == "--seed") {
            const auto value = parse_u64(next());
            if (!value) { std::fputs("invalid --seed\n", stderr); return 2; }
            a.seed = *value;
        }
        else if (s == "--raw") a.raw_path = next();
        else if (s == "--no-store") a.no_store = true;
        else if (s == "--populate-only") a.populate_only = true;
        else if (s == "--help") {
            std::puts("goblin-bench --proto memcache|http --size 8M --conns 16 --duration 10\n"
                      "  --host H --port N (memcache) --http-port N --key PREFIX\n"
                      "  --keys N --hot-start N --hot-keys N --no-store --populate-only\n"
                      "  --requests N [--duration SECONDS] --seed N --raw FILE\n"
                      "  --requests without --duration runs exactly N serial GETs per total");
            return 0;
        } else { std::fprintf(stderr, "unknown arg '%.*s'\n", (int)s.size(), s.data()); return 2; }
    }

    if (a.request_count != 0 && !duration_set) a.duration_s = 0;

    if (a.size == 0 || a.keys == 0 || a.hot_start >= a.keys || a.conns <= 0 ||
        a.duration_s < 0 || (a.duration_s == 0 && a.request_count == 0)) {
        std::fprintf(stderr, "invalid size, keys, hot-start, conns, duration, or request count\n");
        return 2;
    }
    if (a.hot_keys == 0) a.hot_keys = a.keys - a.hot_start;
    if (a.hot_keys > a.keys - a.hot_start) {
        std::fprintf(stderr, "hot range exceeds the populated key range\n");
        return 2;
    }

    if (!a.no_store && !populate(a)) {
        std::fprintf(stderr, "populate failed (is the server up on %s:%u?)\n", a.host.c_str(), a.port);
        return 1;
    }
    if (a.populate_only) return 0;

    const bool http = a.proto == "http";
    std::vector<std::string> requests;
    std::vector<std::size_t> header_bytes;
    std::vector<std::size_t> expected_totals;
    requests.reserve(a.hot_keys);
    header_bytes.reserve(a.hot_keys);
    expected_totals.reserve(a.hot_keys);
    for (std::size_t i = a.hot_start; i < a.hot_start + a.hot_keys; ++i) {
        std::string request = numbered_key(a, i);
        if (http) request.insert(request.begin(), '/');
        // memcache response = "VALUE <key> 0 <size>\r\n" + value + "\r\nEND\r\n".
        const std::size_t header =
            std::string("VALUE " + request + " 0 " + std::to_string(a.size) + "\r\n").size();
        header_bytes.push_back(header);
        expected_totals.push_back(header + a.size + 7);
        requests.push_back(std::move(request));
    }

    std::printf("goblin-bench: proto=%s size=%zu keys=%zu hot=[%zu,%zu) conns=%d "
                "duration=%ds requests=%llu seed=0x%016llx\n",
                a.proto.c_str(), a.size, a.keys, a.hot_start, a.hot_start + a.hot_keys,
                a.conns, a.duration_s, static_cast<unsigned long long>(a.request_count),
                static_cast<unsigned long long>(a.seed));
    timespec clock_resolution{};
    ::clock_getres(kLatencyClock, &clock_resolution);
    const long long clock_resolution_ns =
        static_cast<long long>(clock_resolution.tv_sec) * 1'000'000'000LL +
        clock_resolution.tv_nsec;
    std::printf("  latency clock: %.*s resolution=%lld ns\n",
                static_cast<int>(kLatencyClockName.size()), kLatencyClockName.data(),
                clock_resolution_ns);
    std::fflush(stdout);
    std::vector<Stat> stats(static_cast<std::size_t>(a.conns));
    std::vector<std::uint64_t> request_quotas(static_cast<std::size_t>(a.conns));
    for (std::size_t worker_index = 0; worker_index < stats.size(); ++worker_index) {
        const std::uint64_t request_quota = a.request_count == 0 ? 0
            : a.request_count / static_cast<std::uint64_t>(a.conns) +
                  (worker_index < a.request_count % static_cast<std::uint64_t>(a.conns));
        request_quotas[worker_index] = request_quota;
        const std::size_t reserve = request_quota == 0
            ? (1u << 16)
            : static_cast<std::size_t>(std::min<std::uint64_t>(
                  request_quota, std::numeric_limits<std::size_t>::max()));
        prepare_stat(stats[worker_index], reserve);
    }
    std::vector<std::thread> ts;
    const auto t0 = Clock::now();
    const auto deadline = a.duration_s == 0
        ? Clock::time_point::max()
        : t0 + std::chrono::seconds(a.duration_s);
    for (int i = 0; i < a.conns; ++i) {
        const auto worker_index = static_cast<std::size_t>(i);
        const std::uint64_t request_quota = request_quotas[worker_index];
        ts.emplace_back([&, worker_index, request_quota] {
            worker(a, requests, header_bytes, expected_totals, worker_index, request_quota,
                   deadline, stats[worker_index]);
        });
    }
    for (auto& t : ts) t.join();
    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();

    std::uint64_t ops = 0, bytes = 0;
    std::uint64_t sequence_hash = 1469598103934665603ULL;
    bool failed = false;
    std::vector<ns> response_ttfb, payload_ttfb, total;
    for (auto& s : stats) {
        ops += s.ops;
        bytes += s.bytes;
        sequence_hash ^= s.sequence_hash;
        sequence_hash *= 1099511628211ULL;
        failed = failed || s.failed;
        response_ttfb.insert(response_ttfb.end(), s.response_ttfb.begin(), s.response_ttfb.end());
        payload_ttfb.insert(payload_ttfb.end(), s.payload_ttfb.begin(), s.payload_ttfb.end());
        total.insert(total.end(), s.total.begin(), s.total.end());
    }
    if (ops == 0) { std::fprintf(stderr, "no successful GETs\n"); return 1; }
    if (!a.raw_path.empty()) {
        std::FILE* raw = std::fopen(a.raw_path.c_str(), "w");
        if (!raw) {
            std::fprintf(stderr, "cannot open raw sample file '%s': %s\n",
                         a.raw_path.c_str(), std::strerror(errno));
            return 1;
        }
        std::vector<char> file_buffer(1u << 20);
        std::setvbuf(raw, file_buffer.data(), _IOFBF, file_buffer.size());
        std::fputs("worker,sample,key_index,query_start_ns,response_first_byte_ns,"
                   "response_ttfb_ns,payload_first_byte_ns,payload_ttfb_ns,complete_ns\n", raw);
        for (std::size_t worker_index = 0; worker_index < stats.size(); ++worker_index) {
            const auto& s = stats[worker_index];
            for (std::size_t sample = 0; sample < s.key_indices.size(); ++sample) {
                std::fprintf(raw, "%zu,%zu,%zu,%lld,%lld,%lld,%lld,%lld,%lld\n",
                             worker_index, sample, s.key_indices[sample],
                             static_cast<long long>(s.query_start_ns[sample]),
                             static_cast<long long>(s.query_start_ns[sample] +
                                                    s.response_ttfb[sample].count()),
                             static_cast<long long>(s.response_ttfb[sample].count()),
                             static_cast<long long>(s.query_start_ns[sample] +
                                                    s.payload_ttfb[sample].count()),
                             static_cast<long long>(s.payload_ttfb[sample].count()),
                             static_cast<long long>(s.total[sample].count()));
            }
        }
        if (std::fclose(raw) != 0) {
            std::fprintf(stderr, "failed writing raw sample file '%s': %s\n",
                         a.raw_path.c_str(), std::strerror(errno));
            return 1;
        }
        std::printf("  raw samples  : %s\n", a.raw_path.c_str());
    }
    const double mibps = static_cast<double>(bytes) / (1024.0 * 1024.0) / secs;
    std::printf("  ops          : %llu  (%.0f ops/s)\n", (unsigned long long)ops, ops / secs);
    std::printf("  key sequence : 0x%016llx\n",
                static_cast<unsigned long long>(sequence_hash));
    std::printf("  throughput   : %.1f MiB/s  (%.2f Gib/s)\n", mibps, mibps * 8 / 1024.0);
    std::printf("  response TTFB us: p50 %.1f  p99 %.1f  p99.9 %.1f  max %.1f\n",
                us(pct(response_ttfb, 50)), us(pct(response_ttfb, 99)),
                us(pct(response_ttfb, 99.9)),
                us(*std::max_element(response_ttfb.begin(), response_ttfb.end())));
    std::printf("  payload TTFB  us: p50 %.1f  p99 %.1f  p99.9 %.1f  max %.1f\n",
                us(pct(payload_ttfb, 50)), us(pct(payload_ttfb, 99)),
                us(pct(payload_ttfb, 99.9)),
                us(*std::max_element(payload_ttfb.begin(), payload_ttfb.end())));
    std::printf("  complete us  : p50 %.1f  p99 %.1f  p99.9 %.1f  max %.1f\n",
                us(pct(total, 50)), us(pct(total, 99)), us(pct(total, 99.9)),
                us(*std::max_element(total.begin(), total.end())));
    if (failed || (a.request_count != 0 && ops != a.request_count)) {
        std::fprintf(stderr, "incomplete run: completed %llu of %llu requested GETs\n",
                     static_cast<unsigned long long>(ops),
                     static_cast<unsigned long long>(a.request_count));
        return 1;
    }
    return 0;
}
