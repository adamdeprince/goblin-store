#include "goblin/store/client.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

enum class Mode { load, latency, throughput };

struct Config {
    Mode mode{};
    goblin::client::Options client;
    std::uint64_t object_bytes = 256 * 1024;
    std::uint64_t objects = 16 * 1024;
    unsigned threads = 1;
    std::uint64_t requests = 100'000;
    double seconds = 30.0;
    double warmup_seconds = 5.0;
    std::uint64_t warmup_requests = 1'000;
    std::uint64_t seed = 0x676f626c696eULL;
    std::string key_prefix = "rdma256k-";
    std::string csv;
};

struct GetTiming {
    TimePoint started;
    TimePoint completed;
    std::uint64_t ttfb_ns = 0;
    std::uint64_t body_ready_ns = 0;
    std::uint64_t total_ns = 0;
};

[[noreturn]] void usage_error(std::string_view message) {
    if (!message.empty()) std::cerr << "error: " << message << "\n\n";
    std::cerr
        << "Usage: goblin_store_rdma_benchmark MODE --address ADDRESS [OPTIONS]\n"
        << "\n"
        << "Modes:\n"
        << "  load        Store the complete fixed-size object set.\n"
        << "  latency     Run one request at a time and write per-request timing CSV.\n"
        << "  throughput  Run one sequential stream per QP for a fixed interval.\n"
        << "\n"
        << "Dataset options:\n"
        << "  --address ADDRESS          Numeric server address (required)\n"
        << "  --port PORT                Server port (default 11211)\n"
        << "  --object-bytes BYTES       Bytes per value (default 262144)\n"
        << "  --objects COUNT            Number of keys (default 16384)\n"
        << "  --key-prefix TEXT          Key namespace (default rdma256k-)\n"
        << "  --seed INTEGER             Fixed PRNG seed (default 0x676f626c696e)\n"
        << "\n"
        << "Run options:\n"
        << "  --threads COUNT            Load workers or throughput QPs (default 1)\n"
        << "  --requests COUNT           Measured latency requests (default 100000)\n"
        << "  --warmup-requests COUNT    Latency warmup requests (default 1000)\n"
        << "  --seconds SECONDS          Throughput measurement time (default 30)\n"
        << "  --warmup-seconds SECONDS   Throughput warmup time (default 5)\n"
        << "  --csv PATH                 Raw latency or throughput summary CSV\n"
        << "\n"
        << "Transport options:\n"
        << "  --ring-bytes BYTES         Control-ring bytes (default 65536)\n"
        << "  --bulk-window-bytes BYTES  Registered bulk-window bytes (default 262144)\n"
        << "  --bulk-window-count COUNT  Windows in each direction (default 4)\n"
        << "  --connect-timeout-ms MS    RDMA-CM timeout (default 5000)\n"
        << "  --operation-timeout-ms MS  Per-request timeout (default 30000)\n";
    std::exit(message.empty() ? 0 : 2);
}

template <typename T>
T parse_unsigned(std::string_view text, std::string_view name) {
    static_assert(std::is_unsigned_v<T>);
    T value{};
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
        base = 16;
    }
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (error != std::errc{} || end != text.data() + text.size())
        usage_error(std::string("invalid ") + std::string(name));
    return value;
}

double parse_positive_double(std::string_view text, std::string_view name) {
    std::string owned(text);
    std::size_t consumed = 0;
    double value = 0.0;
    try {
        value = std::stod(owned, &consumed);
    } catch (const std::exception&) {
        usage_error(std::string("invalid ") + std::string(name));
    }
    if (consumed != owned.size() || !std::isfinite(value) || value < 0.0)
        usage_error(std::string("invalid ") + std::string(name));
    return value;
}

std::string_view require_value(int& index, int argc, char** argv) {
    if (++index >= argc) usage_error(std::string("missing value for ") + argv[index - 1]);
    return argv[index];
}

