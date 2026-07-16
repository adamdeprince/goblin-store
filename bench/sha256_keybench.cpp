// Standalone SHA-256 key microbench (C++17 — runs on Loongson GCC 8 and modern x86).
// Copies the production block transforms so we can validate without a full goblin C++23 build.
//
//   # LoongArch (scalar is production path):
//   g++ -O3 -std=c++17 bench/sha256_keybench.cpp -o sha256_keybench
//
//   # x86 SHA-NI:
//   g++ -O3 -std=c++17 -DKEYBENCH_X86 bench/sha256_keybench.cpp -o sha256_keybench
//
//   # OpenSSL reference (any arch):
//   g++ -O3 -std=c++17 -DKEYBENCH_OPENSSL bench/sha256_keybench.cpp -lcrypto -o sha256_keybench

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(KEYBENCH_OPENSSL)
#include <openssl/sha.h>
#endif


using clock_type = std::chrono::steady_clock;

// ---- scalar (matches goblin::crypto production LoongArch path) ----
namespace scalar {
alignas(64) constexpr std::uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline std::uint32_t rotr32(std::uint32_t x, unsigned n) { return (x >> n) | (x << (32u - n)); }
inline std::uint32_t load_be32(const unsigned char* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}

void block(std::uint32_t state[8], const unsigned char* p) {
    std::uint32_t w[16];
    for (int i = 0; i < 16; ++i) w[i] = load_be32(p + i * 4);
    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
#define RND(a, b, c, d, e, f, g, h, i)                                                             \
    do {                                                                                           \
        const std::uint32_t S1 = rotr32((e), 6) ^ rotr32((e), 11) ^ rotr32((e), 25);               \
        const std::uint32_t ch = ((e) & (f)) ^ (~(e) & (g));                                       \
        const std::uint32_t t1 = (h) + S1 + ch + K[(i)] + w[(i) & 15];                             \
        const std::uint32_t S0 = rotr32((a), 2) ^ rotr32((a), 13) ^ rotr32((a), 22);               \
        const std::uint32_t maj = ((a) & (b)) ^ ((a) & (c)) ^ ((b) & (c));                         \
        (d) += t1;                                                                                 \
        (h) = t1 + S0 + maj;                                                                       \
    } while (0)
#define WSCHED(i)                                                                                  \
    do {                                                                                           \
        const std::uint32_t wi15 = w[((i) + 1) & 15];                                              \
        const std::uint32_t wi2 = w[((i) + 14) & 15];                                              \
        const std::uint32_t s0 = rotr32(wi15, 7) ^ rotr32(wi15, 18) ^ (wi15 >> 3);                 \
        const std::uint32_t s1 = rotr32(wi2, 17) ^ rotr32(wi2, 19) ^ (wi2 >> 10);                  \
        w[(i) & 15] += s0 + w[((i) + 9) & 15] + s1;                                                \
    } while (0)
    RND(a, b, c, d, e, f, g, h, 0); RND(h, a, b, c, d, e, f, g, 1);
    RND(g, h, a, b, c, d, e, f, 2); RND(f, g, h, a, b, c, d, e, 3);
    RND(e, f, g, h, a, b, c, d, 4); RND(d, e, f, g, h, a, b, c, 5);
    RND(c, d, e, f, g, h, a, b, 6); RND(b, c, d, e, f, g, h, a, 7);
    RND(a, b, c, d, e, f, g, h, 8); RND(h, a, b, c, d, e, f, g, 9);
    RND(g, h, a, b, c, d, e, f, 10); RND(f, g, h, a, b, c, d, e, 11);
    RND(e, f, g, h, a, b, c, d, 12); RND(d, e, f, g, h, a, b, c, 13);
    RND(c, d, e, f, g, h, a, b, 14); RND(b, c, d, e, f, g, h, a, 15);
    for (int i = 16; i < 64; i += 8) {
        WSCHED(i + 0); RND(a, b, c, d, e, f, g, h, i + 0);
        WSCHED(i + 1); RND(h, a, b, c, d, e, f, g, i + 1);
        WSCHED(i + 2); RND(g, h, a, b, c, d, e, f, i + 2);
        WSCHED(i + 3); RND(f, g, h, a, b, c, d, e, i + 3);
        WSCHED(i + 4); RND(e, f, g, h, a, b, c, d, i + 4);
        WSCHED(i + 5); RND(d, e, f, g, h, a, b, c, i + 5);
        WSCHED(i + 6); RND(c, d, e, f, g, h, a, b, i + 6);
        WSCHED(i + 7); RND(b, c, d, e, f, g, h, a, i + 7);
    }
#undef RND
#undef WSCHED
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}
} // namespace scalar

