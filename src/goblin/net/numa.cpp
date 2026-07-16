#include "goblin/net/numa.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <string>

#if defined(__linux__)
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sched.h>
#include <linux/mempolicy.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace goblin::net {
namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::optional<unsigned> parse_unsigned(std::string_view s) {
    s = trim(s);
    if (s.empty()) return std::nullopt;
    unsigned value = 0;
    const char* const end = s.data() + s.size();
    const auto [p, ec] = std::from_chars(s.data(), end, value);
    if (ec != std::errc{} || p != end) return std::nullopt;
    return value;
}

std::string available_hint(std::span<const unsigned> online) {
    std::string out;
    for (const unsigned node : online) {
        if (!out.empty()) out += ", ";
        out += "--numa " + std::to_string(node);
    }
    return out;
}

std::string ambiguity_detail(std::span<const NicAddress> interfaces,
                             std::span<const unsigned> online) {
    std::string out =
        "automatic NUMA selection is ambiguous: the configured listeners cover network addresses "
        "on different or unknown NUMA nodes:\n";
    if (interfaces.empty()) {
        out += "  (no UP, non-loopback listener interface was found)\n";
    } else {
        for (const auto& nic : interfaces) {
            out += "  " + nic.name + " " + nic.address + " -> ";
            if (nic.numa_node) {
                out += "NUMA node " + std::to_string(*nic.numa_node) +
                       " (use --numa " + std::to_string(*nic.numa_node) + ")";
            } else {
                out += "NUMA node unknown (set --numa explicitly)";
            }
            out += "\n";
        }
    }
    out += "choose one NUMA node explicitly: " + available_hint(online);
    return out;
}

#if defined(__linux__)

Result<std::string> read_line(const std::string& path) {
    std::ifstream in(path);
    if (!in) return err(Errc::io_error, "open " + path + ": " + std::strerror(errno));
    std::string line;
    if (!std::getline(in, line)) return err(Errc::io_error, "read " + path);
    return line;
}

Result<std::vector<unsigned>> online_numa_nodes() {
    auto line = read_line("/sys/devices/system/node/online");
    if (!line) return std::unexpected(line.error());
    return parse_cpu_list(*line);
}

Result<std::vector<unsigned>> node_distances(unsigned node) {
    const std::string path =
        "/sys/devices/system/node/node" + std::to_string(node) + "/distance";
    auto line = read_line(path);
    if (!line) return std::unexpected(line.error());

    std::vector<unsigned> distances;
    std::string_view remaining = trim(*line);
    while (!remaining.empty()) {
        std::size_t end = 0;
        while (end < remaining.size() &&
               !std::isspace(static_cast<unsigned char>(remaining[end])))
            ++end;
        const auto distance = parse_unsigned(remaining.substr(0, end));
        if (!distance)
            return err(Errc::invalid_argument,
                       "invalid NUMA distance row in " + path + ": " + *line);
        distances.push_back(*distance);
        remaining = trim(remaining.substr(end));
    }
    if (distances.empty())
        return err(Errc::invalid_argument, "empty NUMA distance row in " + path);
    return distances;
}

Result<std::vector<unsigned>> read_node_cpus(unsigned node) {
    const std::string path =
        "/sys/devices/system/node/node" + std::to_string(node) + "/cpulist";
    auto line = read_line(path);
    if (!line) return std::unexpected(line.error());
    auto cpus = parse_cpu_list(*line);
    if (!cpus) return std::unexpected(cpus.error());
    if (cpus->empty()) return err(Errc::invalid_argument, "NUMA node " + std::to_string(node) + " has no CPUs");
    return cpus;
}

bool is_ethernet(const std::string& name) {
    std::ifstream in("/sys/class/net/" + name + "/type");
    int type = 0;
    return (in >> type) && type == 1; // ARPHRD_ETHER (physical, bond, VLAN, and Ethernet-like links)
}