Config parse_arguments(int argc, char** argv) {
    if (argc < 2 || std::string_view(argv[1]) == "--help" ||
        std::string_view(argv[1]) == "-h")
        usage_error({});

    Config config;
    const std::string_view mode(argv[1]);
    if (mode == "load") config.mode = Mode::load;
    else if (mode == "latency") config.mode = Mode::latency;
    else if (mode == "throughput") config.mode = Mode::throughput;
    else usage_error("MODE must be load, latency, or throughput");

    for (int i = 2; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--help" || argument == "-h") usage_error({});
        if (argument == "--address") config.client.address = require_value(i, argc, argv);
        else if (argument == "--port")
            config.client.port = parse_unsigned<std::uint16_t>(require_value(i, argc, argv), "port");
        else if (argument == "--object-bytes")
            config.object_bytes = parse_unsigned<std::uint64_t>(require_value(i, argc, argv), "object byte count");
        else if (argument == "--objects")
            config.objects = parse_unsigned<std::uint64_t>(require_value(i, argc, argv), "object count");
        else if (argument == "--threads")
            config.threads = parse_unsigned<unsigned>(require_value(i, argc, argv), "thread count");
        else if (argument == "--requests")
            config.requests = parse_unsigned<std::uint64_t>(require_value(i, argc, argv), "request count");
        else if (argument == "--seconds")
            config.seconds = parse_positive_double(require_value(i, argc, argv), "measurement time");
        else if (argument == "--warmup-seconds")
            config.warmup_seconds = parse_positive_double(require_value(i, argc, argv), "warmup time");
        else if (argument == "--warmup-requests")
            config.warmup_requests = parse_unsigned<std::uint64_t>(require_value(i, argc, argv), "warmup request count");
        else if (argument == "--seed")
            config.seed = parse_unsigned<std::uint64_t>(require_value(i, argc, argv), "seed");
        else if (argument == "--key-prefix") config.key_prefix = require_value(i, argc, argv);
        else if (argument == "--csv") config.csv = require_value(i, argc, argv);
        else if (argument == "--ring-bytes")
            config.client.ring_bytes = parse_unsigned<std::uint64_t>(require_value(i, argc, argv), "ring byte count");
        else if (argument == "--bulk-window-bytes")
            config.client.bulk_window_bytes = parse_unsigned<std::uint32_t>(require_value(i, argc, argv), "bulk-window byte count");
        else if (argument == "--bulk-window-count")
            config.client.bulk_window_count = parse_unsigned<std::uint16_t>(require_value(i, argc, argv), "bulk-window count");
        else if (argument == "--connect-timeout-ms")
            config.client.connect_timeout = std::chrono::milliseconds(
                parse_unsigned<std::uint64_t>(require_value(i, argc, argv), "connect timeout"));
        else if (argument == "--operation-timeout-ms")
            config.client.operation_timeout = std::chrono::milliseconds(
                parse_unsigned<std::uint64_t>(require_value(i, argc, argv), "operation timeout"));
        else usage_error(std::string("unknown option: ") + std::string(argument));
    }

    if (config.client.address.empty()) usage_error("--address is required");
    if (config.client.port == 0) usage_error("port must be nonzero");
    if (config.object_bytes == 0) usage_error("object size must be nonzero");
    if (config.objects == 0) usage_error("object count must be nonzero");
    if (config.threads == 0) usage_error("thread count must be nonzero");
    if (config.mode == Mode::latency && config.requests == 0)
        usage_error("latency request count must be nonzero");
    if (config.mode == Mode::throughput && config.seconds == 0.0)
        usage_error("throughput measurement time must be nonzero");
    if (config.key_prefix.empty()) usage_error("key prefix must be nonempty");
    if (config.key_prefix.size() + 20 > 190) usage_error("key prefix is too long");
    return config;
}

class SplitMix64 {
public:
    explicit SplitMix64(std::uint64_t state) : state_(state) {}

    std::uint64_t operator()() noexcept {
        std::uint64_t result = (state_ += 0x9e3779b97f4a7c15ULL);
        result = (result ^ (result >> 30)) * 0xbf58476d1ce4e5b9ULL;
        result = (result ^ (result >> 27)) * 0x94d049bb133111ebULL;
        return result ^ (result >> 31);
    }

private:
    std::uint64_t state_;
};

