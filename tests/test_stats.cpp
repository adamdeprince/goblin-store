#include "mini_test.hpp"

#include "goblin/common/config.hpp"
#include "goblin/core/stats.hpp"
#include "goblin/crypto/sha256.hpp"
#include "goblin/protocol/memcache/stats_format.hpp"
#include "goblin/storage/index.hpp"
#include "goblin/storage/tier_manager.hpp"

#include <filesystem>
#include <cerrno>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>

using namespace goblin::core;

namespace {

std::optional<std::uint64_t> stat_value(const std::string& response, std::string_view name) {
    const std::string prefix = "STAT " + std::string(name) + " ";
    const auto start = response.find(prefix);
    if (start == std::string::npos) return std::nullopt;
    const auto value = start + prefix.size();
    const auto end = response.find("\r\n", value);
    if (end == std::string::npos) return std::nullopt;
    return std::stoull(response.substr(value, end - value));
}

} // namespace

TEST("stats: memcache aggregation excludes HTTP and HTTPS traffic") {
    StatsRegistry registry;
    Stats tcp(StatsDomain::memcache_tcp);
    Stats rdma(StatsDomain::memcache_rdma);
    Stats http(StatsDomain::http);
    Stats https(StatsDomain::https);
    registry.add(&tcp); registry.add(&rdma); registry.add(&http); registry.add(&https);
    tcp.get_hits.store(2); rdma.get_hits.store(3); http.get_hits.store(5); https.get_hits.store(7);
    CHECK_EQ(registry.aggregate(StatsSelection::memcache).get_hits, std::uint64_t(5));
    CHECK_EQ(registry.aggregate().get_hits, std::uint64_t(17));
}

