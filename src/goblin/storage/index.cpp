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

void Index::account_add(const ObjectMeta& meta) noexcept {
    constexpr auto rlx = std::memory_order_relaxed;
    item_count_.fetch_add(1, rlx);
    logical_bytes_.fetch_add(meta.size, rlx);
    if (meta.head.resident()) {
        resident_heads_.fetch_add(1, rlx);
        resident_head_bytes_.fetch_add(meta.head.len, rlx);
    }
}

void Index::account_remove(const ObjectMeta& meta) noexcept {
    constexpr auto rlx = std::memory_order_relaxed;
    item_count_.fetch_sub(1, rlx);
    logical_bytes_.fetch_sub(meta.size, rlx);
    if (meta.head.resident()) {
        resident_heads_.fetch_sub(1, rlx);
        resident_head_bytes_.fetch_sub(meta.head.len, rlx);
    }
}

void Index::account_replace(const ObjectMeta& old_meta, const ObjectMeta& new_meta) noexcept {
    constexpr auto rlx = std::memory_order_relaxed;
    if (new_meta.size >= old_meta.size)
        logical_bytes_.fetch_add(new_meta.size - old_meta.size, rlx);
    else
        logical_bytes_.fetch_sub(old_meta.size - new_meta.size, rlx);
    const bool old_head = old_meta.head.resident();
    const bool new_head = new_meta.head.resident();
    if (old_head != new_head) {
        if (new_head) resident_heads_.fetch_add(1, rlx);
        else resident_heads_.fetch_sub(1, rlx);
    }
    const Size old_bytes = old_head ? old_meta.head.len : 0;
    const Size new_bytes = new_head ? new_meta.head.len : 0;
    if (new_bytes >= old_bytes) resident_head_bytes_.fetch_add(new_bytes - old_bytes, rlx);
    else resident_head_bytes_.fetch_sub(old_bytes - new_bytes, rlx);
}

std::optional<ObjectMeta> Index::lookup(const Digest& d) const {
    const Shard& s = shard_for(d);
    std::shared_lock lk(s.mu);
    const auto it = s.map.find(d);
    if (it == s.map.end()) return std::nullopt;
    return it->second.meta;
}

std::optional<ObjectRecord> Index::lookup_with_http(const Digest& d) const {
    const Shard& s = shard_for(d);
    std::shared_lock lk(s.mu);
    const auto it = s.map.find(d);
    if (it == s.map.end()) return std::nullopt;
    return ObjectRecord{it->second.meta, it->second.http};
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
    if (!inserted) {
        account_replace(it->second.meta, m);
        it->second.meta = m; // replacing a value does not erase the key's accumulated heat
        it->second.http.reset();
    } else account_add(m);
}

void Index::set_with_score(const Digest& d, const ObjectMeta& m,
                           std::optional<double> local_score,
                           std::shared_ptr<const HttpCacheMetadata> http) {
    if (local_score && (std::isnan(*local_score) || *local_score < 0.0)) {
        assert(false && "a numeric index score must be nonnegative and not NaN");
        return;
    }

    Shard& s = shard_for(d);
    std::unique_lock lk(s.mu);
    const auto [it, inserted] = s.map.try_emplace(d, m);
    if (!inserted) {
        account_replace(it->second.meta, m);
        it->second.meta = m;
    } else account_add(m);
    it->second.http = std::move(http);
    it->second.score.store(local_score.value_or(external_score_marker()),
                           std::memory_order_relaxed);
}

bool Index::add(const Digest& d, const ObjectMeta& m) {
    Shard& s = shard_for(d);
    std::unique_lock lk(s.mu);
    const bool inserted = s.map.try_emplace(d, m).second;
    if (inserted) account_add(m);
    return inserted;
}

bool Index::replace(const Digest& d, const ObjectMeta& m) {
    Shard& s = shard_for(d);
    std::unique_lock lk(s.mu);
    const auto it = s.map.find(d);
    if (it == s.map.end()) return false;
    account_replace(it->second.meta, m);
    it->second.meta = m;
    it->second.http.reset();
    return true;
}

