#include "goblin/storage/index.hpp"

#include <cassert>
#include <bit>
#include <cmath>
#include <mutex> // std::unique_lock

namespace goblin::storage {

namespace {

constexpr std::uint64_t kExternalScoreBits = UINT64_C(0x7ff8000000000000);

double external_score_marker() noexcept { return std::bit_cast<double>(kExternalScoreBits); }

} // namespace

Index::Index(unsigned shard_bits)
    : shards_(std::make_unique<Shard[]>(std::size_t{1} << shard_bits)),
      nshards_(std::size_t{1} << shard_bits),
      mask_((std::uint64_t{1} << shard_bits) - 1) {}

Index::Shard& Index::shard_for(const Digest& d) noexcept { return shards_[d.bucket() & mask_]; }
const Index::Shard& Index::shard_for(const Digest& d) const noexcept {
    return shards_[d.bucket() & mask_];
}

std::optional<ObjectMeta> Index::lookup(const Digest& d) const {
    const Shard& s = shard_for(d);
    std::shared_lock lk(s.mu);
    const auto it = s.map.find(d);
    if (it == s.map.end()) return std::nullopt;
    return it->second.meta;
}

bool Index::contains(const Digest& d) const {
    const Shard& s = shard_for(d);
    std::shared_lock lk(s.mu);
    return s.map.contains(d);
}

void Index::set(const Digest& d, const ObjectMeta& m) {
    Shard& s = shard_for(d);
    std::unique_lock lk(s.mu);
    const auto [it, inserted] = s.map.try_emplace(d, m);
    if (!inserted)
        it->second.meta = m; // replacing a value does not erase the key's accumulated heat
}

void Index::set_with_score(const Digest& d, const ObjectMeta& m,
                           std::optional<double> local_score) {
    if (local_score && (std::isnan(*local_score) || *local_score < 0.0)) {
        assert(false && "a numeric index score must be nonnegative and not NaN");
        return;
    }

    Shard& s = shard_for(d);
    std::unique_lock lk(s.mu);
    const auto [it, inserted] = s.map.try_emplace(d, m);
    if (!inserted) it->second.meta = m;
    it->second.score.store(local_score.value_or(external_score_marker()),
                           std::memory_order_relaxed);
}

bool Index::add(const Digest& d, const ObjectMeta& m) {
    Shard& s = shard_for(d);
    std::unique_lock lk(s.mu);
    return s.map.try_emplace(d, m).second;
}

bool Index::replace(const Digest& d, const ObjectMeta& m) {
    Shard& s = shard_for(d);
    std::unique_lock lk(s.mu);
    const auto it = s.map.find(d);
    if (it == s.map.end()) return false;
    it->second.meta = m;
    return true;
}

bool Index::erase(const Digest& d) {
    Shard& s = shard_for(d);
    std::unique_lock lk(s.mu);
    return s.map.erase(d) > 0;
}

bool Index::set_head(const Digest& d, HeadLoc loc) {
    Shard& s = shard_for(d);
    std::unique_lock lk(s.mu);
    const auto it = s.map.find(d);
    if (it == s.map.end()) return false;
    it->second.meta.head = loc;
    return true;
}

bool Index::update_expiry(const Digest& d, std::uint32_t expiry) {
    Shard& s = shard_for(d);
    std::unique_lock lk(s.mu);
    const auto it = s.map.find(d);
    if (it == s.map.end()) return false;
    it->second.meta.expiry = expiry;
    return true;
}

bool Index::increment_score(const Digest& d, double increment) {
    if (!std::isfinite(increment) || increment <= 0.0) return false;
    Shard& s = shard_for(d);
    std::shared_lock lk(s.mu); // keeps the entry alive while the relaxed atomic is updated
    const auto it = s.map.find(d);
    if (it == s.map.end()) return false;
    double current = it->second.score.load(std::memory_order_relaxed);
    while (!std::isnan(current)) {
        if (it->second.score.compare_exchange_weak(current, current + increment,
                                                   std::memory_order_relaxed,
                                                   std::memory_order_relaxed))
            return true;
    }
    return false;
}

std::optional<double> Index::score(const Digest& d) const {
    const Shard& s = shard_for(d);
    std::shared_lock lk(s.mu);
    const auto it = s.map.find(d);
    if (it == s.map.end()) return std::nullopt;
    const double value = it->second.score.load(std::memory_order_relaxed);
    if (std::isnan(value)) return std::nullopt;
    return value;
}

bool Index::score_external(const Digest& d) const {
    const Shard& s = shard_for(d);
    std::shared_lock lk(s.mu);
    const auto it = s.map.find(d);
    return it != s.map.end() &&
           std::isnan(it->second.score.load(std::memory_order_relaxed));
}

std::optional<double> Index::extract_score(const Digest& d) {
    Shard& s = shard_for(d);
    std::shared_lock lk(s.mu);
    const auto it = s.map.find(d);
    if (it == s.map.end()) return std::nullopt;

    double current = it->second.score.load(std::memory_order_relaxed);
    while (!std::isnan(current)) {
        if (it->second.score.compare_exchange_weak(current, external_score_marker(),
                                                   std::memory_order_relaxed,
                                                   std::memory_order_relaxed))
            return current;
    }
    return std::nullopt;
}

bool Index::restore_score(const Digest& d, double value) {
    if (std::isnan(value) || value < 0.0) return false;

    Shard& s = shard_for(d);
    std::shared_lock lk(s.mu);
    const auto it = s.map.find(d);
    if (it == s.map.end()) return false;

    double current = it->second.score.load(std::memory_order_relaxed);
    while (std::isnan(current)) {
        if (it->second.score.compare_exchange_weak(current, value, std::memory_order_relaxed,
                                                   std::memory_order_relaxed))
            return true;
    }
    return false;
}

void Index::decay_scores(double decay) {
    if (!std::isfinite(decay) || decay <= 0.0 || decay >= 1.0) return;
    for (std::size_t i = 0; i < nshards_; ++i) {
        std::shared_lock lk(shards_[i].mu); // erase waits; score updates remain independent
        for (auto& [digest, entry] : shards_[i].map) {
            (void)digest;
            double current = entry.score.load(std::memory_order_relaxed);
            while (!std::isnan(current) &&
                   !entry.score.compare_exchange_weak(current, current * decay,
                                                       std::memory_order_relaxed,
                                                       std::memory_order_relaxed)) {
            }
        }
    }
}

std::size_t Index::size() const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < nshards_; ++i) {
        std::shared_lock lk(shards_[i].mu);
        n += shards_[i].map.size();
    }
    return n;
}

