// Positional tiering math (ADR-0006 / ADR-0012): how an object's bytes split across RAM / SSD /
// HDD by byte offset, and which disk pool backs a given offset. Pure functions — no I/O.
#pragma once

#include "goblin/common/config.hpp" // TierSizes
#include "goblin/common/types.hpp"

namespace goblin::storage {

enum class Tier { ram, ssd, hdd };

// Byte spans of an object across the tiers.
//   RAM caches  [0, ram_head_bytes)   (also physically resident on SSD — ADR-0003)
//   SSD pool    [0, ssd_bytes)
//   HDD pool    [ssd_bytes, size)     (hdd_bytes == 0 in 2-layer mode)
struct ObjectLayout {
    Size size = 0;
    Size ram_head_bytes = 0;
    Size ssd_bytes = 0;
    Size hdd_bytes = 0;
};

ObjectLayout compute_layout(Size size, const TierSizes& tiers, bool three_layer) noexcept;

// Which disk pool backs global offset `off`, and the offset within that pool's striped region.
struct Placement {
    Tier tier;     // ssd or hdd (RAM is a cache layer, checked separately)
    Offset local;  // offset within that tier's striped data for this object
};
Placement place(Offset off, const ObjectLayout& layout) noexcept;

} // namespace goblin::storage
