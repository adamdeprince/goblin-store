#include "goblin/common/config.hpp"

#include "goblin/store/rdma_wire.hpp"

#include <arpa/inet.h>
#include <cmath>
#include <limits>
#include <netdb.h>

namespace {

bool numeric_network_address(const std::string& address) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST;
    addrinfo* result = nullptr;
    const int status = ::getaddrinfo(address.c_str(), nullptr, &hints, &result);
    if (result) ::freeaddrinfo(result);
    return status == 0;
}

bool numeric_ipv4_address(const std::string& address) {
    in_addr parsed{};
    return ::inet_pton(AF_INET, address.c_str(), &parsed) == 1;
}

bool wildcard_ipv4_address(const std::string& address) {
    in_addr parsed{};
    return ::inet_pton(AF_INET, address.c_str(), &parsed) == 1 &&
           parsed.s_addr == htonl(INADDR_ANY);
}

bool plausible_mirror_url(const std::string& url) {
    const std::string_view value(url);
    const std::string_view scheme = value.starts_with("http://")
        ? std::string_view("http://")
        : (value.starts_with("https://") ? std::string_view("https://") : std::string_view{});
    if (scheme.empty()) return false;
    const std::string_view authority_and_path = value.substr(scheme.size());
    const std::size_t authority_end = authority_and_path.find_first_of("/?#");
    const std::string_view authority = authority_and_path.substr(0, authority_end);
    return !authority.empty() && authority.find_first_of("\r\n") == std::string_view::npos &&
           value.find('#') == std::string_view::npos && value.find('?') == std::string_view::npos;
}

} // namespace