using BlockFn = void (*)(std::uint32_t*, const unsigned char*);

static void digest_hex(const void* data, std::size_t len, BlockFn block, char out[65]) {
    std::uint32_t st[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                           0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < len / 64; ++i) block(st, p + i * 64);
    unsigned char tail[128];
    const std::size_t rem = len % 64;
    if (rem) std::memcpy(tail, p + len - rem, rem);
    tail[rem] = 0x80;
    const std::size_t padlen = (rem < 56) ? 64 : 128;
    std::memset(tail + rem + 1, 0, padlen - rem - 1 - 8);
    const std::uint64_t bits = static_cast<std::uint64_t>(len) * 8;
    for (int i = 0; i < 8; ++i)
        tail[padlen - 1 - static_cast<std::size_t>(i)] = static_cast<unsigned char>(bits >> (i * 8));
    block(st, tail);
    if (padlen == 128) block(st, tail + 64);
    static const char* hexd = "0123456789abcdef";
    for (int i = 0; i < 8; ++i) {
        for (int b = 0; b < 4; ++b) {
            const unsigned v = (st[i] >> (24 - 8 * b)) & 0xFF;
            out[i * 8 + b * 2 + 0] = hexd[v >> 4];
            out[i * 8 + b * 2 + 1] = hexd[v & 0xF];
        }
    }
    out[64] = 0;
}

static std::uint64_t hash_bucket(const void* data, std::size_t len, BlockFn block) {
    std::uint32_t st[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                           0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < len / 64; ++i) block(st, p + i * 64);
    unsigned char tail[128];
    const std::size_t rem = len % 64;
    if (rem) std::memcpy(tail, p + len - rem, rem);
    tail[rem] = 0x80;
    const std::size_t padlen = (rem < 56) ? 64 : 128;
    std::memset(tail + rem + 1, 0, padlen - rem - 1 - 8);
    const std::uint64_t bits = static_cast<std::uint64_t>(len) * 8;
    for (int i = 0; i < 8; ++i)
        tail[padlen - 1 - static_cast<std::size_t>(i)] = static_cast<unsigned char>(bits >> (i * 8));
    block(st, tail);
    if (padlen == 128) block(st, tail + 64);
    // low 64 bits of digest = first 8 BE bytes of H0||H1...
    return (std::uint64_t(st[0]) << 32) | st[1]; // not exact LE bucket but fine as a sink
}


#if defined(KEYBENCH_X86)
#include <cpuid.h>
#include <immintrin.h>
// Reuse production SHANI block (path relative to this file when compiled from repo root)
#include "../src/goblin/crypto/sha256_shani.inc"
static void block_shani(std::uint32_t state[8], const unsigned char* data) {
    sha256_block_shani_impl(state, data);
}
static bool has_sha_ni() {
    unsigned a, b, c, d;
    return __get_cpuid_count(7, 0, &a, &b, &c, &d) && (b & (1u << 29));
}
#endif



#if defined(KEYBENCH_OPENSSL)
static void block_openssl_whole(const void* data, std::size_t len, unsigned char out[32]) {
    SHA256(static_cast<const unsigned char*>(data), len, out);
}
#endif

