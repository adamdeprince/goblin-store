#include "mini_test.hpp"

#include "goblin/http/key_derivation.hpp"

using namespace goblin::http;

static KeyOptions path_mode(bool q = false) { return KeyOptions{KeyMode::path, q}; }
static KeyOptions vhost_mode(bool q = false) { return KeyOptions{KeyMode::vhost, q}; }
static KeyOptions path_strip(bool q = false) { return KeyOptions{KeyMode::path, q, true}; }

TEST("derive_key: path mode = canonical URI path (leading slash)") {
    const auto k = derive_key("ignored", "/foo/bar.tar.gz", path_mode());
    CHECK(k.has_value());
    CHECK(*k == "/foo/bar.tar.gz");
}

TEST("derive_key: vhost mode = host + path, no leading slash") {
    const auto k = derive_key("www.example.com", "/foo/bar.tar.gz", vhost_mode());
    CHECK(k.has_value());
    CHECK(*k == "www.example.com/foo/bar.tar.gz");
}

TEST("derive_key: vhost lowercases host and strips port") {
    const auto k = derive_key("WWW.Example.COM:8080", "/x", vhost_mode());
    CHECK(*k == "www.example.com/x");
}

TEST("derive_key: vhost requires a Host") {
    CHECK(!derive_key("", "/x", vhost_mode()).has_value());
}

TEST("derive_key: query stripped by default, kept with the flag") {
    CHECK(*derive_key("h", "/a.js?v=2", path_mode(false)) == "/a.js");
    CHECK(*derive_key("h", "/a.js?v=2", path_mode(true)) == "/a.js?v=2");
}

TEST("derive_key: percent-decode + dot-segment canonicalization, no root escape") {
    CHECK(*derive_key("h", "/foo/bar%20baz.txt", path_mode()) == "/foo/bar baz.txt");
    CHECK(*derive_key("h", "/a/b/../c", path_mode()) == "/a/c");
    CHECK(*derive_key("h", "//a///b", path_mode()) == "/a/b");
    CHECK(*derive_key("h", "/../../etc/passwd", path_mode()) == "/etc/passwd");
}

TEST("derive_key_from_relpath: relpath is NOT percent-decoded (filesystem names are literal)") {
    CHECK(derive_key_from_relpath("a%41b.txt", path_mode()) == "/a%41b.txt");
}

TEST("duality: an HTTP request and its --source file resolve to the same key") {
    // path mode
    const auto http_p = derive_key("h", "/bar/baz.txt", path_mode());
    CHECK(http_p.has_value());
    CHECK(*http_p == derive_key_from_relpath("bar/baz.txt", path_mode()));

    // vhost mode
    const auto http_v = derive_key("www.example.com", "/bar/baz.txt", vhost_mode());
    CHECK(http_v.has_value());
    CHECK(*http_v == derive_key_from_relpath("www.example.com/bar/baz.txt", vhost_mode()));
}

TEST("derive_key: --key-strip-slash drops the leading '/' in path mode (memcache key == GET path)") {
    CHECK(*derive_key("h", "/foo/bar.txt", path_strip()) == "foo/bar.txt");
    CHECK(*derive_key("h", "/", path_strip()) == "");                     // root collapses to the empty key
    CHECK(*derive_key("h", "/a.js?v=2", path_strip(true)) == "a.js?v=2"); // query kept, leading slash gone
    CHECK(*derive_key("h", "//a///b", path_strip()) == "a/b");           // canonicalization still applies
}

TEST("derive_key: the strip flag does not touch vhost mode (the '/' separates host from path)") {
    const KeyOptions v{KeyMode::vhost, false, /*strip_leading_slash=*/true};
    CHECK(*derive_key("www.example.com", "/foo/bar.txt", v) == "www.example.com/foo/bar.txt");
}

TEST("duality holds under --key-strip-slash: HTTP request == --source file") {
    const auto http_p = derive_key("h", "/bar/baz.txt", path_strip());
    CHECK(http_p.has_value());
    CHECK(*http_p == derive_key_from_relpath("bar/baz.txt", path_strip()));
    CHECK(derive_key_from_relpath("bar/baz.txt", path_strip()) == "bar/baz.txt");
}
