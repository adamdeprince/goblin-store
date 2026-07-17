// Fixed-seed TCP/HTTP benchmark for Goblin Store --mirror versus Vinyl Cache.
//
// Every worker owns one persistent TCP connection. Per-request results remain in RAM until the
// complete case finishes, then are written atomically as CSV. The measured interval begins just
// before the HTTP request bytes are sent and ends when the final response-body byte is received.
// Build on the client host:
//   g++ -O3 -std=c++20 -pthread -Wall -Wextra -Werror mirror_proxy_benchmark.cpp -o mirror-proxy-benchmark

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct Args {
    std::string host;
    std::string source;
    std::filesystem::path manifest;
    std::filesystem::path output;
    std::uint16_t port = 8080;
    unsigned concurrency = 1;
    unsigned passes = 1;
    unsigned timeout_seconds = 300;
    std::uint64_t seed = 0x243f6a8885a308d3ULL;
    std::uint64_t max_requests = 0;
    bool head_only = false;
};

struct Object {
    std::string path;
    std::uint64_t bytes = 0;
};

struct Task {
    std::uint64_t sequence = 0;
    unsigned repetition = 0;
    std::size_t object_index = 0;
};

struct Record {
    std::uint64_t sequence = 0;
    unsigned repetition = 0;
    std::size_t object_index = 0;
    unsigned worker = 0;
    unsigned connection = 0;
    std::uint64_t wall_start_ns = 0;
    std::uint64_t connect_ns = 0;
    std::uint64_t query_start_ns = 0;
    std::uint64_t response_first_byte_ns = 0;
    std::uint64_t body_first_byte_ns = 0;
    std::uint64_t complete_ns = 0;
    std::uint64_t response_ttfb_ns = 0;
    std::uint64_t body_ttfb_ns = 0;
    std::uint64_t ttlb_ns = 0;
    std::uint64_t body_transfer_ns = 0;
    std::uint64_t expected_bytes = 0;
    std::uint64_t received_bytes = 0;
    int status = 0;
    bool new_connection = false;
    std::string path;
    std::string error = "not run";
};

std::uint64_t clock_ns(clockid_t id) {
    timespec ts{};
    if (::clock_gettime(id, &ts) != 0) {
        std::perror("clock_gettime");
        std::exit(2);
    }
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

std::uint64_t monotonic_ns() { return clock_ns(CLOCK_MONOTONIC_RAW); }
std::uint64_t realtime_ns() { return clock_ns(CLOCK_REALTIME); }

[[noreturn]] void usage(const char* argv0, std::string_view error = {}) {
    if (!error.empty()) std::cerr << "error: " << error << "\n\n";
    std::cerr
        << "usage: " << argv0 << " --host IPV4 --manifest FILE --output FILE [options]\n"
        << "  --source IPV4       bind the client side to the IPoIB address\n"
        << "  --port N            proxy HTTP port [8080]\n"
        << "  --concurrency N     persistent connections [1]\n"
        << "  --passes N          independently shuffled passes through the manifest [1]\n"
        << "  --seed N            fixed PRNG seed, decimal or 0x-prefixed\n"
        << "  --timeout N         socket send/receive timeout in seconds [300]\n"
        << "  --max-requests N    truncate the generated request sequence (smoke/probe)\n"
        << "  --head               issue HEAD requests (cache-hit probe)\n";
    std::exit(error.empty() ? 0 : 2);
}

std::uint64_t parse_u64(std::string_view value, std::string_view name) {
    int base = 10;
    if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        value.remove_prefix(2);
        base = 16;
    }
    std::uint64_t out = 0;
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), out, base);
    if (ec != std::errc{} || end != value.data() + value.size())
        usage("mirror_proxy_benchmark", std::string("bad ") + std::string(name));
    return out;
}

