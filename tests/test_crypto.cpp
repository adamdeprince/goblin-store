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

TEST("sha256: backend is resolved and matches NIST on short keys") {
    // Touch the resolver so sha256_backend() is meaningful; still must match scalar NIST.
    CHECK(sha256_backend() != nullptr);
    CHECK(sha256_backend()[0] != '\0');
    // Typical memcache key sizes: empty and one-block standard vectors.
    CHECK(h("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(h("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST("sha256: scalar-short policy boundary matches independent vectors") {
    // 183 bytes needs three compression blocks including padding; 184 needs four. On pre-SHA-NI
    // x86 with TLS these exercise opposite sides of the scalar/OpenSSL policy boundary.
    CHECK(h(std::string(183, 'x')) ==
          "b95933e340383f43cfb72bb337fbb80bd93b9f54f5b49f5cd9635d2c62e7f386");
    CHECK(h(std::string(184, 'x')) ==
          "f3936e2eb513e068318995db8ff8a043fe9cbcf079934b8dfd49046c0fca45be");
    CHECK(h(std::string(250, 'x')) ==
          "086d4a1c293bde318dc1fec9a21b9d828ba7637bcbdc5cdb42662fd84b733e9f");
}
