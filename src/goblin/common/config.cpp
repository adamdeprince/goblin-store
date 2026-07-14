#include "goblin/common/config.hpp"

#include <cmath>

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
    if (c.io_buffers == 0)
        return err(Errc::invalid_argument, "io_buffers must be >= 1");

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

    if (!c.enable_memcache && !c.enable_http && !c.enable_https)
        return err(Errc::invalid_argument,
                   "no listeners enabled (need at least one of memcache, HTTP, HTTPS)");
    if (c.tls_cert_paths.size() != c.tls_key_paths.size())
        return err(Errc::invalid_argument, "each --tls-cert needs a matching --tls-key");
    if (c.enable_https && c.tls_cert_paths.empty())
        return err(Errc::invalid_argument, "HTTPS requires at least one --tls-cert/--tls-key pair");

    return {};
}

} // namespace goblin