Args parse_args(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string_view option(argv[i]);
        auto next = [&]() -> std::string_view {
            if (++i >= argc) usage(argv[0], std::string(option) + " needs an argument");
            return argv[i];
        };
        if (option == "--host") args.host = next();
        else if (option == "--source") args.source = next();
        else if (option == "--manifest") args.manifest = next();
        else if (option == "--output") args.output = next();
        else if (option == "--port") {
            const auto value = parse_u64(next(), "port");
            if (value == 0 || value > 65535) usage(argv[0], "port is out of range");
            args.port = static_cast<std::uint16_t>(value);
        } else if (option == "--concurrency") {
            const auto value = parse_u64(next(), "concurrency");
            if (value == 0 || value > 1024) usage(argv[0], "concurrency is out of range");
            args.concurrency = static_cast<unsigned>(value);
        } else if (option == "--passes") {
            const auto value = parse_u64(next(), "passes");
            if (value == 0 || value > 1000) usage(argv[0], "passes is out of range");
            args.passes = static_cast<unsigned>(value);
        } else if (option == "--timeout") {
            const auto value = parse_u64(next(), "timeout");
            if (value == 0 || value > 86400) usage(argv[0], "timeout is out of range");
            args.timeout_seconds = static_cast<unsigned>(value);
        } else if (option == "--seed") args.seed = parse_u64(next(), "seed");
        else if (option == "--max-requests")
            args.max_requests = parse_u64(next(), "max-requests");
        else if (option == "--head") args.head_only = true;
        else if (option == "--help" || option == "-h") usage(argv[0]);
        else usage(argv[0], std::string("unknown option ") + std::string(option));
    }
    if (args.host.empty()) usage(argv[0], "--host is required");
    if (args.manifest.empty()) usage(argv[0], "--manifest is required");
    if (args.output.empty()) usage(argv[0], "--output is required");
    return args;
}

std::vector<Object> load_manifest(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open manifest " + path.string());
    std::vector<Object> objects;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty() || line[0] == '#') continue;
        const auto tab = line.find('\t');
        if (tab == std::string::npos || tab == 0 || line[0] != '/')
            throw std::runtime_error("bad manifest line " + std::to_string(line_number));
        std::uint64_t bytes = 0;
        const std::string_view value(line.data() + tab + 1, line.size() - tab - 1);
        const auto [end, ec] =
            std::from_chars(value.data(), value.data() + value.size(), bytes);
        if (ec != std::errc{} || end != value.data() + value.size())
            throw std::runtime_error("bad byte count on manifest line " +
                                     std::to_string(line_number));
        objects.push_back({line.substr(0, tab), bytes});
    }
    if (objects.empty()) throw std::runtime_error("manifest is empty");
    return objects;
}

std::vector<Task> make_tasks(std::size_t objects, unsigned passes, std::uint64_t seed,
                             std::uint64_t maximum) {
    std::mt19937_64 random(seed);
    std::vector<Task> tasks;
    if (objects > std::numeric_limits<std::size_t>::max() / passes)
        throw std::runtime_error("task count overflows size_t");
    tasks.reserve(objects * passes);
    std::vector<std::size_t> order(objects);
    for (unsigned repetition = 0; repetition < passes; ++repetition) {
        std::iota(order.begin(), order.end(), 0);
        std::shuffle(order.begin(), order.end(), random);
        for (const auto object : order)
            tasks.push_back({static_cast<std::uint64_t>(tasks.size()), repetition, object});
    }
    if (maximum != 0 && maximum < tasks.size()) tasks.resize(static_cast<std::size_t>(maximum));
    return tasks;
}

std::string errno_message(std::string_view operation) {
    return std::string(operation) + ": " + std::strerror(errno);
}

std::string lower(std::string_view input) {
    std::string result(input);
    for (char& ch : result) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return result;
}

std::string_view trim(std::string_view input) {
    while (!input.empty() && (input.front() == ' ' || input.front() == '\t'))
        input.remove_prefix(1);
    while (!input.empty() && (input.back() == ' ' || input.back() == '\t')) input.remove_suffix(1);
    return input;
}

struct ResponseHead {
    int status = 0;
    std::uint64_t content_length = 0;
    bool has_content_length = false;
    bool close = false;
    bool transfer_encoding = false;
};

std::optional<ResponseHead> parse_response_head(std::string_view head, std::string& error) {
    const auto first_end = head.find("\r\n");
    if (first_end == std::string_view::npos || !head.starts_with("HTTP/1.")) {
        error = "malformed HTTP status line";
        return std::nullopt;
    }
    const auto first_space = head.find(' ');
    if (first_space == std::string_view::npos || first_space + 4 > first_end) {
        error = "malformed HTTP status code";
        return std::nullopt;
    }
    ResponseHead parsed;
    const std::string_view code = head.substr(first_space + 1, 3);
    const auto [code_end, code_ec] =
        std::from_chars(code.data(), code.data() + code.size(), parsed.status);
    if (code_ec != std::errc{} || code_end != code.data() + code.size()) {
        error = "malformed HTTP status code";
        return std::nullopt;
    }

    std::size_t position = first_end + 2;
    while (position < head.size()) {
        const auto end = head.find("\r\n", position);
        const auto line_end = end == std::string_view::npos ? head.size() : end;
        if (line_end == position) break;
        const auto colon = head.find(':', position);
        if (colon == std::string_view::npos || colon >= line_end) {
            error = "malformed HTTP header";
            return std::nullopt;
        }
        const std::string name = lower(trim(head.substr(position, colon - position)));
        const std::string_view value = trim(head.substr(colon + 1, line_end - colon - 1));
        if (name == "content-length") {
            std::uint64_t length = 0;
            const auto [length_end, length_ec] =
                std::from_chars(value.data(), value.data() + value.size(), length);
            if (length_ec != std::errc{} || length_end != value.data() + value.size() ||
                (parsed.has_content_length && parsed.content_length != length)) {
                error = "invalid or conflicting Content-Length";
                return std::nullopt;
            }
            parsed.content_length = length;
            parsed.has_content_length = true;
        } else if (name == "connection" && lower(value).find("close") != std::string::npos) {
            parsed.close = true;
        } else if (name == "transfer-encoding") {
            parsed.transfer_encoding = true;
        }
        if (end == std::string_view::npos) break;
        position = end + 2;
    }
    return parsed;
}

