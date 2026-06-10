#include "mini_test.hpp"

#include "goblin/crypto/sha256.hpp"

#include <string>
#include <string_view>

using namespace goblin::crypto;

static std::string h(std::string_view s) { return sha256(s.data(), s.size()).hex(); }

TEST("sha256: NIST test vectors (empty, one-block, two-block)") {
    CHECK(h("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(h("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(h("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST("hash_key: deterministic, distinct keys differ, 64-hex filename") {
    const auto a = hash_key("/foo/bar.tar.gz");
    const auto b = hash_key("/foo/bar.tar.gz");
    const auto c = hash_key("/foo/baz.tar.gz");
    CHECK(a == b);
    CHECK(!(a == c));
    CHECK(a.hex().size() == 64);
    CHECK(a.bucket() == b.bucket()); // same key -> same bucket
}
