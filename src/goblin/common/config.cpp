#include "goblin/common/config.hpp"

namespace goblin {

Status validate(const ServerConfig& c) {
    const auto& m = c.memory;
    if (!is_power_of_two(m.block_bytes) || m.block_bytes < kDeviceBlock)
        return err(Errc::invalid_argument, "memory.block_bytes must be a power of two >= 4 KiB");
    if (m.total_bytes < m.block_bytes)
        return err(Errc::invalid_argument, "memory.total_bytes must be >= block size");

    const auto& t = c.tiers;
    if (t.ram_head > t.ssd_prefix)
        return err(Errc::invalid_argument, "ram_head must be <= ssd_prefix (ADR-0006)");
    if (!is_aligned(t.ram_head, kDeviceBlock) || !is_aligned(t.ssd_prefix, kDeviceBlock))
        return err(Errc::invalid_argument, "tier sizes must be 4 KiB-aligned (O_DIRECT, ADR-0011)");
    if (t.ssd_prefix > kMaxObjectSize)
        return err(Errc::invalid_argument, "ssd_prefix exceeds the maximum object size");
    if (t.ram_head > m.block_bytes)
        return err(Errc::invalid_argument, "ram_head must be <= memory.block_bytes (head fits one block)");
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

    if (!c.enable_memcache && !c.enable_http && !c.enable_https)
        return err(Errc::invalid_argument,
                   "no listeners enabled (need at least one of memcache, HTTP, HTTPS)");
    if (c.enable_https && (c.tls_cert_path.empty() || c.tls_key_path.empty()))
        return err(Errc::invalid_argument, "HTTPS requires --tls-cert and --tls-key");

    return {};
}

} // namespace goblin
