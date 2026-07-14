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
    CHECK_EQ(kDefaultMemoryBlock, Size(32 * MiB));
#else
    CHECK_EQ(kDefaultMemoryBlock, Size(2 * MiB));
#endif
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