std::uint64_t uniform_index(SplitMix64& random, std::uint64_t count) noexcept {
    const std::uint64_t threshold = static_cast<std::uint64_t>(-count) % count;
    for (;;) {
        const std::uint64_t value = random();
        if (value >= threshold) return value % count;
    }
}

std::string key_for(const Config& config, std::uint64_t index) {
    std::string key = config.key_prefix;
    key += std::to_string(index);
    return key;
}

[[noreturn]] void transport_error(goblin::client::Transport& transport,
                                  std::string_view phase) {
    std::string message(phase);
    message += ": ";
    if (transport.error().empty()) message += "RDMA connection closed";
    else message += transport.error();
    transport.close();
    throw std::runtime_error(message);
}

void check_transport(goblin::client::Transport& transport, TimePoint deadline,
                     std::string_view phase) {
    if (transport.failed() || transport.disconnected()) transport_error(transport, phase);
    if (Clock::now() >= deadline) {
        transport.close();
        throw std::runtime_error(std::string(phase) + ": timed out");
    }
}

class ReceiveCursor {
public:
    explicit ReceiveCursor(goblin::client::Transport& transport) : transport_(transport) {}

    std::pair<std::string_view, TimePoint> available(TimePoint deadline) {
        release_if_consumed();
        while (!fragment_) {
            check_transport(transport_, deadline, "receive response");
            if (auto next = transport_.peek()) {
                if (next->empty()) throw std::runtime_error("RDMA transport returned an empty fragment");
                fragment_ = *next;
                offset_ = 0;
                arrived_ = Clock::now();
                break;
            }
            transport_.wait();
        }
        return {fragment_->substr(offset_), arrived_};
    }

    void consume(std::size_t bytes) {
        if (!fragment_ || bytes > fragment_->size() - offset_)
            throw std::logic_error("invalid receive cursor consumption");
        offset_ += bytes;
    }

    void finish() {
        release_if_consumed();
        if (fragment_) throw std::runtime_error("unexpected bytes after memcache response");
    }

private:
    void release_if_consumed() {
        if (fragment_ && offset_ == fragment_->size()) {
            transport_.pop();
            fragment_.reset();
            offset_ = 0;
        }
    }

    goblin::client::Transport& transport_;
    std::optional<std::string_view> fragment_;
    std::size_t offset_ = 0;
    TimePoint arrived_{};
};

std::string receive_line(ReceiveCursor& cursor, TimePoint deadline,
                         std::optional<TimePoint>& first_response) {
    std::string line;
    line.reserve(64);
    bool carriage_return = false;
    while (line.size() <= 8192) {
        const auto [bytes, arrived] = cursor.available(deadline);
        if (!first_response) first_response = arrived;
        std::size_t used = 0;
        for (const char byte : bytes) {
            ++used;
            if (carriage_return) {
                if (byte == '\n') {
                    cursor.consume(used);
                    return line;
                }
                line.push_back('\r');
                carriage_return = false;
            }
            if (byte == '\r') carriage_return = true;
            else line.push_back(byte);
        }
        cursor.consume(used);
    }
    throw std::runtime_error("memcache response line exceeds 8192 bytes");
}

std::uint64_t parse_value_length(std::string_view line, std::string_view key,
                                 std::uint64_t expected_bytes) {
    constexpr std::string_view prefix = "VALUE ";
    if (!line.starts_with(prefix))
        throw std::runtime_error("unexpected memcache response line: " + std::string(line));
    line.remove_prefix(prefix.size());
    if (!line.starts_with(key) || line.size() <= key.size() || line[key.size()] != ' ')
        throw std::runtime_error("memcache response returned the wrong key");
    line.remove_prefix(key.size() + 1);

    const std::size_t flags_end = line.find(' ');
    if (flags_end == std::string_view::npos)
        throw std::runtime_error("malformed memcache VALUE header");
    std::uint32_t flags = 0;
    const auto flags_result = std::from_chars(line.data(), line.data() + flags_end, flags);
    if (flags_result.ec != std::errc{} || flags_result.ptr != line.data() + flags_end)
        throw std::runtime_error("malformed flags in memcache VALUE header");
    line.remove_prefix(flags_end + 1);

    std::uint64_t bytes = 0;
    const auto bytes_result = std::from_chars(line.data(), line.data() + line.size(), bytes);
    if (bytes_result.ec != std::errc{} || bytes_result.ptr != line.data() + line.size())
        throw std::runtime_error("malformed byte count in memcache VALUE header");
    if (bytes != expected_bytes)
        throw std::runtime_error("memcache value has an unexpected size");
    return bytes;
}

