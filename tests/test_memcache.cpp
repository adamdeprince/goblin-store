#include "mini_test.hpp"

#include "goblin/protocol/memcache/protocol.hpp"

#include <cstdint>

using namespace goblin::memcache;

TEST("memcache: parse get") {
    const auto c = parse_command("get foo");
    CHECK(c.has_value());
    CHECK(c->verb == Verb::get);
    CHECK(c->key == "foo");
}

TEST("memcache: parse set, with and without noreply") {
    const auto c = parse_command("set foo 7 0 5");
    CHECK(c.has_value());
    CHECK(c->verb == Verb::set);
    CHECK(c->is_storage());
    CHECK(c->key == "foo");
    CHECK_EQ(c->flags, std::uint32_t(7));
    CHECK_EQ(c->exptime, std::uint32_t(0));
    CHECK_EQ(c->bytes, std::uint64_t(5));
    CHECK(!c->noreply);

    const auto c2 = parse_command("set bar 1 600 10 noreply");
    CHECK(c2.has_value());
    CHECK(c2->noreply);
    CHECK_EQ(c2->exptime, std::uint32_t(600));
}

TEST("memcache: parse cas command") {
    const auto c = parse_command("cas foo 1 0 5 42");
    CHECK(c.has_value());
    CHECK(c->verb == Verb::cas);
    CHECK(c->is_storage());
    CHECK(c->key == "foo");
    CHECK_EQ(c->flags, std::uint32_t(1));
    CHECK_EQ(c->bytes, std::uint64_t(5));
    CHECK_EQ(c->cas, std::uint64_t(42));
    CHECK(!c->noreply);
    const auto c2 = parse_command("cas foo 0 0 5 7 noreply");
    CHECK(c2.has_value() && c2->noreply && c2->cas == std::uint64_t(7));
    CHECK(!parse_command("cas foo 0 0 5").has_value()); // missing the cas field
}

TEST("memcache: parse delete / version / quit") {
    CHECK(parse_command("delete foo")->verb == Verb::del);
    CHECK(parse_command("delete foo noreply")->noreply);
    CHECK(parse_command("version")->verb == Verb::version);
    CHECK(parse_command("quit")->verb == Verb::quit);
}

TEST("memcache: malformed commands are rejected") {
    CHECK(!parse_command("set foo").has_value());       // too few fields
    CHECK(!parse_command("set foo a b c").has_value()); // non-numeric flags/exptime/bytes
    CHECK(!parse_command("get").has_value());           // missing key
    CHECK(!parse_command("bogus").has_value());         // unknown verb
    CHECK(!parse_command("").has_value());              // empty
}

TEST("memcache: VALUE header formatting") {
    CHECK(value_header("foo", 7, 5) == "VALUE foo 7 5\r\n");
    CHECK(value_header("a/b.bin", 0, 1048576) == "VALUE a/b.bin 0 1048576\r\n");
}

TEST("memcache: exptime -> absolute expiry (never / relative / absolute, 30-day boundary)") {
    const std::uint32_t now = 1'000'000;
    CHECK_EQ(exptime_to_expiry(0, now), std::uint32_t(0));            // 0 = never
    CHECK_EQ(exptime_to_expiry(60, now), now + 60);                  // small -> relative
    constexpr std::uint32_t kMonth = 60u * 60u * 24u * 30u;          // 2592000
    CHECK_EQ(exptime_to_expiry(kMonth, now), now + kMonth);          // exactly 30 days = still relative
    CHECK_EQ(exptime_to_expiry(kMonth + 1, now), kMonth + 1);        // just over -> absolute, unchanged
    CHECK_EQ(exptime_to_expiry(2'000'000'000u, now), 2'000'000'000u); // large absolute Unix time
}
