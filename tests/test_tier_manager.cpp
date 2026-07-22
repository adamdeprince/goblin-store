#include "mini_test.hpp"

#include "goblin/core/reactor.hpp"
#include "goblin/crypto/sha256.hpp"
#include "goblin/storage/index.hpp"
#include "goblin/storage/tier_manager.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <print>
#include <set>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#if defined(__linux__)
#include <sys/stat.h>
#include <sys/statvfs.h>
#endif

namespace fs = std::filesystem;
using namespace goblin;
using namespace goblin::storage;
using goblin::crypto::hash_key;

TEST("pool: filesystem capacity reports space and inode gauges once per filesystem") {
    const std::string base =
        (fs::temp_directory_path() / ("goblin-pool-capacity-" + std::to_string(::getpid())))
            .string();
    fs::create_directories(base + "/a");
    fs::create_directories(base + "/b");
    {
        auto opened = Pool::open({base + "/a", base + "/b"}, 64 * KiB);
        CHECK(opened.has_value());
        if (opened) {
            const auto capacity = opened->filesystem_capacity();
            CHECK_EQ(capacity.size(), std::size_t(1));
            if (!capacity.empty()) {
                const auto& value = capacity.front();
                CHECK(value.total_bytes > 0);
                CHECK(value.allocated_bytes <= value.total_bytes);
                CHECK(value.free_bytes <= value.total_bytes);
                CHECK(value.available_bytes <= value.free_bytes);
                CHECK_EQ(value.allocated_bytes + value.free_bytes, value.total_bytes);
                CHECK(value.free_inodes <= value.total_inodes);
                CHECK(value.available_inodes <= value.free_inodes);
                CHECK_EQ(value.used_inodes + value.free_inodes, value.total_inodes);
            }
        }
    }
    fs::remove_all(base);
}

TEST("pool: Linux reservation keeps size zero and Pool moves preserve O_DIRECT") {
#if defined(__linux__)
    const std::string base =
        (fs::path("/var/tmp") / ("goblin-pool-reserve-" + std::to_string(::getpid()))).string();
    fs::create_directories(base);
    auto opened = Pool::open({base}, 64 * KiB, /*direct_io=*/true);
    CHECK(opened.has_value());
    if (!opened) {
        fs::remove_all(base);
        return;
    }
    Pool moved(std::move(*opened));
    auto reassigned = Pool::open({base}, 64 * KiB, /*direct_io=*/false);
    CHECK(reassigned.has_value());
    if (!reassigned) {
        fs::remove_all(base);
        return;
    }
    *reassigned = std::move(moved);

    constexpr Size extent = 100 * KiB;
    const auto digest = hash_key("/pool/reservation");
    {
        auto files = reassigned->open_object(digest, extent, /*create=*/true, 1);
        if (!files && files.error().code == Errc::io_error) {
            std::println("    (skipped: O_DIRECT unavailable on /var/tmp)");
            fs::remove_all(base);
            return;
        }
        CHECK(files.has_value());
        if (files) {
            const int flags = ::fcntl(files->fds()[0], F_GETFL);
            CHECK(flags >= 0 && (flags & O_DIRECT) != 0);
            const auto reserved = reassigned->reserve_object(digest, extent, *files);
            CHECK(reserved.has_value());
            struct stat st {};
            CHECK(::fstat(files->fds()[0], &st) == 0);
            CHECK_EQ(st.st_size, off_t(0)); // KEEP_SIZE reserves blocks without publishing a length
            CHECK(static_cast<Size>(st.st_blocks) * 512 >= extent);
        }
    }
    reassigned->unlink_object(digest, extent, 1);
    fs::remove_all(base);
#else
    std::println("    (skipped: fallocate/O_DIRECT reservation probe is Linux-only)");
#endif
}

TEST("pool: immutable read descriptors are reused through the shared cache") {
    const std::string base =
        (fs::temp_directory_path() / ("goblin-pool-fds-" + std::to_string(::getpid()))).string();
    fs::create_directories(base);
    auto handles = FileHandleCache::create(4);
    CHECK(handles.has_value());
    if (!handles) {
        fs::remove_all(base);
        return;
    }
    auto opened = Pool::open({base}, 64 * KiB, /*direct_io=*/false, *handles);
    CHECK(opened.has_value());
    if (!opened) {
        fs::remove_all(base);
        return;
    }
    const auto digest = hash_key("/pool/fd-cache");
    {
        auto created = opened->open_object(digest, 128 * KiB, /*create=*/true, 1);
        CHECK(created.has_value());
    }
    int first_fd = -1;
    {
        auto first = opened->open_object(digest, 128 * KiB, /*create=*/false, 1);
        CHECK(first.has_value());
        if (first) first_fd = first->fds()[0];
    }
    {
        auto second = opened->open_object(digest, 128 * KiB, /*create=*/false, 1);
        CHECK(second.has_value());
        if (second) CHECK_EQ(second->fds()[0], first_fd);
    }
    const auto stats = opened->file_handle_cache_stats();
    CHECK_EQ(stats.cached, std::size_t{1});
    CHECK_EQ(stats.misses, std::uint64_t{1});
    CHECK_EQ(stats.hits, std::uint64_t{1});
    opened->unlink_object(digest, 128 * KiB, 1);
    CHECK_EQ(opened->file_handle_cache_stats().cached, std::size_t{0});
    fs::remove_all(base);
}

TEST("tier_manager: store across SSD prefix + HDD tail, read back through io_uring") {
    if (!core::Reactor::available()) {
        std::println("    (skipped: built without liburing)");
        return;
    }
    auto reactor = core::Reactor::create();
    if (!reactor) {
        std::println("    (skipped: io_uring unavailable: {})", reactor.error().detail);
        return;
    }

    const std::string base =
        (fs::temp_directory_path() / ("goblin-tier-" + std::to_string(::getpid()))).string();
    PoolConfig ssd, hdd;
    ssd.stripe_unit = 4 * KiB;
    hdd.stripe_unit = 4 * KiB;
    for (const char* d : {"/ssd0", "/ssd1"}) ssd.dirs.push_back(base + d);
    for (const char* d : {"/hdd0", "/hdd1"}) hdd.dirs.push_back(base + d);
    for (const auto& d : ssd.dirs) fs::create_directories(d);
    for (const auto& d : hdd.dirs) fs::create_directories(d);

    TierSizes tiers;
    tiers.ram_head = 4 * KiB;
    tiers.ssd_prefix = 16 * KiB; // -> 40 KiB object = 16 KiB SSD prefix + 24 KiB HDD tail

    MemoryConfig mem;
    mem.total_bytes = 4 * MiB;
    mem.block_bytes = 256 * KiB;
    mem.lock_memory = false; // tests don't need (and CI can't always) mlock

    EvictionConfig ev;
    Index index;
    auto tm = TierManager::open(tiers, mem, ev, ssd, hdd, index);
    CHECK(tm.has_value());
    CHECK(tm->three_layer());

    const auto digest = hash_key("/big.bin");
    const Size sz = 40 * KiB;
    std::vector<std::byte> data(sz);
    for (Size i = 0; i < sz; ++i) data[i] = static_cast<std::byte>((i * 131 + 17) & 0xFF);

    CHECK(tm->store(digest, ByteView(data.data(), sz), /*flags=*/42).has_value());
    const auto meta = index.lookup(digest);
    CHECK(meta.has_value());
    CHECK_EQ(meta->size, Size(sz));
    CHECK_EQ(meta->flags, std::uint32_t(42));
    CHECK(meta->head.resident()); // head write-through cached in RAM (ADR-0003)
    CHECK_EQ(meta->head.len, std::uint32_t(4 * KiB));

    // Full read-back (spans SSD prefix + HDD tail).
    std::vector<std::byte> got(sz);
    const auto n = tm->read(*reactor, digest, 0, MutBytes(got.data(), sz));
    CHECK(n.has_value());
    if (n) {
        CHECK_EQ(*n, std::size_t(sz));
        CHECK(got == data);
    }

    // Range crossing the 16 KiB tier boundary: [12 KiB, 20 KiB) = SSD [12,16) + HDD [16,20).
    const Size off = 12 * KiB, len = 8 * KiB;
    std::vector<std::byte> sub(len);
    const auto m = tm->read(*reactor, digest, off, MutBytes(sub.data(), len));
    CHECK(m.has_value());
    if (m) {
        CHECK_EQ(*m, std::size_t(len));
        CHECK(std::equal(sub.begin(), sub.end(), data.begin() + static_cast<std::ptrdiff_t>(off)));
    }

    // A read fully inside the head [0, 2 KiB) is served straight from the RAM cache.
    std::vector<std::byte> headbuf(2 * KiB);
    const auto hn = tm->read(*reactor, digest, 0, MutBytes(headbuf.data(), headbuf.size()));
    CHECK(hn.has_value());
    if (hn) {
        CHECK_EQ(*hn, std::size_t(2 * KiB));
        CHECK(std::equal(headbuf.begin(), headbuf.end(), data.begin()));
    }

    // Missing key -> not_found.
    std::vector<std::byte> none(16);
    CHECK(!tm->read(*reactor, hash_key("/nope"), 0, MutBytes(none.data(), none.size())).has_value());

    fs::remove_all(base);
}

TEST("tier_manager: head cache stays bounded under many stores; all objects still readable") {
    if (!core::Reactor::available()) {
        std::println("    (skipped: built without liburing)");
        return;
    }
    auto reactor = core::Reactor::create();
    if (!reactor) {
        std::println("    (skipped: io_uring unavailable: {})", reactor.error().detail);
        return;
    }

    const std::string base =
        (fs::temp_directory_path() / ("goblin-evict-" + std::to_string(::getpid()))).string();
    PoolConfig ssd, hdd; // 2-layer (no HDD)
    ssd.stripe_unit = 64 * KiB;
    for (const char* d : {"/s0", "/s1"}) ssd.dirs.push_back(base + d);
    for (const auto& d : ssd.dirs) fs::create_directories(d);

    TierSizes tiers;
    tiers.ram_head = 16 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 256 * KiB; // holds ~16 heads of 16 KiB
    mem.block_bytes = 64 * KiB;
    mem.lock_memory = false;
    EvictionConfig ev; // s3fifo

    Index index;
    auto tm = TierManager::open(tiers, mem, ev, ssd, hdd, index);
    CHECK(tm.has_value());

    const int kN = 60;
    const Size sz = 32 * KiB; // > ram_head (16 KiB): disk-backed, so an evicted head re-reads from SSD
    auto pattern = [](int i) {
        std::vector<std::byte> v(sz);
        for (Size j = 0; j < v.size(); ++j) v[j] = static_cast<std::byte>((j + std::size_t(i) * 7) & 0xFF);
        return v;
    };
    for (int i = 0; i < kN; ++i) {
        const auto d = pattern(i);
        CHECK(tm->store(hash_key("/obj/" + std::to_string(i)), ByteView(d.data(), d.size()), 0)
                  .has_value());
    }

    // 60 heads stored into a pool that holds ~16 -> eviction kept RAM bounded.
    CHECK(tm->head_resident() <= std::size_t(16));
    CHECK_EQ(index.size(), std::size_t(kN)); // every object still tracked

    // All objects still read back correctly (resident heads from RAM, evicted ones re-read from SSD).
    for (const int i : {0, kN / 2, kN - 1}) {
        const auto want = pattern(i);
        std::vector<std::byte> got(sz);
        const auto n = tm->read(*reactor, hash_key("/obj/" + std::to_string(i)), 0,
                                MutBytes(got.data(), got.size()));
        CHECK(n.has_value());
        if (n) {
            CHECK_EQ(*n, std::size_t(sz));
            CHECK(got == want);
        }
    }
    fs::remove_all(base);
}

TEST("tier_manager: small objects (<= ram_head) are RAM-only -- no SSD copy; head-evict drops them") {
    if (!core::Reactor::available()) {
        std::println("    (skipped: built without liburing)");
        return;
    }
    auto reactor = core::Reactor::create();
    if (!reactor) {
        std::println("    (skipped: io_uring unavailable: {})", reactor.error().detail);
        return;
    }

    const std::string base =
        (fs::temp_directory_path() / ("goblin-ramonly-" + std::to_string(::getpid()))).string();
    PoolConfig ssd, hdd; // 2-layer (no HDD)
    ssd.stripe_unit = 64 * KiB;
    for (const char* d : {"/s0"}) ssd.dirs.push_back(base + d);
    for (const auto& d : ssd.dirs) fs::create_directories(d);

    TierSizes tiers;
    tiers.ram_head = 16 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 256 * KiB; // holds ~16 heads of 16 KiB
    mem.block_bytes = 64 * KiB;
    mem.lock_memory = false;
    EvictionConfig ev; // s3fifo

    Index index;
    auto tm = TierManager::open(tiers, mem, ev, ssd, hdd, index);
    CHECK(tm.has_value());

    const int kN = 60;
    const Size sz = 16 * KiB; // == ram_head -> RAM-only (no disk copy)
    auto pattern = [](int i) {
        std::vector<std::byte> v(sz);
        for (Size j = 0; j < v.size(); ++j) v[j] = static_cast<std::byte>((j + std::size_t(i) * 5) & 0xFF);
        return v;
    };
    for (int i = 0; i < kN; ++i) {
        const auto d = pattern(i);
        CHECK(tm->store(hash_key("/s/" + std::to_string(i)), ByteView(d.data(), d.size()), 0).has_value());
    }

    // RAM-only: the head is the only copy, so head-evict == object-evict. The live count is bounded by
    // the head cache (NOT kN), and every live object has a resident head.
    CHECK(index.size() < std::size_t(kN)); // eviction happened
    CHECK(index.size() <= std::size_t(16));
    CHECK_EQ(tm->head_resident(), index.size());

    // The whole point: small objects wrote NO per-object SSD files (vs the old redundant disk copy).
    std::size_t files = 0;
    for (const auto& e : fs::recursive_directory_iterator(base))
        if (e.is_regular_file()) ++files;
    CHECK_EQ(files, std::size_t(0));

    // A survivor still reads back correctly from RAM.
    bool any = false;
    for (int i = kN - 1; i >= 0 && !any; --i) {
        std::vector<std::byte> got(sz);
        const auto n = tm->read(*reactor, hash_key("/s/" + std::to_string(i)), 0,
                                MutBytes(got.data(), got.size()));
        if (n && *n == sz) {
            any = true;
            CHECK(got == pattern(i));
        }
    }
    CHECK(any);
    fs::remove_all(base);
}

