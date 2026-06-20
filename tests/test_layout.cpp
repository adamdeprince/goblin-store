#include "mini_test.hpp"

#include "goblin/storage/layout.hpp"

using namespace goblin;
using namespace goblin::storage;

static TierSizes ts() {
    TierSizes t;
    t.ram_head = 256 * KiB;
    t.ssd_prefix = 32 * MiB;
    return t;
}

TEST("layout: 2-layer puts the whole object on SSD") {
    const auto L = compute_layout(100 * MiB, ts(), /*three_layer=*/false);
    CHECK_EQ(L.ssd_bytes, Size(100 * MiB));
    CHECK_EQ(L.hdd_bytes, Size(0));
    CHECK_EQ(L.ram_head_bytes, Size(256 * KiB));
}

TEST("layout: 3-layer large object splits into SSD prefix + HDD tail") {
    const auto L = compute_layout(1 * GiB, ts(), /*three_layer=*/true);
    CHECK_EQ(L.ssd_bytes, Size(32 * MiB));
    CHECK_EQ(L.hdd_bytes, Size(1 * GiB - 32 * MiB));
    CHECK_EQ(L.ram_head_bytes, Size(256 * KiB));
}

TEST("layout: object that fits in the head is RAM-only (no SSD/HDD copy)") {
    const auto L = compute_layout(100 * KiB, ts(), /*three_layer=*/true); // 100 KiB <= ram_head (256 KiB)
    CHECK_EQ(L.ram_head_bytes, Size(100 * KiB)); // min(size, ram_head) -- the head is the whole object
    CHECK_EQ(L.ssd_bytes, Size(0));              // RAM-only: no redundant disk copy (ADR-0003-rev)
    CHECK_EQ(L.hdd_bytes, Size(0));
}

TEST("layout: place() maps offsets to SSD then HDD") {
    const auto L = compute_layout(1 * GiB, ts(), /*three_layer=*/true); // ssd_bytes = 32 MiB
    CHECK(place(0, L).tier == Tier::ssd);
    CHECK(place(32 * MiB - 1, L).tier == Tier::ssd);

    const auto at_split = place(32 * MiB, L);
    CHECK(at_split.tier == Tier::hdd);
    CHECK_EQ(at_split.local, Offset(0));

    const auto past = place(32 * MiB + 4096, L);
    CHECK(past.tier == Tier::hdd);
    CHECK_EQ(past.local, Offset(4096));
}