std::optional<unsigned> interface_numa_node(const std::string& name) {
    std::ifstream in("/sys/class/net/" + name + "/device/numa_node");
    long node = -1;
    if (!(in >> node) || node < 0 ||
        static_cast<unsigned long>(node) > std::numeric_limits<unsigned>::max())
        return std::nullopt;
    return static_cast<unsigned>(node);
}

bool matches_numeric_address(std::string_view requested, const sockaddr* actual,
                             std::string_view interface_name) {
    if (!actual) return false;
    if (actual->sa_family == AF_INET) {
        in_addr parsed{};
        if (::inet_pton(AF_INET, std::string(requested).c_str(), &parsed) != 1) return false;
        return std::memcmp(&parsed,
                           &reinterpret_cast<const sockaddr_in*>(actual)->sin_addr,
                           sizeof(parsed)) == 0;
    }
    if (actual->sa_family != AF_INET6) return false;
    const std::size_t percent = requested.find('%');
    const std::string_view address = requested.substr(0, percent);
    const std::string_view zone = percent == std::string_view::npos
                                      ? std::string_view{}
                                      : requested.substr(percent + 1);
    in6_addr parsed{};
    if (::inet_pton(AF_INET6, std::string(address).c_str(), &parsed) != 1) return false;
    if (!zone.empty()) {
        const auto actual_scope = reinterpret_cast<const sockaddr_in6*>(actual)->sin6_scope_id;
        unsigned numeric_scope = 0;
        const auto [end, ec] = std::from_chars(zone.data(), zone.data() + zone.size(),
                                               numeric_scope);
        if (ec == std::errc{} && end == zone.data() + zone.size()) {
            if (numeric_scope != actual_scope) return false;
        } else {
            const unsigned index = ::if_nametoindex(std::string(zone).c_str());
            if (index != 0) {
                if (index != actual_scope) return false;
            } else if (zone != interface_name) {
                return false;
            }
        }
    }
    return std::memcmp(&parsed,
                       &reinterpret_cast<const sockaddr_in6*>(actual)->sin6_addr,
                       sizeof(parsed)) == 0;
}

Result<std::vector<NicAddress>> discover_listener_interfaces(
    std::span<const std::string> exact_addresses, bool wildcard_ethernet) {
    ifaddrs* head = nullptr;
    if (::getifaddrs(&head) != 0)
        return err(Errc::io_error, std::string("getifaddrs: ") + std::strerror(errno));

    std::vector<NicAddress> out;
    std::vector<bool> matched(exact_addresses.size(), false);
    for (const ifaddrs* p = head; p; p = p->ifa_next) {
        if (!p->ifa_addr || !p->ifa_name ||
            (p->ifa_addr->sa_family != AF_INET && p->ifa_addr->sa_family != AF_INET6))
            continue;
        if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
        const std::string name(p->ifa_name);
        bool selected = wildcard_ethernet && p->ifa_addr->sa_family == AF_INET &&
                        is_ethernet(name);
        for (std::size_t i = 0; i < exact_addresses.size(); ++i) {
            if (!matches_numeric_address(exact_addresses[i], p->ifa_addr, name)) continue;
            matched[i] = true;
            selected = true; // includes native InfiniBand (ARPHRD_INFINIBAND) netdevs
        }
        if (!selected) continue;

        char address[INET6_ADDRSTRLEN]{};
        const void* source = p->ifa_addr->sa_family == AF_INET
            ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in*>(p->ifa_addr)->sin_addr)
            : static_cast<const void*>(&reinterpret_cast<const sockaddr_in6*>(p->ifa_addr)->sin6_addr);
        if (!::inet_ntop(p->ifa_addr->sa_family, source, address, sizeof address)) continue;
        out.push_back(NicAddress{name, address, interface_numa_node(name)});
    }
    ::freeifaddrs(head);

    for (std::size_t i = 0; i < matched.size(); ++i)
        if (!matched[i])
            return err(Errc::invalid_argument,
                       "listener address " + exact_addresses[i] +
                           " is not assigned to an UP, non-loopback Linux interface; "
                           "choose its node explicitly with --numa NODE");

    std::sort(out.begin(), out.end(), [](const NicAddress& a, const NicAddress& b) {
        return a.name < b.name || (a.name == b.name && a.address < b.address);
    });
    out.erase(std::unique(out.begin(), out.end(), [](const NicAddress& a, const NicAddress& b) {
                  return a.name == b.name && a.address == b.address;
              }),
              out.end());
    return out;
}