TEST("tier_manager: sliding compaction reclaims fragmented small heads without evicting survivors") {
    if (!core::Reactor::available()) {
        std::println("    (skipped: built without liburing)");
        return;
    }
    auto reactor = core::Reactor::create();
    if (!reactor) {
        std::println("    (skipped: io_uring unavailable: {})", reactor.error().detail);
        return;
    }

    const std::string base =
        (fs::temp_directory_path() / ("goblin-compact-" + std::to_string(::getpid()))).string();
    PoolConfig ssd, hdd; // 2-layer; objects are RAM-only so no files are ever written
    ssd.stripe_unit = 64 * KiB;
    ssd.dirs.push_back(base + "/s0");
    for (const auto& d : ssd.dirs) fs::create_directories(d);

    TierSizes tiers;
    tiers.ram_head = 4 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 256 * KiB; // 4 blocks x 64 KiB
    mem.block_bytes = 64 * KiB;
    mem.lock_memory = false;
    EvictionConfig ev;

    Index index;
    auto tm = TierManager::open(tiers, mem, ev, ssd, hdd, index);
    CHECK(tm.has_value());

    const int kFill = 64;    // 256 KiB / 4 KiB -> fills RAM exactly, no eviction
    const Size sz = 4 * KiB; // == ram_head -> RAM-only (16-aligned, so packs with zero waste)
    auto pattern = [](int i) {
        std::vector<std::byte> v(sz);
        for (Size j = 0; j < v.size(); ++j)
            v[j] = static_cast<std::byte>((j * 7 + std::size_t(i) * 131) & 0xFF);
        return v;
    };
    auto key = [](int i) { return hash_key("/c/" + std::to_string(i)); };

    for (int i = 0; i < kFill; ++i) {
        const auto d = pattern(i);
        CHECK(tm->store(key(i), ByteView(d.data(), d.size()), 0).has_value());
    }
    CHECK_EQ(index.size(), std::size_t(kFill)); // everything fit -- no eviction on fill

    // Fragment: remove every other object. Each 64 KiB block keeps 8 of its 16 heads, so no block fully
    // drains -- the bump arenas are riddled with dead holes they can't reuse without compaction.
    for (int i = 0; i < kFill; i += 2) CHECK(tm->remove(key(i)));
    CHECK_EQ(index.size(), std::size_t(kFill / 2));

    // Refill with kFill/2 new objects. Every block is bump-full (frontiers never rewound), so the first
    // admission fails to allocate and triggers sliding compaction, which squeezes out the holes in
    // place. If that works, the new objects fit in the reclaimed space and NO odd survivor is evicted.
    for (int i = kFill; i < kFill + kFill / 2; ++i) {
        const auto d = pattern(i);
        CHECK(tm->store(key(i), ByteView(d.data(), d.size()), 0).has_value());
    }
    CHECK_EQ(index.size(), std::size_t(kFill));  // 32 survivors + 32 new, still no eviction
    CHECK_EQ(tm->head_resident(), index.size());

    // Every odd survivor still reads back correctly -- from its NEW, compaction-rewritten HeadLoc.
    for (int i = 1; i < kFill; i += 2) {
        std::vector<std::byte> got(sz);
        const auto n = tm->read(*reactor, key(i), 0, MutBytes(got.data(), got.size()));
        CHECK(n.has_value() && *n == sz);
        CHECK(got == pattern(i));
    }
    // ...and so do the newly admitted objects.
    for (int i = kFill; i < kFill + kFill / 2; ++i) {
        std::vector<std::byte> got(sz);
        const auto n = tm->read(*reactor, key(i), 0, MutBytes(got.data(), got.size()));
        CHECK(n.has_value() && *n == sz);
        CHECK(got == pattern(i));
    }
    // Still RAM-only: no per-object SSD files were written.
    std::size_t files = 0;
    for (const auto& e : fs::recursive_directory_iterator(base))
        if (e.is_regular_file()) ++files;
    CHECK_EQ(files, std::size_t(0));

    fs::remove_all(base);
}

TEST("tier_manager: fixed-head pressure cannot borrow or evict from the split small pool") {
    const std::string base =
        (fs::temp_directory_path() / ("goblin-split-fixed-pressure-" +
                                      std::to_string(::getpid())))
            .string();
    PoolConfig ssd, hdd;
    ssd.stripe_unit = 64 * KiB;
    ssd.dirs.push_back(base + "/ssd");
    fs::create_directories(ssd.dirs.front());

    TierSizes tiers;
    tiers.ram_head = 16 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 64 * KiB;             // one fixed-head block: four 16 KiB heads
    mem.small_total_bytes = 128 * KiB;      // two separate small-object blocks
    mem.block_bytes = 64 * KiB;
    mem.lock_memory = false;
    mem.use_hugepages = false;
    EvictionConfig ev;
    Index index;
    auto tm = TierManager::open(tiers, mem, ev, ssd, hdd, index, 4 * KiB, 1,
                                /*direct_io=*/false);
    CHECK(tm.has_value());
    if (!tm) {
        fs::remove_all(base);
        return;
    }

    // Leave most of the small pool idle. A fifth fixed head must evict another fixed head rather
    // than consume that spare small-object capacity, and the small residents must be untouched.
    std::vector<Digest> small_keys;
    std::vector<ObjectMeta> small_before;
    const std::vector<std::byte> small_value(8 * KiB, std::byte{0x51});
    for (int i = 0; i < 2; ++i) {
        const auto key = hash_key("/split/fixed-pressure/small/" + std::to_string(i));
        CHECK(tm->store(key, ByteView(small_value.data(), small_value.size()), 0x51).has_value());
        small_keys.push_back(key);
        const auto meta = index.lookup(key);
        CHECK(meta.has_value());
        if (meta) small_before.push_back(*meta);
    }
    CHECK_EQ(small_before.size(), small_keys.size());

    std::vector<Digest> fixed_keys;
    const std::vector<std::byte> fixed_value(tiers.ram_head, std::byte{0xA4});
    for (int i = 0; i < 4; ++i) {
        const auto key = hash_key("/split/fixed-pressure/head/" + std::to_string(i));
        CHECK(tm->store(key, ByteView(fixed_value.data(), fixed_value.size()), 0xA4).has_value());
        fixed_keys.push_back(key);
    }
    CHECK_EQ(tm->head_resident(), std::size_t(6));

    const auto overflow = hash_key("/split/fixed-pressure/head/overflow");
    CHECK(tm->store(overflow, ByteView(fixed_value.data(), fixed_value.size()), 0xA5).has_value());
    fixed_keys.push_back(overflow);

    std::size_t fixed_live = 0;
    for (const auto& key : fixed_keys)
        if (index.contains(key)) ++fixed_live;
    CHECK_EQ(fixed_live, std::size_t(4));
    CHECK(index.contains(overflow));
    CHECK(std::any_of(fixed_keys.begin(), fixed_keys.end() - 1,
                      [&](const Digest& key) { return !index.contains(key); }));

    for (std::size_t i = 0; i < small_keys.size() && i < small_before.size(); ++i) {
        const auto after = index.lookup(small_keys[i]);
        CHECK(after.has_value());
        if (!after) continue;
        CHECK_EQ(after->etag, small_before[i].etag);
        CHECK_EQ(after->head.block, small_before[i].head.block);
        CHECK_EQ(after->head.offset, small_before[i].head.offset);
        CHECK_EQ(after->head.len, small_before[i].head.len);
    }
    CHECK_EQ(tm->head_resident(), std::size_t(6)); // four fixed heads plus both small residents
    fs::remove_all(base);
}

TEST("tier_manager: fractional pressure cannot borrow or evict from the split head pool") {
    const std::string base =
        (fs::temp_directory_path() / ("goblin-split-small-pressure-" +
                                      std::to_string(::getpid())))
            .string();
    PoolConfig ssd, hdd;
    ssd.stripe_unit = 64 * KiB;
    ssd.dirs.push_back(base + "/ssd");
    fs::create_directories(ssd.dirs.front());

    TierSizes tiers;
    tiers.ram_head = 16 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 128 * KiB;        // two fixed-head blocks, mostly deliberately idle
    mem.small_total_bytes = 64 * KiB;   // one small-object arena: eight 8 KiB objects
    mem.block_bytes = 64 * KiB;
    mem.lock_memory = false;
    mem.use_hugepages = false;
    EvictionConfig ev;
    Index index;
    auto tm = TierManager::open(tiers, mem, ev, ssd, hdd, index, 4 * KiB, 1,
                                /*direct_io=*/false);
    CHECK(tm.has_value());
    if (!tm) {
        fs::remove_all(base);
        return;
    }

    // Leave ample fixed-head capacity idle. Small-object overflow must reclaim from its own arena,
    // never borrow a fixed block and never select a fixed resident as its victim.
    std::vector<Digest> fixed_keys;
    std::vector<ObjectMeta> fixed_before;
    const std::vector<std::byte> fixed_value(tiers.ram_head, std::byte{0x62});
    for (int i = 0; i < 2; ++i) {
        const auto key = hash_key("/split/small-pressure/head/" + std::to_string(i));
        CHECK(tm->store(key, ByteView(fixed_value.data(), fixed_value.size()), 0x62).has_value());
        fixed_keys.push_back(key);
        const auto meta = index.lookup(key);
        CHECK(meta.has_value());
        if (meta) fixed_before.push_back(*meta);
    }
    CHECK_EQ(fixed_before.size(), fixed_keys.size());

    std::vector<Digest> small_keys;
    const std::vector<std::byte> small_value(8 * KiB, std::byte{0xB7});
    for (int i = 0; i < 8; ++i) {
        const auto key = hash_key("/split/small-pressure/small/" + std::to_string(i));
        CHECK(tm->store(key, ByteView(small_value.data(), small_value.size()), 0xB7).has_value());
        small_keys.push_back(key);
    }
    CHECK_EQ(tm->head_resident(), std::size_t(10));

    const auto overflow = hash_key("/split/small-pressure/small/overflow");
    CHECK(tm->store(overflow, ByteView(small_value.data(), small_value.size()), 0xB8).has_value());
    small_keys.push_back(overflow);

    std::size_t small_live = 0;
    for (const auto& key : small_keys)
        if (index.contains(key)) ++small_live;
    CHECK_EQ(small_live, std::size_t(8));
    CHECK(index.contains(overflow));
    CHECK(std::any_of(small_keys.begin(), small_keys.end() - 1,
                      [&](const Digest& key) { return !index.contains(key); }));

    for (std::size_t i = 0; i < fixed_keys.size() && i < fixed_before.size(); ++i) {
        const auto after = index.lookup(fixed_keys[i]);
        CHECK(after.has_value());
        if (!after) continue;
        CHECK_EQ(after->etag, fixed_before[i].etag);
        CHECK_EQ(after->head.block, fixed_before[i].head.block);
        CHECK_EQ(after->head.offset, fixed_before[i].head.offset);
        CHECK_EQ(after->head.len, fixed_before[i].head.len);
    }
    CHECK_EQ(tm->head_resident(), small_live + fixed_keys.size());
    fs::remove_all(base);
}

TEST("tier_manager: a committed RAM-only StoreHandle rejects a second commit without mutation") {
    const std::string base =
        (fs::temp_directory_path() / ("goblin-double-commit-" + std::to_string(::getpid())))
            .string();
    PoolConfig ssd, hdd;
    ssd.stripe_unit = 64 * KiB;
    ssd.dirs.push_back(base + "/ssd");
    fs::create_directories(ssd.dirs.front());

    TierSizes tiers;
    tiers.ram_head = 64 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 64 * KiB;
    mem.block_bytes = 64 * KiB;
    mem.lock_memory = false;
    mem.use_hugepages = false;
    EvictionConfig ev;
    AccessScoreConfig score;
    score.increment = 2.0;
    Index index;
    auto tm = TierManager::open(tiers, mem, ev, ssd, hdd, index, 256 * KiB, 8,
                                /*direct_io=*/false, score);
    CHECK(tm.has_value());
    if (!tm) {
        fs::remove_all(base);
        return;
    }

    const auto key = hash_key("/store-handle/double-commit");
    std::vector<std::byte> value(tiers.ram_head, std::byte{0x6D});
    auto pending = tm->begin_store(key, value.size());
    CHECK(pending.has_value());
    if (!pending) {
        fs::remove_all(base);
        return;
    }
    CHECK(pending->write(ByteView(value.data(), value.size())).has_value());
    CHECK(pending->commit(17).has_value());
    tm->touch(key);
    tm->touch(key);

    const auto before = index.lookup(key);
    CHECK(before.has_value());
    const auto before_score = tm->access_score(key);
    CHECK(before_score.has_value());
    CHECK(before && before->head.resident());
    CHECK_EQ(*before_score, 4.0);

    // A StoreHandle is one-shot. Re-entering publication must fail before it allocates a new ETag,
    // republishes the same score slot, or frees the head that the live Index entry still names.
    const auto again = pending->commit(99);
    CHECK(!again.has_value());

    const auto after = index.lookup(key);
    CHECK(after.has_value());
    if (before && after) {
        CHECK_EQ(after->size, before->size);
        CHECK_EQ(after->flags, before->flags);
        CHECK_EQ(after->expiry, before->expiry);
        CHECK_EQ(after->etag, before->etag);
        CHECK_EQ(after->head.block, before->head.block);
        CHECK_EQ(after->head.offset, before->head.offset);
        CHECK_EQ(after->head.len, before->head.len);
    }
    CHECK_EQ(tm->head_resident(), std::size_t(1));
    CHECK_EQ(*tm->access_score(key), *before_score);

    // Snapshot without record_access verifies the publication's bytes while leaving its score
    // untouched. In particular, the failed call must not have returned the live head to the pool.
    auto snapshot = tm->open_snapshot(key, /*record_access=*/false);
    CHECK(snapshot.has_value());
    CHECK(snapshot && snapshot->pin.valid);
    if (snapshot && snapshot->pin.valid) {
        const auto bytes = tm->pinned_bytes(snapshot->pin);
        CHECK_EQ(bytes.size(), value.size());
        CHECK(std::equal(bytes.begin(), bytes.end(), value.begin()));
        tm->unpin_head(snapshot->pin);
    }
    CHECK_EQ(*tm->access_score(key), *before_score);
    fs::remove_all(base);
}