void receive_exact(ReceiveCursor& cursor, std::uint64_t bytes, TimePoint deadline,
                   std::optional<TimePoint>& first_arrival) {
    while (bytes != 0) {
        const auto [available, arrived] = cursor.available(deadline);
        if (!first_arrival) first_arrival = arrived;
        const std::size_t take = static_cast<std::size_t>(
            std::min<std::uint64_t>(bytes, available.size()));
        cursor.consume(take);
        bytes -= take;
    }
}

void expect_bytes(ReceiveCursor& cursor, std::string_view expected, TimePoint deadline) {
    while (!expected.empty()) {
        const auto [available, arrived] = cursor.available(deadline);
        static_cast<void>(arrived);
        const std::size_t take = std::min(available.size(), expected.size());
        if (available.substr(0, take) != expected.substr(0, take))
            throw std::runtime_error("malformed memcache response trailer");
        cursor.consume(take);
        expected.remove_prefix(take);
    }
}

std::uint64_t nanoseconds(TimePoint begin, TimePoint end) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
}

GetTiming get_one(goblin::client::Transport& transport, const Config& config,
                  std::uint64_t key_index) {
    const std::string key = key_for(config, key_index);
    const std::string command = "get " + key + "\r\n";
    if (command.size() > transport.max_fragment_size())
        throw std::runtime_error("benchmark key does not fit in one inline request record");

    const TimePoint deadline = Clock::now() + config.client.operation_timeout;
    TimePoint started{};
    for (;;) {
        check_transport(transport, deadline, "send get request");
        const TimePoint attempt = Clock::now();
        if (transport.try_send(command)) {
            started = attempt;
            break;
        }
        transport.wait();
    }

    ReceiveCursor cursor(transport);
    std::optional<TimePoint> first_response;
    const std::string header = receive_line(cursor, deadline, first_response);
    const std::uint64_t body_bytes =
        parse_value_length(header, key, config.object_bytes);
    std::optional<TimePoint> body_ready;
    receive_exact(cursor, body_bytes, deadline, body_ready);
    expect_bytes(cursor, "\r\nEND\r\n", deadline);
    cursor.finish();
    const TimePoint completed = Clock::now();

    if (!first_response || !body_ready)
        throw std::runtime_error("memcache response omitted timing boundary");
    return GetTiming{started, completed,
                     nanoseconds(started, *first_response),
                     nanoseconds(started, *body_ready),
                     nanoseconds(started, completed)};
}

std::unique_ptr<goblin::client::Transport> connect_transport(const Config& config) {
    auto transport = goblin::client::connect_rdma(config.client);
    if (!transport) throw std::runtime_error("native RDMA transport is unavailable");
    return transport;
}

void rethrow_worker_errors(const std::vector<std::exception_ptr>& errors) {
    for (const auto& error : errors) {
        if (error) std::rethrow_exception(error);
    }
}

void run_load(const Config& config) {
    std::vector<std::thread> workers;
    std::vector<std::exception_ptr> errors(config.threads);
    std::atomic<std::uint64_t> loaded{0};
    workers.reserve(config.threads);
    for (unsigned thread = 0; thread < config.threads; ++thread) {
        workers.emplace_back([&, thread] {
            try {
                auto client = goblin::client::Client::connect(config.client);
                std::string value(static_cast<std::size_t>(config.object_bytes),
                                  static_cast<char>('A' + thread % 26));
                for (std::uint64_t index = thread; index < config.objects;
                     index += config.threads) {
                    const auto result = client.set(key_for(config, index), value);
                    if (result != goblin::client::StoreResult::stored)
                        throw std::runtime_error("server did not store key " +
                                                 key_for(config, index));
                    loaded.fetch_add(1, std::memory_order_relaxed);
                }
                client.close();
            } catch (...) {
                errors[thread] = std::current_exception();
            }
        });
    }
    for (auto& worker : workers) worker.join();
    rethrow_worker_errors(errors);
    const long double gib = static_cast<long double>(loaded.load()) * config.object_bytes /
                            (1024.0L * 1024.0L * 1024.0L);
    std::cout << "loaded_objects=" << loaded.load() << " object_bytes="
              << config.object_bytes << " dataset_gib=" << std::fixed
              << std::setprecision(3) << gib << "\n";
}

