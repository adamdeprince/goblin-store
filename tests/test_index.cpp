#include "mini_test.hpp"

#include "goblin/crypto/sha256.hpp"
#include "goblin/storage/index.hpp"

#include <cstdint>
#include <string>

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