static double ns_per(std::size_t key_len, int iters, BlockFn block) {
    std::string key(key_len, 'k');
    volatile std::uint64_t sink = 0;
    for (int i = 0; i < 1000; ++i) sink ^= hash_bucket(key.data(), key.size(), block);
    const auto t0 = clock_type::now();
    for (int i = 0; i < iters; ++i) {
        key[static_cast<std::size_t>(i) % key_len] = static_cast<char>('a' + (i % 26));
        sink ^= hash_bucket(key.data(), key.size(), block);
    }
    const auto t1 = clock_type::now();
    (void)sink;
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / iters;
}

int main() {
    char hex[65];
    digest_hex("", 0, scalar::block, hex);
    if (std::strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") != 0) {
        std::fprintf(stderr, "scalar NIST empty fail: %s\n", hex);
        return 1;
    }
    digest_hex("abc", 3, scalar::block, hex);
    if (std::strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") != 0) {
        std::fprintf(stderr, "scalar NIST abc fail: %s\n", hex);
        return 1;
    }

    BlockFn block = scalar::block;
    const char* name = "scalar-unrolled";

#if defined(KEYBENCH_X86)
    if (has_sha_ni()) {
        digest_hex("", 0, block_shani, hex);
        if (std::strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") != 0) {
            std::fprintf(stderr, "sha-ni NIST empty fail: %s\n", hex);
            return 1;
        }
        digest_hex("abc", 3, block_shani, hex);
        if (std::strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") != 0) {
            std::fprintf(stderr, "sha-ni NIST abc fail: %s\n", hex);
            return 1;
        }
        block = block_shani;
        name = "x86-sha-ni";
    } else {
        name = "scalar-unrolled (no sha_ni)";
    }
#endif

#if defined(KEYBENCH_OPENSSL)
    {
        unsigned char d[32];
        block_openssl_whole("abc", 3, d);
        // compare to scalar
        char ohex[65];
        static const char* hexd = "0123456789abcdef";
        for (int i = 0; i < 32; ++i) {
            ohex[i * 2] = hexd[d[i] >> 4];
            ohex[i * 2 + 1] = hexd[d[i] & 0xF];
        }
        ohex[64] = 0;
        if (std::strcmp(ohex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") != 0) {
            std::fprintf(stderr, "openssl NIST fail\n");
            return 1;
        }
        name = "openssl (timing separate)";
    }
#endif

    std::printf("backend: %s\n", name);
    std::printf("%6s  %12s\n", "key_B", "ns/hash");
    const int iters = 300000;
    for (std::size_t n : {8u, 16u, 32u, 64u, 128u, 160u, 183u, 184u, 192u,
                          224u, 240u, 247u, 248u, 250u, 256u}) {
        std::printf("%6zu  %12.1f\n", n, ns_per(n, iters, block));
    }

#if defined(KEYBENCH_OPENSSL)
    // Time OpenSSL one-shot for comparison on the same sizes.
    std::printf("openssl one-shot:\n");
    for (std::size_t n : {8u, 16u, 32u, 64u, 128u, 160u, 183u, 184u, 192u,
                          224u, 240u, 247u, 248u, 250u, 256u}) {
        std::string key(n, 'k');
        volatile std::uint64_t sink = 0;
        unsigned char d[32];
        for (int i = 0; i < 1000; ++i) {
            SHA256(reinterpret_cast<const unsigned char*>(key.data()), key.size(), d);
            sink ^= d[0];
        }
        const int it = 300000;
        const auto t0 = clock_type::now();
        for (int i = 0; i < it; ++i) {
            key[static_cast<std::size_t>(i) % n] = static_cast<char>('a' + (i % 26));
            SHA256(reinterpret_cast<const unsigned char*>(key.data()), key.size(), d);
            sink ^= d[0];
        }
        const auto t1 = clock_type::now();
        (void)sink;
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / it;
        std::printf("%6zu  %12.1f\n", n, ns);
    }
#endif
    return 0;
}
