#include "goblin/storage/eviction.hpp"

#include "goblin/storage/s3fifo.hpp"

namespace goblin::storage {

std::unique_ptr<EvictionPolicy> make_eviction_policy(EvictionPolicyKind kind,
                                                     std::size_t capacity_hint) {
    if (kind == EvictionPolicyKind::s3fifo) return std::make_unique<S3Fifo>(capacity_hint);
    // TODO: SIEVE and W-TinyLFU — fall back to S3-FIFO until they're implemented behind this seam.
    return std::make_unique<S3Fifo>(capacity_hint);
}

} // namespace goblin::storage