Result<std::vector<unsigned>> current_thread_cpus() {
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (::sched_getaffinity(0, sizeof allowed, &allowed) != 0)
        return err(Errc::io_error, std::string("sched_getaffinity: ") + std::strerror(errno));

    std::vector<unsigned> cpus;
    for (unsigned cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        if (CPU_ISSET(cpu, &allowed)) cpus.push_back(cpu);
    if (cpus.empty())
        return err(Errc::invalid_argument, "this process has no CPUs in its affinity mask");
    return cpus;
}

Status bind_current_thread(std::span<const unsigned> cpus) {
    cpu_set_t target;
    CPU_ZERO(&target);
    if (cpus.empty())
        return err(Errc::invalid_argument, "cannot bind a thread to an empty CPU list");
    for (const unsigned cpu : cpus) {
        if (cpu >= CPU_SETSIZE)
            return err(Errc::unsupported,
                       "CPU " + std::to_string(cpu) + " exceeds this build's CPU_SETSIZE");
        CPU_SET(cpu, &target);
    }
    if (::sched_setaffinity(0, sizeof target, &target) != 0)
        return err(Errc::io_error, std::string("sched_setaffinity: ") + std::strerror(errno));
    return {};
}

Status bind_current_thread_memory(unsigned node) {
    constexpr std::size_t bits_per_word = sizeof(unsigned long) * 8;
    const std::size_t words = static_cast<std::size_t>(node) / bits_per_word + 1;
    std::vector<unsigned long> mask(words, 0);
    mask[node / bits_per_word] |= 1UL << (node % bits_per_word);
    // The raw Linux syscall interprets maxnode as a nodemask bit count with an historical
    // off-by-one convention. Pass the complete storage width, as libnuma does, so node zero does
    // not become an empty mask on kernels such as Ubuntu 5.15.
    const unsigned long maxnode = static_cast<unsigned long>(words * bits_per_word);
    if (::syscall(SYS_set_mempolicy, MPOL_BIND | MPOL_F_STATIC_NODES, mask.data(), maxnode) != 0)
        return err(Errc::io_error,
                   "set_mempolicy NUMA node " + std::to_string(node) + ": " +
                       std::strerror(errno));
    return {};
}

#endif

} // namespace

Result<std::vector<unsigned>> parse_cpu_list(std::string_view text) {
    text = trim(text);
    if (text.empty()) return err(Errc::invalid_argument, "empty CPU/node list");

    std::vector<unsigned> values;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        std::string_view item = trim(text.substr(0, comma));
        if (item.empty()) return err(Errc::invalid_argument, "empty item in CPU/node list");

        const std::size_t dash = item.find('-');
        const auto first = parse_unsigned(item.substr(0, dash));
        if (!first) return err(Errc::invalid_argument, "invalid CPU/node list item: " + std::string(item));
        unsigned last = *first;
        if (dash != std::string_view::npos) {
            if (item.find('-', dash + 1) != std::string_view::npos)
                return err(Errc::invalid_argument, "invalid CPU/node range: " + std::string(item));
            const auto parsed_last = parse_unsigned(item.substr(dash + 1));
            if (!parsed_last || *parsed_last < *first)
                return err(Errc::invalid_argument, "invalid CPU/node range: " + std::string(item));
            last = *parsed_last;
        }
        for (unsigned value = *first;; ++value) {
            values.push_back(value);
            if (value == last) break;
            if (value == std::numeric_limits<unsigned>::max())
                return err(Errc::invalid_argument, "CPU/node range overflows unsigned");
        }

        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
        if (text.empty()) return err(Errc::invalid_argument, "trailing comma in CPU/node list");
    }
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