TEST("tier_manager: small compaction preserves an in-flight fractional reservation") {
    const std::string base =
        (fs::temp_directory_path() / ("goblin-compact-pending-" + std::to_string(::getpid())))
            .string();
    PoolConfig ssd, hdd;
    ssd.stripe_unit = 64 * KiB;
    ssd.dirs.push_back(base + "/ssd");
    fs::create_directories(ssd.dirs.front());

    TierSizes tiers;
    tiers.ram_head = 8 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 128 * KiB; // two 64 KiB fractional-head arenas
    mem.block_bytes = 64 * KiB;
    mem.lock_memory = false;
    mem.use_hugepages = false;
    EvictionConfig ev;
    Index index;
    auto tm = TierManager::open(tiers, mem, ev, ssd, hdd, index, 256 * KiB, 8,
                                /*direct_io=*/false);
    CHECK(tm.has_value());
    if (!tm) {
        fs::remove_all(base);
        return;
    }

    constexpr Size kSize = 4 * KiB; // fractional: strictly smaller than ram_head -> bump arena
    auto key = [](std::string_view name) { return hash_key(name); };
    auto value = [](unsigned tag) {
        return std::vector<std::byte>(kSize, static_cast<std::byte>(tag));
    };
    auto put = [&](std::string_view name, unsigned tag) {
        const auto bytes = value(tag);
        CHECK(tm->store(key(name), ByteView(bytes.data(), bytes.size()), tag).has_value());
    };

    // Put the pending reservation between published slots in block 0. A hole before it makes an
    // unaware sliding compactor copy a later published slot directly over the pending bytes.
    put("/compact-pending/a0", 0x10); // block 0, offset 0
    put("/compact-pending/a1", 0x11); // block 0, offset 4 KiB
    const auto pending_key = key("/compact-pending/in-flight");
    const auto pending_value = value(0xE1);
    auto pending = tm->begin_store(pending_key, pending_value.size()); // block 0, offset 8 KiB
    CHECK(pending.has_value());
    if (!pending) {
        fs::remove_all(base);
        return;
    }
    CHECK(pending->write(ByteView(pending_value.data(), pending_value.size())).has_value());

    std::vector<std::pair<Digest, std::byte>> expected;
    expected.emplace_back(key("/compact-pending/a1"), std::byte{0x11});
    for (unsigned i = 2; i < 15; ++i) {
        const auto name = "/compact-pending/a" + std::to_string(i);
        put(name, 0x10 + i);
        expected.emplace_back(key(name), static_cast<std::byte>(0x10 + i));
    }
    for (unsigned i = 0; i < 16; ++i) {
        const auto name = "/compact-pending/b" + std::to_string(i);
        put(name, 0x40 + i);
        if (i != 0) expected.emplace_back(key(name), static_cast<std::byte>(0x40 + i));
    }

    const auto a1_before = index.lookup(key("/compact-pending/a1"));
    const auto a2_before = index.lookup(key("/compact-pending/a2"));
    const auto b0_before = index.lookup(key("/compact-pending/b0"));
    CHECK(a1_before && a2_before && b0_before);
    if (a1_before && a2_before && b0_before) {
        CHECK_EQ(a1_before->head.block, 0u);
        CHECK_EQ(a1_before->head.offset, std::uint32_t(4 * KiB));
        CHECK_EQ(a2_before->head.block, 0u);
        CHECK_EQ(a2_before->head.offset, std::uint32_t(12 * KiB));
        CHECK_EQ(b0_before->head.block, 1u);
        CHECK_EQ(b0_before->head.offset, std::uint32_t(0));
    }

    // Both arena frontiers are full. Leave one hole in each block; the second block gives a safe
    // compaction target if the block containing the in-flight reservation is correctly excluded.
    CHECK(tm->remove(key("/compact-pending/a0")));
    CHECK(tm->remove(key("/compact-pending/b0")));

    const auto competitor_key = key("/compact-pending/competitor");
    const auto competitor_value = value(0xF2);
    CHECK(tm->store(competitor_key,
                    ByteView(competitor_value.data(), competitor_value.size()), 0xF2)
              .has_value()); // allocation failure above the full frontiers triggers compact_small()

    // The pending handle still owns distinct, valid bytes after the competing admission completes.
    CHECK(pending->commit(0xE1).has_value());
    expected.emplace_back(competitor_key, std::byte{0xF2});
    expected.emplace_back(pending_key, std::byte{0xE1});
    CHECK_EQ(index.size(), expected.size());
    CHECK_EQ(tm->head_resident(), expected.size());

    std::vector<std::uint64_t> locations;
    locations.reserve(expected.size());
    for (const auto& [digest, expected_byte] : expected) {
        const auto meta = index.lookup(digest);
        CHECK(meta && meta->head.resident());
        if (meta)
            locations.push_back((static_cast<std::uint64_t>(meta->head.block) << 32) |
                                meta->head.offset);

        auto snapshot = tm->open_snapshot(digest, /*record_access=*/false);
        CHECK(snapshot && snapshot->pin.valid);
        if (snapshot && snapshot->pin.valid) {
            const auto bytes = tm->pinned_bytes(snapshot->pin);
            CHECK_EQ(bytes.size(), std::size_t(kSize));
            CHECK(std::all_of(bytes.begin(), bytes.end(),
                              [expected_byte](std::byte b) { return b == expected_byte; }));
            tm->unpin_head(snapshot->pin);
        }
    }
    std::sort(locations.begin(), locations.end());
    CHECK(std::adjacent_find(locations.begin(), locations.end()) == locations.end());
    fs::remove_all(base);
}

TEST("tier_manager: streaming begin_store / write(chunks) / commit round-trips") {
    if (!core::Reactor::available()) {
        std::println("    (skipped: built without liburing)");
        return;
    }
    auto reactor = core::Reactor::create();
    if (!reactor) {
        std::println("    (skipped: io_uring unavailable: {})", reactor.error().detail);
        return;
    }

    const std::string base =
        (fs::temp_directory_path() / ("goblin-stream-" + std::to_string(::getpid()))).string();
    PoolConfig ssd, hdd; // 2-layer
    ssd.stripe_unit = 64 * KiB;
    for (const char* d : {"/s0", "/s1"}) ssd.dirs.push_back(base + d);
    for (const auto& d : ssd.dirs) fs::create_directories(d);

    TierSizes tiers;
    tiers.ram_head = 8 * KiB; // head spans several 7 KiB write chunks
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 1 * MiB;
    mem.block_bytes = 64 * KiB;
    mem.lock_memory = false;
    EvictionConfig ev;

    Index index;
    auto tm = TierManager::open(tiers, mem, ev, ssd, hdd, index);
    CHECK(tm.has_value());

    const Size sz = 50 * KiB;
    std::vector<std::byte> data(sz);
    for (Size i = 0; i < sz; ++i) data[i] = static_cast<std::byte>((i * 53 + 3) & 0xFF);

    const auto digest = hash_key("/streamed.bin");
    auto h = tm->begin_store(digest, sz);
    CHECK(h.has_value());
    for (Size off = 0; off < sz;) {
        const Size n = std::min<Size>(7 * KiB, sz - off);
        CHECK(h->write(ByteView(data.data() + off, n)).has_value());
        off += n;
    }
    CHECK(h->commit(123).has_value());

    const auto m = index.lookup(digest);
    CHECK(m.has_value());
    CHECK_EQ(m->size, Size(sz));
    CHECK_EQ(m->flags, std::uint32_t(123));

    std::vector<std::byte> got(sz);
    const auto rn = tm->read(*reactor, digest, 0, MutBytes(got.data(), got.size()));
    CHECK(rn.has_value());
    if (rn) {
        CHECK_EQ(*rn, std::size_t(sz));
        CHECK(got == data);
    }
    fs::remove_all(base);
}

TEST("tier_manager: rejects oversized lengths before layout arithmetic or allocation") {
    const std::string base =
        (fs::temp_directory_path() / ("goblin-too-large-" + std::to_string(::getpid()))).string();
    PoolConfig ssd, hdd;
    ssd.stripe_unit = 64 * KiB;
    ssd.dirs.push_back(base + "/ssd");
    fs::create_directories(ssd.dirs.front());

    TierSizes tiers;
    tiers.ram_head = 4 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 64 * KiB;
    mem.block_bytes = 64 * KiB;
    mem.lock_memory = false;
    mem.use_hugepages = false;
    EvictionConfig ev;
    Index index;
    auto tm = TierManager::open(tiers, mem, ev, ssd, hdd, index);
    CHECK(tm.has_value());
    if (!tm) {
        fs::remove_all(base);
        return;
    }

    const auto just_over = tm->begin_store(hash_key("/too-large"), kMaxObjectSize + 1);
    CHECK(!just_over.has_value());
    if (!just_over) CHECK_EQ(just_over.error().code, Errc::too_large);
    const auto overflowing =
        tm->begin_store(hash_key("/size-max"), std::numeric_limits<Size>::max());
    CHECK(!overflowing.has_value());
    if (!overflowing) CHECK_EQ(overflowing.error().code, Errc::too_large);
    CHECK_EQ(index.size(), std::size_t(0));

    std::size_t files = 0;
    for (const auto& entry : fs::recursive_directory_iterator(base))
        if (entry.is_regular_file()) ++files;
    CHECK_EQ(files, std::size_t(0));
    fs::remove_all(base);
}

TEST("tier_manager: configurable object limit is enforced by common storage admission") {
    const std::string base =
        (fs::temp_directory_path() / ("goblin-object-limit-" + std::to_string(::getpid()))).string();
    PoolConfig ssd, hdd;
    ssd.stripe_unit = 64 * KiB;
    ssd.dirs.push_back(base + "/ssd");
    fs::create_directories(ssd.dirs.front());
    TierSizes tiers;
    tiers.ram_head = 4 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 64 * KiB;
    mem.block_bytes = 64 * KiB;
    mem.lock_memory = false;
    mem.use_hugepages = false;
    Index index;
    auto tm = TierManager::open(tiers, mem, EvictionConfig{}, ssd, hdd, index,
                                64 * KiB, 2, false, {}, 0, 8 * KiB);
    CHECK(tm.has_value());
    if (tm) {
        CHECK_EQ(tm->max_object_size(), 8 * KiB);
        auto allowed = tm->begin_store(hash_key("/allowed"), 8 * KiB);
        CHECK(allowed.has_value());
        auto rejected = tm->begin_store(hash_key("/rejected"), 8 * KiB + 1);
        CHECK(!rejected.has_value());
        if (!rejected) CHECK_EQ(rejected.error().code, Errc::too_large);
    }
    fs::remove_all(base);
}

TEST("tier_manager: concurrent disk-backed generations isolate commit from abort") {
    const std::string base =
        (fs::temp_directory_path() / ("goblin-generation-abort-" + std::to_string(::getpid())))
            .string();
    PoolConfig ssd, hdd; // 2-layer; every object shard is immutable within the SSD pool
    ssd.stripe_unit = 64 * KiB;
    for (const char* d : {"/s0", "/s1", "/s2"}) ssd.dirs.push_back(base + d);
    for (const auto& d : ssd.dirs) fs::create_directories(d);

    TierSizes tiers;
    tiers.ram_head = 4 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 1 * MiB;
    mem.block_bytes = 64 * KiB;
    mem.lock_memory = false;
    EvictionConfig ev;
    Index index;
    auto tm = TierManager::open(tiers, mem, ev, ssd, hdd, index, 256 * KiB,
                                /*write_buffers=*/4, /*direct_io=*/false);
    CHECK(tm.has_value());
    if (!tm) {
        fs::remove_all(base);
        return;
    }

    constexpr Size SZ = 160 * KiB; // three stripes: one immutable shard on every configured drive
    const auto digest = hash_key("/same-key/concurrent-abort");
    std::vector<std::byte> committed(SZ, std::byte{0x31});
    std::vector<std::byte> aborted(SZ, std::byte{0xA2});
    std::optional<TierManager::StoreHandle> first;
    std::optional<TierManager::StoreHandle> second;
    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};
    std::atomic<unsigned> begin_failures{0};
    auto begin = [&](std::optional<TierManager::StoreHandle>& out) {
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        auto h = tm->begin_store(digest, SZ);
        if (h)
            out.emplace(std::move(*h));
        else
            begin_failures.fetch_add(1, std::memory_order_relaxed);
    };
    std::thread t1(begin, std::ref(first));
    std::thread t2(begin, std::ref(second));
    while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    t1.join();
    t2.join();

    CHECK_EQ(begin_failures.load(), 0u);
    CHECK(first.has_value());
    CHECK(second.has_value());
    if (!first || !second) {
        fs::remove_all(base);
        return;
    }

    const std::string prefix = digest.hex() + ".g";
    const auto generation_names = [&](const std::string& dir) {
        std::set<std::string> names;
        for (const auto& entry : fs::directory_iterator(dir)) {
            const std::string name = entry.path().filename().string();
            if (name.starts_with(prefix)) names.insert(name);
        }
        return names;
    };
    std::set<std::string> in_flight_names;
    for (const auto& dir : ssd.dirs) {
        const auto names = generation_names(dir);
        CHECK_EQ(names.size(), std::size_t(2));
        if (in_flight_names.empty())
            in_flight_names = names;
        else
            CHECK(names == in_flight_names);
    }
    CHECK_EQ(in_flight_names.size(), std::size_t(2));

    CHECK(first->write(ByteView(committed.data(), committed.size())).has_value());
    CHECK(second->write(ByteView(aborted.data(), aborted.size())).has_value());
    CHECK(first->commit(/*flags=*/0x31).has_value());
    const auto live = index.lookup(digest);
    CHECK(live.has_value());
    CHECK(live && live->file_generation != 0);
    CHECK(live && live->flags == std::uint32_t(0x31));
    const std::uint64_t committed_generation = live ? live->file_generation : 0;
    const std::string committed_name = prefix + std::to_string(committed_generation);

    second.reset(); // abort the other overlapping generation; it must not unlink the committed one
    for (const auto& dir : ssd.dirs) {
        const auto names = generation_names(dir);
        CHECK_EQ(names.size(), std::size_t(1));
        CHECK(names.contains(committed_name));
    }
    const auto after_abort = index.lookup(digest);
    CHECK(after_abort && after_abort->file_generation == committed_generation);
    CHECK(after_abort && after_abort->flags == std::uint32_t(0x31));
    const auto head = tm->head_view(digest);
    CHECK(head.has_value());
    if (head)
        CHECK(std::all_of(head->begin(), head->end(),
                          [](std::byte b) { return b == std::byte{0x31}; }));

    first.reset();
    fs::remove_all(base);
}

