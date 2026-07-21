// Per-worker protocol counters and process-wide overload telemetry. Hot-path writers touch only
// their worker-local cache lines; the uncommon stats/admin path aggregates snapshots.
#pragma once

#include "goblin/common/config.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <vector>

namespace goblin::core {

enum class StatsDomain : std::uint8_t { memcache_tcp, memcache_rdma, http, https, count };
enum class StatsSelection : std::uint8_t { all, memcache };
enum class CommandKind : std::uint8_t {
    set, add, replace, append, prepend, cas, delete_, touch, incr, decr, flush, meta, count
};
enum class CommandResult : std::uint8_t { success, miss, condition, error };

inline constexpr std::size_t kStatsDomains = static_cast<std::size_t>(StatsDomain::count);
inline constexpr std::size_t kCommandKinds = static_cast<std::size_t>(CommandKind::count);
inline constexpr std::array<std::uint64_t, 12> kDurationUpperNs = {
    1'000, 4'000, 16'000, 64'000, 256'000, 1'000'000,
    4'000'000, 16'000'000, 64'000'000, 256'000'000, 1'000'000'000,
    std::numeric_limits<std::uint64_t>::max(),
};

inline constexpr bool selected(StatsDomain domain, StatsSelection selection) noexcept {
    return selection == StatsSelection::all || domain == StatsDomain::memcache_tcp ||
           domain == StatsDomain::memcache_rdma;
}

struct DurationSnapshot {
    std::uint64_t count = 0;
    std::uint64_t total_ns = 0;
    std::uint64_t max_ns = 0;
    std::array<std::uint64_t, kDurationUpperNs.size()> buckets{};

    DurationSnapshot& operator+=(const DurationSnapshot& o) noexcept {
        count += o.count;
        total_ns += o.total_ns;
        max_ns = std::max(max_ns, o.max_ns);
        for (std::size_t i = 0; i < buckets.size(); ++i) buckets[i] += o.buckets[i];
        return *this;
    }
};

struct DurationMetric {
    std::atomic<std::uint64_t> count{0};
    std::atomic<std::uint64_t> total_ns{0};
    std::atomic<std::uint64_t> max_ns{0};
    std::array<std::atomic<std::uint64_t>, kDurationUpperNs.size()> buckets{};

    void observe(std::uint64_t ns) noexcept {
        constexpr auto rlx = std::memory_order_relaxed;
        count.fetch_add(1, rlx);
        total_ns.fetch_add(ns, rlx);
        auto old = max_ns.load(rlx);
        while (old < ns && !max_ns.compare_exchange_weak(old, ns, rlx)) {}
        const auto it = std::lower_bound(kDurationUpperNs.begin(), kDurationUpperNs.end(), ns);
        buckets[static_cast<std::size_t>(it - kDurationUpperNs.begin())].fetch_add(1, rlx);
    }

    DurationSnapshot snapshot() const noexcept {
        constexpr auto rlx = std::memory_order_relaxed;
        DurationSnapshot s{count.load(rlx), total_ns.load(rlx), max_ns.load(rlx), {}};
        for (std::size_t i = 0; i < s.buckets.size(); ++i) s.buckets[i] = buckets[i].load(rlx);
        return s;
    }

    void reset() noexcept {
        constexpr auto rlx = std::memory_order_relaxed;
        count.store(0, rlx);
        total_ns.store(0, rlx);
        max_ns.store(0, rlx);
        for (auto& bucket : buckets) bucket.store(0, rlx);
    }
};

struct StatsSnapshot {
    std::uint64_t conns = 0;
    std::uint64_t curr_conns = 0;
    std::uint64_t process_curr_conns = 0; // every protocol, for the shared listener budget
    std::uint64_t get_hits = 0;
    std::uint64_t get_misses = 0;
    std::uint64_t sets = 0;
    std::uint64_t set_rejected = 0;
    std::uint64_t bytes_served = 0;
    std::uint64_t bytes_received = 0;
    std::uint64_t bytes_stored = 0;
    std::uint64_t get_backpressure = 0;
    std::uint64_t set_backpressure = 0;
    std::uint64_t slow_drops = 0;
    std::uint64_t idle_drops = 0;
    std::uint64_t queue_drops = 0;
    std::uint64_t uring_sqe_shortage = 0;
    std::uint64_t uring_cqes = 0;
    std::uint64_t auth_cmds = 0;
    std::uint64_t auth_errors = 0;
    std::array<std::uint64_t, kCommandKinds> command_success{};
    std::array<std::uint64_t, kCommandKinds> command_miss{};
    std::array<std::uint64_t, kCommandKinds> command_condition{};
    std::array<std::uint64_t, kCommandKinds> command_error{};
    DurationSnapshot ttfb;
    DurationSnapshot get_queue_wait;
    DurationSnapshot set_queue_wait;
    DurationSnapshot backpressure;
    std::uint64_t rejected_connections = 0;
    std::uint64_t rejected_connection_limit = 0;
    std::uint64_t rejected_dispatch_queue = 0;
    std::uint64_t rejected_resource = 0;
    std::uint64_t listen_disabled_num = 0;
    std::uint64_t connection_limit = 0;