Result<unsigned> select_numa_node(std::optional<unsigned> requested,
                                  std::span<const NicAddress> interfaces,
                                  std::span<const unsigned> online_nodes) {
    if (online_nodes.empty()) return err(Errc::unsupported, "Linux reports no online NUMA nodes");
    const auto online = [&](unsigned node) {
        return std::find(online_nodes.begin(), online_nodes.end(), node) != online_nodes.end();
    };
    if (requested) {
        if (!online(*requested))
            return err(Errc::invalid_argument,
                       "NUMA node " + std::to_string(*requested) +
                           " is not online; choose one of: " + available_hint(online_nodes));
        return *requested;
    }

    // A one-node machine has no placement ambiguity, even when a virtual NIC has no device locality.
    if (online_nodes.size() == 1) return online_nodes.front();

    std::set<unsigned> nic_nodes;
    bool unknown = false;
    for (const auto& nic : interfaces) {
        if (!nic.numa_node) {
            unknown = true;
            continue;
        }
        if (!online(*nic.numa_node))
            return err(Errc::invalid_argument,
                       nic.name + " " + nic.address + " reports offline NUMA node " +
                           std::to_string(*nic.numa_node));
        nic_nodes.insert(*nic.numa_node);
    }
    if (nic_nodes.size() == 1 && !unknown) return *nic_nodes.begin();
    return err(Errc::invalid_argument, ambiguity_detail(interfaces, online_nodes));
}

Result<unsigned> select_farthest_numa_node(
    unsigned serving_node, std::span<const unsigned> online_nodes,
    std::span<const unsigned> distance_by_node) {
    if (std::find(online_nodes.begin(), online_nodes.end(), serving_node) == online_nodes.end())
        return err(Errc::invalid_argument,
                   "serving NUMA node " + std::to_string(serving_node) + " is not online");

    std::optional<unsigned> farthest;
    unsigned farthest_distance = 0;
    for (const unsigned node : online_nodes) {
        if (node == serving_node) continue;
        if (node >= distance_by_node.size())
            return err(Errc::invalid_argument,
                       "NUMA distance row for node " + std::to_string(serving_node) +
                           " has no entry for online node " + std::to_string(node));
        const unsigned distance = distance_by_node[node];
        if (!farthest || distance > farthest_distance ||
            (distance == farthest_distance && node < *farthest)) {
            farthest = node;
            farthest_distance = distance;
        }
    }
    if (!farthest)
        return err(Errc::invalid_argument,
                   "--perverse requires at least two online NUMA nodes");
    return *farthest;
}

Result<std::vector<unsigned>> numa_node_cpus(unsigned node,
                                             std::span<const unsigned> allowed_cpus) {
#if defined(__linux__)
    auto node_list = read_node_cpus(node);
    if (!node_list) return std::unexpected(node_list.error());
    std::vector<unsigned> allowed(allowed_cpus.begin(), allowed_cpus.end());
    std::sort(allowed.begin(), allowed.end());
    allowed.erase(std::unique(allowed.begin(), allowed.end()), allowed.end());
    std::vector<unsigned> effective;
    std::set_intersection(node_list->begin(), node_list->end(), allowed.begin(), allowed.end(),
                          std::back_inserter(effective));
    if (effective.empty())
        return err(Errc::invalid_argument,
                   "NUMA node " + std::to_string(node) +
                       " has no CPUs allowed by this process/cgroup/taskset");
    return effective;
#else
    (void)node;
    (void)allowed_cpus;
    return err(Errc::unsupported, "NUMA affinity is supported only on Linux");
#endif
}

Status bind_numa_worker(unsigned node, std::span<const unsigned> cpus) {
#if defined(__linux__)
    if (auto affinity = bind_current_thread(cpus); !affinity) return affinity;
    return bind_current_thread_memory(node);
#else
    (void)node;
    (void)cpus;
    return err(Errc::unsupported, "NUMA affinity is supported only on Linux");
#endif
}