TEST("tier_manager: later concurrent disk generation atomically retires the earlier one") {
    const std::string base =
        (fs::temp_directory_path() / ("goblin-generation-commit-" + std::to_string(::getpid())))
            .string();
    PoolConfig ssd, hdd;
    ssd.stripe_unit = 64 * KiB;
    for (const char* d : {"/s0", "/s1"}) ssd.dirs.push_back(base + d);
    for (const auto& d : ssd.dirs) fs::create_directories(d);

    TierSizes tiers;
    tiers.ram_head = 4 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 1 * MiB;
    mem.block_bytes = 64 * KiB;
    mem.lock_memory = false;
    EvictionConfig ev;
    Index index;
    auto tm = TierManager::open(tiers, mem, ev, ssd, hdd, index, 256 * KiB,
                                /*write_buffers=*/4, /*direct_io=*/false);
    CHECK(tm.has_value());
    if (!tm) {
        fs::remove_all(base);
        return;
    }

    constexpr Size SZ = 128 * KiB;
    const auto digest = hash_key("/same-key/concurrent-commit");
    std::vector<std::byte> first_value(SZ, std::byte{0x41});
    std::vector<std::byte> second_value(SZ, std::byte{0x52});
    auto first = tm->begin_store(digest, SZ);
    auto second = tm->begin_store(digest, SZ);
    CHECK(first.has_value());
    CHECK(second.has_value());
    if (!first || !second) {
        fs::remove_all(base);
        return;
    }
    CHECK(first->write(ByteView(first_value.data(), first_value.size())).has_value());
    CHECK(second->write(ByteView(second_value.data(), second_value.size())).has_value());

    CHECK(first->commit(/*flags=*/0x41).has_value());
    const auto first_meta = index.lookup(digest);
    CHECK(first_meta && first_meta->file_generation != 0);
    const std::uint64_t first_generation = first_meta ? first_meta->file_generation : 0;

    CHECK(second->commit(/*flags=*/0x52).has_value());
    const auto second_meta = index.lookup(digest);
    CHECK(second_meta && second_meta->file_generation != 0);
    CHECK(second_meta && second_meta->file_generation != first_generation);
    CHECK(second_meta && second_meta->flags == std::uint32_t(0x52));
    const std::string first_name = digest.hex() + ".g" + std::to_string(first_generation);
    const std::string second_name =
        digest.hex() + ".g" + std::to_string(second_meta ? second_meta->file_generation : 0);
    for (const auto& dir : ssd.dirs) {
        CHECK(!fs::exists(fs::path(dir) / first_name));
        CHECK(fs::exists(fs::path(dir) / second_name));
    }
    const auto head = tm->head_view(digest);
    CHECK(head.has_value());
    if (head)
        CHECK(std::all_of(head->begin(), head->end(),
                          [](std::byte b) { return b == std::byte{0x52}; }));

    fs::remove_all(base);
}

TEST("tier_manager: concurrent conditional stores linearize add and replace at commit") {
    const std::string base =
        (fs::temp_directory_path() / ("goblin-conditional-commit-" + std::to_string(::getpid())))
            .string();
    PoolConfig ssd, hdd;
    ssd.stripe_unit = 64 * KiB;
    ssd.dirs.push_back(base + "/ssd");
    fs::create_directories(ssd.dirs.front());

    TierSizes tiers;
    tiers.ram_head = 4 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 1 * MiB;
    mem.block_bytes = 64 * KiB;
    mem.lock_memory = false;
    mem.use_hugepages = false;
    EvictionConfig ev;
    Index index;
    auto tm = TierManager::open(tiers, mem, ev, ssd, hdd, index, 256 * KiB,
                                /*write_buffers=*/8, /*direct_io=*/false);
    CHECK(tm.has_value());
    if (!tm) {
        fs::remove_all(base);
        return;
    }

    constexpr unsigned kWriters = 8;
    constexpr Size kValueSize = 64;
    const auto add_key = hash_key("/conditional/concurrent-add");
    std::array<std::optional<TierManager::StoreHandle>, kWriters> handles;
    std::array<std::array<std::byte, kValueSize>, kWriters> values{};
    for (unsigned i = 0; i < kWriters; ++i) {
        values[i].fill(static_cast<std::byte>(i + 1));
        auto handle = tm->begin_store(add_key, kValueSize, WriteMode::evict,
                                      StoreCondition::add);
        CHECK(handle.has_value());
        if (!handle) continue;
        handles[i].emplace(std::move(*handle));
        CHECK(handles[i]->write(ByteView(values[i].data(), values[i].size())).has_value());
    }

    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};
    std::atomic<unsigned> stored{0};
    std::atomic<unsigned> conflicts{0};
    std::atomic<unsigned> other_errors{0};
    std::vector<std::thread> writers;
    writers.reserve(kWriters);
    for (unsigned i = 0; i < kWriters; ++i) {
        writers.emplace_back([&, i] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            if (!handles[i]) {
                other_errors.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            const auto committed = handles[i]->commit(i + 1);
            if (committed)
                stored.fetch_add(1, std::memory_order_relaxed);
            else if (committed.error().code == Errc::condition_not_met)
                conflicts.fetch_add(1, std::memory_order_relaxed);
            else
                other_errors.fetch_add(1, std::memory_order_relaxed);
        });
    }
    while (ready.load(std::memory_order_acquire) != kWriters) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    for (auto& writer : writers) writer.join();

    CHECK_EQ(stored.load(), 1u);
    CHECK_EQ(conflicts.load(), kWriters - 1);
    CHECK_EQ(other_errors.load(), 0u);
    const auto winner = index.lookup(add_key);
    CHECK(winner.has_value());
    CHECK(winner && winner->flags >= 1 && winner->flags <= kWriters);
    if (winner) {
        const auto head = tm->head_view(add_key);
        CHECK(head.has_value());
        if (head) {
            const std::byte expected = static_cast<std::byte>(winner->flags);
            CHECK(std::all_of(head->begin(), head->end(),
                              [expected](std::byte value) { return value == expected; }));
        }
    }

    const auto replace_key = hash_key("/conditional/removed-before-replace");
    std::array<std::byte, kValueSize> old_value{};
    std::array<std::byte, kValueSize> new_value{};
    old_value.fill(std::byte{0x31});
    new_value.fill(std::byte{0x52});
    CHECK(tm->store(replace_key, ByteView(old_value.data(), old_value.size()), 1).has_value());
    auto replacement = tm->begin_store(replace_key, new_value.size(), WriteMode::evict,
                                       StoreCondition::replace);
    CHECK(replacement.has_value());
    if (replacement) {
        CHECK(replacement->write(ByteView(new_value.data(), new_value.size())).has_value());
        CHECK(tm->remove(replace_key));
        const auto committed = replacement->commit(2);
        CHECK(!committed.has_value());
        CHECK(!committed && committed.error().code == Errc::condition_not_met);
        CHECK(!index.contains(replace_key));
    }

    fs::remove_all(base);
}

TEST("tier_manager: arithmetic, meta refresh state, access metadata, and flush are atomic") {
    const fs::path base = fs::temp_directory_path() /
        ("goblin-meta-state-" + std::to_string(::getpid()));
    fs::remove_all(base);
    fs::create_directories(base / "ssd");
    PoolConfig ssd, hdd;
    ssd.dirs.push_back((base / "ssd").string());
    ssd.stripe_unit = 64 * KiB;
    TierSizes tiers;
    tiers.ram_head = 4 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig memory;
    memory.total_bytes = 1 * MiB;
    memory.block_bytes = 64 * KiB;
    memory.lock_memory = false;
    memory.use_hugepages = false;
    EvictionConfig eviction;
    Index index;
    auto tm = TierManager::open(tiers, memory, eviction, ssd, hdd, index,
                                64 * KiB, 8, false);
    CHECK(tm.has_value());
    if (!tm) { fs::remove_all(base); return; }

    const auto number = hash_key("number");
    const std::string ten = "10";
    CHECK(tm->store(number, ByteView(reinterpret_cast<const std::byte*>(ten.data()), ten.size()),
                    17).has_value());
    const auto incremented = tm->arithmetic(number, 5, false);
    CHECK(incremented.has_value());
    if (incremented) CHECK_EQ(incremented->value, std::uint64_t(15));
    const auto decremented = tm->arithmetic(number, 20, true);
    CHECK(decremented.has_value());
    if (decremented) CHECK_EQ(decremented->value, std::uint64_t(0));
    auto before_read = index.lookup(number);
    CHECK(before_read.has_value() && !before_read->fetched);
    const auto head = tm->head_view(number);
    CHECK(head.has_value());
    if (head) CHECK_EQ(std::string(reinterpret_cast<const char*>(head->data()), head->size()),
                       std::string("0"));

    const auto created = tm->arithmetic(hash_key("new-number"), 2, false,
                                        std::uint64_t(41), std::uint32_t(0));
    CHECK(created.has_value());
    if (created) CHECK(created->created && created->value == 41);
    const std::string text = "not-a-number";
    const auto text_key = hash_key("text");
    CHECK(tm->store(text_key,
                    ByteView(reinterpret_cast<const std::byte*>(text.data()), text.size()), 0)
              .has_value());
    const auto invalid = tm->arithmetic(text_key, 1, false);
    CHECK(!invalid.has_value());
    if (!invalid) CHECK_EQ(invalid.error().code, Errc::invalid_argument);

    // head_view is itself a successful fetch and updates h/l metadata.
    auto snapshot = tm->open_snapshot(number, /*record_access=*/true);
    CHECK(snapshot.has_value());
    if (snapshot && snapshot->pin.valid) tm->unpin_head(snapshot->pin);
    const auto after_read = index.lookup(number);
    CHECK(after_read.has_value() && after_read->fetched);
    if (after_read) CHECK(after_read->last_access != 0);

    const auto stale = tm->mark_stale(number, 0);
    CHECK(stale == Index::MetaMutation::stored);
    const auto stale_meta = index.lookup(number);
    CHECK(stale_meta.has_value() && stale_meta->stale && !stale_meta->recache_claimed);
    CHECK(tm->claim_recache(number) == Index::RecacheClaim::winner);
    CHECK(tm->claim_recache(number) == Index::RecacheClaim::already_claimed);

    tm->flush_all();
    CHECK(!tm->lookup_live(number).has_value());
    const auto survivor = hash_key("after-flush");
    CHECK(tm->store(survivor, ByteView{}, 0).has_value());
    CHECK(tm->lookup_live(survivor).has_value());
    CHECK(tm->reap_expired() >= 3); // number, text, and new-number predate the flush cutoff
    CHECK(index.contains(survivor));
    fs::remove_all(base);
}

TEST("tier_manager: begin_store with odd, stripe-crossing chunks round-trips") {
    if (!core::Reactor::available()) {
        std::println("    (skipped: built without liburing)");
        return;
    }
    auto reactor = core::Reactor::create();
    if (!reactor) {
        std::println("    (skipped: io_uring unavailable: {})", reactor.error().detail);
        return;
    }
    const std::string base =
        (fs::temp_directory_path() / ("goblin-chunky-" + std::to_string(::getpid()))).string();
    PoolConfig ssd, hdd;
    ssd.stripe_unit = 64 * KiB;
    for (const char* d : {"/s0", "/s1"}) ssd.dirs.push_back(base + d);
    for (const auto& d : ssd.dirs) fs::create_directories(d);
    TierSizes tiers;
    tiers.ram_head = 4 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 8 * MiB;
    mem.block_bytes = 1 * MiB;
    mem.lock_memory = false;
    EvictionConfig ev;
    Index index;
    auto tm = TierManager::open(tiers, mem, ev, ssd, hdd, index);
    CHECK(tm.has_value());

    const Size sz = 300 * KiB;
    std::vector<std::byte> data(sz);
    for (Size i = 0; i < sz; ++i) data[i] = static_cast<std::byte>((i * 97 + 13) & 0xFF);

    const auto digest = hash_key("/chunky");
    auto h = tm->begin_store(digest, sz);
    CHECK(h.has_value());
    if (h) {
        const Size chunk_sizes[] = {1, 12345, 65536, 70000, 100000}; // odd, stripe-crossing
        Size off = 0;
        for (const Size cs : chunk_sizes) {
            const Size n = std::min<Size>(cs, sz - off);
            if (n == 0) break;
            CHECK(h->write(ByteView(data.data() + off, n)).has_value());
            off += n;
        }
        if (off < sz) CHECK(h->write(ByteView(data.data() + off, sz - off)).has_value());
        CHECK(h->commit(0).has_value());
    }

    std::vector<std::byte> got(sz);
    const auto n = tm->read(*reactor, digest, 0, MutBytes(got.data(), got.size()));
    CHECK(n.has_value());
    if (n) {
        CHECK_EQ(*n, std::size_t(sz));
        CHECK(got == data);
    }
    fs::remove_all(base);
}

