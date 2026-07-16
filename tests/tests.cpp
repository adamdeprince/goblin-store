#include "mini_test.hpp"

#include "goblin/common/config.hpp"
#include "goblin/common/types.hpp"

#include <limits>

using namespace goblin;

TEST("types: power-of-two & alignment helpers") {
    CHECK(is_power_of_two(1024));
    CHECK(is_power_of_two(kDeviceBlock));
    CHECK(!is_power_of_two(1000));
    CHECK(!is_power_of_two(0));
    CHECK_EQ(align_up(4097, 4096), Size(8192));
    CHECK_EQ(align_down(4097, 4096), Size(4096));
    CHECK(is_aligned(8192, 4096));
    CHECK(!is_aligned(4097, 4096));
}

static ServerConfig good_2layer() {
    ServerConfig c;
    c.ssd.dirs = {"/tmp/goblin-ssd0"};
    return c;
}

TEST("config: defaults validate (2-layer)") {
    auto c = good_2layer();
    CHECK(validate(c).has_value());
    CHECK(!c.three_layer());
    CHECK_EQ(c.memory.block_bytes, kDefaultMemoryBlock);
}

TEST("config: default backing block follows platform hugetlb geometry") {
#if defined(__aarch64__) || defined(__arm__) || defined(__loongarch__)
    CHECK_EQ(kDefaultHugeTlbPage, Size(32 * MiB));
    CHECK_EQ(kDefaultMemoryBlock, Size(32 * MiB));
#else
    CHECK_EQ(kDefaultHugeTlbPage, Size(2 * MiB));
    CHECK_EQ(kDefaultMemoryBlock, Size(2 * MiB));
#endif
}

TEST("config: allocation block is a power-of-two multiple of the HugeTLB page") {
    auto c = good_2layer();
    c.memory.block_bytes = 2 * kDefaultHugeTlbPage;
    CHECK(validate(c).has_value());

    c.memory.block_bytes = kDefaultHugeTlbPage / 2;
    CHECK(!validate(c).has_value());
}

TEST("config: ram_head is a power of two that packs exactly into allocation blocks") {
    auto c = good_2layer();
    c.tiers.ram_head = 384 * KiB;
    CHECK(!validate(c).has_value());

    c.tiers.ram_head = 512 * KiB;
    CHECK(validate(c).has_value());

    c.tiers.ram_head = 2 * c.memory.block_bytes;
    CHECK(!validate(c).has_value());
}

TEST("config: ram_head must be <= ssd_prefix") {
    auto c = good_2layer();
    c.tiers.ram_head = 64 * MiB; // > ssd_prefix (32 MiB)
    CHECK(!validate(c).has_value());
}

TEST("config: empty ssd pool is rejected") {
    ServerConfig c; // no ssd dirs
    CHECK(!validate(c).has_value());
}

TEST("config: shock-absorber law (ssd_middle >= hdd stripe, ADR-0006)") {
    auto c = good_2layer();
    c.hdd.dirs = {"/tmp/goblin-hdd0"}; // now 3-layer
    CHECK(c.three_layer());
    c.hdd.stripe_unit = 64 * MiB; // bigger than ssd_middle (~31.75 MiB) -> reject
    CHECK(!validate(c).has_value());
    c.hdd.stripe_unit = 4 * MiB;  // fits -> ok
    CHECK(validate(c).has_value());
}

TEST("config: memcache-only is valid (HTTP and HTTPS both off)") {
    auto c = good_2layer();
    c.enable_http = false;
    c.enable_https = false; // memcache stays on by default
    CHECK(validate(c).has_value());
}

TEST("config: rejects when no listener at all is enabled") {
    auto c = good_2layer();
    c.enable_memcache = false;
    c.enable_http = false;
    c.enable_https = false;
    CHECK(!validate(c).has_value());
}

TEST("config: native RDMA may be the only listener and validates v3 bulk geometry") {
    auto c = good_2layer();
    c.enable_memcache = false;
    c.enable_http = false;
    c.enable_https = false;
    c.rdma.enabled = true;
    c.rdma.address = "192.0.2.10";
    CHECK(validate(c).has_value());

    c.rdma.bulk_window_count = 1; // endpoint requires a second slot for first-head tail prefetch
    CHECK(!validate(c).has_value());
    c.rdma.bulk_window_count = 4;
    c.rdma.bulk_window_bytes = 192 * KiB; // not a power of two
    CHECK(!validate(c).has_value());
    c.rdma.bulk_window_bytes = 256 * KiB;
    c.rdma.address.clear();
    CHECK(!validate(c).has_value());
}

