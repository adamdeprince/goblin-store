#include "mini_test.hpp"

#include "goblin/protocol/memcache/protocol.hpp"

#include <cstdint>

using namespace goblin::memcache;

TEST("memcache: parse get") {
    const auto c = parse_command("get foo");
    CHECK(c.has_value());
    CHECK(c->verb == Verb::get);
    CHECK(c->key == "foo");
    CHECK(c->extra_keys.empty());
}

TEST("memcache: parse every key in multi-key get and gets") {
    const auto get = parse_command("get a b c d e f g h i");
    CHECK(get.has_value());
    CHECK(get->verb == Verb::get);
    CHECK(get->key == "a");
    CHECK_EQ(get->extra_keys.size(), std::size_t(8));
    if (get->extra_keys.size() == 8) {
        CHECK(get->extra_keys.front() == "b");
        CHECK(get->extra_keys[5] == "g"); // beyond the parser's fixed storage-token array
        CHECK(get->extra_keys.back() == "i");
    }

    const auto gets = parse_command("gets first second third");
    CHECK(gets.has_value());
    CHECK(gets->verb == Verb::gets);
    CHECK(gets->key == "first");
    CHECK_EQ(gets->extra_keys.size(), std::size_t(2));
    if (gets->extra_keys.size() == 2) {
        CHECK(gets->extra_keys[0] == "second");
        CHECK(gets->extra_keys[1] == "third");
    }
}

