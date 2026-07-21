#include "goblin/protocol/memcache/stats_format.hpp"

#include "goblin/protocol/memcache/protocol.hpp"
#include "goblin/storage/tier_manager.hpp"

#include <array>
#include <format>
#include <limits>
#include <string_view>
#include <unistd.h>

namespace goblin::memcache {
namespace {

void line(std::string& out, std::string_view name, std::uint64_t value) {
    out += std::format("STAT {} {}\r\n", name, value);
}

std::string_view class_name(core::BufferPoolClass value) {
    switch (value) {
        case core::BufferPoolClass::shared: return "ram";
        case core::BufferPoolClass::fixed_head: return "ram_head";
        case core::BufferPoolClass::small_object: return "small_pool";
    }
    return "unknown";
}

std::size_t command_index(core::CommandKind value) {
    return static_cast<std::size_t>(value);
}

void duration(std::string& out, std::string_view prefix, const core::DurationSnapshot& value) {
    line(out, std::format("{}_count", prefix), value.count);
    line(out, std::format("{}_total_ns", prefix), value.total_ns);
    line(out, std::format("{}_max_ns", prefix), value.max_ns);
    for (std::size_t i = 0; i < value.buckets.size(); ++i)
        line(out, std::format("{}_bucket_{}", prefix, i), value.buckets[i]);
}

void filesystem_capacity(std::string& out, storage::TierManager& tm) {
    const auto filesystems = tm.filesystem_capacity_snapshot();
    std::array<std::size_t, 2> index{};
    std::array<std::size_t, 2> count{};
    for (const auto& fs : filesystems)
        ++count[fs.tier == storage::Tier::ssd ? 0 : 1];
    line(out, "goblin_ssd_filesystem_count", count[0]);
    line(out, "goblin_hdd_filesystem_count", count[1]);
    for (const auto& filesystem : filesystems) {
        const std::size_t tier = filesystem.tier == storage::Tier::ssd ? 0 : 1;
        const auto prefix = std::format("goblin_{}_filesystem_{}",
                                        tier == 0 ? "ssd" : "hdd", index[tier]++);
        const auto& fs = filesystem.capacity;
        line(out, std::format("{}_device", prefix), fs.device);
        line(out, std::format("{}_capacity_bytes", prefix), fs.total_bytes);
        line(out, std::format("{}_used_bytes", prefix), fs.allocated_bytes);
        line(out, std::format("{}_free_bytes", prefix), fs.free_bytes);
        line(out, std::format("{}_available_bytes", prefix), fs.available_bytes);
        line(out, std::format("{}_inodes", prefix), fs.total_inodes);
        line(out, std::format("{}_inodes_used", prefix), fs.used_inodes);
        line(out, std::format("{}_inodes_free", prefix), fs.free_inodes);
        line(out, std::format("{}_inodes_available", prefix), fs.available_inodes);
    }
}

void buddy_free_blocks(std::string& out, storage::TierManager& tm) {
    for (const auto& value : tm.buddy_free_blocks_snapshot()) {
        const auto prefix = class_name(value.allocation_class);
        line(out, std::format("goblin_{}_buddy_free_{}_blocks", prefix, value.block_bytes),
             value.count);
    }
}

} // namespace

std::string format_stats_response(storage::TierManager& tm, const core::StatsSnapshot& stats,
                                  std::uint64_t uptime, bool detailed) {
    const auto compatible = tm.compatibility_snapshot();
    std::string out;
    out += std::format("STAT pid {}\r\n", ::getpid());
    line(out, "uptime", uptime);
    line(out, "time", storage::now_unix());
    out += "STAT version goblin-store 0.0.3\r\n";
    line(out, "pointer_size", sizeof(void*) * 8);
    line(out, "curr_connections", stats.curr_conns);
    line(out, "total_connections", stats.conns);
    line(out, "connection_structures", stats.curr_conns);
    line(out, "accepting_conns",
         stats.connection_limit == 0 || stats.process_curr_conns < stats.connection_limit ? 1 : 0);
    line(out, "cmd_get", stats.get_hits + stats.get_misses);
    line(out, "cmd_set", stats.sets + stats.set_rejected);
    line(out, "get_hits", stats.get_hits);
    line(out, "get_misses", stats.get_misses);
    line(out, "curr_items", compatible.objects.items);
    line(out, "total_items", compatible.total_items);
    line(out, "bytes", compatible.objects.logical_bytes);
    line(out, "evictions", compatible.evictions.objects);
    line(out, "reclaimed", compatible.evictions.ttl);
    line(out, "limit_maxbytes", compatible.memory_limit_bytes);
    line(out, "sets_stored", stats.sets);
    line(out, "sets_rejected", stats.set_rejected);
    line(out, "bytes_served", stats.bytes_served);
    line(out, "bytes_read", stats.bytes_received);
    line(out, "bytes_written", stats.bytes_served);
    line(out, "bytes_stored", stats.bytes_stored);

    const auto command = [&](std::string_view prefix, core::CommandKind kind) {
        const auto i = command_index(kind);
        line(out, std::format("{}_success", prefix), stats.command_success[i]);
        line(out, std::format("{}_miss", prefix), stats.command_miss[i]);
        line(out, std::format("{}_condition", prefix), stats.command_condition[i]);
        line(out, std::format("{}_errors", prefix), stats.command_error[i]);
    };
    command("set", core::CommandKind::set); command("add", core::CommandKind::add);
    command("replace", core::CommandKind::replace); command("append", core::CommandKind::append);
    command("prepend", core::CommandKind::prepend); command("cas", core::CommandKind::cas);
    command("delete", core::CommandKind::delete_); command("touch", core::CommandKind::touch);
    command("incr", core::CommandKind::incr); command("decr", core::CommandKind::decr);
    command("flush", core::CommandKind::flush); command("meta", core::CommandKind::meta);
    const auto cas = command_index(core::CommandKind::cas);
    line(out, "cas_hits", stats.command_success[cas]);
    line(out, "cas_misses", stats.command_miss[cas]);
    line(out, "cas_badval", stats.command_condition[cas]);
    const auto del = command_index(core::CommandKind::delete_);
    line(out, "delete_hits", stats.command_success[del]);
    line(out, "delete_misses", stats.command_miss[del]);
    const auto touch = command_index(core::CommandKind::touch);
    line(out, "touch_hits", stats.command_success[touch]);
    line(out, "touch_misses", stats.command_miss[touch]);
    const auto incr = command_index(core::CommandKind::incr);
    line(out, "incr_hits", stats.command_success[incr]);
    line(out, "incr_misses", stats.command_miss[incr]);
    const auto decr = command_index(core::CommandKind::decr);
    line(out, "decr_hits", stats.command_success[decr]);
    line(out, "decr_misses", stats.command_miss[decr]);
    line(out, "cmd_flush", stats.command_success[command_index(core::CommandKind::flush)]);
    const auto command_total = [&](core::CommandKind kind) {
        const auto i = command_index(kind);
        return stats.command_success[i] + stats.command_miss[i] +
               stats.command_condition[i] + stats.command_error[i];
    };
    line(out, "cmd_delete", command_total(core::CommandKind::delete_));
    line(out, "cmd_touch", command_total(core::CommandKind::touch));
    line(out, "cmd_incr", command_total(core::CommandKind::incr));
    line(out, "cmd_decr", command_total(core::CommandKind::decr));

    line(out, "get_backpressure", stats.get_backpressure);
    line(out, "set_backpressure", stats.set_backpressure);
    duration(out, "ttfb", stats.ttfb);
    duration(out, "get_queue_wait", stats.get_queue_wait);
    duration(out, "set_queue_wait", stats.set_queue_wait);
    duration(out, "backpressure", stats.backpressure);
    line(out, "io_uring_sqe_shortage", stats.uring_sqe_shortage);
    line(out, "io_uring_cqes", stats.uring_cqes);
    line(out, "slow_drops", stats.slow_drops);
    line(out, "idle_drops", stats.idle_drops);
    line(out, "queue_drops", stats.queue_drops);
    line(out, "rejected_connections", stats.rejected_connections);
    line(out, "rejected_connection_limit", stats.rejected_connection_limit);
    line(out, "rejected_dispatch_queue", stats.rejected_dispatch_queue);
    line(out, "rejected_resource", stats.rejected_resource);
    line(out, "listen_disabled_num", stats.listen_disabled_num);
    line(out, "auth_cmds", stats.auth_cmds);
    line(out, "auth_errors", stats.auth_errors);
    buddy_free_blocks(out, tm);
    filesystem_capacity(out, tm);
    const auto health = tm.storage_health_snapshot();
    out += std::format("STAT storage_state {}\r\n",
                       storage::TierManager::health_state_name(health.state));
    line(out, "storage_ready", health.ready ? 1 : 0);
    line(out, "quarantined_objects", health.quarantined_objects);
    line(out, "quarantine_failures", health.quarantine_failures);
    line(out, "watermark_scans", health.watermark_scans);
    line(out, "watermark_reclaim_runs", health.watermark_reclaim_runs);
    line(out, "watermark_reclaimed_objects", health.watermark_reclaimed_objects);
    for (std::size_t i = 0; i < health.devices.size(); ++i) {
        const auto& device = health.devices[i];
        const auto prefix = std::format("storage_device_{}", i);
        line(out, std::format("{}_id", prefix), device.device);
        out += std::format("STAT {}_paths {}\r\n", prefix, device.paths);
        out += std::format("STAT {}_state {}\r\n", prefix,
                           storage::TierManager::health_state_name(device.state));
        line(out, std::format("{}_tier_mask", prefix), device.tier_mask);
        line(out, std::format("{}_read_errors", prefix), device.read_errors);
        line(out, std::format("{}_write_errors", prefix), device.write_errors);
        line(out, std::format("{}_capacity_errors", prefix), device.capacity_errors);
        line(out, std::format("{}_last_errno", prefix), device.last_errno);
        line(out, std::format("{}_last_error_unix", prefix), device.last_error_unix);
    }

    if (!detailed) { out += kEnd; return out; }
    const auto store = tm.observability_snapshot();
    line(out, "head_resident_items", store.objects.head_resident);
    line(out, "headless_items", store.objects.headless);
    line(out, "resident_head_bytes", store.objects.resident_head_bytes);
    line(out, "head_evictions", store.evictions.heads);
    line(out, "capacity_evictions", store.evictions.capacity);
    line(out, "ttl_evictions", store.evictions.ttl);
    Size limit_maxbytes = 0;
    for (const auto& memory : store.memory) {
        const auto prefix = class_name(memory.allocation_class);
        line(out, std::format("{}_capacity_bytes", prefix), memory.capacity_bytes);
        line(out, std::format("{}_used_bytes", prefix), memory.used_bytes);
        line(out, std::format("{}_free_bytes", prefix), memory.free_bytes);
        line(out, std::format("{}_fragmented_bytes", prefix), memory.fragmented_bytes);
        limit_maxbytes += memory.capacity_bytes;
    }
    line(out, "goblin_memory_limit_bytes", limit_maxbytes);
    for (const auto& mapping : store.mappings) {
        const auto prefix = std::format("numa_region_{}", mapping.region);
        line(out, std::format("{}_node", prefix),
             mapping.numa_node.value_or(std::numeric_limits<unsigned>::max()));
        line(out, std::format("{}_hugetlb_bytes", prefix), mapping.hugetlb ? mapping.bytes : 0);
        line(out, std::format("{}_fallback_bytes", prefix), mapping.hugetlb ? 0 : mapping.bytes);
    }
    for (std::size_t i = 0; i < store.filesystems.size(); ++i) {
        const auto& fs = store.filesystems[i];
        const auto prefix = std::format("{}_filesystem_{}",
                                        fs.tier == storage::Tier::ssd ? "ssd" : "hdd", i);
        line(out, std::format("{}_device", prefix), fs.device);
        out += std::format("STAT {}_path {}\r\n", prefix, fs.path);
        line(out, std::format("{}_logical_bytes", prefix), fs.logical_bytes);
        line(out, std::format("{}_reserved_bytes", prefix), fs.reserved_bytes);
        line(out, std::format("{}_allocated_bytes", prefix), fs.filesystem_allocated_bytes);
        line(out, std::format("{}_free_bytes", prefix), fs.filesystem_free_bytes);
        line(out, std::format("{}_available_bytes", prefix), fs.filesystem_available_bytes);
        line(out, std::format("{}_total_bytes", prefix), fs.filesystem_total_bytes);
        line(out, std::format("{}_inodes", prefix), fs.filesystem_total_inodes);
        line(out, std::format("{}_inodes_used", prefix), fs.filesystem_used_inodes);
        line(out, std::format("{}_inodes_free", prefix), fs.filesystem_free_inodes);
        line(out, std::format("{}_inodes_available", prefix),
             fs.filesystem_available_inodes);
    }
    for (const auto& io : store.io) {
        const std::string_view tier = io.tier == storage::Tier::ssd ? "ssd" : "hdd";
        duration(out, std::format("{}_read_latency", tier), io.reads);
        duration(out, std::format("{}_write_latency", tier), io.writes);
        line(out, std::format("{}_read_errors", tier), io.read_errors);
        line(out, std::format("{}_write_errors", tier), io.write_errors);
        for (const auto& [error, count] : io.read_errno)
            line(out, std::format("{}_read_errno_{}", tier, error), count);
        for (const auto& [error, count] : io.write_errno)
            line(out, std::format("{}_write_errno_{}", tier, error), count);
    }
    const auto promotion = tm.numa_promotion_stats();
    line(out, "numa_optimizer_healthy", promotion.healthy ? 1 : 0);
    line(out, "numa_optimizer_scans", promotion.scans);
    line(out, "numa_optimizer_no_candidate", promotion.no_candidate);
    line(out, "numa_optimizer_maintenance_yields", promotion.maintenance_yields);
    line(out, "numa_optimizer_failures", promotion.failures);
    line(out, "numa_optimizer_last_success", promotion.last_success_unix);
    line(out, "numa_promotions", promotion.count);
    line(out, "numa_promotion_bytes", promotion.bytes_moved);
    line(out, "numa_promotion_total_ns", promotion.total_ns);
    line(out, "numa_promotion_max_ns", promotion.max_ns);
    out += kEnd;
    return out;
}

std::string format_settings_response(const core::StatsSettings& settings) {
    std::string out;
    if (!settings.configured) { out += kEnd; return out; }
    const auto& c = settings.config;
    line(out, "maxbytes", c.memory.arena_bytes() + c.memory.small_arena_bytes());
    line(out, "maxconns", c.max_connections);
    line(out, "tcpport", c.memcache_port);
    line(out, "num_threads", c.cores);
    line(out, "item_size_max", c.max_object_size);
    line(out, "idle_timeout_ms", c.idle_timeout_ms);
    line(out, "queue_timeout_ms", c.queue_timeout_ms);
    line(out, "listen_backlog", c.listen_backlog);
    line(out, "io_chunk_bytes", c.io_chunk_bytes);
    line(out, "io_buffers", c.io_buffers);
    line(out, "ram_head_bytes", c.tiers.ram_head);
    line(out, "ssd_prefix_bytes", c.tiers.ssd_prefix);
    line(out, "memory_block_bytes", c.memory.block_bytes);
    line(out, "hugetlb_page_bytes", c.memory.hugetlb_page_bytes);
    line(out, "numa_enabled", c.numa_enabled ? 1 : 0);
    line(out, "numa_promotion", c.numa_promotion ? 1 : 0);
    out += std::format("STAT disk_high_watermark {}\r\n", c.eviction.high_watermark);
    out += std::format("STAT disk_low_watermark {}\r\n", c.eviction.low_watermark);
    line(out, "disk_reclaim_interval_ms", c.eviction.reclaim_interval_ms);
    out += std::format("STAT auth_enabled_ascii {}\r\n",
                       c.memcache_auth_file ? "yes" : "no");
    line(out, "memcache_tls", c.memcache_tls ? 1 : 0);
    if (c.memcache_socket)
        out += std::format("STAT domain_socket {}\r\n", *c.memcache_socket);
    out += kEnd;
    return out;
}

} // namespace goblin::memcache
