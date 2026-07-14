#include "mini_test.hpp"

#include "goblin/crypto/sha256.hpp"
#include "goblin/storage/index.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using namespace goblin;
using namespace goblin::storage;
using goblin::crypto::hash_key;

static ObjectMeta meta(Size sz, std::uint32_t flags = 0) {
    ObjectMeta m;
    m.size = sz;
    m.flags = flags;
    return m;
}

TEST("index: set / lookup roundtrip") {
    Index ix;
    const auto k = hash_key("/a");
    CHECK(!ix.lookup(k).has_value());
    ix.set(k, meta(1234, 7));
    const auto m = ix.lookup(k);
    CHECK(m.has_value());
    CHECK_EQ(m->size, Size(1234));
    CHECK_EQ(m->flags, std::uint32_t(7));
    CHECK(ix.contains(k));
    CHECK_EQ(ix.size(), std::size_t(1));
}

TEST("index: add only-if-absent, replace only-if-present (memcache semantics)") {
    Index ix;
    const auto k = hash_key("/b");
    CHECK(!ix.replace(k, meta(1)));         // absent -> false
    CHECK(ix.add(k, meta(10)));             // inserted
    CHECK(!ix.add(k, meta(20)));            // already present -> false
    CHECK_EQ(ix.lookup(k)->size, Size(10)); // unchanged by the failed add
    CHECK(ix.replace(k, meta(30)));         // present -> replaced
    CHECK_EQ(ix.lookup(k)->size, Size(30));
}

TEST("index: erase") {
    Index ix;
    const auto k = hash_key("/c");
    ix.set(k, meta(5));
    CHECK(ix.erase(k));
    CHECK(!ix.erase(k)); // already gone
    CHECK(!ix.lookup(k).has_value());
}

TEST("index: set_head updates the RAM-head locator") {
    Index ix;
    const auto k = hash_key("/d");
    HeadLoc hl;
    hl.block = 3;
    hl.offset = 4096;
    hl.len = 256 * 1024;
    CHECK(!ix.set_head(k, hl)); // absent
    ix.set(k, meta(9));
    CHECK(!ix.lookup(k)->head.resident());
    CHECK(ix.set_head(k, hl));
    const auto m = ix.lookup(k);
    CHECK(m->head.resident());
    CHECK_EQ(m->head.block, 3u);
    CHECK_EQ(m->head.len, std::uint32_t(256 * 1024));
}

TEST("index: many keys distribute across shards; size + clear") {
    Index ix(/*shard_bits=*/6);
    for (int i = 0; i < 1000; ++i) ix.set(hash_key("/obj/" + std::to_string(i)), meta(Size(i)));
    CHECK_EQ(ix.size(), std::size_t(1000));
    CHECK(ix.lookup(hash_key("/obj/500")).has_value());
    ix.clear();
    CHECK_EQ(ix.size(), std::size_t(0));
    CHECK(!ix.lookup(hash_key("/obj/500")).has_value());
}

TEST("index: concurrent read increments and decay update one atomic double score") {
    Index ix(/*shard_bits=*/2);
    const auto key = hash_key("/hot");
    ix.set(key, meta(64 * KiB));
    CHECK_EQ(*ix.score(key), 0.0);

    constexpr int kThreads = 4;
    constexpr int kReads = 4'000;
    std::atomic<bool> increments_ok{true};
    std::vector<std::thread> readers;
    for (int t = 0; t < kThreads; ++t)
        readers.emplace_back([&] {
            for (int i = 0; i < kReads; ++i)
                if (!ix.increment_score(key, 0.25)) increments_ok.store(false);
        });
    for (auto& reader : readers) reader.join();
    CHECK(increments_ok.load());
    CHECK_EQ(*ix.score(key), double(kThreads * kReads) * 0.25);

    ix.decay_scores(0.5);
    CHECK_EQ(*ix.score(key), double(kThreads * kReads) * 0.125);

    // Replacing the value keeps heat attached to the key; erase + reinsert starts a new key at zero.
    ix.set(key, meta(128 * KiB));
    CHECK_EQ(*ix.score(key), double(kThreads * kReads) * 0.125);
    CHECK(ix.erase(key));
    ix.set(key, meta(128 * KiB));
    CHECK_EQ(*ix.score(key), 0.0);
}

TEST("index: swapping backing blocks rewrites every resident head and preserves scores") {
    Index ix(/*shard_bits=*/2);
    const auto a = hash_key("/block/a");
    const auto b = hash_key("/block/b");
    const auto c = hash_key("/block/c");
    auto ma = meta(32 * KiB);
    auto mb = meta(32 * KiB);
    auto mc = meta(32 * KiB);
    ma.head = HeadLoc{1, 0, 32 * KiB};
    mb.head = HeadLoc{7, 0, 32 * KiB};
    mc.head = HeadLoc{7, 32 * KiB, 32 * KiB};
    ix.set(a, ma);
    ix.set(b, mb);
    ix.set(c, mc);
    CHECK(ix.increment_score(a, 2.0));
    CHECK(ix.increment_score(b, 9.0));
    CHECK(ix.increment_score(c, 3.0));

    CHECK_EQ(ix.swap_head_blocks(1, 7), std::size_t(3));
    CHECK_EQ(ix.lookup(a)->head.block, 7u);
    CHECK_EQ(ix.lookup(b)->head.block, 1u);
    CHECK_EQ(ix.lookup(c)->head.block, 1u);
    CHECK_EQ(*ix.score(a), 2.0);
    CHECK_EQ(*ix.score(b), 9.0);
    CHECK_EQ(*ix.score(c), 3.0);
}

TEST("index: minute decay can race read increments without a data race") {
    Index ix(/*shard_bits=*/1);
    const auto key = hash_key("/decay-race");
    ix.set(key, meta(64 * KiB));

    std::thread reader([&] {
        for (int i = 0; i < 50'000; ++i) ix.increment_score(key, 0.125);
    });
    std::thread decayer([&] {
        for (int i = 0; i < 2'000; ++i) ix.decay_scores(0.999);
    });
    reader.join();
    decayer.join();

    const double score = *ix.score(key);
    CHECK(std::isfinite(score));
    CHECK(score >= 0.0);
    CHECK(score <= 50'000 * 0.125);
}