    StatsSnapshot& operator+=(const StatsSnapshot& o) noexcept {
        conns += o.conns; curr_conns += o.curr_conns;
        process_curr_conns += o.process_curr_conns;
        get_hits += o.get_hits; get_misses += o.get_misses;
        sets += o.sets; set_rejected += o.set_rejected;
        bytes_served += o.bytes_served; bytes_received += o.bytes_received;
        bytes_stored += o.bytes_stored;
        get_backpressure += o.get_backpressure; set_backpressure += o.set_backpressure;
        slow_drops += o.slow_drops; idle_drops += o.idle_drops; queue_drops += o.queue_drops;
        uring_sqe_shortage += o.uring_sqe_shortage; uring_cqes += o.uring_cqes;
        auth_cmds += o.auth_cmds; auth_errors += o.auth_errors;
        for (std::size_t i = 0; i < kCommandKinds; ++i) {
            command_success[i] += o.command_success[i];
            command_miss[i] += o.command_miss[i];
            command_condition[i] += o.command_condition[i];
            command_error[i] += o.command_error[i];
        }
        ttfb += o.ttfb; get_queue_wait += o.get_queue_wait;
        set_queue_wait += o.set_queue_wait; backpressure += o.backpressure;
        rejected_connections += o.rejected_connections;
        rejected_connection_limit += o.rejected_connection_limit;
        rejected_dispatch_queue += o.rejected_dispatch_queue;
        rejected_resource += o.rejected_resource;
        listen_disabled_num += o.listen_disabled_num;
        connection_limit = std::max(connection_limit, o.connection_limit);
        return *this;
    }
};

struct Stats {
    explicit Stats(StatsDomain value = StatsDomain::memcache_tcp) : domain(value) {}
    StatsDomain domain;
    std::atomic<std::uint64_t> conns{0}, curr_conns{0}, get_hits{0}, get_misses{0};
    std::atomic<std::uint64_t> sets{0}, set_rejected{0}, bytes_served{0}, bytes_received{0},
        bytes_stored{0};
    std::atomic<std::uint64_t> get_backpressure{0}, set_backpressure{0};
    std::atomic<std::uint64_t> slow_drops{0}, idle_drops{0}, queue_drops{0};
    std::atomic<std::uint64_t> uring_sqe_shortage{0}, uring_cqes{0};
    std::atomic<std::uint64_t> auth_cmds{0}, auth_errors{0};
    std::array<std::atomic<std::uint64_t>, kCommandKinds> command_success{};
    std::array<std::atomic<std::uint64_t>, kCommandKinds> command_miss{};
    std::array<std::atomic<std::uint64_t>, kCommandKinds> command_condition{};
    std::array<std::atomic<std::uint64_t>, kCommandKinds> command_error{};
    DurationMetric ttfb, get_queue_wait, set_queue_wait, backpressure;

    void note_command(CommandKind kind, CommandResult result) noexcept {
        const auto i = static_cast<std::size_t>(kind);
        auto* counters = &command_success;
        if (result == CommandResult::miss) counters = &command_miss;
        else if (result == CommandResult::condition) counters = &command_condition;
        else if (result == CommandResult::error) counters = &command_error;
        (*counters)[i].fetch_add(1, std::memory_order_relaxed);
    }

    StatsSnapshot snapshot() const noexcept {
        constexpr auto rlx = std::memory_order_relaxed;
        StatsSnapshot s;
#define GOBLIN_STATS_LOAD(name) s.name = name.load(rlx)
        GOBLIN_STATS_LOAD(conns); GOBLIN_STATS_LOAD(curr_conns);
        GOBLIN_STATS_LOAD(get_hits); GOBLIN_STATS_LOAD(get_misses);
        GOBLIN_STATS_LOAD(sets); GOBLIN_STATS_LOAD(set_rejected);
        GOBLIN_STATS_LOAD(bytes_served); GOBLIN_STATS_LOAD(bytes_received);
        GOBLIN_STATS_LOAD(bytes_stored);
        GOBLIN_STATS_LOAD(get_backpressure); GOBLIN_STATS_LOAD(set_backpressure);
        GOBLIN_STATS_LOAD(slow_drops); GOBLIN_STATS_LOAD(idle_drops); GOBLIN_STATS_LOAD(queue_drops);
        GOBLIN_STATS_LOAD(uring_sqe_shortage); GOBLIN_STATS_LOAD(uring_cqes);
        GOBLIN_STATS_LOAD(auth_cmds); GOBLIN_STATS_LOAD(auth_errors);
#undef GOBLIN_STATS_LOAD
        for (std::size_t i = 0; i < kCommandKinds; ++i) {
            s.command_success[i] = command_success[i].load(rlx);
            s.command_miss[i] = command_miss[i].load(rlx);
            s.command_condition[i] = command_condition[i].load(rlx);
            s.command_error[i] = command_error[i].load(rlx);
        }
        s.ttfb = ttfb.snapshot(); s.get_queue_wait = get_queue_wait.snapshot();
        s.set_queue_wait = set_queue_wait.snapshot(); s.backpressure = backpressure.snapshot();
        return s;
    }