class Connection {
public:
    explicit Connection(const Args& args) : args_(args), body_buffer_(1U << 20) {}
    ~Connection() { close(); }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    bool connected() const noexcept { return fd_ >= 0; }

    bool connect(std::uint64_t& elapsed, std::string& error) {
        close();
        const auto start = monotonic_ns();
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0) {
            error = errno_message("socket");
            return false;
        }
        const int one = 1;
        (void)::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        timeval timeout{static_cast<time_t>(args_.timeout_seconds), 0};
        if (::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
            ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
            error = errno_message("setsockopt timeout");
            close();
            return false;
        }

        if (!args_.source.empty()) {
            sockaddr_in source{};
            source.sin_family = AF_INET;
            source.sin_port = 0;
            if (::inet_pton(AF_INET, args_.source.c_str(), &source.sin_addr) != 1) {
                error = "invalid --source IPv4 address";
                close();
                return false;
            }
            if (::bind(fd_, reinterpret_cast<const sockaddr*>(&source), sizeof(source)) != 0) {
                error = errno_message("bind source address");
                close();
                return false;
            }
        }

        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(args_.port);
        if (::inet_pton(AF_INET, args_.host.c_str(), &destination.sin_addr) != 1) {
            error = "invalid --host IPv4 address";
            close();
            return false;
        }
        if (::connect(fd_, reinterpret_cast<const sockaddr*>(&destination), sizeof(destination)) != 0) {
            error = errno_message("connect");
            close();
            return false;
        }
        elapsed = monotonic_ns() - start;
        return true;
    }

    bool request(const Object& object, bool head_only, Record& record) {
        const std::string method = head_only ? "HEAD" : "GET";
        const std::string request =
            method + " " + object.path + " HTTP/1.1\r\nHost: " + args_.host +
            "\r\nUser-Agent: goblin-mirror-bench/1\r\nAccept: */*\r\n"
            "Accept-Encoding: identity\r\nConnection: keep-alive\r\n\r\n";
        record.wall_start_ns = realtime_ns();
        record.query_start_ns = monotonic_ns();
        if (!send_all(request, record.error)) {
            close();
            return false;
        }

        std::string head;
        head.reserve(4096);
        std::uint64_t first_response = 0;
        std::array<char, 64 * 1024> header_buffer{};
        std::size_t header_end = std::string::npos;
        while (header_end == std::string::npos) {
            const ssize_t got = receive(header_buffer.data(), header_buffer.size(), record.error);
            if (got <= 0) {
                close();
                return false;
            }
            const auto received_at = monotonic_ns();
            if (first_response == 0) first_response = received_at;
            head.append(header_buffer.data(), static_cast<std::size_t>(got));
            if (head.size() > 256 * 1024) {
                record.error = "HTTP response headers exceed 256 KiB";
                close();
                return false;
            }
            header_end = head.find("\r\n\r\n");
        }

        std::string parse_error;
        const auto parsed = parse_response_head(std::string_view(head).substr(0, header_end + 2),
                                                parse_error);
        if (!parsed) {
            record.error = std::move(parse_error);
            close();
            return false;
        }
        record.status = parsed->status;
        record.response_first_byte_ns = first_response;
        record.response_ttfb_ns = first_response - record.query_start_ns;
        if (parsed->transfer_encoding || !parsed->has_content_length) {
            record.error = parsed->transfer_encoding ? "chunked/encoded response is unsupported"
                                                     : "response has no Content-Length";
            close();
            return false;
        }
        if (parsed->status != 200) {
            record.error = "HTTP status " + std::to_string(parsed->status);
            close();
            return false;
        }
        if (parsed->content_length != object.bytes) {
            record.error = "Content-Length mismatch: expected " + std::to_string(object.bytes) +
                           ", received header " + std::to_string(parsed->content_length);
            close();
            return false;
        }

        const std::size_t body_offset = header_end + 4;
        const std::uint64_t initial = head.size() - body_offset;
        if (initial > object.bytes) {
            record.error = "received bytes beyond declared response body";
            close();
            return false;
        }
        if (head_only && initial != 0) {
            record.error = "HEAD response included a body";
            close();
            return false;
        }

        record.received_bytes = head_only ? 0 : initial;
        if (!head_only && initial != 0) record.body_first_byte_ns = first_response;
        std::uint64_t last_receive = first_response;
        while (!head_only && record.received_bytes < object.bytes) {
            const auto remaining = object.bytes - record.received_bytes;
            const auto wanted = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, body_buffer_.size()));
            const ssize_t got = receive(body_buffer_.data(), wanted, record.error);
            if (got <= 0) {
                close();
                return false;
            }
            last_receive = monotonic_ns();
            if (record.body_first_byte_ns == 0) record.body_first_byte_ns = last_receive;
            record.received_bytes += static_cast<std::uint64_t>(got);
        }

        record.complete_ns = last_receive;
        record.ttlb_ns = record.complete_ns - record.query_start_ns;
        if (record.body_first_byte_ns != 0) {
            record.body_ttfb_ns = record.body_first_byte_ns - record.query_start_ns;
            record.body_transfer_ns = record.complete_ns - record.body_first_byte_ns;
        } else {
            record.body_ttfb_ns = record.response_ttfb_ns;
            record.body_transfer_ns = 0;
        }
        record.error.clear();
        if (parsed->close) close();
        return true;
    }