TEST("config: --sub-memory requires an explicit --numa node") {
    auto c = good_2layer();
    c.memory.sub_bytes = 4 * GiB;
    auto invalid = validate(c);
    CHECK(!invalid.has_value());
    if (!invalid)
        CHECK(invalid.error().detail.find("--sub-memory requires an explicit --numa") !=
              std::string::npos);

    c.numa_node = 0;
    CHECK(validate(c).has_value());
}

TEST("config: --small-memory opts into a separate whole-block pool") {
    auto c = good_2layer();
    CHECK(!c.memory.split_pools());
    CHECK_EQ(c.memory.small_arena_bytes(), Size(0));

    c.memory.small_total_bytes = 256 * MiB;
    CHECK(c.memory.split_pools());
    CHECK_EQ(c.memory.small_arena_bytes(), Size(256 * MiB));
    CHECK(validate(c).has_value());

    c.memory.small_total_bytes = 256 * MiB + 4 * KiB;
    auto invalid = validate(c);
    CHECK(!invalid.has_value());
    if (!invalid)
        CHECK(invalid.error().detail.find("--small-memory") != std::string::npos);
}

TEST("config: --small-sub-memory requires the split pool and explicit --numa") {
    auto c = good_2layer();
    c.memory.small_sub_bytes = 4 * GiB;
    auto no_split = validate(c);
    CHECK(!no_split.has_value());
    if (!no_split)
        CHECK(no_split.error().detail.find("--small-sub-memory requires --small-memory") !=
              std::string::npos);

    c.memory.small_total_bytes = 4 * GiB;
    auto no_node = validate(c);
    CHECK(!no_node.has_value());
    if (!no_node)
        CHECK(no_node.error().detail.find("requires an explicit --numa") != std::string::npos);

    c.numa_node = 0;
    CHECK(validate(c).has_value());
}

TEST("config: --no-numa rejects remote small-object memory") {
    auto c = good_2layer();
    c.numa_enabled = false;
    c.memory.small_total_bytes = 4 * GiB;
    CHECK(validate(c).has_value());

    c.memory.small_sub_bytes = 4 * GiB;
    auto invalid = validate(c);
    CHECK(!invalid.has_value());
    if (!invalid)
        CHECK(invalid.error().detail.find("--small-sub-memory cannot be used with --no-numa") !=
              std::string::npos);
}

TEST("config: --no-numa rejects explicit NUMA placement") {
    auto c = good_2layer();
    c.numa_enabled = false;
    CHECK(validate(c).has_value());

    c.numa_node = 0;
    CHECK(!validate(c).has_value());
    c.numa_node.reset();
    c.memory.sub_bytes = 4 * GiB;
    CHECK(!validate(c).has_value());
}

TEST("config: --perverse requires NUMA mode") {
    auto c = good_2layer();
    c.numa_perverse = true;
    CHECK(validate(c).has_value());

    c.numa_enabled = false;
    auto invalid = validate(c);
    CHECK(!invalid.has_value());
    if (!invalid)
        CHECK(invalid.error().detail.find("--perverse cannot be used with --no-numa") !=
              std::string::npos);
}

TEST("config: local and subordinate NUMA budgets contain whole blocks") {
    auto c = good_2layer();
    c.memory.total_bytes += 4 * KiB;
    CHECK(!validate(c).has_value());

    c.memory.total_bytes = 1 * GiB;
    c.numa_node = 0;
    c.memory.sub_bytes = 4 * KiB;
    CHECK(!validate(c).has_value());
}

TEST("config: access score increment is finite and positive") {
    auto c = good_2layer();
    for (const double invalid : {0.0, -0.25, std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::quiet_NaN()}) {
        c.access_score.increment = invalid;
        CHECK(!validate(c).has_value());
    }
    c.access_score.increment = 0.25;
    CHECK(validate(c).has_value());
}

TEST("config: access score decay is strictly between zero and one") {
    auto c = good_2layer();
    for (const double invalid : {0.0, 1.0, -0.1, 1.1,
                                 std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::quiet_NaN()}) {
        c.access_score.decay = invalid;
        CHECK(!validate(c).has_value());
    }
    c.access_score.decay = 0.5;
    CHECK(validate(c).has_value());
}
