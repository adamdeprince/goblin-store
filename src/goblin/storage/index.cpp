#include "goblin/storage/index.hpp"

#include <mutex> // std::unique_lock

namespace goblin::storage {

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
    return it->second;
}

bool Index::contains(const Digest& d) const {
    const Shard& s = shard_for(d);
    std::shared_lock lk(s.mu);
    return s.map.contains(d);
}

void Index::set(const Digest& d, const ObjectMeta& m) {
    Shard& s = shard_for(d);
    std::unique_lock lk(s.mu);
    s.map.insert_or_assign(d, m);
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
    it->second = m;
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
    it->second.head = loc;
    return true;
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

} // namespace goblin::storage