std::uint64_t percentile(const std::vector<std::uint64_t>& values, double probability) {
    if (values.empty()) return 0;
    const std::size_t index = static_cast<std::size_t>(
        std::ceil(probability * static_cast<double>(values.size()))) - 1;
    return values[std::min(index, values.size() - 1)];
}

void print_latency_field(std::string_view name, std::vector<std::uint64_t> values) {
    long double sum = 0.0;
    for (const auto value : values) sum += value;
    std::sort(values.begin(), values.end());
    std::cout << name << "_mean_ns=" << std::fixed << std::setprecision(1)
              << static_cast<double>(sum / values.size())
              << " " << name << "_p50_ns=" << percentile(values, 0.50)
              << " " << name << "_p90_ns=" << percentile(values, 0.90)
              << " " << name << "_p99_ns=" << percentile(values, 0.99)
              << " " << name << "_p99_9_ns=" << percentile(values, 0.999)
              << " " << name << "_p99_99_ns=" << percentile(values, 0.9999)
              << "\n";
}

void run_latency(const Config& config) {
    auto transport = connect_transport(config);
    SplitMix64 warmup_random(config.seed ^ 0x7761726d7570ULL);
    for (std::uint64_t request = 0; request < config.warmup_requests; ++request) {
        static_cast<void>(get_one(*transport, config,
                                  uniform_index(warmup_random, config.objects)));
    }

    // Allocate and fault the sample arrays before timing. CSV formatting and filesystem I/O are
    // deliberately deferred until the connection has closed so they cannot perturb later samples.
    const auto sample_count = static_cast<std::size_t>(config.requests);
    std::vector<std::uint64_t> key_indices(sample_count);
    std::vector<std::uint64_t> ttfb(sample_count);
    std::vector<std::uint64_t> body_ready(sample_count);
    std::vector<std::uint64_t> total(sample_count);
    SplitMix64 random(config.seed);
    for (std::uint64_t sample = 0; sample < config.requests; ++sample) {
        const std::uint64_t key_index = uniform_index(random, config.objects);
        const GetTiming timing = get_one(*transport, config, key_index);
        const auto index = static_cast<std::size_t>(sample);
        key_indices[index] = key_index;
        ttfb[index] = timing.ttfb_ns;
        body_ready[index] = timing.body_ready_ns;
        total[index] = timing.total_ns;
    }
    transport->close();

    if (!config.csv.empty()) {
        std::ofstream csv(config.csv, std::ios::out | std::ios::trunc);
        if (!csv) throw std::runtime_error("cannot open latency CSV: " + config.csv);
        csv << "sample,key_index,ttfb_ns,body_ready_ns,total_ns,response_transfer_ns\n";
        for (std::size_t sample = 0; sample < sample_count; ++sample) {
            csv << sample << ',' << key_indices[sample] << ',' << ttfb[sample] << ','
                << body_ready[sample] << ',' << total[sample] << ','
                << total[sample] - ttfb[sample] << '\n';
        }
        if (!csv) throw std::runtime_error("failed to write latency CSV: " + config.csv);
    }

    std::cout << "requests=" << config.requests << " object_bytes="
              << config.object_bytes << " objects=" << config.objects << "\n";
    print_latency_field("ttfb", ttfb);
    print_latency_field("body_ready", body_ready);
    print_latency_field("total", total);
}

std::int64_t clock_ns(TimePoint time) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        time.time_since_epoch()).count();
}

TimePoint from_clock_ns(std::int64_t value) {
    return TimePoint(std::chrono::nanoseconds(value));
}

