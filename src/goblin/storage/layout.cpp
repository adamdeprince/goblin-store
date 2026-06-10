#include "goblin/storage/layout.hpp"

#include <algorithm>

namespace goblin::storage {

ObjectLayout compute_layout(Size size, const TierSizes& tiers, bool three_layer) noexcept {
    ObjectLayout layout;
    layout.size = size;
    layout.ram_head_bytes = std::min(size, tiers.ram_head);

    if (!three_layer) {
        // 2-layer: the whole object lives on the SSD pool.
        layout.ssd_bytes = size;
        layout.hdd_bytes = 0;
    } else if (size <= tiers.ssd_prefix) {
        // small/medium: fits entirely within the SSD prefix.
        layout.ssd_bytes = size;
        layout.hdd_bytes = 0;
    } else {
        // large: SSD holds the prefix, HDD holds the tail.
        layout.ssd_bytes = tiers.ssd_prefix;
        layout.hdd_bytes = size - tiers.ssd_prefix;
    }
    return layout;
}

Placement place(Offset off, const ObjectLayout& layout) noexcept {
    if (off < layout.ssd_bytes) return {Tier::ssd, off};
    return {Tier::hdd, off - layout.ssd_bytes};
}

} // namespace goblin::storage
