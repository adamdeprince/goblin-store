#include "mini_test.hpp"

#include "goblin/crypto/sha256.hpp"
#include "goblin/storage/index.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
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

TEST("index: score ownership moves atomically to and from external storage") {
    Index ix(/*shard_bits=*/2);
    const auto key = hash_key("/external-score");
    const auto absent = hash_key("/absent-score");
    ix.set(key, meta(256 * KiB));
    CHECK(ix.increment_score(key, 7.5));

    const auto extracted = ix.extract_score(key);
    CHECK(extracted.has_value());
    CHECK_EQ(*extracted, 7.5);
    CHECK(ix.score_external(key));
    CHECK(!ix.score(key).has_value());
    CHECK(!ix.increment_score(key, 1.0));

    CHECK(!ix.extract_score(key).has_value());
    CHECK(!ix.restore_score(key, std::numeric_limits<double>::quiet_NaN()));
    CHECK(!ix.restore_score(key, -1.0));
    CHECK(ix.score_external(key));
    CHECK(ix.restore_score(key, 7.5));
    CHECK(!ix.score_external(key));
    CHECK_EQ(*ix.score(key), 7.5);
    CHECK(!ix.increment_score(key, std::numeric_limits<double>::quiet_NaN()));
    CHECK(!ix.increment_score(key, std::numeric_limits<double>::infinity()));
    CHECK(!ix.increment_score(key, 0.0));
    ix.decay_scores(std::numeric_limits<double>::quiet_NaN());
    CHECK_EQ(*ix.score(key), 7.5);
    CHECK(!ix.restore_score(key, 11.0));
    CHECK_EQ(*ix.score(key), 7.5);

    CHECK(!ix.score_external(absent));
    CHECK(!ix.extract_score(absent).has_value());
    CHECK(!ix.restore_score(absent, 1.0));
}

TEST("index: set_with_score explicitly selects local or external score ownership") {
    Index ix(/*shard_bits=*/2);
    const auto key = hash_key("/set-with-score");
    auto first = meta(256 * KiB, 1);
    first.head = HeadLoc{3, 0, 256 * KiB};

    ix.set_with_score(key, first, 4.5);
    CHECK_EQ(ix.lookup(key)->flags, std::uint32_t(1));
    CHECK_EQ(*ix.score(key), 4.5);
    CHECK(!ix.score_external(key));

    auto external = meta(512 * KiB, 2);
    external.head = HeadLoc{7, 256 * KiB, 256 * KiB};
    ix.set_with_score(key, external, std::nullopt);
    CHECK_EQ(ix.lookup(key)->flags, std::uint32_t(2));
    CHECK_EQ(ix.lookup(key)->head.block, 7u);
    CHECK(ix.score_external(key));
    CHECK(!ix.score(key).has_value());

    // Ordinary set still preserves whichever ownership/value the entry already has.
    auto external_update = external;
    external_update.flags = 3;
    ix.set(key, external_update);
    CHECK_EQ(ix.lookup(key)->flags, std::uint32_t(3));
    CHECK(ix.score_external(key));

    ix.set_with_score(key, meta(2 * KiB, 4), 9.25);
    CHECK_EQ(ix.lookup(key)->flags, std::uint32_t(4));
    CHECK(!ix.score_external(key));
    CHECK_EQ(*ix.score(key), 9.25);
}

TEST("index: decay and increments skip externally owned scores") {
    Index ix(/*shard_bits=*/2);
    const auto external = hash_key("/external-decay");
    const auto local = hash_key("/local-decay");
    ix.set(external, meta(256 * KiB));
    ix.set(local, meta(4 * KiB));
    CHECK(ix.increment_score(external, 12.0));
    CHECK(ix.increment_score(local, 8.0));
    CHECK_EQ(*ix.extract_score(external), 12.0);

    ix.decay_scores(0.25);
    CHECK(ix.score_external(external));
    CHECK(!ix.increment_score(external, 100.0));
    CHECK_EQ(*ix.score(local), 2.0);
    CHECK(ix.restore_score(external, 12.0));
    CHECK_EQ(*ix.score(external), 12.0);
}

TEST("index: concurrent increments are safe across external ownership transfers") {
    Index ix(/*shard_bits=*/1);
    const auto key = hash_key("/ownership-race");
    ix.set(key, meta(256 * KiB));

    constexpr int kThreads = 4;
    constexpr int kReads = 20'000;
    constexpr int kTransfers = 5'000;
    std::atomic<std::uint64_t> accepted{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < kThreads; ++t) {
        readers.emplace_back([&] {
            for (int i = 0; i < kReads; ++i) {
                if (ix.increment_score(key, 1.0))
                    accepted.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (int i = 0; i < kTransfers; ++i) {
        const auto extracted = ix.extract_score(key);
        CHECK(extracted.has_value());
        CHECK(ix.score_external(key));
        CHECK(ix.restore_score(key, *extracted));
    }
    for (auto& reader : readers) reader.join();

    CHECK(!ix.score_external(key));
    CHECK_EQ(*ix.score(key), static_cast<double>(accepted.load(std::memory_order_relaxed)));
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