TEST("stats: command results, durations, and rejection reasons aggregate") {
    StatsRegistry registry;
    Stats worker;
    registry.add(&worker);
    worker.note_command(CommandKind::cas, CommandResult::condition);
    worker.ttfb.observe(10'000);
    registry.note_connection_rejected(StatsDomain::memcache_tcp,
                                      ConnectionRejectReason::dispatch_queue);
    const auto snapshot = registry.aggregate(StatsSelection::memcache);
    CHECK_EQ(snapshot.command_condition[static_cast<std::size_t>(CommandKind::cas)],
             std::uint64_t(1));
    CHECK_EQ(snapshot.ttfb.count, std::uint64_t(1));
    CHECK_EQ(snapshot.ttfb.total_ns, std::uint64_t(10'000));
    CHECK_EQ(snapshot.rejected_dispatch_queue, std::uint64_t(1));
}

TEST("stats settings exposes memcache security and Unix listener configuration") {
    StatsSettings settings;
    settings.configured = true;
    settings.config.memcache_auth_file = "/run/secrets/goblin-users";
    settings.config.memcache_tls = true;
    settings.config.memcache_socket = "/run/goblin-store/memcache.sock";
    const std::string response = goblin::memcache::format_settings_response(settings);
    CHECK(response.find("STAT auth_enabled_ascii yes\r\n") != std::string::npos);
    CHECK(response.find("STAT memcache_tls 1\r\n") != std::string::npos);
    CHECK(response.find("STAT domain_socket /run/goblin-store/memcache.sock\r\n") !=
          std::string::npos);
}

TEST("stats: normal response exposes capacity and inodes for both disk tiers") {
    namespace fs = std::filesystem;
    using namespace goblin;
    using namespace goblin::storage;

    const std::string base =
        (fs::temp_directory_path() / ("goblin-stats-" + std::to_string(::getpid()))).string();
    PoolConfig ssd, hdd;
    ssd.stripe_unit = hdd.stripe_unit = 4 * KiB;
    ssd.dirs = {base + "/ssd-a", base + "/ssd-b"};
    hdd.dirs = {base + "/hdd-a", base + "/hdd-b"};
    for (const auto& dir : ssd.dirs) fs::create_directories(dir);
    for (const auto& dir : hdd.dirs) fs::create_directories(dir);

    TierSizes tiers;
    tiers.ram_head = 4 * KiB;
    tiers.ssd_prefix = 16 * KiB;
    MemoryConfig memory;
    memory.total_bytes = 4 * MiB;
    memory.block_bytes = 256 * KiB;
    memory.lock_memory = false;
    Index index;
    auto manager = TierManager::open(tiers, memory, EvictionConfig{}, ssd, hdd, index);
    CHECK(manager.has_value());
    if (manager) {
        const std::string response =
            goblin::memcache::format_stats_response(*manager, StatsSnapshot{}, 1);
        CHECK_EQ(stat_value(response, "goblin_ram_buddy_free_4096_blocks"), 0);
        CHECK_EQ(stat_value(response, "goblin_ram_buddy_free_262144_blocks"), 16);
        CHECK_EQ(stat_value(response, "goblin_ssd_filesystem_count"), 1);
        CHECK_EQ(stat_value(response, "goblin_hdd_filesystem_count"), 1);
        for (const std::string_view tier : {"ssd", "hdd"}) {
            const std::string prefix = "goblin_" + std::string(tier) + "_filesystem_0_";
            CHECK(stat_value(response, prefix + "capacity_bytes").value_or(0) > 0);
            CHECK(stat_value(response, prefix + "used_bytes").has_value());
            CHECK(stat_value(response, prefix + "free_bytes").has_value());
            CHECK(stat_value(response, prefix + "available_bytes").has_value());
            CHECK(stat_value(response, prefix + "inodes").value_or(0) > 0);
            CHECK(stat_value(response, prefix + "inodes_used").has_value());
            CHECK(stat_value(response, prefix + "inodes_free").has_value());
            CHECK(stat_value(response, prefix + "inodes_available").has_value());
        }
    }
    fs::remove_all(base);
}

TEST("storage health distinguishes degraded reads, read-only writes, and protocol stats") {
    namespace fs = std::filesystem;
    using namespace goblin;
    using namespace goblin::storage;

    const std::string base =
        (fs::temp_directory_path() / ("goblin-health-" + std::to_string(::getpid()))).string();
    fs::create_directories(base);
    PoolConfig ssd;
    ssd.dirs = {base};
    ssd.stripe_unit = 4 * KiB;
    TierSizes tiers;
    tiers.ram_head = 4 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig memory;
    memory.total_bytes = 4 * MiB;
    memory.block_bytes = 256 * KiB;
    memory.lock_memory = false;
    Index index;
    auto manager = TierManager::open(tiers, memory, EvictionConfig{}, ssd, {}, index);
    CHECK(manager.has_value());
    if (manager) {
        auto health = manager->storage_health_snapshot();
        CHECK_EQ(health.devices.size(), std::size_t(1));
        CHECK(health.ready);
        if (!health.devices.empty()) {
            const auto device = health.devices.front().device;
            manager->note_device_read(device, EIO);
            health = manager->storage_health_snapshot();
            CHECK(health.ready); // degraded caches still serve; operators can drain them deliberately
            CHECK_EQ(health.state, TierManager::StorageHealthState::degraded);
            manager->note_device_write(device, EIO);
            health = manager->storage_health_snapshot();
            CHECK(!health.ready);
            CHECK_EQ(health.state, TierManager::StorageHealthState::read_only);

            std::vector<std::byte> value(8 * KiB, std::byte{0x42});
            const auto stored = manager->store(
                goblin::crypto::hash_key("/read-only"), ByteView(value.data(), value.size()), 0);
            CHECK(!stored.has_value());
            if (!stored) CHECK_EQ(stored.error().code, Errc::read_only);

            const std::string response =
                goblin::memcache::format_stats_response(*manager, StatsSnapshot{}, 1);
            CHECK(response.find("STAT storage_state read_only\r\n") != std::string::npos);
            CHECK_EQ(stat_value(response, "storage_ready"), 0);
            CHECK_EQ(stat_value(response, "storage_device_0_read_errors"), 1);
            CHECK_EQ(stat_value(response, "storage_device_0_write_errors"), 1);
        }
    }
    fs::remove_all(base);
}