void Index::clear() {
    for (std::size_t i = 0; i < nshards_; ++i) {
        std::unique_lock lk(shards_[i].mu);
        shards_[i].map.clear();
    }
}

std::vector<Digest> Index::expired_keys(std::uint32_t now) const {
    std::vector<Digest> out;
    for (std::size_t i = 0; i < nshards_; ++i) {
        std::shared_lock lk(shards_[i].mu);
        for (const auto& [d, entry] : shards_[i].map)
            if (is_expired(entry.meta, now)) out.push_back(d);
    }
    return out;
}

std::vector<std::pair<Digest, HeadLoc>> Index::resident_heads() const {
    std::vector<std::pair<Digest, HeadLoc>> out;
    for (std::size_t i = 0; i < nshards_; ++i) {
        std::shared_lock lk(shards_[i].mu);
        for (const auto& [d, entry] : shards_[i].map)
            if (entry.meta.head.resident()) out.push_back({d, entry.meta.head});
    }
    return out;
}

std::size_t Index::swap_head_blocks(unsigned first, unsigned second) {
    if (first == second) return 0;
    std::vector<std::unique_lock<std::shared_mutex>> locks;
    locks.reserve(nshards_);
    for (std::size_t i = 0; i < nshards_; ++i) locks.emplace_back(shards_[i].mu);

    std::size_t changed = 0;
    for (std::size_t i = 0; i < nshards_; ++i) {
        for (auto& [digest, entry] : shards_[i].map) {
            (void)digest;
            if (entry.meta.head.block == first) {
                entry.meta.head.block = second;
                ++changed;
            } else if (entry.meta.head.block == second) {
                entry.meta.head.block = first;
                ++changed;
            }
        }
    }
    return changed;
}

} // namespace goblin::storage