TEST("tier_manager: SSD object-count bound evicts whole objects; index stays capped") {
    if (!core::Reactor::available()) {
        std::println("    (skipped: built without liburing)");
        return;
    }
    auto reactor = core::Reactor::create();
    if (!reactor) {
        std::println("    (skipped: io_uring unavailable: {})", reactor.error().detail);
        return;
    }

    const std::string base =
        (fs::temp_directory_path() / ("goblin-objcap-" + std::to_string(::getpid()))).string();
    PoolConfig ssd, hdd; // 2-layer
    ssd.stripe_unit = 64 * KiB;
    for (const char* d : {"/s0", "/s1"}) ssd.dirs.push_back(base + d);
    for (const auto& d : ssd.dirs) fs::create_directories(d);

    TierSizes tiers;
    tiers.ram_head = 4 * KiB;
    tiers.ssd_prefix = 64 * KiB;
    MemoryConfig mem;
    mem.total_bytes = 4 * MiB;
    mem.block_bytes = 256 * KiB;
    mem.lock_memory = false;
    EvictionConfig ev;
    ev.max_ssd_objects = 10; // cap the whole store at 10 objects

    Index index;
    auto tm = TierManager::open(tiers, mem, ev, ssd, hdd, index);
    CHECK(tm.has_value());

    const Size sz = 8 * KiB; // larger than ram_head, so each entry is an SSD-backed object
    auto data = [&](int i) {
        std::vector<std::byte> v(sz);
        for (Size j = 0; j < sz; ++j) v[j] = static_cast<std::byte>((j + std::size_t(i)) & 0xFF);
        return v;
    };
    for (int i = 0; i < 40; ++i) {
        const auto d = data(i);
        CHECK(tm->store(hash_key("/o" + std::to_string(i)), ByteView(d.data(), d.size()), 0)
                  .has_value());
    }

    CHECK(index.size() <= std::size_t(10)); // whole-object eviction kept the index capped

    // The most-recent object survives and reads back correctly...
    {
        const auto want = data(39);
        std::vector<std::byte> got(sz);
        const auto n = tm->read(*reactor, hash_key("/o39"), 0, MutBytes(got.data(), got.size()));
        CHECK(n.has_value());
        if (n) {
            CHECK_EQ(*n, std::size_t(sz));
            CHECK(got == want);
        }
    }
    // ...and an early one was evicted as a whole object (gone from the index + disk).
    {
        std::vector<std::byte> got(sz);
        CHECK(!tm->read(*reactor, hash_key("/o0"), 0, MutBytes(got.data(), got.size())).has_value());
    }
    fs::remove_all(base);
}

TEST("tier_manager: a pinned head's RAM survives removal (deferred free)") {
    if (!core::Reactor::available()) {
        std::println("    (skipped: built without liburing)");
        return;
    }
    auto reactor = core::Reactor::create();
    if (!reactor) {
        std::println("    (skipped: io_uring unavailable: {})", reactor.error().detail);
        return;
    }
    const std::string base =
        (fs::temp_directory_path() / ("goblin-pin-" + std::to_string(::getpid()))).string();
    PoolConfig ssd, hdd;
    ssd.stripe_unit = 64 * KiB;
    for (const char* d : {"/s0", "/s1"}) ssd.dirs.push_back(base + d);
    for (const auto& d : ssd.dirs) fs::create_directories(d);
    TierSizes tiers;
    tiers.ram_head = 16 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 8 * MiB;
    mem.block_bytes = 1 * MiB;
    mem.lock_memory = false;
    EvictionConfig ev;
    Index index;
    auto tm = TierManager::open(tiers, mem, ev, ssd, hdd, index);
    CHECK(tm.has_value());

    std::string val(16 * 1024, '\0');
    for (std::size_t i = 0; i < val.size(); ++i) val[i] = static_cast<char>((i * 41 + 7) & 0xFF);
    const auto digest = hash_key("pinme");
    CHECK(tm->store(digest, ByteView(reinterpret_cast<const std::byte*>(val.data()), val.size()), 0)
              .has_value());
    CHECK(index.lookup(digest) && index.lookup(digest)->head.resident());

    auto pin = tm->pin_head(digest);
    CHECK(pin.has_value() && pin->valid);
    if (!pin) { fs::remove_all(base); return; }
    auto as_str = [](ByteView b) {
        return std::string(reinterpret_cast<const char*>(b.data()), b.size());
    };
    CHECK(as_str(tm->pinned_bytes(*pin)) == val); // the pinned head matches the stored value

    // Remove while pinned: the head RAM is orphaned (not freed), the index entry is erased.
    CHECK(tm->remove(digest));
    CHECK(!index.lookup(digest).has_value());

    // Allocate other heads — the orphaned region must NOT be reused while pinned.
    for (int i = 0; i < 8; ++i) {
        const std::string o(16 * 1024, static_cast<char>('a' + i));
        CHECK(tm->store(hash_key("o" + std::to_string(i)),
                        ByteView(reinterpret_cast<const std::byte*>(o.data()), o.size()), 0)
                  .has_value());
    }
    CHECK(as_str(tm->pinned_bytes(*pin)) == val); // still intact: zero-copy stayed valid

    tm->unpin_head(*pin); // frees the orphaned region (ASan: no leak, no double-free)
    fs::remove_all(base);
}

TEST("tier_manager: concurrent replace + read never sees a torn value (atomic publish)") {
    if (!core::Reactor::available()) {
        std::println("    (skipped: built without liburing)");
        return;
    }
    const std::string base =
        (fs::temp_directory_path() / ("goblin-cow-" + std::to_string(::getpid()))).string();
    PoolConfig ssd, hdd;
    ssd.stripe_unit = 64 * KiB;
    for (const char* d : {"/s0", "/s1"}) ssd.dirs.push_back(base + d);
    for (const auto& d : ssd.dirs) fs::create_directories(d);
    TierSizes tiers;
    tiers.ram_head = 16 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 32 * MiB;
    mem.block_bytes = 1 * MiB;
    mem.lock_memory = false;
    EvictionConfig ev;
    Index index;
    auto tm = TierManager::open(tiers, mem, ev, ssd, hdd, index);
    CHECK(tm.has_value());

    constexpr Size SZ = 64 * 1024; // 16 KiB head (RAM) + 48 KiB SSD -> spans both tiers
    const std::string A(SZ, '\xAA'), B(SZ, '\xBB');
    const auto digest = hash_key("k");
    auto store = [&](const std::string& v) {
        return tm->store(digest, ByteView(reinterpret_cast<const std::byte*>(v.data()), v.size()), 0)
            .has_value();
    };
    CHECK(store(A)); // seed

    std::atomic<bool> stop{false};
    std::atomic<long> torn{0}, reads{0};
    std::thread writer([&] {
        for (int i = 0; i < 4000 && !stop.load(); ++i) store((i & 1) ? B : A); // replace in place
        stop.store(true);
    });
    auto reader = [&] {
        auto r = core::Reactor::create();
        if (!r) return;
        void* p = std::aligned_alloc(kDeviceBlock, SZ); // page-aligned, 4 KiB-multiple
        auto* buf = static_cast<std::byte*>(p);
        while (!stop.load()) {
            auto n = tm->read(*r, digest, 0, MutBytes(buf, SZ));
            if (!n || *n != SZ) continue;
            const std::byte v0 = buf[0]; // a consistent snapshot is all-A or all-B
            const bool ok = (v0 == std::byte{0xAA} || v0 == std::byte{0xBB}) &&
                            std::all_of(buf, buf + SZ, [v0](std::byte b) { return b == v0; });
            if (!ok) torn.fetch_add(1);
            reads.fetch_add(1);
        }
        std::free(p);
    };
    std::thread r1(reader), r2(reader);
    writer.join();
    r1.join();
    r2.join();
    std::println("    [cow] reads={} torn={}", reads.load(), torn.load());
    CHECK(torn.load() == 0); // copy-on-write publish: readers see old-or-new, never a mix
    fs::remove_all(base);
}

TEST("tier_manager: O_DIRECT round-trips an odd-sized object across all tiers") {
    if (!core::Reactor::available()) {
        std::println("    (skipped: built without liburing)");
        return;
    }
    auto reactor = core::Reactor::create();
    if (!reactor) {
        std::println("    (skipped: io_uring unavailable: {})", reactor.error().detail);
        return;
    }
    const std::string base =
        (fs::temp_directory_path() / ("goblin-odirect-" + std::to_string(::getpid()))).string();
    fs::create_directories(base);
    { // probe O_DIRECT support; skip on filesystems that reject it
        const std::string probe = base + "/.od-probe";
        const int fd = ::open(probe.c_str(), O_RDWR | O_CREAT | O_DIRECT, 0644);
        if (fd < 0) {
            std::println("    (skipped: O_DIRECT unsupported on this filesystem)");
            fs::remove_all(base);
            return;
        }
        ::close(fd);
        ::unlink(probe.c_str());
    }
    PoolConfig ssd, hdd;
    ssd.stripe_unit = 64 * KiB;
    hdd.stripe_unit = 64 * KiB;
    for (const char* d : {"/s0", "/s1"}) ssd.dirs.push_back(base + d);
    hdd.dirs.push_back(base + "/h0");
    for (const auto& d : ssd.dirs) fs::create_directories(d);
    for (const auto& d : hdd.dirs) fs::create_directories(d);
    TierSizes tiers;
    tiers.ram_head = 16 * KiB;
    tiers.ssd_prefix = 64 * KiB;
    MemoryConfig mem;
    mem.total_bytes = 16 * MiB;
    mem.block_bytes = 1 * MiB;
    mem.lock_memory = false;
    EvictionConfig ev;
    Index index;
    auto tm = TierManager::open(tiers, mem, ev, ssd, hdd, index, 256 * KiB, 8, /*direct_io=*/true);
    CHECK(tm.has_value());

    constexpr Size SZ = 200000; // 16 KiB head (RAM) + 48 KiB SSD + ~135 KiB HDD; not a 4 KiB multiple
    std::string val(SZ, '\0');
    for (std::size_t i = 0; i < SZ; ++i) val[i] = static_cast<char>((i * 131 + 7) & 0xFF);
    const auto digest = hash_key("od");
    CHECK(tm->store(digest, ByteView(reinterpret_cast<const std::byte*>(val.data()), SZ), 0)
              .has_value());

    const Size cap = align_up(SZ, kDeviceBlock); // page-aligned, 4 KiB-multiple read buffer
    void* p = std::aligned_alloc(kDeviceBlock, cap);
    auto* buf = static_cast<std::byte*>(p);
    auto n = tm->read(*reactor, digest, 0, MutBytes(buf, cap));
    CHECK(n.has_value() && *n == SZ);
    if (n) CHECK(std::equal(buf, buf + SZ, reinterpret_cast<const std::byte*>(val.data())));

    // The blocking protocol keeps one immutable ReadStream for the full response. Its final direct
    // read must use the aligned tail room while reporting only the object's logical odd length.
    std::memset(buf, 0, cap);
    auto snapshot = tm->open_snapshot(digest, /*record_access=*/false);
    CHECK(snapshot && snapshot->rs.has_value());
    if (snapshot && snapshot->rs) {
        auto streamed = snapshot->rs->read(*reactor, 0, MutBytes(buf, cap));
        CHECK(streamed.has_value() && *streamed == SZ);
        if (streamed)
            CHECK(std::equal(buf, buf + SZ, reinterpret_cast<const std::byte*>(val.data())));
        if (snapshot->pin.valid) tm->unpin_head(snapshot->pin);
    }
    std::free(p);
    fs::remove_all(base);
}

TEST("tier_manager: evicting a full head moves its score from NUMA storage to the Index") {
    const std::string base =
        (fs::temp_directory_path() / ("goblin-score-head-evict-" + std::to_string(::getpid())))
            .string();
    PoolConfig ssd, hdd;
    ssd.stripe_unit = 64 * KiB;
    ssd.dirs.push_back(base + "/ssd");
    fs::create_directories(ssd.dirs.front());

    TierSizes tiers;
    tiers.ram_head = 64 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 64 * KiB; // exactly one fixed head: the second store evicts the first head
    mem.block_bytes = 64 * KiB;
    mem.lock_memory = false;
    EvictionConfig ev;
    AccessScoreConfig score;
    score.increment = 3.0;
    Index index;
    auto tm = TierManager::open(tiers, mem, ev, ssd, hdd, index, 256 * KiB, 8,
                                /*direct_io=*/false, score);
    CHECK(tm.has_value());
    if (!tm) {
        fs::remove_all(base);
        return;
    }

    constexpr Size kSize = 68 * KiB; // disk-backed, with one fixed 64 KiB resident head
    const auto first_key = hash_key("/score-evict/first");
    const auto second_key = hash_key("/score-evict/second");
    std::vector<std::byte> first(kSize, std::byte{0x31});
    std::vector<std::byte> second(kSize, std::byte{0x72});
    CHECK(tm->store(first_key, ByteView(first.data(), first.size()), 0).has_value());
    CHECK(index.score_external(first_key));
    CHECK(!index.score(first_key).has_value());
    tm->touch(first_key);
    tm->touch(first_key);
    CHECK_EQ(*tm->access_score(first_key), 6.0);

    CHECK(tm->store(second_key, ByteView(second.data(), second.size()), 0).has_value());
    const auto first_meta = index.lookup(first_key);
    CHECK(first_meta.has_value());
    if (first_meta) CHECK(!first_meta->head.resident());

    // The disk-backed object survives head eviction. Its one logical score moves into the Index;
    // the vacated NUMA slot is no longer an owner.
    CHECK(!index.score_external(first_key));
    CHECK_EQ(*index.score(first_key), 6.0);
    CHECK_EQ(*tm->access_score(first_key), 6.0);
    tm->touch(first_key);
    CHECK_EQ(*index.score(first_key), 9.0); // subsequent reads now update the Index owner
    CHECK_EQ(*tm->access_score(first_key), 9.0);
    CHECK(index.score_external(second_key));
    CHECK(!index.score(second_key).has_value());
    fs::remove_all(base);
}

