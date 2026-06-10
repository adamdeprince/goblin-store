#include "mini_test.hpp"

#include "goblin/crypto/sha256.hpp"
#include "goblin/storage/s3fifo.hpp"

#include <cstddef>
#include <optional>

using namespace goblin::storage;
using goblin::crypto::Digest;
using goblin::crypto::hash_key;

static Digest k(const char* s) { return hash_key(s); }

TEST("s3fifo: insert / resident / evict drains everything") {
    S3Fifo p(64);
    p.insert(k("a"));
    p.insert(k("b"));
    p.insert(k("c"));
    CHECK_EQ(p.resident(), std::size_t(3));
    CHECK(p.contains(k("a")));

    int n = 0;
    while (p.evict()) ++n;
    CHECK_EQ(n, 3);
    CHECK_EQ(p.resident(), std::size_t(0));
}

TEST("s3fifo: a touched item is promoted and survives, the un-touched one is evicted") {
    S3Fifo p(64);
    p.insert(k("a"));
    p.insert(k("b")); // both in the small queue
    p.touch(k("a"));  // a accessed

    const auto v = p.evict(); // a promoted to main; b evicted
    CHECK(v.has_value());
    CHECK(*v == k("b"));
    CHECK(p.contains(k("a")));
    CHECK(p.in_main(k("a")) == std::optional<bool>(true));
}

TEST("s3fifo: re-inserting a recently-evicted key (ghost hit) admits straight to main") {
    S3Fifo p(64);
    p.insert(k("x"));
    const auto v = p.evict(); // x un-accessed -> evicted into the ghost queue
    CHECK(v == std::optional<Digest>(k("x")));
    CHECK(!p.contains(k("x")));

    p.insert(k("x")); // seen in ghost -> admitted to main
    CHECK(p.in_main(k("x")) == std::optional<bool>(true));
}

TEST("s3fifo: remove drops an item without evicting it") {
    S3Fifo p(64);
    p.insert(k("a"));
    p.insert(k("b"));
    p.remove(k("a"));
    CHECK(!p.contains(k("a")));
    CHECK_EQ(p.resident(), std::size_t(1));

    const auto v = p.evict(); // only b remains (stale 'a' in the deque is skipped)
    CHECK(v.has_value());
    CHECK(*v == k("b"));
}
