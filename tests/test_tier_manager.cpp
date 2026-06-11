#include "mini_test.hpp"

#include "goblin/core/reactor.hpp"
#include "goblin/crypto/sha256.hpp"
#include "goblin/storage/index.hpp"
#include "goblin/storage/tier_manager.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <print>
#include <string>
#include <thread>
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
    const Size sz = 16 * KiB;
    auto pattern = [](int i) {
        std::vector<std::byte> v(16 * KiB);
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