TEST("tier_manager: split-pool replacements preserve one score while changing head ownership") {
    const std::string base =
        (fs::temp_directory_path() / ("goblin-score-replace-" + std::to_string(::getpid())))
            .string();
    PoolConfig ssd, hdd;
    ssd.stripe_unit = 64 * KiB;
    ssd.dirs.push_back(base + "/ssd");
    fs::create_directories(ssd.dirs.front());

    TierSizes tiers;
    tiers.ram_head = 64 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 256 * KiB;
    mem.small_total_bytes = 64 * KiB;
    mem.block_bytes = 64 * KiB;
    mem.lock_memory = false;
    EvictionConfig ev;
    AccessScoreConfig score;
    score.increment = 2.5;
    Index index;
    auto tm = TierManager::open(tiers, mem, ev, ssd, hdd, index, 256 * KiB, 8,
                                /*direct_io=*/false, score);
    CHECK(tm.has_value());
    if (!tm) {
        fs::remove_all(base);
        return;
    }

    const auto key = hash_key("/score-replacement");
    std::vector<std::byte> small(128, std::byte{0x11});
    std::vector<std::byte> first_full(68 * KiB, std::byte{0x22});
    std::vector<std::byte> second_full(72 * KiB, std::byte{0x33});

    CHECK(tm->store(key, ByteView(small.data(), small.size()), 1).has_value());
    CHECK(!index.score_external(key));
    tm->touch(key);
    CHECK_EQ(*index.score(key), 2.5);
    CHECK_EQ(*tm->access_score(key), 2.5);

    // Small -> full: ownership moves out of the hash entry into the fixed-head NUMA slot.
    CHECK(tm->store(key, ByteView(first_full.data(), first_full.size()), 2).has_value());
    CHECK(index.score_external(key));
    CHECK(!index.score(key).has_value());
    CHECK_EQ(*tm->access_score(key), 2.5);
    tm->touch(key);
    CHECK_EQ(*tm->access_score(key), 5.0);

    // Full -> full: the new physical slot becomes the sole owner without resetting or duplicating
    // the logical score.
    CHECK(tm->store(key, ByteView(second_full.data(), second_full.size()), 3).has_value());
    CHECK(index.score_external(key));
    CHECK(!index.score(key).has_value());
    CHECK_EQ(*tm->access_score(key), 5.0);
    tm->touch(key);
    CHECK_EQ(*tm->access_score(key), 7.5);

    // Full -> small: ownership returns to the Index when the replacement uses an arena sub-block.
    CHECK(tm->store(key, ByteView(small.data(), small.size()), 4).has_value());
    CHECK(!index.score_external(key));
    CHECK_EQ(*index.score(key), 7.5);
    CHECK_EQ(*tm->access_score(key), 7.5);
    fs::remove_all(base);
}

TEST("tier_manager: deleting a fixed head clears its score before immediate slot reuse") {
    const std::string base =
        (fs::temp_directory_path() / ("goblin-score-slot-reuse-" + std::to_string(::getpid())))
            .string();
    PoolConfig ssd, hdd;
    ssd.stripe_unit = 64 * KiB;
    ssd.dirs.push_back(base + "/ssd");
    fs::create_directories(ssd.dirs.front());

    TierSizes tiers;
    tiers.ram_head = 64 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 64 * KiB; // exactly one physical fixed-head slot
    mem.block_bytes = 64 * KiB;
    mem.lock_memory = false;
    EvictionConfig ev;
    AccessScoreConfig score;
    score.increment = 2.0;
    Index index;
    auto tm = TierManager::open(tiers, mem, ev, ssd, hdd, index, 256 * KiB, 8,
                                /*direct_io=*/false, score);
    CHECK(tm.has_value());
    if (!tm) {
        fs::remove_all(base);
        return;
    }

    const auto first_key = hash_key("/score/reuse/first");
    const auto second_key = hash_key("/score/reuse/second");
    std::vector<std::byte> first(64 * KiB, std::byte{0x31});
    std::vector<std::byte> second(64 * KiB, std::byte{0x72});
    CHECK(tm->store(first_key, ByteView(first.data(), first.size()), 1).has_value());
    const auto first_meta = index.lookup(first_key);
    CHECK(first_meta.has_value());
    if (!first_meta) {
        fs::remove_all(base);
        return;
    }
    tm->touch(first_key);
    tm->touch(first_key);
    CHECK_EQ(*tm->access_score(first_key), 4.0);

    CHECK(tm->remove(first_key));
    CHECK(!index.contains(first_key));
    CHECK(!tm->access_score(first_key).has_value());

    CHECK(tm->store(second_key, ByteView(second.data(), second.size()), 2).has_value());
    const auto second_meta = index.lookup(second_key);
    CHECK(second_meta.has_value());
    if (second_meta) {
        CHECK_EQ(second_meta->head.block, first_meta->head.block);
        CHECK_EQ(second_meta->head.offset, first_meta->head.offset);
    }
    CHECK(index.score_external(second_key));
    CHECK_EQ(*tm->access_score(second_key), 0.0);
    tm->touch(second_key);
    CHECK_EQ(*tm->access_score(second_key), 2.0);
    fs::remove_all(base);
}

TEST("tier_manager: replacement moves one score from full head to headless and back") {
    const std::string base =
        (fs::temp_directory_path() / ("goblin-score-headless-cycle-" + std::to_string(::getpid())))
            .string();
    PoolConfig ssd, hdd;
    ssd.stripe_unit = 64 * KiB;
    ssd.dirs.push_back(base + "/ssd");
    fs::create_directories(ssd.dirs.front());

    TierSizes tiers;
    tiers.ram_head = 64 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 64 * KiB; // replacement cannot reserve a head until the old one is retired
    mem.block_bytes = 64 * KiB;
    mem.lock_memory = false;
    EvictionConfig ev;
    AccessScoreConfig score;
    score.increment = 2.5;
    Index index;
    auto tm = TierManager::open(tiers, mem, ev, ssd, hdd, index, 256 * KiB, 8,
                                /*direct_io=*/false, score);
    CHECK(tm.has_value());
    if (!tm) {
        fs::remove_all(base);
        return;
    }

    constexpr Size kSize = 68 * KiB; // disk-backed with a 64 KiB fixed head when RAM is available
    const auto key = hash_key("/score/headless-cycle");
    std::vector<std::byte> first(kSize, std::byte{0x11});
    std::vector<std::byte> headless(kSize, std::byte{0x22});
    std::vector<std::byte> full_again(kSize, std::byte{0x33});

    CHECK(tm->store(key, ByteView(first.data(), first.size()), 1).has_value());
    const auto first_meta = index.lookup(key);
    CHECK(first_meta && first_meta->head.resident());
    CHECK(index.score_external(key));
    tm->touch(key);
    tm->touch(key);
    CHECK_EQ(*tm->access_score(key), 5.0);

    // The pending replacement protects the only resident head, so this version is admitted without
    // a head. Its score must move into the Index rather than reset or remain in the retired slot.
    CHECK(tm->store(key, ByteView(headless.data(), headless.size()), 2).has_value());
    const auto middle_meta = index.lookup(key);
    CHECK(middle_meta && !middle_meta->head.resident());
    CHECK(!index.score_external(key));
    CHECK_EQ(*index.score(key), 5.0);
    CHECK_EQ(*tm->access_score(key), 5.0);
    tm->touch(key);
    CHECK_EQ(*tm->access_score(key), 7.5);

    // Retiring the old full head freed the sole slot. The next replacement takes it and transfers
    // the same logical score back into dense NUMA storage.
    CHECK(tm->store(key, ByteView(full_again.data(), full_again.size()), 3).has_value());
    const auto last_meta = index.lookup(key);
    CHECK(last_meta && last_meta->head.resident());
    if (first_meta && last_meta) {
        CHECK_EQ(last_meta->head.block, first_meta->head.block);
        CHECK_EQ(last_meta->head.offset, first_meta->head.offset);
    }
    CHECK(index.score_external(key));
    CHECK(!index.score(key).has_value());
    CHECK_EQ(*tm->access_score(key), 7.5);
    fs::remove_all(base);
}

TEST("tier_manager: admission cannot evict the version it is replacing") {
    const std::string base =
        (fs::temp_directory_path() / ("goblin-score-self-evict-" + std::to_string(::getpid())))
            .string();
    PoolConfig ssd, hdd;
    ssd.stripe_unit = 64 * KiB;
    ssd.dirs.push_back(base + "/ssd");
    fs::create_directories(ssd.dirs.front());

    TierSizes tiers;
    tiers.ram_head = 64 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 192 * KiB; // old value, an ordinary victim, and the replacement reservation
    mem.block_bytes = 64 * KiB;
    mem.lock_memory = false;
    EvictionConfig ev;
    AccessScoreConfig score;
    score.increment = 3.0;
    Index index;
    auto tm = TierManager::open(tiers, mem, ev, ssd, hdd, index, 256 * KiB, 8,
                                /*direct_io=*/false, score);
    CHECK(tm.has_value());
    if (!tm) {
        fs::remove_all(base);
        return;
    }

    const auto key = hash_key("/score/self-replacement");
    const auto victim = hash_key("/score/ordinary-victim");
    const auto next = hash_key("/score/concurrent-admission");
    std::vector<std::byte> old_value(64 * KiB, std::byte{0x41});
    std::vector<std::byte> replacement(64 * KiB, std::byte{0x52});
    CHECK(tm->store(key, ByteView(old_value.data(), old_value.size()), 11).has_value());
    CHECK(tm->store(victim, ByteView(old_value.data(), old_value.size()), 12).has_value());
    // The legacy stream records heat without touching the eviction policy, leaving `key` as the
    // first ordinary victim if its StoreHandle protection is released too early.
    CHECK(tm->open_read(key).has_value());
    CHECK_EQ(*tm->access_score(key), 3.0);

    auto pending = tm->begin_store(key, replacement.size());
    CHECK(pending.has_value());
    if (!pending) {
        fs::remove_all(base);
        return;
    }
    CHECK(pending->write(ByteView(replacement.data(), replacement.size())).has_value());

    // The pending replacement owns the last free block. A concurrent admission must evict the
    // ordinary key, not the old incarnation that the pending handle is replacing.
    CHECK(tm->store(next, ByteView(replacement.data(), replacement.size()), 33).has_value());
    CHECK(index.contains(key));
    CHECK(!index.contains(victim));
    CHECK(index.contains(next));
    CHECK_EQ(index.lookup(key)->flags, std::uint32_t(11));
    CHECK(index.score_external(key));
    CHECK_EQ(*tm->access_score(key), 3.0);

    CHECK(pending->commit(22).has_value());
    CHECK_EQ(index.lookup(key)->flags, std::uint32_t(22));
    CHECK(index.score_external(key));
    CHECK_EQ(*tm->access_score(key), 3.0);
    CHECK_EQ(tm->head_resident(), std::size_t(2));
    fs::remove_all(base);
}

TEST("tier_manager: concurrent new StoreHandles share digest eviction protection") {
    const std::string base =
        (fs::temp_directory_path() / ("goblin-score-new-guards-" + std::to_string(::getpid())))
            .string();
    PoolConfig ssd, hdd;
    ssd.stripe_unit = 64 * KiB;
    ssd.dirs.push_back(base + "/ssd");
    fs::create_directories(ssd.dirs.front());

    TierSizes tiers;
    tiers.ram_head = 64 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 192 * KiB;
    mem.block_bytes = 64 * KiB;
    mem.lock_memory = false;
    EvictionConfig ev;
    AccessScoreConfig score;
    score.increment = 2.0;
    Index index;
    auto tm = TierManager::open(tiers, mem, ev, ssd, hdd, index, 256 * KiB, 8,
                                /*direct_io=*/false, score);
    CHECK(tm.has_value());
    if (!tm) {
        fs::remove_all(base);
        return;
    }

    const auto key = hash_key("/score/concurrent-new");
    const auto expendable = hash_key("/score/expendable");
    const auto next = hash_key("/score/next");
    std::vector<std::byte> first(64 * KiB, std::byte{0x11});
    std::vector<std::byte> second(64 * KiB, std::byte{0x22});

    auto a = tm->begin_store(key, first.size());
    auto b = tm->begin_store(key, second.size());
    CHECK(a.has_value());
    CHECK(b.has_value());
    if (!a || !b) {
        fs::remove_all(base);
        return;
    }
    CHECK(a->write(ByteView(first.data(), first.size())).has_value());
    CHECK(b->write(ByteView(second.data(), second.size())).has_value());
    CHECK(a->commit(1).has_value());
    CHECK(tm->open_read(key).has_value());
    CHECK_EQ(*tm->access_score(key), 2.0);

    CHECK(tm->store(expendable, ByteView(first.data(), first.size()), 3).has_value());
    CHECK(tm->store(next, ByteView(first.data(), first.size()), 4).has_value());
    CHECK(index.contains(key));
    CHECK(!index.contains(expendable));
    CHECK(index.contains(next));

    CHECK(b->commit(2).has_value());
    CHECK_EQ(index.lookup(key)->flags, std::uint32_t(2));
    CHECK(index.score_external(key));
    CHECK_EQ(*tm->access_score(key), 2.0);
    CHECK_EQ(tm->head_resident(), std::size_t(2));
    fs::remove_all(base);
}

