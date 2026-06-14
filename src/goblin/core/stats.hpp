// Server counters (observability). Thread-per-core friendly: each worker owns one Stats slot and is
// the only writer to it, so the hot path is a relaxed atomic increment on a per-core cache line (no
// cross-core contention). The memcache `stats` command sums every slot on demand via the registry —
// exposed on the internal memcache channel only, never the public HTTP one.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

namespace goblin::core {

// A summed, plain-value view of one or more slots.
struct StatsSnapshot {
    std::uint64_t conns = 0;            // connections accepted (lifetime total)
    std::uint64_t curr_conns = 0;       // connections currently open (gauge)
    std::uint64_t get_hits = 0;         // GET served (memcache + HTTP)
    std::uint64_t get_misses = 0;       // GET not found
    std::uint64_t sets = 0;             // objects stored (memcache SET committed)
    std::uint64_t set_rejected = 0;     // SET that ended NOT_STORED (admission / write / commit fail)
    std::uint64_t bytes_served = 0;     // bytes sent to clients
    std::uint64_t bytes_stored = 0;     // body bytes written to the store
    std::uint64_t get_backpressure = 0; // GET park events on read-pool exhaustion
    std::uint64_t set_backpressure = 0; // SET park events on write-staging exhaustion
    std::uint64_t slow_drops = 0;       // connections aborted by the stall sweep

    StatsSnapshot& operator+=(const StatsSnapshot& o) {
        conns += o.conns;
        curr_conns += o.curr_conns;
        get_hits += o.get_hits;
        get_misses += o.get_misses;
        sets += o.sets;
        set_rejected += o.set_rejected;
        bytes_served += o.bytes_served;
        bytes_stored += o.bytes_stored;
        get_backpressure += o.get_backpressure;
        set_backpressure += o.set_backpressure;
        slow_drops += o.slow_drops;
        return *this;
    }
};

// One per worker loop. The owning worker is the only writer (relaxed atomic add on its own cache
// line); aggregate() reads every slot. conns/curr_conns/get_* are bumped by net::StreamLoop;
// sets/set_*/bytes_stored by memcache::EventLoop.
struct Stats {
    std::atomic<std::uint64_t> conns{0};
    std::atomic<std::uint64_t> curr_conns{0};
    std::atomic<std::uint64_t> get_hits{0};
    std::atomic<std::uint64_t> get_misses{0};
    std::atomic<std::uint64_t> sets{0};
    std::atomic<std::uint64_t> set_rejected{0};
    std::atomic<std::uint64_t> bytes_served{0};
    std::atomic<std::uint64_t> bytes_stored{0};
    std::atomic<std::uint64_t> get_backpressure{0};
    std::atomic<std::uint64_t> set_backpressure{0};
    std::atomic<std::uint64_t> slow_drops{0};

    StatsSnapshot snapshot() const {
        constexpr auto rlx = std::memory_order_relaxed;
        StatsSnapshot s;
        s.conns = conns.load(rlx);
        s.curr_conns = curr_conns.load(rlx);
        s.get_hits = get_hits.load(rlx);
        s.get_misses = get_misses.load(rlx);
        s.sets = sets.load(rlx);
        s.set_rejected = set_rejected.load(rlx);
        s.bytes_served = bytes_served.load(rlx);
        s.bytes_stored = bytes_stored.load(rlx);
        s.get_backpressure = get_backpressure.load(rlx);
        s.set_backpressure = set_backpressure.load(rlx);
        s.slow_drops = slow_drops.load(rlx);
        return s;
    }
};

// Owns nothing but the slot list; loops add/remove their own Stats. add/remove/aggregate are off the
// hot path (worker startup/shutdown and the rare `stats` command), so a plain mutex is fine.
class StatsRegistry {
public:
    StatsRegistry() : start_(std::chrono::steady_clock::now()) {}

    void add(Stats* s) {
        std::lock_guard<std::mutex> lk(mu_);
        slots_.push_back(s);
    }
    void remove(Stats* s) {
        std::lock_guard<std::mutex> lk(mu_);
        std::erase(slots_, s);
    }

    StatsSnapshot aggregate() const {
        std::lock_guard<std::mutex> lk(mu_);
        StatsSnapshot t;
        for (const Stats* s : slots_) t += s->snapshot();
        return t;
    }

    std::uint64_t uptime_secs() const {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_)
                .count());
    }

private:
    mutable std::mutex mu_;
    std::vector<Stats*> slots_;
    std::chrono::steady_clock::time_point start_;
};

} // namespace goblin::core