private:
    bool send_all(std::string_view bytes, std::string& error) {
        while (!bytes.empty()) {
            const ssize_t sent = ::send(fd_, bytes.data(), bytes.size(), MSG_NOSIGNAL);
            if (sent > 0) {
                bytes.remove_prefix(static_cast<std::size_t>(sent));
                continue;
            }
            if (sent < 0 && errno == EINTR) continue;
            error = sent == 0 ? "send returned zero" : errno_message("send");
            return false;
        }
        return true;
    }

    ssize_t receive(void* output, std::size_t length, std::string& error) {
        for (;;) {
            const ssize_t got = ::recv(fd_, output, length, 0);
            if (got > 0) return got;
            if (got < 0 && errno == EINTR) continue;
            error = got == 0 ? "peer closed the connection" : errno_message("recv");
            return got;
        }
    }

    void close() noexcept {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

    const Args& args_;
    int fd_ = -1;
    std::vector<char> body_buffer_;
};

std::string csv_quote(std::string_view value) {
    bool needs_quotes = false;
    for (const char ch : value)
        if (ch == ',' || ch == '"' || ch == '\r' || ch == '\n') needs_quotes = true;
    if (!needs_quotes) return std::string(value);
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const char ch : value) {
        if (ch == '"') out.push_back('"');
        out.push_back(ch);
    }
    out.push_back('"');
    return out;
}

void write_results(const Args& args, const std::vector<Record>& records) {
    if (!args.output.parent_path().empty())
        std::filesystem::create_directories(args.output.parent_path());
    const auto temporary = args.output.string() + ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create " + temporary);
    output << "sequence,repetition,object_index,worker,connection,new_connection,wall_start_ns,"
              "connect_ns,query_start_ns,response_first_byte_ns,body_first_byte_ns,complete_ns,"
              "response_ttfb_ns,body_ttfb_ns,ttlb_ns,body_transfer_ns,expected_bytes,"
              "received_bytes,status,path,error\n";
    for (const auto& row : records) {
        output << row.sequence << ',' << row.repetition << ',' << row.object_index << ','
               << row.worker << ',' << row.connection << ',' << (row.new_connection ? 1 : 0) << ','
               << row.wall_start_ns << ',' << row.connect_ns << ',' << row.query_start_ns << ','
               << row.response_first_byte_ns << ',' << row.body_first_byte_ns << ','
               << row.complete_ns << ',' << row.response_ttfb_ns << ',' << row.body_ttfb_ns << ','
               << row.ttlb_ns << ',' << row.body_transfer_ns << ',' << row.expected_bytes << ','
               << row.received_bytes << ',' << row.status << ',' << csv_quote(row.path) << ','
               << csv_quote(row.error) << '\n';
    }
    output.close();
    if (!output) throw std::runtime_error("write failed for " + temporary);
    std::error_code ec;
    std::filesystem::rename(temporary, args.output, ec);
    if (ec) throw std::runtime_error("rename results: " + ec.message());
}

}  // namespace