bool Index::erase(const Digest& d) {
    Shard& s = shard_for(d);
    std::unique_lock lk(s.mu);
    const auto it = s.map.find(d);
    if (it == s.map.end()) return false;
    account_remove(it->second.meta);
    s.map.erase(it);
    return true;
}

bool Index::set_head(const Digest& d, HeadLoc loc) {
    Shard& s = shard_for(d);
    std::unique_lock lk(s.mu);
    const auto it = s.map.find(d);
    if (it == s.map.end()) return false;
    const ObjectMeta old = it->second.meta;
    it->second.meta.head = loc;
    account_replace(old, it->second.meta);
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

bool Index::mark_fetched(const Digest& d, std::uint32_t now) {
    Shard& s = shard_for(d);
    std::unique_lock lk(s.mu);
    const auto it = s.map.find(d);
    if (it == s.map.end()) return false;
    it->second.meta.fetched = true;
    it->second.meta.last_access = now;
    return true;
}

Index::RecacheClaim Index::claim_recache(const Digest& d) {
    Shard& s = shard_for(d);
    std::unique_lock lk(s.mu);
    const auto it = s.map.find(d);
    if (it == s.map.end()) return RecacheClaim::missing;
    if (it->second.meta.recache_claimed) return RecacheClaim::already_claimed;
    it->second.meta.recache_claimed = true;
    return RecacheClaim::winner;
}

Index::MetaMutation Index::mark_stale(const Digest& d, std::uint64_t cas_expected,
                                      std::uint64_t new_cas,
                                      std::optional<std::uint32_t> expiry) {
    Shard& s = shard_for(d);
    std::unique_lock lk(s.mu);
    const auto it = s.map.find(d);
    if (it == s.map.end()) return MetaMutation::missing;
    if (cas_expected != 0 && it->second.meta.etag != cas_expected)
        return MetaMutation::cas_mismatch;
    it->second.meta.etag = new_cas;
    if (expiry) it->second.meta.expiry = *expiry;
    it->second.meta.stale = true;
    it->second.meta.recache_claimed = false;
    return MetaMutation::stored;
}

bool Index::update_http_if_etag(const Digest& d, std::uint64_t etag,
                                std::shared_ptr<const HttpCacheMetadata> http) {
    Shard& s = shard_for(d);
    std::unique_lock lk(s.mu);
    const auto it = s.map.find(d);
    if (it == s.map.end() || it->second.meta.etag != etag) return false;
    it->second.http = std::move(http);
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

Index::Usage Index::usage() const {
    constexpr auto rlx = std::memory_order_relaxed;
    Usage usage;
    usage.items = item_count_.load(rlx);
    usage.logical_bytes = logical_bytes_.load(rlx);
    usage.head_resident = resident_heads_.load(rlx);
    usage.headless = usage.items - usage.head_resident;
    usage.resident_head_bytes = resident_head_bytes_.load(rlx);
    return usage;
}

std::vector<std::pair<Digest, ObjectMeta>> Index::records() const {
    std::vector<std::pair<Digest, ObjectMeta>> out;
    for (std::size_t i = 0; i < nshards_; ++i) {
        std::shared_lock lk(shards_[i].mu);
        out.reserve(out.size() + shards_[i].map.size());
        for (const auto& [digest, entry] : shards_[i].map)
            out.emplace_back(digest, entry.meta);
    }
    return out;
}

void Index::clear() {
    for (std::size_t i = 0; i < nshards_; ++i) {
        std::unique_lock lk(shards_[i].mu);
        shards_[i].map.clear();
    }
    item_count_.store(0, std::memory_order_relaxed);
    logical_bytes_.store(0, std::memory_order_relaxed);
    resident_heads_.store(0, std::memory_order_relaxed);
    resident_head_bytes_.store(0, std::memory_order_relaxed);
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

std::vector<Digest> Index::keys() const {
    std::vector<Digest> out;
    for (std::size_t i = 0; i < nshards_; ++i) {
        std::shared_lock lk(shards_[i].mu);
        out.reserve(out.size() + shards_[i].map.size());
        for (const auto& [digest, entry] : shards_[i].map) {
            (void)entry;
            out.push_back(digest);
        }
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