TEST("tier_manager: hotter full foreign block replaces colder local block") {
    const std::string base =
        (fs::temp_directory_path() / ("goblin-numa-score-" + std::to_string(::getpid()))).string();
    PoolConfig ssd, hdd;
    ssd.stripe_unit = 64 * KiB;
    ssd.dirs.push_back(base + "/ssd");
    fs::create_directories(ssd.dirs.front());

    TierSizes tiers;
    tiers.ram_head = 64 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 64 * KiB;
    mem.block_bytes = 64 * KiB;
    mem.lock_memory = false;
    // Two unbound logical regions exercise local/foreign ordering portably; production supplies the
    // physical node IDs and BlockPool installs mbind policies for the same ranges. The extra local
    // block holds a fractional small-object arena, which must not enter or disable the fixed-head
    // score table.
    mem.numa_regions = {{std::nullopt, 128 * KiB}, {std::nullopt, 64 * KiB}};
    EvictionConfig ev;
    AccessScoreConfig score;
    score.increment = 2.0;
    score.decay = 0.5;
    Index index;
    auto tm = TierManager::open(tiers, mem, ev, ssd, hdd, index, 256 * KiB, 8,
                                /*direct_io=*/false, score);
    CHECK(tm.has_value());
    if (!tm) {
        fs::remove_all(base);
        return;
    }

    constexpr Size kSize = 68 * KiB; // disk-backed object with one whole 64 KiB buddy head
    const std::array<std::byte, 128> small{};
    std::vector<std::byte> cold(kSize, std::byte{0x19});
    std::vector<std::byte> hot(kSize, std::byte{0x5A});
    const auto small_key = hash_key("/fractional-local");
    const auto cold_key = hash_key("/cold-local");
    const auto hot_key = hash_key("/hot-remote");
    CHECK(tm->store(small_key, ByteView(small.data(), small.size()), 0).has_value());
    CHECK(tm->store(cold_key, ByteView(cold.data(), cold.size()), 0).has_value());
    CHECK(tm->store(hot_key, ByteView(hot.data(), hot.size()), 0).has_value());
    CHECK_EQ(index.lookup(small_key)->head.block, 0u);
    CHECK_EQ(index.lookup(cold_key)->head.block, 1u);
    CHECK_EQ(index.lookup(hot_key)->head.block, 2u);
    CHECK(!index.score_external(small_key));
    CHECK(index.score_external(cold_key));
    CHECK(index.score_external(hot_key));
    CHECK(!index.score(cold_key).has_value());
    CHECK(!index.score(hot_key).has_value());
    tm->touch(small_key); // the fractional head's score remains Index-owned
    CHECK_EQ(*index.score(small_key), 2.0);

    // An active zero-copy reader pins the remote block, so the first pass must leave it in place.
    const auto pin = tm->pin_head(hot_key); // one successful read: remote score becomes 2
    CHECK(pin.has_value());
    CHECK(!tm->promote_hot_remote_block());
    if (pin) tm->unpin_head(*pin);

    CHECK(tm->promote_hot_remote_block());
    const auto promotion = tm->numa_promotion_stats();
    CHECK_EQ(promotion.count, std::uint64_t(1));
    CHECK_EQ(promotion.bytes_moved, std::uint64_t(128 * KiB));
    CHECK(promotion.total_ns > 0);
    CHECK(promotion.max_ns > 0);
    const auto cold_meta = index.lookup(cold_key);
    const auto hot_meta = index.lookup(hot_key);
    CHECK(cold_meta && hot_meta);
    if (cold_meta && hot_meta) {
        CHECK_EQ(cold_meta->head.block, 2u);
        CHECK_EQ(hot_meta->head.block, 1u);
        CHECK(index.score_external(cold_key));
        CHECK(index.score_external(hot_key));
        CHECK_EQ(*tm->access_score(hot_key), 2.0); // score follows the key, not the physical block
    }
    const auto cold_head = tm->head_view(cold_key);
    const auto hot_head = tm->head_view(hot_key);
    CHECK(cold_head && hot_head);
    if (cold_head && hot_head) {
        CHECK(std::all_of(cold_head->begin(), cold_head->end(),
                          [](std::byte b) { return b == std::byte{0x19}; }));
        CHECK(std::all_of(hot_head->begin(), hot_head->end(),
                          [](std::byte b) { return b == std::byte{0x5A}; }));
    }
    CHECK(!tm->promote_hot_remote_block()); // local minimum is now hotter than remote maximum
    tm->decay_access_scores();
    CHECK_EQ(*tm->access_score(hot_key), 2.0);  // pin + head_view, then x0.5
    CHECK_EQ(*tm->access_score(cold_key), 1.0); // head_view, then x0.5
    CHECK_EQ(*tm->access_score(small_key), 1.0);

    // Exercise the priority gate under TSan: a tight promoter must repeatedly yield while the
    // rescore thread takes exclusive maintenance turns, with neither overlap nor deadlock.
    std::atomic<bool> promoter_ready{false};
    std::atomic<bool> start_maintenance{false};
    std::atomic<bool> rescoring_done{false};
    std::atomic<unsigned> promotion_calls{0};
    std::thread promoter([&] {
        promoter_ready.store(true, std::memory_order_release);
        while (!start_maintenance.load(std::memory_order_acquire)) std::this_thread::yield();
        do {
            tm->promote_hot_remote_block();
            promotion_calls.fetch_add(1, std::memory_order_relaxed);
        } while (!rescoring_done.load(std::memory_order_acquire));
    });
    while (!promoter_ready.load(std::memory_order_acquire)) std::this_thread::yield();
    start_maintenance.store(true, std::memory_order_release);
    for (unsigned i = 0; i < 50; ++i) tm->decay_access_scores();
    rescoring_done.store(true, std::memory_order_release);
    promoter.join();
    CHECK(promotion_calls.load(std::memory_order_relaxed) > 0);
    fs::remove_all(base);
}

TEST("tier_manager: exact ram_head objects pack into promotable allocation blocks") {
    const std::string base =
        (fs::temp_directory_path() / ("goblin-head-pack-" + std::to_string(::getpid()))).string();
    PoolConfig ssd, hdd;
    ssd.stripe_unit = 64 * KiB;
    ssd.dirs.push_back(base + "/ssd");
    fs::create_directories(ssd.dirs.front());

    TierSizes tiers;
    tiers.ram_head = 256 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 2 * MiB;
    mem.small_total_bytes = 2 * MiB; // trailing split reservation must stay outside score/promotion
    mem.block_bytes = 2 * MiB;
    mem.lock_memory = false;
    mem.use_hugepages = false;
    mem.numa_regions = {{std::nullopt, 2 * MiB}, {std::nullopt, 2 * MiB}};
    EvictionConfig ev;
    Index index;
    auto tm = TierManager::open(tiers, mem, ev, ssd, hdd, index, 256 * KiB, 8,
                                /*direct_io=*/false);
    CHECK(tm.has_value());
    if (!tm) {
        fs::remove_all(base);
        return;
    }

    constexpr unsigned kHeadsPerBlock = 8;
    constexpr unsigned kObjects = 2 * kHeadsPerBlock;
    std::vector<Digest> keys;
    keys.reserve(kObjects);
    for (unsigned i = 0; i < kObjects; ++i) {
        keys.push_back(hash_key("/packed/" + std::to_string(i)));
        std::vector<std::byte> value(tiers.ram_head, static_cast<std::byte>(i));
        CHECK(tm->store(keys.back(), ByteView(value.data(), value.size()), 0).has_value());
    }

    // Eight exact-size heads occupy each 2 MiB buddy block. They are RAM-only, so no backing
    // object files should have been created in the SSD pool.
    for (unsigned i = 0; i < kHeadsPerBlock; ++i) {
        const auto meta = index.lookup(keys[i]);
        CHECK(meta.has_value());
        if (meta) CHECK_EQ(meta->head.block, 0u);
    }
    for (unsigned i = kHeadsPerBlock; i < kObjects; ++i) {
        const auto meta = index.lookup(keys[i]);
        CHECK(meta.has_value());
        if (meta) CHECK_EQ(meta->head.block, 1u);
    }
    std::size_t files = 0;
    for (const auto& entry : fs::recursive_directory_iterator(base))
        if (entry.is_regular_file()) ++files;
    CHECK_EQ(files, std::size_t(0));

    // One remote hit makes that complete block hotter. Promotion moves all eight heads together
    // and rewrites each key's locator while preserving its bytes.
    CHECK(tm->head_view(keys[kHeadsPerBlock]).has_value());
    CHECK(tm->promote_hot_remote_block());
    const auto promotion = tm->numa_promotion_stats();
    CHECK_EQ(promotion.count, std::uint64_t(1));
    CHECK_EQ(promotion.bytes_moved, std::uint64_t(4 * MiB));
    for (unsigned i = 0; i < kObjects; ++i) {
        const auto meta = index.lookup(keys[i]);
        CHECK(meta.has_value());
        if (!meta) continue;
        CHECK_EQ(meta->head.block, i < kHeadsPerBlock ? 1u : 0u);
        const auto head = tm->head_view(keys[i]);
        CHECK(head.has_value());
        if (head)
            CHECK(std::all_of(head->begin(), head->end(), [i](std::byte b) {
                return b == static_cast<std::byte>(i);
            }));
    }
    fs::remove_all(base);
}

