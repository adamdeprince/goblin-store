#include "mini_test.hpp"

#include "goblin/net/numa.hpp"

#include <array>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using namespace goblin;
using namespace goblin::net;

TEST("Linux CPU-list syntax is parsed, sorted, and deduplicated") {
    auto parsed = parse_cpu_list("3,0-2,2,8,10-11\n");
    CHECK(parsed.has_value());
    if (parsed) {
        const std::vector<unsigned> expected{0, 1, 2, 3, 8, 10, 11};
        CHECK_EQ(*parsed, expected);
    }
}

TEST("Malformed Linux CPU lists are rejected") {
    CHECK(!parse_cpu_list("").has_value());
    CHECK(!parse_cpu_list("3-1").has_value());
    CHECK(!parse_cpu_list("1,,2").has_value());
    CHECK(!parse_cpu_list("1,").has_value());
    CHECK(!parse_cpu_list("1-2-3").has_value());
}

TEST("CPU lists are formatted compactly") {
    const std::array<unsigned, 7> cpus{7, 0, 1, 2, 6, 4, 4};
    CHECK_EQ(format_cpu_list(cpus), std::string("0-2,4,6-7"));
}

TEST("Explicit NUMA selection overrides listener NIC locality") {
    const std::array<unsigned, 2> online{0, 1};
    const std::array<NicAddress, 2> nics{{
        {"enp1s0", "192.0.2.10", 0},
        {"enp65s0", "198.51.100.10", 1},
    }};
    auto node = select_numa_node(1, nics, online);
    CHECK(node.has_value());
    if (node) CHECK_EQ(*node, 1u);
}

TEST("An offline explicit NUMA node is rejected with valid commands") {
    const std::array<unsigned, 2> online{0, 2};
    const std::array<NicAddress, 0> nics{};
    auto node = select_numa_node(1, nics, online);
    CHECK(!node.has_value());
    if (!node) {
        CHECK(node.error().detail.find("--numa 0") != std::string::npos);
        CHECK(node.error().detail.find("--numa 2") != std::string::npos);
    }
}

TEST("A single-node machine is unambiguous without NIC locality") {
    const std::array<unsigned, 1> online{0};
    const std::array<NicAddress, 0> nics{};
    auto node = select_numa_node(std::nullopt, nics, online);
    CHECK(node.has_value());
    if (node) CHECK_EQ(*node, 0u);
}

TEST("Automatic NUMA selection accepts multiple addresses on one node") {
    const std::array<unsigned, 2> online{0, 1};
    const std::array<NicAddress, 2> nics{{
        {"enp1s0", "192.0.2.10", 1},
        {"enp1s0.20", "192.0.2.20", 1},
    }};
    auto node = select_numa_node(std::nullopt, nics, online);
    CHECK(node.has_value());
    if (node) CHECK_EQ(*node, 1u);
}

TEST("Automatic NUMA ambiguity identifies every NIC, address, node, and override") {
    const std::array<unsigned, 2> online{0, 1};
    const std::array<NicAddress, 2> nics{{
        {"enp1s0", "192.0.2.10", 0},
        {"enp65s0", "198.51.100.10", 1},
    }};
    auto node = select_numa_node(std::nullopt, nics, online);
    CHECK(!node.has_value());
    if (!node) {
        const std::string& detail = node.error().detail;
        CHECK(detail.find("enp1s0 192.0.2.10 -> NUMA node 0 (use --numa 0)") !=
              std::string::npos);
        CHECK(detail.find("enp65s0 198.51.100.10 -> NUMA node 1 (use --numa 1)") !=
              std::string::npos);
    }
}

TEST("Unknown NIC locality on a multi-node host requires an explicit override") {
    const std::array<unsigned, 2> online{0, 1};
    const std::array<NicAddress, 1> nics{{{"bond0", "203.0.113.10", std::nullopt}}};
    auto node = select_numa_node(std::nullopt, nics, online);
    CHECK(!node.has_value());
    if (!node) {
        CHECK(node.error().detail.find("bond0 203.0.113.10 -> NUMA node unknown") !=
              std::string::npos);
        CHECK(node.error().detail.find("--numa 0, --numa 1") != std::string::npos);
    }
}

TEST("NUMA memory planning puts the local budget first and sub-memory on every other node") {
    const std::array<unsigned, 4> online{0, 1, 2, 3};
    auto plan = plan_numa_memory(/*local_node=*/2, online, 100 * GiB, 20 * GiB);
    CHECK(plan.has_value());
    if (plan) {
        CHECK_EQ(plan->size(), std::size_t(4));
        CHECK_EQ((*plan)[0].node, 2u);
        CHECK_EQ((*plan)[0].bytes, Size(100 * GiB));
        CHECK_EQ((*plan)[1].node, 0u);
        CHECK_EQ((*plan)[2].node, 1u);
        CHECK_EQ((*plan)[3].node, 3u);
        Size total = 0;
        for (const auto& region : *plan) total += region.bytes;
        CHECK_EQ(total, Size(160 * GiB));
    }
}

TEST("Zero sub-memory produces only the local NUMA region") {
    const std::array<unsigned, 4> online{0, 1, 2, 3};
    auto plan = plan_numa_memory(1, online, 8 * GiB, 0);
    CHECK(plan.has_value());
    if (plan) {
        CHECK_EQ(plan->size(), std::size_t(1));
        CHECK_EQ((*plan)[0].node, 1u);
        CHECK_EQ((*plan)[0].bytes, Size(8 * GiB));
    }
}

TEST("Sub-memory rejects a single-node host and aggregate overflow") {
    const std::array<unsigned, 1> one_node{0};
    CHECK(!plan_numa_memory(0, one_node, 1 * GiB, 1 * GiB).has_value());

    const std::array<unsigned, 2> two_nodes{0, 1};
    CHECK(!plan_numa_memory(0, two_nodes, std::numeric_limits<Size>::max(), 1).has_value());
}
