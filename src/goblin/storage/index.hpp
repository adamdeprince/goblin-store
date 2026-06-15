// In-RAM object index (ADR-0001 / ADR-0014): a sharded, read-mostly map from the 256-bit key
// Digest to per-object metadata. RAM-only and never persisted (ADR-0013) — rebuilt empty on
// start. Few, large objects => low contention, so a sharded shared_mutex is plenty for v1
// (RCU/epoch reclamation later if a profile says so).
#pragma once

#include "goblin/common/types.hpp"
#include "goblin/crypto/sha256.hpp" // Digest

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace goblin::storage {

using crypto::Digest;

// Locator for an object's RAM-resident head inside the BlockPool (ADR-0008 / ADR-0014).
struct HeadLoc {
    static constexpr unsigned kNone = 0xFFFFFFFFu;
    unsigned block = kNone;     // BlockPool block index, or kNone if the head isn't RAM-resident
    std::uint32_t offset = 0;   // buddy offset within that block
    std::uint32_t len = 0;      // head bytes resident
    bool resident() const noexcept { return block != kNone; }
};

// Per-object metadata kept in RAM. Small and trivially copyable (lookup returns a copy).
struct ObjectMeta {
    Size size = 0;            // total object size in bytes
    std::uint32_t flags = 0;  // opaque memcache flags
    std::uint32_t expiry = 0; // unix seconds; 0 = never (TTL swept separately, ADR-0007)
    std::uint64_t etag = 0;   // store generation -> HTTP ETag validator; changes on every (re)store
    HeadLoc head;             // RAM-head cache locator
    // eviction bookkeeping (SIEVE visited bit, etc.) lands with the eviction module (ADR-0012)
};

struct DigestHash {
    std::size_t operator()(const Digest& d) const noexcept {
        return static_cast<std::size_t>(d.bucket()); // low 64 bits; map buckets on top (ADR-0014)
    }
};

// memcache TTL is stored as an absolute Unix expiry (0 = never). uint32 seconds suffices to 2106.
inline std::uint32_t now_unix() {
    return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}
inline bool is_expired(const ObjectMeta& m, std::uint32_t now) {
    return m.expiry != 0 && m.expiry <= now;
}

class Index {
public:
    explicit Index(unsigned shard_bits = 8); // 2^shard_bits shards

    std::optional<ObjectMeta> lookup(const Digest& d) const;
    bool contains(const Digest& d) const;

    void set(const Digest& d, const ObjectMeta& m);     // memcache SET (unconditional)
    bool add(const Digest& d, const ObjectMeta& m);     // memcache ADD (only if absent)
    bool replace(const Digest& d, const ObjectMeta& m); // memcache REPLACE (only if present)
    bool erase(const Digest& d);                        // memcache DELETE

    bool set_head(const Digest& d, HeadLoc loc);        // update head residency; false if absent
    bool update_expiry(const Digest& d, std::uint32_t expiry); // overwrite the TTL (meta T); false if absent

    std::size_t size() const; // live object count across shards
    void clear();             // blank slate (startup, ADR-0013)

    // Digests of objects whose absolute expiry has passed `now` (TTL reaper, lazy-skip is elsewhere).
    std::vector<Digest> expired_keys(std::uint32_t now) const;

private:
    struct Shard {
        mutable std::shared_mutex mu;
        std::unordered_map<Digest, ObjectMeta, DigestHash> map;
    };
    Shard& shard_for(const Digest& d) noexcept;
    const Shard& shard_for(const Digest& d) const noexcept;

    std::unique_ptr<Shard[]> shards_;
    std::size_t nshards_;
    std::uint64_t mask_;
};

} // namespace goblin::storage