    void reset() noexcept {
        constexpr auto rlx = std::memory_order_relaxed;
        for (auto* value : {&conns, &get_hits, &get_misses, &sets, &set_rejected, &bytes_served,
                            &bytes_received, &bytes_stored, &get_backpressure, &set_backpressure, &slow_drops,
                            &idle_drops, &queue_drops, &uring_sqe_shortage, &uring_cqes})
            value->store(0, rlx);
        auth_cmds.store(0, rlx); auth_errors.store(0, rlx);
        for (std::size_t i = 0; i < kCommandKinds; ++i) {
            command_success[i].store(0, rlx); command_miss[i].store(0, rlx);
            command_condition[i].store(0, rlx); command_error[i].store(0, rlx);
        }
        ttfb.reset(); get_queue_wait.reset(); set_queue_wait.reset(); backpressure.reset();
    }
};

struct StatsSettings {
    ServerConfig config{};
    bool configured = false;
};

enum class ConnectionRejectReason : std::uint8_t { limit, dispatch_queue, resource };

class StatsRegistry {
public:
    StatsRegistry() : start_(std::chrono::steady_clock::now()) {}
    void add(Stats* s) { std::lock_guard lk(mu_); slots_.push_back(s); }
    void remove(Stats* s) { std::lock_guard lk(mu_); std::erase(slots_, s); }

    StatsSnapshot aggregate(StatsSelection selection = StatsSelection::all) const {
        std::lock_guard lk(mu_);
        StatsSnapshot total;
        for (const Stats* s : slots_) {
            auto snapshot = s->snapshot();
            total.process_curr_conns += snapshot.curr_conns;
            snapshot.process_curr_conns = 0;
            if (selected(s->domain, selection)) total += snapshot;
        }
        for (std::size_t i = 0; i < kStatsDomains; ++i) {
            const auto domain = static_cast<StatsDomain>(i);
            if (!selected(domain, selection)) continue;
            total.rejected_connections += rejected_[i].load(std::memory_order_relaxed);
            total.rejected_connection_limit += rejected_limit_[i].load(std::memory_order_relaxed);
            total.rejected_dispatch_queue += rejected_dispatch_[i].load(std::memory_order_relaxed);
            total.rejected_resource += rejected_resource_[i].load(std::memory_order_relaxed);
            total.listen_disabled_num += listen_disabled_[i].load(std::memory_order_relaxed);
        }
        if (settings_.configured) total.connection_limit = settings_.config.max_connections;
        return total;
    }

    void reset() {
        std::lock_guard lk(mu_);
        for (Stats* s : slots_) s->reset();
        for (std::size_t i = 0; i < kStatsDomains; ++i) {
            rejected_[i].store(0, std::memory_order_relaxed);
            rejected_limit_[i].store(0, std::memory_order_relaxed);
            rejected_dispatch_[i].store(0, std::memory_order_relaxed);
            rejected_resource_[i].store(0, std::memory_order_relaxed);
            listen_disabled_[i].store(0, std::memory_order_relaxed);
        }
    }

    void note_connection_rejected(
        StatsDomain domain = StatsDomain::memcache_tcp,
        ConnectionRejectReason reason = ConnectionRejectReason::limit) noexcept {
        const auto i = static_cast<std::size_t>(domain);
        rejected_[i].fetch_add(1, std::memory_order_relaxed);
        auto* counter = &rejected_limit_[i];
        if (reason == ConnectionRejectReason::dispatch_queue) counter = &rejected_dispatch_[i];
        else if (reason == ConnectionRejectReason::resource) counter = &rejected_resource_[i];
        counter->fetch_add(1, std::memory_order_relaxed);
    }
    void note_listener_disabled(StatsDomain domain = StatsDomain::memcache_tcp) noexcept {
        listen_disabled_[static_cast<std::size_t>(domain)].fetch_add(1,
                                                                    std::memory_order_relaxed);
    }
    void set_settings(const ServerConfig& config) { std::lock_guard lk(mu_); settings_ = {config, true}; }
    StatsSettings settings() const { std::lock_guard lk(mu_); return settings_; }
    std::uint64_t uptime_secs() const {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_).count());
    }

private:
    mutable std::mutex mu_;
    std::vector<Stats*> slots_;
    std::chrono::steady_clock::time_point start_;
    StatsSettings settings_;
    std::array<std::atomic<std::uint64_t>, kStatsDomains> rejected_{};
    std::array<std::atomic<std::uint64_t>, kStatsDomains> rejected_limit_{};
    std::array<std::atomic<std::uint64_t>, kStatsDomains> rejected_dispatch_{};
    std::array<std::atomic<std::uint64_t>, kStatsDomains> rejected_resource_{};
    std::array<std::atomic<std::uint64_t>, kStatsDomains> listen_disabled_{};
};

} // namespace goblin::core
