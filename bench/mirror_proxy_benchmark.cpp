// Fixed-seed TCP/HTTP benchmark for Goblin Store --mirror versus Vinyl Cache.
//
// One or more io_uring event loops multiplex the requested number of persistent TCP connections.
// Each connection keeps exactly one request in flight, preserving the benchmark's closed-loop
// HTTP/1.1 semantics without dedicating a thread to every socket. Idle connections claim work from
// one cache-line-isolated queue, so neither an event loop nor a connection can retain a private
// long tail. Per-request results remain in RAM until the complete case finishes, then are written
// atomically as CSV. The measured interval begins just before the HTTP request bytes are sent and
// ends when the final response-body byte is received.
// Build on the client host:
//   g++ -O3 -std=c++20 -pthread -Wall -Wextra -Werror mirror_proxy_benchmark.cpp -luring -o mirror-proxy-benchmark

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <unistd.h>

#include <liburing.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <charconv>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
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
    unsigned threads = 1;
    unsigned passes = 1;
    unsigned timeout_seconds = 300;
    std::uint64_t seed = 0x243f6a8885a308d3ULL;
    std::uint64_t max_requests = 0;
    bool head_only = false;
    bool manifest_order = false;
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
        << "  --threads N         independent io_uring event loops [1]\n"
        << "  --passes N          independently shuffled passes through the manifest [1]\n"
        << "  --seed N            fixed PRNG seed, decimal or 0x-prefixed\n"
        << "  --timeout N         in-flight io_uring operation timeout in seconds [300]\n"
        << "  --max-requests N    truncate the generated request sequence (smoke/probe)\n"
        << "  --manifest-order    replay manifest rows exactly; do not shuffle\n"
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
        } else if (option == "--threads") {
            const auto value = parse_u64(next(), "threads");
            if (value == 0 || value > 1024) usage(argv[0], "threads is out of range");
            args.threads = static_cast<unsigned>(value);
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
        else if (option == "--manifest-order") args.manifest_order = true;
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
                             std::uint64_t maximum, bool manifest_order) {
    std::mt19937_64 random(seed);
    std::vector<Task> tasks;
    if (objects > std::numeric_limits<std::size_t>::max() / passes)
        throw std::runtime_error("task count overflows size_t");
    tasks.reserve(objects * passes);
    std::vector<std::size_t> order(objects);
    for (unsigned repetition = 0; repetition < passes; ++repetition) {
        std::iota(order.begin(), order.end(), 0);
        if (!manifest_order) std::shuffle(order.begin(), order.end(), random);
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

constexpr std::size_t cache_line_bytes = 64;

template <typename T>
struct alignas(cache_line_bytes) CacheLineAtomic {
    std::atomic<T> value{0};
};

static_assert(sizeof(CacheLineAtomic<std::uint64_t>) == cache_line_bytes);
static_assert(sizeof(CacheLineAtomic<unsigned>) == cache_line_bytes);

struct Counters {
    CacheLineAtomic<std::uint64_t> completed;
    CacheLineAtomic<std::uint64_t> successful_bytes;
    CacheLineAtomic<unsigned> failures;
};

static_assert(alignof(Counters) == cache_line_bytes);
static_assert(sizeof(Counters) == 3 * cache_line_bytes);

std::vector<unsigned> allowed_cpus() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (::sched_getaffinity(0, sizeof(set), &set) != 0)
        throw std::runtime_error(errno_message("sched_getaffinity"));
    std::vector<unsigned> cpus;
    for (unsigned cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        if (CPU_ISSET(cpu, &set)) cpus.push_back(cpu);
    if (cpus.empty()) throw std::runtime_error("process CPU affinity mask is empty");
    return cpus;
}

void pin_current_thread(unsigned cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    const int result = ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set);
    if (result != 0)
        throw std::runtime_error("pthread_setaffinity_np: " +
                                 std::string(std::strerror(result)));
}

enum class Operation { none, connect, send, receive };

struct alignas(cache_line_bytes) TaskQueue {
    std::atomic<std::size_t> next{0};

    std::optional<std::size_t> take(std::size_t task_count) noexcept {
        const auto index = next.fetch_add(1, std::memory_order_relaxed);
        if (index >= task_count) return std::nullopt;
        return index;
    }
};

static_assert(sizeof(TaskQueue) == cache_line_bytes);

struct ConnectionState {
    explicit ConnectionState(unsigned lane_number) : lane(lane_number), body_buffer(1U << 20) {
        head.reserve(4096);
    }

    unsigned lane = 0;
    unsigned connection_id = 0;
    int fd = -1;
    Operation operation = Operation::none;
    std::uint64_t operation_started_ns = 0;
    std::uint64_t connect_started_ns = 0;
    std::size_t task_index = 0;
    bool has_task = false;
    bool headers_complete = false;
    bool close_after_response = false;
    std::size_t sent_bytes = 0;
    std::string request;
    std::string head;
    std::array<char, 64 * 1024> header_buffer{};
    std::vector<char> body_buffer;
};

class UringClient {
public:
    UringClient(const Args& args, const std::vector<Object>& objects,
                const std::vector<Task>& tasks, std::vector<Record>& records, Counters& counters,
                const std::vector<std::unique_ptr<Counters>>& all_counters,
                std::atomic<bool>& cancelled, TaskQueue& task_queue, unsigned lane_base,
                bool report_progress)
        : args_(args), objects_(objects), tasks_(tasks), records_(records), counters_(counters),
          all_counters_(all_counters), cancelled_(cancelled), task_queue_(task_queue),
          lane_base_(lane_base),
          report_progress_(report_progress) {
        destination_.sin_family = AF_INET;
        destination_.sin_port = htons(args_.port);
        if (::inet_pton(AF_INET, args_.host.c_str(), &destination_.sin_addr) != 1)
            throw std::runtime_error("invalid --host IPv4 address");

        if (!args_.source.empty()) {
            source_.emplace();
            source_->sin_family = AF_INET;
            source_->sin_port = 0;
            if (::inet_pton(AF_INET, args_.source.c_str(), &source_->sin_addr) != 1)
                throw std::runtime_error("invalid --source IPv4 address");
        }

        const auto lanes = std::min<std::size_t>(args_.concurrency, tasks_.size());
        states_.reserve(lanes);
        for (std::size_t lane = 0; lane < lanes; ++lane) {
            const auto global_lane = lane_base_ + static_cast<unsigned>(lane);
            states_.emplace_back(global_lane);
        }

        const unsigned entries = static_cast<unsigned>(std::max<std::size_t>(256, lanes * 2 + 8));
        const int result = ::io_uring_queue_init(entries, &ring_, 0);
        if (result < 0)
            throw std::runtime_error("io_uring_queue_init: " +
                                     std::string(std::strerror(-result)));
        ring_ready_ = true;
    }

    ~UringClient() {
        if (ring_ready_) ::io_uring_queue_exit(&ring_);
        for (auto& state : states_) close_socket(state);
    }

    UringClient(const UringClient&) = delete;
    UringClient& operator=(const UringClient&) = delete;

    void run(std::uint64_t case_start_ns) {
        for (auto& state : states_) assign_task(state);
        start_progress_timer();

        std::uint64_t previous_completed = 0;
        while (!cancelled_.load(std::memory_order_acquire) && has_active_tasks()) {
            const int result = ::io_uring_submit_and_wait(&ring_, 1);
            if (result < 0 && result != -EINTR)
                throw std::runtime_error("io_uring_submit_and_wait: " +
                                         std::string(std::strerror(-result)));
            if (result == -EINTR) continue;

            bool progress_timer_fired = false;
            io_uring_cqe* completion = nullptr;
            while (::io_uring_peek_cqe(&ring_, &completion) == 0) {
                if (completion->user_data == 0) {
                    if (completion->res != -ETIME)
                        throw std::runtime_error("io_uring progress timer: " +
                                                 std::string(std::strerror(-completion->res)));
                    progress_timer_fired = true;
                } else {
                    handle(*completion);
                }
                ::io_uring_cqe_seen(&ring_, completion);
            }

            if (progress_timer_fired && has_active_tasks()) {
                const auto now = monotonic_ns();
                if (report_progress_) report_progress(case_start_ns, previous_completed);
                check_timeouts(now);
                start_progress_timer();
            }
        }
    }

private:
    bool has_active_tasks() const noexcept {
        return std::ranges::any_of(states_, [](const auto& state) { return state.has_task; });
    }

    io_uring_sqe* get_sqe() {
        io_uring_sqe* submission = ::io_uring_get_sqe(&ring_);
        if (!submission) throw std::runtime_error("io_uring submission queue is full");
        return submission;
    }

    void start_progress_timer() {
        auto* submission = get_sqe();
        ::io_uring_prep_timeout(submission, &progress_timeout_, 0, 0);
        submission->user_data = 0;
    }

    void set_submission(ConnectionState& state, io_uring_sqe* submission, Operation operation) {
        state.operation = operation;
        state.operation_started_ns = monotonic_ns();
        submission->user_data = static_cast<std::uint64_t>(state.lane - lane_base_) + 1;
    }

    void close_socket(ConnectionState& state) noexcept {
        if (state.fd >= 0) ::close(state.fd);
        state.fd = -1;
        state.operation = Operation::none;
        state.operation_started_ns = 0;
    }

    bool prepare_socket(ConnectionState& state, Record& record) {
        close_socket(state);
        state.fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (state.fd < 0) {
            record.error = errno_message("socket");
            return false;
        }
        const int one = 1;
        (void)::setsockopt(state.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (source_ &&
            ::bind(state.fd, reinterpret_cast<const sockaddr*>(&*source_), sizeof(*source_)) != 0) {
            record.error = errno_message("bind source address");
            close_socket(state);
            return false;
        }
        return true;
    }

    void start_connect(ConnectionState& state) {
        state.connect_started_ns = monotonic_ns();
        auto* submission = get_sqe();
        ::io_uring_prep_connect(submission, state.fd,
                                reinterpret_cast<const sockaddr*>(&destination_),
                                sizeof(destination_));
        set_submission(state, submission, Operation::connect);
    }

    void start_request(ConnectionState& state) {
        const auto& task = tasks_[state.task_index];
        const auto& object = objects_[task.object_index];
        const std::string_view method = args_.head_only ? "HEAD" : "GET";
        state.request.clear();
        state.request.reserve(method.size() + object.path.size() + args_.host.size() + 128);
        state.request.append(method).append(" ").append(object.path)
            .append(" HTTP/1.1\r\nHost: ").append(args_.host)
            .append("\r\nUser-Agent: goblin-mirror-bench/1\r\nAccept: */*\r\n")
            .append("Accept-Encoding: identity\r\nConnection: keep-alive\r\n\r\n");
        state.sent_bytes = 0;
        state.head.clear();
        state.headers_complete = false;
        state.close_after_response = false;

        Record& record = records_[state.task_index];
        record.wall_start_ns = realtime_ns();
        record.query_start_ns = monotonic_ns();
        start_send(state);
    }

    void start_send(ConnectionState& state) {
        auto* submission = get_sqe();
        ::io_uring_prep_send(submission, state.fd,
                             state.request.data() + state.sent_bytes,
                             state.request.size() - state.sent_bytes, MSG_NOSIGNAL);
        set_submission(state, submission, Operation::send);
    }

    void start_receive(ConnectionState& state) {
        void* buffer = state.header_buffer.data();
        std::size_t wanted = state.header_buffer.size();
        if (state.headers_complete) {
            const auto& task = tasks_[state.task_index];
            const auto& object = objects_[task.object_index];
            const auto received = records_[state.task_index].received_bytes;
            buffer = state.body_buffer.data();
            wanted = static_cast<std::size_t>(
                std::min<std::uint64_t>(object.bytes - received, state.body_buffer.size()));
        }
        auto* submission = get_sqe();
        ::io_uring_prep_recv(submission, state.fd, buffer, wanted, 0);
        set_submission(state, submission, Operation::receive);
    }

    void assign_task(ConnectionState& state) {
        for (;;) {
            const auto next = task_queue_.take(tasks_.size());
            if (!next) {
                state.has_task = false;
                close_socket(state);
                return;
            }
            const std::size_t index = *next;

            state.task_index = index;
            state.has_task = true;
            const auto& task = tasks_[index];
            const auto& object = objects_[task.object_index];
            Record& record = records_[index];
            record.sequence = task.sequence;
            record.repetition = task.repetition;
            record.object_index = task.object_index;
            record.worker = state.lane;
            record.expected_bytes = object.bytes;
            record.path = object.path;
            record.new_connection = state.fd < 0;

            if (record.new_connection) {
                ++state.connection_id;
                record.connection = state.connection_id;
                if (!prepare_socket(state, record)) {
                    finish(state, false);
                    continue;
                }
                start_connect(state);
            } else {
                record.connection = state.connection_id;
                start_request(state);
            }
            return;
        }
    }

    void finish(ConnectionState& state, bool success) {
        Record& record = records_[state.task_index];
        if (success) {
            counters_.successful_bytes.value.fetch_add(record.received_bytes,
                                                        std::memory_order_relaxed);
        } else {
            counters_.failures.value.fetch_add(1, std::memory_order_relaxed);
            close_socket(state);
        }
        counters_.completed.value.fetch_add(1, std::memory_order_release);
        state.has_task = false;
    }

    void finish_and_continue(ConnectionState& state, bool success) {
        const bool close_after = state.close_after_response;
        finish(state, success);
        if (success && close_after) close_socket(state);
        assign_task(state);
    }

    void fail(ConnectionState& state, std::string error) {
        records_[state.task_index].error = std::move(error);
        finish_and_continue(state, false);
    }

    void succeed(ConnectionState& state, std::uint64_t completed_at) {
        Record& record = records_[state.task_index];
        record.complete_ns = completed_at;
        record.ttlb_ns = record.complete_ns - record.query_start_ns;
        if (record.body_first_byte_ns != 0) {
            record.body_ttfb_ns = record.body_first_byte_ns - record.query_start_ns;
            record.body_transfer_ns = record.complete_ns - record.body_first_byte_ns;
        } else {
            record.body_ttfb_ns = record.response_ttfb_ns;
            record.body_transfer_ns = 0;
        }
        record.error.clear();
        finish_and_continue(state, true);
    }

    void handle_connect(ConnectionState& state, int result, std::uint64_t completed_at) {
        Record& record = records_[state.task_index];
        record.connect_ns = completed_at - state.connect_started_ns;
        if (result < 0) {
            fail(state, "connect: " + std::string(std::strerror(-result)));
            return;
        }
        start_request(state);
    }

    void handle_send(ConnectionState& state, int result) {
        if (result <= 0) {
            fail(state, result == 0 ? "send returned zero"
                                    : "send: " + std::string(std::strerror(-result)));
            return;
        }
        state.sent_bytes += static_cast<std::size_t>(result);
        if (state.sent_bytes > state.request.size()) {
            fail(state, "send completed beyond request boundary");
        } else if (state.sent_bytes != state.request.size()) {
            start_send(state);
        } else {
            start_receive(state);
        }
    }

    void handle_response_head(ConnectionState& state, std::size_t received,
                              std::uint64_t received_at) {
        Record& record = records_[state.task_index];
        const auto& task = tasks_[state.task_index];
        const auto& object = objects_[task.object_index];
        if (record.response_first_byte_ns == 0) {
            record.response_first_byte_ns = received_at;
            record.response_ttfb_ns = received_at - record.query_start_ns;
        }
        state.head.append(state.header_buffer.data(), received);
        const std::size_t header_end = state.head.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            if (state.head.size() > 256 * 1024) {
                fail(state, "HTTP response headers exceed 256 KiB");
            } else {
                start_receive(state);
            }
            return;
        }

        std::string parse_error;
        const auto parsed = parse_response_head(
            std::string_view(state.head).substr(0, header_end + 2), parse_error);
        if (!parsed) {
            fail(state, std::move(parse_error));
            return;
        }
        record.status = parsed->status;
        if (parsed->transfer_encoding || !parsed->has_content_length) {
            fail(state, parsed->transfer_encoding ? "chunked/encoded response is unsupported"
                                                   : "response has no Content-Length");
            return;
        }
        if (parsed->status != 200) {
            fail(state, "HTTP status " + std::to_string(parsed->status));
            return;
        }
        if (parsed->content_length != object.bytes) {
            fail(state, "Content-Length mismatch: expected " + std::to_string(object.bytes) +
                            ", received header " + std::to_string(parsed->content_length));
            return;
        }

        const std::size_t body_offset = header_end + 4;
        const std::uint64_t initial = state.head.size() - body_offset;
        if (initial > object.bytes) {
            fail(state, "received bytes beyond declared response body");
            return;
        }
        if (args_.head_only && initial != 0) {
            fail(state, "HEAD response included a body");
            return;
        }

        state.headers_complete = true;
        state.close_after_response = parsed->close;
        record.received_bytes = args_.head_only ? 0 : initial;
        if (!args_.head_only && initial != 0) record.body_first_byte_ns = received_at;
        if (args_.head_only || record.received_bytes == object.bytes) {
            succeed(state, received_at);
        } else {
            start_receive(state);
        }
    }

    void handle_receive(ConnectionState& state, int result, std::uint64_t received_at) {
        if (result <= 0) {
            fail(state, result == 0 ? "peer closed the connection"
                                    : "recv: " + std::string(std::strerror(-result)));
            return;
        }
        if (!state.headers_complete) {
            handle_response_head(state, static_cast<std::size_t>(result), received_at);
            return;
        }

        Record& record = records_[state.task_index];
        const auto& task = tasks_[state.task_index];
        const auto& object = objects_[task.object_index];
        if (record.body_first_byte_ns == 0) record.body_first_byte_ns = received_at;
        record.received_bytes += static_cast<std::uint64_t>(result);
        if (record.received_bytes > object.bytes) {
            fail(state, "received bytes beyond declared response body");
        } else if (record.received_bytes == object.bytes) {
            succeed(state, received_at);
        } else {
            start_receive(state);
        }
    }

    void handle(const io_uring_cqe& completion) {
        const std::uint64_t user_data = completion.user_data;
        if (user_data == 0 || user_data > states_.size())
            throw std::runtime_error("io_uring completion has invalid user data");
        ConnectionState& state = states_[static_cast<std::size_t>(user_data - 1)];
        const Operation operation = state.operation;
        state.operation = Operation::none;
        state.operation_started_ns = 0;
        const auto completed_at = monotonic_ns();
        switch (operation) {
            case Operation::connect: handle_connect(state, completion.res, completed_at); break;
            case Operation::send: handle_send(state, completion.res); break;
            case Operation::receive: handle_receive(state, completion.res, completed_at); break;
            case Operation::none:
                throw std::runtime_error("io_uring completion has no matching operation");
        }
    }

    void report_progress(std::uint64_t case_start_ns, std::uint64_t& previous_completed) const {
        std::uint64_t done = 0;
        std::uint64_t bytes = 0;
        unsigned failures = 0;
        for (const auto& counters : all_counters_) {
            done += counters->completed.value.load(std::memory_order_acquire);
            bytes += counters->successful_bytes.value.load(std::memory_order_relaxed);
            failures += counters->failures.value.load(std::memory_order_relaxed);
        }
        if (done == previous_completed) return;
        const double seconds = static_cast<double>(monotonic_ns() - case_start_ns) / 1e9;
        const double gib = static_cast<double>(bytes) / static_cast<double>(1ULL << 30);
        std::cerr << "progress=" << done << '/' << tasks_.size() << " failures=" << failures
                  << " GiB=" << gib
                  << " elapsed_s=" << seconds << '\n';
        previous_completed = done;
    }

    void check_timeouts(std::uint64_t now) const {
        const std::uint64_t timeout_ns =
            static_cast<std::uint64_t>(args_.timeout_seconds) * 1'000'000'000ULL;
        for (const auto& state : states_) {
            if (state.operation != Operation::none && now - state.operation_started_ns >= timeout_ns)
                throw std::runtime_error("io_uring operation timed out on connection lane " +
                                         std::to_string(state.lane));
        }
    }

    const Args& args_;
    const std::vector<Object>& objects_;
    const std::vector<Task>& tasks_;
    std::vector<Record>& records_;
    Counters& counters_;
    const std::vector<std::unique_ptr<Counters>>& all_counters_;
    std::atomic<bool>& cancelled_;
    TaskQueue& task_queue_;
    unsigned lane_base_ = 0;
    bool report_progress_ = false;
    sockaddr_in destination_{};
    std::optional<sockaddr_in> source_;
    std::vector<ConnectionState> states_;
    io_uring ring_{};
    __kernel_timespec progress_timeout_{5, 0};
    bool ring_ready_ = false;
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
    const auto tasks =
        make_tasks(objects.size(), args.passes, args.seed, args.max_requests, args.manifest_order);
    std::vector<Record> records(tasks.size());
    std::atomic<bool> cancelled{false};
    const unsigned lanes =
        static_cast<unsigned>(std::min<std::size_t>(args.concurrency, tasks.size()));
    const unsigned loops = std::min(args.threads, lanes);
    const auto cpus = allowed_cpus();

    std::cerr << "requests=" << tasks.size() << " objects=" << objects.size()
              << " passes=" << args.passes << " concurrency=" << args.concurrency
              << " connections=" << lanes << " threads=" << loops
              << " allowed_cpus=" << cpus.size() << " engine=io_uring"
              << " task_dispatch=shared-queue"
              << " seed=0x" << std::hex << args.seed << std::dec
              << " ordering=" << (args.manifest_order ? "manifest" : "shuffled")
              << " method=" << (args.head_only ? "HEAD" : "GET") << '\n';

    std::vector<Args> loop_args(loops, args);
    std::vector<std::unique_ptr<Counters>> counters;
    counters.reserve(loops);
    for (unsigned loop = 0; loop < loops; ++loop)
        counters.push_back(std::make_unique<Counters>());
    std::vector<std::unique_ptr<UringClient>> clients;
    clients.reserve(loops);
    TaskQueue task_queue;
    unsigned lane_base = 0;
    for (unsigned loop = 0; loop < loops; ++loop) {
        loop_args[loop].concurrency =
            lanes / loops + (loop < lanes % loops ? 1U : 0U);
        clients.push_back(std::make_unique<UringClient>(
            loop_args[loop], objects, tasks, records, *counters[loop], counters, cancelled,
            task_queue, lane_base, loop == 0));
        lane_base += loop_args[loop].concurrency;
    }
    if (lane_base != lanes)
        throw std::runtime_error("internal connection-lane partition is incomplete");

    std::barrier start_gate(static_cast<std::ptrdiff_t>(loops + 1));
    std::vector<std::thread> workers;
    std::vector<std::exception_ptr> errors(loops);
    workers.reserve(loops);
    std::uint64_t case_start = 0;
    for (unsigned loop = 0; loop < loops; ++loop) {
        workers.emplace_back([&, loop] {
            try {
                pin_current_thread(cpus[loop % cpus.size()]);
            } catch (...) {
                errors[loop] = std::current_exception();
                cancelled.store(true, std::memory_order_release);
            }
            start_gate.arrive_and_wait();
            if (cancelled.load(std::memory_order_acquire)) return;
            try {
                clients[loop]->run(case_start);
            } catch (...) {
                errors[loop] = std::current_exception();
                cancelled.store(true, std::memory_order_release);
            }
        });
    }
    case_start = monotonic_ns();
    start_gate.arrive_and_wait();
    for (auto& worker : workers) worker.join();
    const auto case_end = monotonic_ns();
    for (const auto& error : errors)
        if (error) std::rethrow_exception(error);
    write_results(args, records);

    const double seconds = static_cast<double>(case_end - case_start) / 1e9;
    std::uint64_t completed = 0;
    std::uint64_t bytes = 0;
    unsigned failures = 0;
    for (const auto& loop_counters : counters) {
        completed += loop_counters->completed.value.load(std::memory_order_relaxed);
        bytes += loop_counters->successful_bytes.value.load(std::memory_order_relaxed);
        failures += loop_counters->failures.value.load(std::memory_order_relaxed);
    }
    const double gib = static_cast<double>(bytes) / static_cast<double>(1ULL << 30);
    const double gbps = seconds == 0.0 ? 0.0 : static_cast<double>(bytes) * 8.0 / seconds / 1e9;
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) throw std::runtime_error(errno_message("getrusage"));
    const double cpu_seconds =
        static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
        static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
    const double cpu_percent = seconds == 0.0 ? 0.0 : cpu_seconds / seconds * 100.0;
    std::cout << "completed=" << completed << " failures=" << failures
              << " bytes=" << bytes << " GiB=" << gib << " elapsed_s=" << seconds
              << " aggregate_Gbit_s=" << gbps << " client_cpu_percent=" << cpu_percent
              << " client_max_rss_kib=" << usage.ru_maxrss << " output=" << args.output << '\n';
    return failures == 0 ? 0 : 1;
} catch (const std::exception& error) {
    std::cerr << "fatal: " << error.what() << '\n';
    return 2;
}
