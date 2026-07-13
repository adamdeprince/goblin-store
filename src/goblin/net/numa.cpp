#include "goblin/net/numa.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <string>

#if defined(__linux__)
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sched.h>
#include <sys/socket.h>
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
        "automatic NUMA selection is ambiguous: the 0.0.0.0 listeners cover Ethernet addresses "
        "on different or unknown NUMA nodes:\n";
    if (interfaces.empty()) {
        out += "  (no UP, non-loopback IPv4 Ethernet interface was found)\n";
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

Result<std::vector<unsigned>> node_cpus(unsigned node) {
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

Result<std::vector<NicAddress>> discover_listener_interfaces() {
    ifaddrs* head = nullptr;
    if (::getifaddrs(&head) != 0)
        return err(Errc::io_error, std::string("getifaddrs: ") + std::strerror(errno));

    std::vector<NicAddress> out;
    for (const ifaddrs* p = head; p; p = p->ifa_next) {
        if (!p->ifa_addr || !p->ifa_name || p->ifa_addr->sa_family != AF_INET) continue;
        if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
        const std::string name(p->ifa_name);
        if (!is_ethernet(name)) continue;

        char address[INET_ADDRSTRLEN]{};
        const auto* sin = reinterpret_cast<const sockaddr_in*>(p->ifa_addr);
        if (!::inet_ntop(AF_INET, &sin->sin_addr, address, sizeof address)) continue;
        out.push_back(NicAddress{name, address, interface_numa_node(name)});
    }
    ::freeifaddrs(head);

    std::sort(out.begin(), out.end(), [](const NicAddress& a, const NicAddress& b) {
        return a.name < b.name || (a.name == b.name && a.address < b.address);
    });
    out.erase(std::unique(out.begin(), out.end(), [](const NicAddress& a, const NicAddress& b) {
                  return a.name == b.name && a.address == b.address;
              }),
              out.end());
    return out;
}

Result<std::vector<unsigned>> bind_current_thread(std::span<const unsigned> node_cpus_list) {
    cpu_set_t allowed;
    cpu_set_t target;
    CPU_ZERO(&allowed);
    CPU_ZERO(&target);
    if (::sched_getaffinity(0, sizeof allowed, &allowed) != 0)
        return err(Errc::io_error, std::string("sched_getaffinity: ") + std::strerror(errno));

    std::vector<unsigned> effective;
    for (const unsigned cpu : node_cpus_list) {
        if (cpu >= CPU_SETSIZE)
            return err(Errc::unsupported,
                       "CPU " + std::to_string(cpu) + " exceeds this build's CPU_SETSIZE");
        if (CPU_ISSET(cpu, &allowed)) {
            CPU_SET(cpu, &target);
            effective.push_back(cpu);
        }
    }
    if (effective.empty())
        return err(Errc::invalid_argument,
                   "the selected NUMA node has no CPUs allowed by this process/cgroup");
    if (::sched_setaffinity(0, sizeof target, &target) != 0)
        return err(Errc::io_error, std::string("sched_setaffinity: ") + std::strerror(errno));
    return effective;
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

Result<NumaBinding> configure_numa(std::optional<unsigned> requested) {
#if defined(__linux__)
    auto online = online_numa_nodes();
    if (!online) return std::unexpected(online.error());

    std::vector<NicAddress> interfaces;
    if (!requested) {
        auto discovered = discover_listener_interfaces();
        if (!discovered) return std::unexpected(discovered.error());
        interfaces = std::move(*discovered);
    }
    auto node = select_numa_node(requested, interfaces, *online);
    if (!node) return std::unexpected(node.error());
    auto cpus = node_cpus(*node);
    if (!cpus) return std::unexpected(cpus.error());
    auto effective = bind_current_thread(*cpus);
    if (!effective) return std::unexpected(effective.error());
    NumaBinding binding;
    binding.node = *node;
    binding.cpus = std::move(*effective);
    binding.interfaces = std::move(interfaces);
    binding.online_nodes = std::move(*online);
    binding.automatic = !requested.has_value();
    return binding;
#else
    (void)requested;
    return err(Errc::unsupported, "NUMA affinity is supported only on Linux");
#endif
}

Result<std::vector<NumaMemoryBudget>> plan_numa_memory(
    unsigned local_node, std::span<const unsigned> online_nodes, Size local_bytes,
    Size foreign_bytes) {
    if (local_bytes == 0) return err(Errc::invalid_argument, "--memory must be greater than zero");
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
            return err(Errc::invalid_argument, "NUMA head-memory budget overflows Size");
        local_bytes += foreign_bytes;
        plan.push_back({node, foreign_bytes});
    }
    if (foreign_bytes > 0 && plan.size() == 1)
        return err(Errc::invalid_argument,
                   "--sub-memory was specified but there are no other online NUMA nodes");
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