TEST("tier_manager capacity integration: fallocate admission evicts without generation litter") {
#if !defined(__linux__)
    std::println("    (skipped: GOBLIN_TEST_CAPACITY_DIR integration requires Linux)");
    return;
#else
    const char* configured_root = std::getenv("GOBLIN_TEST_CAPACITY_DIR");
    if (!configured_root || configured_root[0] == '\0') {
        std::println("    (skipped: set GOBLIN_TEST_CAPACITY_DIR to an empty tiny ext4 mount)");
        return;
    }

    const fs::path root(configured_root);
    if (!fs::is_directory(root)) {
        CHECK(false && "GOBLIN_TEST_CAPACITY_DIR must name a directory");
        return;
    }
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path base =
        root / ("goblin-capacity-" + std::to_string(::getpid()) + "-" + std::to_string(nonce));
    std::error_code mkdir_error;
    fs::create_directory(base, mkdir_error);
    CHECK(!mkdir_error);
    if (mkdir_error) return;
    struct UniqueDirCleanup {
        fs::path path;
        ~UniqueDirCleanup() {
            std::error_code ignored;
            fs::remove_all(path, ignored); // `path` is the unique child, never the supplied mount root
        }
    } cleanup{base};

    struct statvfs vfs {};
    CHECK_EQ(::statvfs(base.c_str(), &vfs), 0);
    const Size available = static_cast<Size>(vfs.f_bavail) * static_cast<Size>(vfs.f_frsize);
    if (available < 8 * MiB) {
        std::println("    (skipped: capacity mount has less than 8 MiB available)");
        return;
    }
    // Refuse a mistakenly configured normal filesystem instead of trying to fill it. The integration
    // contract is deliberately a tiny, disposable ext4 mount under /var/tmp.
    if (available > 256 * MiB) {
        std::println("    (skipped: capacity mount is not tiny: {} MiB available)",
                     available / MiB);
        return;
    }

    PoolConfig ssd, hdd;
    ssd.stripe_unit = 64 * KiB;
    ssd.dirs.push_back(base.string());
    TierSizes tiers;
    tiers.ram_head = 4 * KiB;
    tiers.ssd_prefix = 16 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 4 * MiB;
    mem.block_bytes = 64 * KiB;
    mem.lock_memory = false;
    mem.use_hugepages = false;
    EvictionConfig ev;
    Index index;
    auto tm = TierManager::open(tiers, mem, ev, ssd, hdd, index, 256 * KiB,
                                /*write_buffers=*/4, /*direct_io=*/false,
                                AccessScoreConfig{});
    CHECK(tm.has_value());
    if (!tm) return;

    const Size object_bytes =
        align_down(std::clamp<Size>(available / 8, 1 * MiB, 8 * MiB), kDeviceBlock);
    const std::size_t object_count = static_cast<std::size_t>(available / object_bytes) + 8;
    std::vector<std::byte> value(static_cast<std::size_t>(object_bytes), std::byte{0x5A});
    std::vector<Digest> keys;
    constexpr std::size_t kConcurrentWriters = 4;
    keys.reserve(object_count + kConcurrentWriters);

    // Prove begin_store performed a real KEEP_SIZE reservation before accepting any body bytes:
    // the generation has size zero but already owns at least its final number of filesystem blocks.
    keys.push_back(hash_key("/capacity/0"));
    auto first_result = tm->begin_store(keys.front(), object_bytes);
    CHECK(first_result.has_value());
    if (!first_result) return;
    std::optional<TierManager::StoreHandle> first;
    first.emplace(std::move(*first_result));
    std::vector<fs::path> reserved_files;
    for (const auto& entry : fs::directory_iterator(base))
        if (entry.is_regular_file()) reserved_files.push_back(entry.path());
    CHECK_EQ(reserved_files.size(), std::size_t(1));
    if (reserved_files.size() != 1) return;
    const fs::path first_generation_path = reserved_files.front();
    if (reserved_files.size() == 1) {
        struct stat st {};
        CHECK_EQ(::stat(reserved_files.front().c_str(), &st), 0);
        CHECK_EQ(st.st_size, off_t(0));
        CHECK(static_cast<Size>(st.st_blocks) * 512 >= object_bytes);
    }
    CHECK(first->write(ByteView(value.data(), value.size())).has_value());
    CHECK(first->commit(/*flags=*/0).has_value());
    first.reset(); // close the writer, then model a reader that opened the committed generation
    const int reader_fd = ::open(first_generation_path.c_str(), O_RDONLY);
    CHECK(reader_fd >= 0);
    if (reader_fd < 0) return;
    struct ReaderFd {
        int fd;
        ~ReaderFd() { ::close(fd); }
    } reader{reader_fd};

    bool all_admitted = true;
    for (std::size_t i = 1; i < object_count; ++i) {
        const Digest digest = hash_key("/capacity/" + std::to_string(i));
        value.front() = static_cast<std::byte>(i & 0xFF);
        const auto stored =
            tm->store(digest, ByteView(value.data(), value.size()), static_cast<std::uint32_t>(i));
        CHECK(stored.has_value());
        if (!stored) {
            all_admitted = false;
            break;
        }
        keys.push_back(digest);
    }
    CHECK(all_admitted);
    CHECK(static_cast<Size>(keys.size()) * object_bytes > available);
    CHECK(index.contains(keys.back()));
    CHECK(!index.contains(keys.front()));
    CHECK(index.size() < keys.size());
    std::size_t early_missing = 0;
    for (std::size_t i = 0; i < keys.size() / 2; ++i)
        if (!index.contains(keys[i])) ++early_missing;
    CHECK(early_missing > 1); // reclaim continued after unlinking the still-open first inode

    // The first generation is gone from the namespace and Index, but its reader keeps the inode's
    // blocks charged to the filesystem. Admission above had to observe ENOSPC again and evict other
    // committed generations rather than assuming one successful unlink necessarily freed capacity.
    CHECK(!fs::exists(first_generation_path));
    struct stat pinned_stat {};
    CHECK_EQ(::fstat(reader.fd, &pinned_stat), 0);
    CHECK_EQ(pinned_stat.st_nlink, nlink_t(0));
    CHECK(static_cast<Size>(pinned_stat.st_blocks) * 512 >= object_bytes);
    std::byte pinned_first_byte{};
    CHECK_EQ(::pread(reader.fd, &pinned_first_byte, 1, 0), ssize_t(1));
    CHECK_EQ(pinned_first_byte, std::byte{0x5A});

    // Start several writers together on the already-full capacity domain. The slow path serializes
    // victim selection, while reservations and body writes remain private to each immutable
    // generation. All writers must make progress even while the unlinked reader inode stays pinned.
    std::array<Digest, kConcurrentWriters> concurrent_keys{};
    std::array<bool, kConcurrentWriters> writer_ok{};
    std::atomic<unsigned> writers_ready{0};
    std::atomic<bool> start_writers{false};
    std::vector<std::thread> writers;
    writers.reserve(kConcurrentWriters);
    for (std::size_t i = 0; i < kConcurrentWriters; ++i) {
        concurrent_keys[i] = hash_key("/capacity/concurrent/" + std::to_string(i));
        writers.emplace_back([&, i] {
            writers_ready.fetch_add(1, std::memory_order_release);
            while (!start_writers.load(std::memory_order_acquire)) std::this_thread::yield();
            writer_ok[i] =
                tm->store(concurrent_keys[i], ByteView(value.data(), value.size()),
                          static_cast<std::uint32_t>(0xC0 + i))
                    .has_value();
        });
    }
    while (writers_ready.load(std::memory_order_acquire) != kConcurrentWriters)
        std::this_thread::yield();
    start_writers.store(true, std::memory_order_release);
    for (auto& writer : writers) writer.join();
    std::size_t concurrent_live = 0;
    for (std::size_t i = 0; i < kConcurrentWriters; ++i) {
        CHECK(writer_ok[i]);
        keys.push_back(concurrent_keys[i]);
        if (index.contains(concurrent_keys[i])) ++concurrent_live;
    }
    CHECK(concurrent_live > 0); // the final committed generation cannot have been reclaimed later
    CHECK_EQ(::fstat(reader.fd, &pinned_stat), 0);
    CHECK_EQ(pinned_stat.st_nlink, nlink_t(0));

    // Make the BLOCK precondition explicit rather than relying on how much slack ext4 happened to
    // leave after the concurrent wave. Fixed-size EVICT stores consume whole-object gaps first; a
    // final smaller disk-backed EVICT object leaves approximately half an object of free blocks.
    const auto read_available = [&](Size& out) {
        struct statvfs current {};
        if (::statvfs(base.c_str(), &current) != 0) return false;
        out = static_cast<Size>(current.f_bavail) * static_cast<Size>(current.f_frsize);
        return true;
    };
    Size available_now = 0;
    CHECK(read_available(available_now));
    std::size_t refill_sequence = 0;
    const std::size_t refill_limit = static_cast<std::size_t>(available / object_bytes) + 2;
    while (available_now >= 2 * object_bytes && refill_sequence < refill_limit) {
        const Digest digest =
            hash_key("/capacity/refill/" + std::to_string(refill_sequence++));
        const auto stored = tm->store(digest, ByteView(value.data(), value.size()), 0xE1,
                                      /*expiry=*/0, WriteMode::evict);
        CHECK(stored.has_value());
        if (!stored) break;
        keys.push_back(digest);
        Size next_available = 0;
        CHECK(read_available(next_available));
        CHECK(next_available < available_now);
        if (next_available >= available_now) break;
        available_now = next_available;
    }
    if (available_now >= object_bytes) {
        const Size filler_bytes =
            align_down(available_now - object_bytes / 2, kDeviceBlock);
        CHECK(filler_bytes > tiers.ram_head);
        CHECK(filler_bytes < 2 * object_bytes);
        std::vector<std::byte> filler(static_cast<std::size_t>(filler_bytes), std::byte{0xEF});
        const Digest filler_digest = hash_key("/capacity/refill/final");
        const auto stored =
            tm->store(filler_digest, ByteView(filler.data(), filler.size()), 0xEF,
                      /*expiry=*/0, WriteMode::evict);
        CHECK(stored.has_value());
        if (stored) keys.push_back(filler_digest);
        CHECK(read_available(available_now));
    }
    CHECK(available_now < object_bytes);

    struct LiveBeforeBlock {
        Digest digest;
        ObjectMeta meta;
    };
    std::vector<LiveBeforeBlock> live_before_block;
    for (const auto& digest : keys)
        if (const auto meta = index.lookup(digest))
            live_before_block.push_back({digest, *meta});
    CHECK_EQ(live_before_block.size(), index.size());
    std::set<std::string> names_before_block;
    for (const auto& entry : fs::directory_iterator(base))
        if (entry.is_regular_file())
            names_before_block.insert(entry.path().filename().string());

    // The same manager serves both protocol policies. BLOCK must trust the failed reservation and
    // return without selecting a victim, even though EVICT entries are available on this st_dev.
    const Digest blocked_digest = hash_key("/capacity/block-must-not-evict");
    const auto blocked = tm->store(blocked_digest, ByteView(value.data(), value.size()), 0xB10C,
                                   /*expiry=*/0, WriteMode::block);
    CHECK(!blocked.has_value());
    if (!blocked) CHECK_EQ(blocked.error().code, Errc::out_of_space);
    CHECK(!index.contains(blocked_digest));
    CHECK_EQ(index.size(), live_before_block.size());
    for (const auto& before : live_before_block) {
        const auto after = index.lookup(before.digest);
        CHECK(after.has_value());
        if (!after) continue;
        CHECK_EQ(after->size, before.meta.size);
        CHECK_EQ(after->flags, before.meta.flags);
        CHECK_EQ(after->expiry, before.meta.expiry);
        CHECK_EQ(after->etag, before.meta.etag);
        CHECK_EQ(after->file_generation, before.meta.file_generation);
        CHECK_EQ(after->head.block, before.meta.head.block);
        CHECK_EQ(after->head.offset, before.meta.head.offset);
        CHECK_EQ(after->head.len, before.meta.head.len);
    }
    std::set<std::string> names_after_block;
    for (const auto& entry : fs::directory_iterator(base))
        if (entry.is_regular_file())
            names_after_block.insert(entry.path().filename().string());
    CHECK(names_after_block == names_before_block); // failed generation was fully unlinked

    std::map<std::string, Size> expected_files;
    for (const auto& digest : keys) {
        const auto meta = index.lookup(digest);
        if (!meta) continue;
        CHECK(meta->file_generation != 0);
        expected_files.emplace(digest.hex() + ".g" + std::to_string(meta->file_generation),
                               align_up(meta->size, kDeviceBlock));
    }
    std::map<std::string, Size> actual_files;
    for (const auto& entry : fs::directory_iterator(base)) {
        CHECK(entry.is_regular_file());
        actual_files.emplace(entry.path().filename().string(), entry.file_size());
    }
    CHECK(actual_files == expected_files);
    CHECK_EQ(actual_files.size(), index.size());
#endif
}

TEST("tier_manager: quarantine is generation-safe and removes the poisoned incarnation") {
    const std::string base =
        (fs::temp_directory_path() / ("goblin-quarantine-" + std::to_string(::getpid()))).string();
    fs::create_directories(base);
    PoolConfig ssd;
    ssd.dirs = {base};
    ssd.stripe_unit = 4 * KiB;
    TierSizes tiers;
    tiers.ram_head = 4 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 4 * MiB;
    mem.block_bytes = 256 * KiB;
    mem.lock_memory = false;
    Index index;
    auto tm = TierManager::open(tiers, mem, EvictionConfig{}, ssd, {}, index);
    CHECK(tm.has_value());
    if (tm) {
        const auto digest = hash_key("/quarantine");
        std::vector<std::byte> first(16 * KiB, std::byte{0x11});
        std::vector<std::byte> second(16 * KiB, std::byte{0x22});
        CHECK(tm->store(digest, ByteView(first.data(), first.size()), 1).has_value());
        const auto old = index.lookup(digest);
        CHECK(old.has_value());
        CHECK(tm->store(digest, ByteView(second.data(), second.size()), 2).has_value());
        const auto current = index.lookup(digest);
        CHECK(current.has_value());
        if (old && current) {
            CHECK(old->file_generation != current->file_generation);
            CHECK(!tm->quarantine_object(digest, old->file_generation));
            CHECK(index.contains(digest));
            CHECK(tm->quarantine_object(digest, current->file_generation));
            CHECK(!index.contains(digest));
            const auto health = tm->storage_health_snapshot();
            CHECK_EQ(health.quarantined_objects, std::uint64_t(1));
            CHECK_EQ(health.quarantine_failures, std::uint64_t(0));
        }
    }
    fs::remove_all(base);
}

TEST("tier_manager: proactive watermark maintenance reclaims filesystem-local objects") {
    const std::string base =
        (fs::temp_directory_path() / ("goblin-watermark-" + std::to_string(::getpid()))).string();
    fs::create_directories(base);
    PoolConfig ssd;
    ssd.dirs = {base};
    ssd.stripe_unit = 4 * KiB;
    TierSizes tiers;
    tiers.ram_head = 4 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 4 * MiB;
    mem.block_bytes = 256 * KiB;
    mem.lock_memory = false;
    EvictionConfig eviction;
    eviction.high_watermark = 1e-12;
    eviction.low_watermark = 5e-13;
    Index index;
    auto tm = TierManager::open(tiers, mem, eviction, ssd, {}, index);
    CHECK(tm.has_value());
    if (tm) {
        const auto digest = hash_key("/watermark");
        std::vector<std::byte> value(16 * KiB, std::byte{0x33});
        CHECK(tm->store(digest, ByteView(value.data(), value.size()), 0).has_value());
        CHECK_EQ(tm->reclaim_to_watermarks(4), std::size_t(1));
        CHECK(!index.contains(digest));
        const auto health = tm->storage_health_snapshot();
        CHECK_EQ(health.watermark_scans, std::uint64_t(1));
        CHECK_EQ(health.watermark_reclaim_runs, std::uint64_t(1));
        CHECK_EQ(health.watermark_reclaimed_objects, std::uint64_t(1));
    }
    fs::remove_all(base);
}

TEST("tier_manager: a short tail read quarantines the indexed generation") {
#if defined(__linux__)
    if (!core::Reactor::available()) {
        std::println("    (skipped: built without liburing)");
        return;
    }
    auto reactor = core::Reactor::create();
    if (!reactor) {
        std::println("    (skipped: io_uring unavailable: {})", reactor.error().detail);
        return;
    }
    const std::string base =
        (fs::path("/var/tmp") / ("goblin-tail-poison-" + std::to_string(::getpid()))).string();
    fs::create_directories(base);
    PoolConfig ssd;
    ssd.dirs = {base};
    ssd.stripe_unit = 4 * KiB;
    TierSizes tiers;
    tiers.ram_head = 4 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 4 * MiB;
    mem.block_bytes = 256 * KiB;
    mem.lock_memory = false;
    Index index;
    auto tm = TierManager::open(tiers, mem, EvictionConfig{}, ssd, {}, index);
    CHECK(tm.has_value());
    if (tm) {
        const auto digest = hash_key("/short-tail");
        std::vector<std::byte> value(16 * KiB, std::byte{0x44});
        CHECK(tm->store(digest, ByteView(value.data(), value.size()), 0).has_value());
        auto snapshot = tm->open_snapshot(digest);
        CHECK(snapshot.has_value());
        if (snapshot && snapshot->rs) {
            const auto object = fs::path(base) /
                (digest.hex() + ".g" + std::to_string(snapshot->meta.file_generation));
            fs::resize_file(object, 0);
            void* allocation = nullptr;
            CHECK_EQ(::posix_memalign(&allocation, kDeviceBlock, kDeviceBlock), 0);
            if (allocation) {
                const auto read = snapshot->rs->read(
                    *reactor, tiers.ram_head,
                    MutBytes(static_cast<std::byte*>(allocation), kDeviceBlock));
                CHECK(!read.has_value());
                std::free(allocation);
            }
            CHECK(!index.contains(digest));
            const auto health = tm->storage_health_snapshot();
            CHECK_EQ(health.quarantined_objects, std::uint64_t(1));
            CHECK_EQ(health.state, TierManager::StorageHealthState::degraded);
        }
        if (snapshot && snapshot->pin.valid) tm->unpin_head(snapshot->pin);
    }
    fs::remove_all(base);
#else
    std::println("    (skipped: io_uring tail corruption probe is Linux-only)");
#endif
}
