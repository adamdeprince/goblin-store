// Pluggable cache-eviction policy over object digests (ADR-0007). Lock-light: touch() sets a bit,
// no list surgery. The caller drives eviction — evict() returns the next victim to free. Selectable
// at runtime among SIEVE / S3-FIFO / W-TinyLFU via the factory.
#pragma once

#include "goblin/common/config.hpp" // EvictionPolicyKind
#include "goblin/storage/index.hpp" // Digest

#include <cstddef>
#include <memory>
#include <optional>

namespace goblin::storage {

class EvictionPolicy {
public:
    virtual ~EvictionPolicy() = default;

    virtual void insert(const Digest&) = 0;        // newly admitted item
    virtual void touch(const Digest&) = 0;         // a hit (lazy promotion)
    virtual void remove(const Digest&) = 0;        // dropped externally (DELETE)
    virtual std::optional<Digest> evict() = 0;     // next victim (removed), or nullopt if empty
    virtual std::size_t resident() const = 0;      // items currently tracked
};

// `capacity_hint` ≈ the max number of resident items (used to size the policy's internal queues).
std::unique_ptr<EvictionPolicy> make_eviction_policy(EvictionPolicyKind kind,
                                                     std::size_t capacity_hint);

} // namespace goblin::storage