Result<NumaBinding> configure_numa(std::optional<unsigned> requested, bool perverse,
                                   std::span<const std::string> exact_listener_addresses,
                                   bool wildcard_ethernet_listener) {
#if defined(__linux__)
    auto allowed = current_thread_cpus();
    if (!allowed) return std::unexpected(allowed.error());
    auto online = online_numa_nodes();
    if (!online) return std::unexpected(online.error());

    std::vector<NicAddress> interfaces;
    if (!requested) {
        auto discovered = discover_listener_interfaces(exact_listener_addresses,
                                                       wildcard_ethernet_listener);
        if (!discovered) return std::unexpected(discovered.error());
        interfaces = std::move(*discovered);
    }
    auto node = select_numa_node(requested, interfaces, *online);
    if (!node) return std::unexpected(node.error());
    unsigned preferred_memory_node = *node;
    std::optional<unsigned> preferred_memory_distance;
    if (perverse) {
        auto distances = node_distances(*node);
        if (!distances) return std::unexpected(distances.error());
        auto farthest = select_farthest_numa_node(*node, *online, *distances);
        if (!farthest) return std::unexpected(farthest.error());
        preferred_memory_node = *farthest;
        preferred_memory_distance = (*distances)[*farthest];
    }
    auto effective = numa_node_cpus(*node, *allowed);
    if (!effective) return std::unexpected(effective.error());
    if (auto affinity = bind_current_thread(*effective); !affinity)
        return std::unexpected(affinity.error());
    if (auto memory = bind_current_thread_memory(*node); !memory)
        return std::unexpected(memory.error());
    NumaBinding binding;
    binding.node = *node;
    binding.preferred_memory_node = preferred_memory_node;
    binding.preferred_memory_distance = preferred_memory_distance;
    binding.cpus = std::move(*effective);
    binding.allowed_cpus = std::move(*allowed);
    binding.interfaces = std::move(interfaces);
    binding.online_nodes = std::move(*online);
    binding.automatic = !requested.has_value();
    return binding;
#else
    (void)requested;
    (void)perverse;
    (void)exact_listener_addresses;
    (void)wildcard_ethernet_listener;
    return err(Errc::unsupported, "NUMA affinity is supported only on Linux");
#endif
}

Result<std::vector<NumaMemoryBudget>> plan_numa_memory(
    unsigned local_node, std::span<const unsigned> online_nodes, Size local_bytes,
    Size foreign_bytes, std::string_view local_option, std::string_view foreign_option) {
    if (local_bytes == 0)
        return err(Errc::invalid_argument,
                   std::string(local_option) + " must be greater than zero");
    if (std::find(online_nodes.begin(), online_nodes.end(), local_node) == online_nodes.end())
        return err(Errc::invalid_argument,
                   "local NUMA node " + std::to_string(local_node) + " is not online");

    std::vector<NumaMemoryBudget> plan;
    plan.push_back({local_node, local_bytes}); // region zero is always the preferred/local arena
    if (foreign_bytes == 0) return plan;

    std::set<unsigned> seen{local_node};
    for (const unsigned node : online_nodes) {
        if (!seen.insert(node).second) continue;
        if (foreign_bytes > std::numeric_limits<Size>::max() - local_bytes)
            return err(Errc::invalid_argument, "NUMA memory budget overflows Size");
        local_bytes += foreign_bytes;
        plan.push_back({node, foreign_bytes});
    }
    if (foreign_bytes > 0 && plan.size() == 1)
        return err(Errc::invalid_argument,
                   std::string(foreign_option) +
                       " was specified but there are no other online NUMA nodes");
    return plan;
}

std::string format_cpu_list(std::span<const unsigned> cpus) {
    if (cpus.empty()) return "(none)";
    std::vector<unsigned> sorted(cpus.begin(), cpus.end());
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    std::string out;
    for (std::size_t i = 0; i < sorted.size();) {
        const unsigned first = sorted[i];
        unsigned last = first;
        while (i + 1 < sorted.size() && sorted[i + 1] == last + 1) last = sorted[++i];
        if (!out.empty()) out += ',';
        out += std::to_string(first);
        if (last != first) {
            out += '-';
            out += std::to_string(last);
        }
        ++i;
    }
    return out;
}

} // namespace goblin::net