void run_throughput(const Config& config) {
    std::vector<std::thread> workers;
    std::vector<std::exception_ptr> errors(config.threads);
    std::vector<std::uint64_t> request_counts(config.threads);
    std::atomic<unsigned> ready{0};
    std::atomic<std::int64_t> start_at_ns{0};
    workers.reserve(config.threads);

    for (unsigned thread = 0; thread < config.threads; ++thread) {
        workers.emplace_back([&, thread] {
            std::unique_ptr<goblin::client::Transport> transport;
            try {
                transport = connect_transport(config);
            } catch (...) {
                errors[thread] = std::current_exception();
            }
            ready.fetch_add(1, std::memory_order_release);
            std::int64_t published_start = 0;
            while ((published_start = start_at_ns.load(std::memory_order_acquire)) == 0)
                std::this_thread::yield();
            if (!transport) return;

            try {
                const TimePoint start_at = from_clock_ns(published_start);
                while (Clock::now() < start_at) std::this_thread::yield();
                const TimePoint measure_at = start_at +
                    std::chrono::duration_cast<Clock::duration>(
                        std::chrono::duration<double>(config.warmup_seconds));
                const TimePoint stop_at = measure_at +
                    std::chrono::duration_cast<Clock::duration>(
                        std::chrono::duration<double>(config.seconds));
                SplitMix64 random(config.seed +
                                  0x9e3779b97f4a7c15ULL * (thread + 1));
                for (;;) {
                    const TimePoint before = Clock::now();
                    if (before >= stop_at) break;
                    const GetTiming timing = get_one(
                        *transport, config, uniform_index(random, config.objects));
                    if (timing.started >= measure_at && timing.completed <= stop_at)
                        ++request_counts[thread];
                }
                transport->close();
            } catch (...) {
                errors[thread] = std::current_exception();
            }
        });
    }

    while (ready.load(std::memory_order_acquire) != config.threads)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const TimePoint start_at = Clock::now() + std::chrono::milliseconds(100);
    start_at_ns.store(clock_ns(start_at), std::memory_order_release);
    for (auto& worker : workers) worker.join();
    rethrow_worker_errors(errors);

    std::uint64_t requests = 0;
    for (const auto count : request_counts) requests += count;
    const long double payload_bytes =
        static_cast<long double>(requests) * config.object_bytes;
    const long double qps = requests / static_cast<long double>(config.seconds);
    const long double gib_per_second = payload_bytes /
        (1024.0L * 1024.0L * 1024.0L * config.seconds);
    const long double gigabits_per_second = payload_bytes * 8.0L /
        (1'000'000'000.0L * config.seconds);
    const std::uint64_t elapsed_ns = static_cast<std::uint64_t>(
        config.seconds * 1'000'000'000.0);

    std::cout << std::fixed << std::setprecision(3)
              << "threads=" << config.threads << " requests=" << requests
              << " qps=" << qps << " payload_gib_per_s=" << gib_per_second
              << " payload_gbit_per_s=" << gigabits_per_second << "\n";

    if (!config.csv.empty()) {
        std::ofstream csv(config.csv, std::ios::out | std::ios::trunc);
        if (!csv) throw std::runtime_error("cannot open throughput CSV: " + config.csv);
        csv << "address,port,object_bytes,objects,threads,seed,warmup_seconds,"
               "measurement_seconds,requests,payload_bytes,elapsed_ns,qps,"
               "payload_gib_per_second,payload_gigabits_per_second\n";
        csv << config.client.address << ',' << config.client.port << ','
            << config.object_bytes << ',' << config.objects << ',' << config.threads
            << ',' << config.seed << ',' << config.warmup_seconds << ','
            << config.seconds << ',' << requests << ','
            << static_cast<std::uint64_t>(payload_bytes) << ',' << elapsed_ns << ','
            << std::setprecision(9) << static_cast<double>(qps) << ','
            << static_cast<double>(gib_per_second) << ','
            << static_cast<double>(gigabits_per_second) << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Config config = parse_arguments(argc, argv);
        if (!goblin::client::rdma_available())
            throw std::runtime_error("this binary was built without native RDMA support");
        switch (config.mode) {
            case Mode::load: run_load(config); break;
            case Mode::latency: run_latency(config); break;
            case Mode::throughput: run_throughput(config); break;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
