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
