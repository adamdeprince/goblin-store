// Linux NUMA placement for the thread-per-core runtime. With an explicit --numa NODE we validate
// and bind to that node. Without it, the wildcard IPv4 listeners inherit the NUMA node of every
// UP, non-loopback Ethernet address; spanning more than one node is an operator-visible error.
#pragma once

#include "goblin/common/error.hpp"
#include "goblin/common/types.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace goblin::net {

struct NicAddress {
    std::string name;                 // Linux interface name (for example, enp65s0f0)
    std::string address;              // numeric IPv4 address reached by an INADDR_ANY listener
    std::optional<unsigned> numa_node; // /sys/class/net/<name>/device/numa_node; null = unknown
};

struct NumaBinding {
    unsigned node = 0;                    // serving CPU/default-allocation node
    unsigned preferred_memory_node = 0;   // region-zero head arena; normally == node
    std::optional<unsigned> preferred_memory_distance; // populated by --perverse
    std::vector<unsigned> cpus;       // node CPUs intersected with this process's allowed CPU mask
    std::vector<NicAddress> interfaces; // populated for automatic NIC-derived selection
    std::vector<unsigned> online_nodes; // physical node IDs reported by Linux sysfs
    bool automatic = false;
};

struct NumaMemoryBudget {
    unsigned node = 0;
    Size bytes = 0;
};

// Parse Linux list syntax such as "0-3,8,10-11" (used by node*/cpulist and node/online).
Result<std::vector<unsigned>> parse_cpu_list(std::string_view text);

// Pure selection policy, separated from sysfs/getifaddrs so ambiguity behavior is unit-testable.
Result<unsigned> select_numa_node(std::optional<unsigned> requested,
                                  std::span<const NicAddress> interfaces,
                                  std::span<const unsigned> online_nodes);

// Select the most distant online node from serving_node using Linux's dense node-distance row.
// The serving node itself is excluded; equal distances choose the lowest node ID deterministically.
Result<unsigned> select_farthest_numa_node(unsigned serving_node,
                                           std::span<const unsigned> online_nodes,
                                           std::span<const unsigned> distance_by_node);

// Discover/select a node, bind the calling thread to its effective CPUs, and install a strict local
// default memory policy. Threads subsequently created by it inherit both policies, keeping index,
// protocol, I/O, and maintenance allocations on the selected node. Explicit head ranges may still
// override the default with mbind() for --sub-memory.
Result<NumaBinding> configure_numa(std::optional<unsigned> requested, bool perverse = false);

// Build a local-first memory layout: --memory on local_node, followed by --sub-memory on every
// other online node. A nonzero foreign_bytes budget needs at least one foreign node.
Result<std::vector<NumaMemoryBudget>> plan_numa_memory(unsigned local_node,
                                                       std::span<const unsigned> online_nodes,
                                                       Size local_bytes, Size foreign_bytes);

std::string format_cpu_list(std::span<const unsigned> cpus);

} // namespace goblin::net
