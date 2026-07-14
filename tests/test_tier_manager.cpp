#include "mini_test.hpp"

#include "goblin/core/reactor.hpp"
#include "goblin/crypto/sha256.hpp"
#include "goblin/storage/index.hpp"
#include "goblin/storage/tier_manager.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <print>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace goblin;
using namespace goblin::storage;
using goblin::crypto::hash_key;

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
    auto pattern = [sz](int i) {
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
    auto pattern = [sz](int i) {
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
    auto pattern = [sz](int i) {
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

    const Size sz = 4 * KiB;
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
    std::free(p);
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
    // physical node IDs and BlockPool installs mbind policies for the same ranges.
    mem.numa_regions = {{std::nullopt, 64 * KiB}, {std::nullopt, 64 * KiB}};
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
    std::vector<std::byte> cold(kSize, std::byte{0x19});
    std::vector<std::byte> hot(kSize, std::byte{0x5A});
    const auto cold_key = hash_key("/cold-local");
    const auto hot_key = hash_key("/hot-remote");
    CHECK(tm->store(cold_key, ByteView(cold.data(), cold.size()), 0).has_value());
    CHECK(tm->store(hot_key, ByteView(hot.data(), hot.size()), 0).has_value());
    CHECK_EQ(index.lookup(cold_key)->head.block, 0u);
    CHECK_EQ(index.lookup(hot_key)->head.block, 1u);

    // An active zero-copy reader pins the remote block, so the first pass must leave it in place.
    const auto pin = tm->pin_head(hot_key); // one successful read: remote score becomes 2
    CHECK(pin.has_value());
    CHECK(!tm->promote_hot_remote_block());
    if (pin) tm->unpin_head(*pin);

    CHECK(tm->promote_hot_remote_block());
    const auto cold_meta = index.lookup(cold_key);
    const auto hot_meta = index.lookup(hot_key);
    CHECK(cold_meta && hot_meta);
    if (cold_meta && hot_meta) {
        CHECK_EQ(cold_meta->head.block, 1u);
        CHECK_EQ(hot_meta->head.block, 0u);
        CHECK_EQ(*index.score(hot_key), 2.0); // score follows the key, not the physical block
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
    CHECK_EQ(*index.score(hot_key), 2.0);  // pin + head_view, then x0.5
    CHECK_EQ(*index.score(cold_key), 1.0); // head_view, then x0.5

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