namespace goblin {

Status validate(const ServerConfig& c) {
    const auto& m = c.memory;
    if (!is_power_of_two(m.block_bytes) || m.block_bytes < kDeviceBlock)
        return err(Errc::invalid_argument, "memory.block_bytes must be a power of two >= 4 KiB");
    if (!is_power_of_two(m.hugetlb_page_bytes) || m.hugetlb_page_bytes < kDeviceBlock)
        return err(Errc::invalid_argument,
                   "memory.hugetlb_page_bytes must be a power of two >= 4 KiB");
    if (m.block_bytes < m.hugetlb_page_bytes ||
        m.block_bytes % m.hugetlb_page_bytes != 0)
        return err(Errc::invalid_argument,
                   "--block must be a multiple of the platform HugeTLB page size");
    if (m.total_bytes < m.block_bytes)
        return err(Errc::invalid_argument, "memory.total_bytes must be >= block size");
    if (m.total_bytes % m.block_bytes != 0)
        return err(Errc::invalid_argument, "--memory must be a multiple of --block");
    if (!c.numa_enabled && c.numa_node)
        return err(Errc::invalid_argument, "--numa and --no-numa are mutually exclusive");
    if (!c.numa_enabled && c.numa_perverse)
        return err(Errc::invalid_argument, "--perverse cannot be used with --no-numa");
    if (!c.numa_enabled && m.sub_bytes > 0)
        return err(Errc::invalid_argument, "--sub-memory cannot be used with --no-numa");
    if (m.sub_bytes > 0) {
        if (!c.numa_node)
            return err(Errc::invalid_argument, "--sub-memory requires an explicit --numa NODE");
        if (m.sub_bytes < m.block_bytes || m.sub_bytes % m.block_bytes != 0)
            return err(Errc::invalid_argument,
                       "--sub-memory must be at least one --block and a multiple of --block");
    }
    if (m.small_sub_bytes && !m.small_total_bytes)
        return err(Errc::invalid_argument,
                   "--small-sub-memory requires --small-memory");
    if (m.small_total_bytes) {
        if (*m.small_total_bytes < m.block_bytes ||
            *m.small_total_bytes % m.block_bytes != 0)
            return err(Errc::invalid_argument,
                       "--small-memory must be at least one --block and a multiple of --block");
    }
    if (m.small_sub_bytes && *m.small_sub_bytes > 0) {
        if (!c.numa_enabled)
            return err(Errc::invalid_argument,
                       "--small-sub-memory cannot be used with --no-numa");
        if (!c.numa_node)
            return err(Errc::invalid_argument,
                       "--small-sub-memory requires an explicit --numa NODE");
        if (*m.small_sub_bytes < m.block_bytes ||
            *m.small_sub_bytes % m.block_bytes != 0)
            return err(Errc::invalid_argument,
                       "--small-sub-memory must be at least one --block and a multiple of --block");
    }
    if (!is_power_of_two(m.small_min_alloc) || m.small_min_alloc < 8 || m.small_min_alloc > m.block_bytes)
        return err(Errc::invalid_argument, "memory.small_min_alloc must be a power of two in [8, block_bytes]");

    const auto& t = c.tiers;
    if (t.ram_head > t.ssd_prefix)
        return err(Errc::invalid_argument, "ram_head must be <= ssd_prefix (ADR-0006)");
    if (!is_power_of_two(t.ram_head) || t.ram_head < kDeviceBlock)
        return err(Errc::invalid_argument, "--ram-head must be a power of two >= 4 KiB");
    if (!is_aligned(t.ssd_prefix, kDeviceBlock))
        return err(Errc::invalid_argument, "ssd_prefix must be 4 KiB-aligned (O_DIRECT, ADR-0011)");
    if (t.ssd_prefix > kMaxObjectSize)
        return err(Errc::invalid_argument, "ssd_prefix exceeds the maximum object size");
    if (t.ram_head > m.block_bytes)
        return err(Errc::invalid_argument, "ram_head must be <= memory.block_bytes (head fits one block)");
    if (m.block_bytes % t.ram_head != 0)
        return err(Errc::invalid_argument, "--ram-head must divide --block exactly");
    if (!is_power_of_two(c.io_chunk_bytes) || c.io_chunk_bytes < kDeviceBlock)
        return err(Errc::invalid_argument, "io_chunk_bytes must be a power of two >= 4 KiB");
    if (!is_power_of_two(c.write_io_chunk_bytes) || c.write_io_chunk_bytes < kDeviceBlock)
        return err(Errc::invalid_argument,
                   "write_io_chunk_bytes must be a power of two >= 4 KiB");
    if (c.io_buffers == 0)
        return err(Errc::invalid_argument, "io_buffers must be >= 1");

    if (c.listen_address.empty() || !numeric_ipv4_address(c.listen_address))
        return err(Errc::invalid_argument,
                   "--listen-address needs a numeric IPv4 address");
    if (c.net == NetMode::exasock) {
        if (wildcard_ipv4_address(c.listen_address))
            return err(Errc::invalid_argument,
                       "--net exasock requires an exact, non-wildcard --listen-address");
        if (!c.enable_memcache && !c.enable_http)
            return err(Errc::invalid_argument,
                       "--net exasock requires memcache and/or plaintext HTTP");
    }

    if (c.mirror_url) {
        if (c.http_vhost)
            return err(Errc::invalid_argument,
                       "--mirror and --http-vhost/--virtual-host are mutually exclusive");
        if (!c.enable_http && !c.enable_https)
            return err(Errc::invalid_argument,
                       "--mirror requires the HTTP and/or HTTPS listener");
        if (!plausible_mirror_url(*c.mirror_url))
            return err(Errc::invalid_argument,
                       "--mirror needs an absolute http:// or https:// URL without a query or fragment");
        if (c.mirror_client == MirrorClient::uring &&
            !std::string_view(*c.mirror_url).starts_with("http://"))
            return err(Errc::invalid_argument,
                       "--mirror-client uring currently requires an http:// origin");
    }

    if (c.rdma.enabled) {
        namespace wire = store::rdma_wire;
        if (c.rdma.address.empty() || !numeric_network_address(c.rdma.address))
            return err(Errc::invalid_argument, "--rdma needs a numeric IPv4 or IPv6 address");
        if (c.rdma.port == 0)
            return err(Errc::invalid_argument, "--rdma-port must be between 1 and 65535");
        if (c.rdma.ring_bytes / wire::kControlSlotStride < 2 ||
            c.rdma.ring_bytes / wire::kControlSlotStride >
                std::numeric_limits<std::uint32_t>::max())
            return err(Errc::invalid_argument,
                       "--rdma-ring must hold between 2 and 2^32-1 control slots");
        if (c.rdma.bulk_window_bytes > std::numeric_limits<std::uint32_t>::max() ||
            c.rdma.bulk_window_count > std::numeric_limits<std::uint16_t>::max() ||
            !wire::valid_bulk_geometry(
                static_cast<std::uint32_t>(c.rdma.bulk_window_bytes),
                static_cast<std::uint32_t>(c.rdma.bulk_window_count)))
            return err(Errc::invalid_argument,
                       "--rdma-window must be a power of two of at least 4 KiB, and the "
                       "combined TX/RX window mapping must fit the v3 descriptor");
        if (c.rdma.bulk_window_count < 2)
            return err(Errc::invalid_argument,
                       "--rdma-windows must be at least 2 so tail I/O can overlap the RAM head");
        if (c.rdma.backlog == 0)
            return err(Errc::invalid_argument, "RDMA listen backlog must be nonzero");
    }

    if (c.ssd.dirs.empty())
        return err(Errc::invalid_argument, "at least one --ssd-dir is required");
    if (c.ssd.stripe_unit == 0 || !is_aligned(c.ssd.stripe_unit, kDeviceBlock))
        return err(Errc::invalid_argument, "ssd stripe_unit must be a nonzero multiple of 4 KiB");

    if (c.three_layer()) {
        if (c.hdd.stripe_unit == 0 || !is_aligned(c.hdd.stripe_unit, kDeviceBlock))
            return err(Errc::invalid_argument, "hdd stripe_unit must be a nonzero multiple of 4 KiB");
        // ADR-0006 shock-absorber law: the SSD middle must cover one HDD read-ahead gulp.
        const Size ssd_middle = t.ssd_prefix - t.ram_head;
        if (ssd_middle < c.hdd.stripe_unit)
            return err(Errc::invalid_argument,
                       "ssd middle (ssd_prefix - ram_head) must be >= hdd stripe_unit (ADR-0006)");
    }

    if (c.eviction.low_watermark <= 0.0 || c.eviction.high_watermark > 1.0 ||
        c.eviction.low_watermark >= c.eviction.high_watermark)
        return err(Errc::invalid_argument, "require 0 < low_watermark < high_watermark <= 1.0");

    if (!std::isfinite(c.access_score.increment) || c.access_score.increment <= 0.0)
        return err(Errc::invalid_argument, "--increment must be a finite positive number");
    if (!std::isfinite(c.access_score.decay) || c.access_score.decay <= 0.0 ||
        c.access_score.decay >= 1.0)
        return err(Errc::invalid_argument, "--decay must be finite and strictly between 0 and 1");

    if (!c.enable_memcache && !c.enable_http && !c.enable_https && !c.rdma.enabled)
        return err(Errc::invalid_argument,
                   "no listeners enabled (need memcache, RDMA memcache, HTTP, or HTTPS)");
    if (c.tls_cert_paths.size() != c.tls_key_paths.size())
        return err(Errc::invalid_argument, "each --tls-cert needs a matching --tls-key");
    if (c.enable_https && c.tls_cert_paths.empty())
        return err(Errc::invalid_argument, "HTTPS requires at least one --tls-cert/--tls-key pair");

    return {};
}

} // namespace goblin