TEST("memcache: parse set, with and without noreply") {
    const auto c = parse_command("set foo 7 0 5");
    CHECK(c.has_value());
    CHECK(c->verb == Verb::set);
    CHECK(c->is_storage());
    CHECK(c->key == "foo");
    CHECK_EQ(c->flags, std::uint32_t(7));
    CHECK_EQ(c->exptime, std::int64_t(0));
    CHECK_EQ(c->bytes, std::uint64_t(5));
    CHECK(!c->noreply);

    const auto c2 = parse_command("set bar 1 600 10 noreply");
    CHECK(c2.has_value());
    CHECK(c2->noreply);
    CHECK_EQ(c2->exptime, std::int64_t(600));
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

TEST("memcache: parse the complete classic mutation and retrieval surface") {
    const auto gat = parse_command("gat 60 first second third");
    CHECK(gat.has_value());
    if (gat) {
        CHECK(gat->verb == Verb::gat);
        CHECK_EQ(gat->exptime, std::int64_t(60));
        CHECK_EQ(gat->extra_keys.size(), std::size_t(2));
    }
    CHECK(parse_command("gats -1 key")->verb == Verb::gats);
    CHECK(parse_command("touch key -1 noreply")->verb == Verb::touch);
    CHECK(parse_command("append key 0 0 4")->verb == Verb::append);
    CHECK(parse_command("prepend key 0 0 4")->verb == Verb::prepend);
    const auto increment = parse_command("incr key 18446744073709551615 noreply");
    CHECK(increment.has_value());
    if (increment) CHECK_EQ(increment->delta, UINT64_MAX);
    CHECK(parse_command("decr key 1")->verb == Verb::decr);
    CHECK(parse_command("flush_all")->verb == Verb::flush_all);
    const auto delayed = parse_command("flush_all 30 noreply");
    CHECK(delayed.has_value());
    if (delayed) CHECK_EQ(delayed->delay, std::uint32_t(30));
}

TEST("memcache: parse meta commands (mn/mg/ms/md + flags)") {
    CHECK(parse_meta("mn")->verb == MetaVerb::mn);
    const auto g = parse_meta("mg foo v f s t c k T90 Oabc");
    CHECK(g.has_value());
    CHECK(g->verb == MetaVerb::mg && g->key == "foo");
    CHECK(g->rf_value && g->rf_flags && g->rf_size && g->rf_ttl && g->rf_cas && g->rf_key);
    CHECK(g->has_ttl && g->ttl == 90);
    CHECK(g->opaque == "abc");
    const auto s = parse_meta("ms foo 5 F7 T300 C42 ME q");
    CHECK(s.has_value());
    CHECK(s->verb == MetaVerb::ms && s->key == "foo" && s->datalen == std::uint64_t(5));
    CHECK(s->has_set_flags && s->set_flags == std::uint32_t(7));
    CHECK(s->has_ttl && s->ttl == 300);
    CHECK(s->has_cas && s->cas == std::uint64_t(42));
    CHECK(s->mode == 'E' && s->quiet);
    const auto d = parse_meta("md foo C42 q");
    CHECK(d.has_value() && d->verb == MetaVerb::md && d->has_cas && d->cas == std::uint64_t(42) && d->quiet);
    CHECK(!parse_meta("mg").has_value());       // no key
    CHECK(!parse_meta("ms foo").has_value());   // no datalen
    CHECK(!parse_meta("mg foo Z").has_value()); // unsupported flag
    CHECK(!parse_meta("mx foo").has_value());   // unknown verb
}

TEST("memcache: parse modern meta coordination, arithmetic, debug, and binary keys") {
    const auto get = parse_meta("mg dog v h l u N30 R10 C42 k Proute Lleader Oopaque");
    CHECK(get.has_value());
    if (get) {
        CHECK(get->rf_value && get->rf_hit && get->rf_last_access && get->no_update);
        CHECK(get->has_vivify && get->vivify_ttl == 30);
        CHECK(get->has_recache && get->recache_ttl == 10);
        CHECK(get->has_cas && get->cas == 42);
        CHECK(!get->key_binary);
        CHECK_EQ(get->lookup_key(), std::string_view("dog"));
    }
    const auto binary = parse_meta("mg YmluAGtleQ== b v k");
    CHECK(binary.has_value());
    if (binary) CHECK_EQ(binary->lookup_key(), std::string_view("bin\0key", 7));
    const auto arithmetic = parse_meta("ma counter MD N60 J5 D2 c k");
    CHECK(arithmetic.has_value());
    if (arithmetic) {
        CHECK(arithmetic->verb == MetaVerb::ma && arithmetic->mode == 'D');
        CHECK(arithmetic->has_vivify && arithmetic->has_initial);
        CHECK_EQ(arithmetic->initial, std::uint64_t(5));
        CHECK_EQ(arithmetic->delta, std::uint64_t(2));
    }
    CHECK(parse_meta("me counter")->verb == MetaVerb::me);
    CHECK(parse_meta("md stale I x T30")->invalidate);
    CHECK(!parse_meta("ma counter J5").has_value()); // J creation value requires N expiry
    CHECK(!parse_meta("mg !!! b").has_value());      // strict base64
}

TEST("memcache: parse delete / version / quit") {
    CHECK(parse_command("delete foo")->verb == Verb::del);
    CHECK(parse_command("delete foo noreply")->noreply);
    CHECK(parse_command("version")->verb == Verb::version);
    CHECK(parse_command("quit")->verb == Verb::quit);
}

TEST("memcache: stats reset and settings are explicit; unsupported variants are rejected") {
    const auto basic = parse_command("stats");
    CHECK(basic.has_value());
    CHECK(basic->stats_verb == StatsVerb::read);
    const auto reset = parse_command("stats reset");
    CHECK(reset.has_value());
    CHECK(reset->stats_verb == StatsVerb::reset);
    const auto settings = parse_command("stats settings");
    CHECK(settings.has_value());
    CHECK(settings->stats_verb == StatsVerb::settings);
    const auto items = parse_command("stats items");
    CHECK(!items.has_value());
    if (!items) CHECK_EQ(items.error().code, goblin::Errc::unsupported);
    CHECK(!parse_command("stats reset extra").has_value());
}

TEST("memcache: keys are bounded and exclude whitespace and control characters") {
    const std::string max_key(250, 'k');
    CHECK(parse_command("get " + max_key).has_value());
    CHECK(!parse_command("get " + std::string(251, 'k')).has_value());
    CHECK(!parse_command("get bad\tkey").has_value());
    CHECK(!parse_command("set bad\x7fkey 0 0 1").has_value());
    CHECK(!parse_meta("mg " + std::string(251, 'm')).has_value());
    CHECK(!parse_meta("mg bad\tkey").has_value());
}

TEST("memcache: malformed known commands are client errors; unknown commands are ERROR") {
    const auto malformed = parse_command("set key 0 nope 1");
    CHECK(!malformed.has_value());
    if (!malformed) {
        CHECK_EQ(malformed.error().code, goblin::Errc::invalid_protocol);
        CHECK(command_parse_error_reply(malformed.error()) == kClientErrorBadCommand);
    }
    const auto unknown = parse_command("frobnicate");
    CHECK(!unknown.has_value());
    if (!unknown) {
        CHECK_EQ(unknown.error().code, goblin::Errc::unsupported);
        CHECK(command_parse_error_reply(unknown.error()) == kError);
    }
    CHECK(!parse_command("delete key surprise").has_value());
    CHECK(!parse_command("version extra").has_value());
    CHECK(!parse_command("set key 0 0 1 noreply extra").has_value());
}

TEST("memcache: storage failures are server errors, not NOT_STORED") {
    CHECK(storage_failure_reply(goblin::Errc::too_large) == kServerErrorTooLarge);
    CHECK(storage_failure_reply(goblin::Errc::out_of_space) == kServerErrorOutOfSpace);
    CHECK(storage_failure_reply(goblin::Errc::out_of_memory) == kServerErrorOutOfMemory);
    CHECK(storage_failure_reply(goblin::Errc::io_error) == kServerErrorStorage);
    CHECK(commit_failure_reply(goblin::Errc::condition_not_met, false) == kNotStored);
    CHECK(commit_failure_reply(goblin::Errc::condition_not_met, true) == kMetaNotStored);
    CHECK(commit_failure_reply(goblin::Errc::not_found, false) == kServerErrorStorage);
    CHECK(commit_failure_reply(goblin::Errc::io_error, false) == kServerErrorStorage);
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
    CHECK_EQ(exptime_to_expiry(-1, now), std::uint32_t(1));            // negative = immediate expiry
    const auto negative = parse_command("set gone 0 -42 1");
    CHECK(negative.has_value());
    if (negative) CHECK_EQ(negative->exptime, std::int64_t(-42));
}