int main(int argc, char** argv) try {
    const Args args = parse_args(argc, argv);
    const auto objects = load_manifest(args.manifest);
    const auto tasks = make_tasks(objects.size(), args.passes, args.seed, args.max_requests);
    std::vector<Record> records(tasks.size());
    std::atomic<std::size_t> next{0};
    std::atomic<std::uint64_t> completed{0};
    std::atomic<std::uint64_t> successful_bytes{0};
    std::atomic<unsigned> failures{0};
    std::atomic<bool> start{false};
    std::mutex start_mutex;
    std::condition_variable start_cv;
    std::mutex progress_mutex;
    std::condition_variable progress_cv;

    std::cerr << "requests=" << tasks.size() << " objects=" << objects.size()
              << " passes=" << args.passes << " concurrency=" << args.concurrency
              << " seed=0x" << std::hex << args.seed << std::dec
              << " method=" << (args.head_only ? "HEAD" : "GET") << '\n';

    std::vector<std::thread> workers;
    workers.reserve(args.concurrency);
    for (unsigned worker = 0; worker < args.concurrency; ++worker) {
        workers.emplace_back([&, worker] {
            {
                std::unique_lock lock(start_mutex);
                start_cv.wait(lock, [&] { return start.load(std::memory_order_acquire); });
            }
            Connection connection(args);
            unsigned connection_id = 0;
            for (;;) {
                const std::size_t index = next.fetch_add(1, std::memory_order_relaxed);
                if (index >= tasks.size()) break;
                const auto& task = tasks[index];
                const auto& object = objects[task.object_index];
                Record& record = records[index];
                record.sequence = task.sequence;
                record.repetition = task.repetition;
                record.object_index = task.object_index;
                record.worker = worker;
                record.expected_bytes = object.bytes;
                record.path = object.path;
                record.new_connection = !connection.connected();
                if (record.new_connection) {
                    ++connection_id;
                    if (!connection.connect(record.connect_ns, record.error)) {
                        record.connection = connection_id;
                        failures.fetch_add(1, std::memory_order_relaxed);
                        if (completed.fetch_add(1, std::memory_order_release) + 1 == tasks.size())
                            progress_cv.notify_one();
                        continue;
                    }
                }
                record.connection = connection_id;
                if (connection.request(object, args.head_only, record)) {
                    successful_bytes.fetch_add(record.received_bytes, std::memory_order_relaxed);
                } else {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
                if (completed.fetch_add(1, std::memory_order_release) + 1 == tasks.size())
                    progress_cv.notify_one();
            }
        });
    }

    const auto case_start = monotonic_ns();
    start.store(true, std::memory_order_release);
    start_cv.notify_all();
    std::uint64_t previous_completed = 0;
    while (completed.load(std::memory_order_acquire) < tasks.size()) {
        std::unique_lock lock(progress_mutex);
        progress_cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return completed.load(std::memory_order_acquire) == tasks.size();
        });
        lock.unlock();
        const auto done = completed.load(std::memory_order_acquire);
        if (done != previous_completed) {
            const double seconds = static_cast<double>(monotonic_ns() - case_start) / 1e9;
            const double gib = static_cast<double>(successful_bytes.load(std::memory_order_relaxed)) /
                               static_cast<double>(1ULL << 30);
            std::cerr << "progress=" << done << '/' << tasks.size() << " failures="
                      << failures.load(std::memory_order_relaxed) << " GiB=" << gib
                      << " elapsed_s=" << seconds << '\n';
            previous_completed = done;
        }
    }
    for (auto& worker : workers) worker.join();
    const auto case_end = monotonic_ns();
    write_results(args, records);

    const double seconds = static_cast<double>(case_end - case_start) / 1e9;
    const auto bytes = successful_bytes.load(std::memory_order_relaxed);
    const double gib = static_cast<double>(bytes) / static_cast<double>(1ULL << 30);
    const double gbps = seconds == 0.0 ? 0.0 : static_cast<double>(bytes) * 8.0 / seconds / 1e9;
    std::cout << "completed=" << completed.load() << " failures=" << failures.load()
              << " bytes=" << bytes << " GiB=" << gib << " elapsed_s=" << seconds
              << " aggregate_Gbit_s=" << gbps << " output=" << args.output << '\n';
    return failures.load() == 0 ? 0 : 1;
} catch (const std::exception& error) {
    std::cerr << "fatal: " << error.what() << '\n';
    return 2;
}
