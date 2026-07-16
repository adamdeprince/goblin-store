#include "goblin/crypto/sha256.hpp"

#include <cstring>

// Multi-backend SHA-256 (ADR-0014). The hardware policy is selected once at first call:
//
//   1. x86 SHA-NI        — runtime CPUID; public-domain Intel/miTLS path
//                          (noloader/SHA-Intrinsics). Used when present even if
//                          OpenSSL is linked (avoids a library hop on the hot path).
//   2. pre-SHA-NI x86    — optimized scalar through 183 input bytes (at most three
//                          compression blocks including padding), then OpenSSL when linked.
//   3. Optimized scalar  — LoongArch primary path: 3A6000 has lsx/lasx/crc32
//                          but no SHA crypto ISA, so scalar is first-class there.
//   4. OpenSSL (optional)— fallback on other arches when TLS is linked.
//
// Short keys dominate hash_key(); the pre-SHA-NI x86 cutoff is benchmarked at the SHA padding
// boundary where the scalar implementation jumps from three compression blocks to four.

#if defined(GOBLIN_HAVE_TLS) && GOBLIN_HAVE_TLS
#include <openssl/sha.h>
#define GOBLIN_SHA256_OPENSSL 1
#endif

#if defined(__x86_64__) || defined(_M_X64)
#if defined(__GNUC__) || defined(__clang__)
#include <cpuid.h>
#endif
#include <immintrin.h>
#define GOBLIN_SHA256_X86 1
#endif

namespace goblin::crypto {
namespace {

// ---------------------------------------------------------------------------
// Scalar block — LoongArch primary path + portable fallback
// ---------------------------------------------------------------------------

alignas(64) constexpr std::uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline std::uint32_t rotr32(std::uint32_t x, unsigned n) noexcept {
    return (x >> n) | (x << (32u - n));
}

inline std::uint32_t load_be32(const unsigned char* p) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return __builtin_bswap32(v);
#else
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) |
           std::uint32_t(p[3]);
#endif
}

// Circular W[16] schedule + 8-wide unrolled rounds: keeps the working set in L1 on 3A6000
// and gives the compiler a straight-line body to schedule.
void sha256_block_scalar(std::uint32_t state[8], const unsigned char* p) noexcept {
    std::uint32_t w[16];
    for (int i = 0; i < 16; ++i) w[i] = load_be32(p + static_cast<std::size_t>(i) * 4);

    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

#define GOBLIN_RND(a, b, c, d, e, f, g, h, i)                                                      \
    do {                                                                                           \
        const std::uint32_t S1 = rotr32((e), 6) ^ rotr32((e), 11) ^ rotr32((e), 25);               \
        const std::uint32_t ch = ((e) & (f)) ^ (~(e) & (g));                                       \
        const std::uint32_t t1 = (h) + S1 + ch + K[(i)] + w[(i) & 15];                             \
        const std::uint32_t S0 = rotr32((a), 2) ^ rotr32((a), 13) ^ rotr32((a), 22);               \
        const std::uint32_t maj = ((a) & (b)) ^ ((a) & (c)) ^ ((b) & (c));                         \
        (d) += t1;                                                                                 \
        (h) = t1 + S0 + maj;                                                                       \
    } while (0)

#define GOBLIN_WSCHED(i)                                                                           \
    do {                                                                                           \
        const std::uint32_t wi15 = w[((i) + 1) & 15];                                              \
        const std::uint32_t wi2 = w[((i) + 14) & 15];                                              \
        const std::uint32_t s0 = rotr32(wi15, 7) ^ rotr32(wi15, 18) ^ (wi15 >> 3);                 \
        const std::uint32_t s1 = rotr32(wi2, 17) ^ rotr32(wi2, 19) ^ (wi2 >> 10);                  \
        w[(i) & 15] += s0 + w[((i) + 9) & 15] + s1;                                                \
    } while (0)

    GOBLIN_RND(a, b, c, d, e, f, g, h, 0);
    GOBLIN_RND(h, a, b, c, d, e, f, g, 1);
    GOBLIN_RND(g, h, a, b, c, d, e, f, 2);
    GOBLIN_RND(f, g, h, a, b, c, d, e, 3);
    GOBLIN_RND(e, f, g, h, a, b, c, d, 4);
    GOBLIN_RND(d, e, f, g, h, a, b, c, 5);
    GOBLIN_RND(c, d, e, f, g, h, a, b, 6);
    GOBLIN_RND(b, c, d, e, f, g, h, a, 7);
    GOBLIN_RND(a, b, c, d, e, f, g, h, 8);
    GOBLIN_RND(h, a, b, c, d, e, f, g, 9);
    GOBLIN_RND(g, h, a, b, c, d, e, f, 10);
    GOBLIN_RND(f, g, h, a, b, c, d, e, 11);
    GOBLIN_RND(e, f, g, h, a, b, c, d, 12);
    GOBLIN_RND(d, e, f, g, h, a, b, c, 13);
    GOBLIN_RND(c, d, e, f, g, h, a, b, 14);
    GOBLIN_RND(b, c, d, e, f, g, h, a, 15);

    for (int i = 16; i < 64; i += 8) {
        GOBLIN_WSCHED(i + 0); GOBLIN_RND(a, b, c, d, e, f, g, h, i + 0);
        GOBLIN_WSCHED(i + 1); GOBLIN_RND(h, a, b, c, d, e, f, g, i + 1);
        GOBLIN_WSCHED(i + 2); GOBLIN_RND(g, h, a, b, c, d, e, f, i + 2);
        GOBLIN_WSCHED(i + 3); GOBLIN_RND(f, g, h, a, b, c, d, e, i + 3);
        GOBLIN_WSCHED(i + 4); GOBLIN_RND(e, f, g, h, a, b, c, d, i + 4);
        GOBLIN_WSCHED(i + 5); GOBLIN_RND(d, e, f, g, h, a, b, c, i + 5);
        GOBLIN_WSCHED(i + 6); GOBLIN_RND(c, d, e, f, g, h, a, b, i + 6);
        GOBLIN_WSCHED(i + 7); GOBLIN_RND(b, c, d, e, f, g, h, a, i + 7);
    }

#undef GOBLIN_RND
#undef GOBLIN_WSCHED

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

// ---------------------------------------------------------------------------
// x86 SHA-NI (public domain: Jeffrey Walton / Intel / miTLS)
// ---------------------------------------------------------------------------

#if defined(GOBLIN_SHA256_X86)

bool cpu_has_sha_ni() noexcept {
#if defined(__GNUC__) || defined(__clang__)
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx))
        return (ebx & (1u << 29)) != 0; // SHA
#endif
    return false;
}

#include "sha256_shani.inc"

void sha256_block_shani(std::uint32_t state[8], const unsigned char* data) noexcept {
    sha256_block_shani_impl(state, data);
}

#endif // GOBLIN_SHA256_X86

// ---------------------------------------------------------------------------
// Digest assembly
// ---------------------------------------------------------------------------

using BlockFn = void (*)(std::uint32_t*, const unsigned char*) noexcept;

Digest sha256_with_block(const void* data, std::size_t len, BlockFn block) noexcept {
    std::uint32_t st[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                           0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto* p = static_cast<const unsigned char*>(data);
    const std::size_t full = len / 64;
    for (std::size_t i = 0; i < full; ++i) block(st, p + i * 64);

    unsigned char tail[128];
    const std::size_t rem = len % 64;
    if (rem) std::memcpy(tail, p + full * 64, rem);
    tail[rem] = 0x80;
    const std::size_t padlen = (rem < 56) ? 64 : 128;
    std::memset(tail + rem + 1, 0, padlen - rem - 1 - 8);
    const std::uint64_t bits = static_cast<std::uint64_t>(len) * 8;
    for (int i = 0; i < 8; ++i)
        tail[padlen - 1 - static_cast<std::size_t>(i)] = static_cast<unsigned char>(bits >> (i * 8));
    block(st, tail);
    if (padlen == 128) block(st, tail + 64);

    Digest d;
    for (int i = 0; i < 8; ++i) {
        d.bytes[static_cast<std::size_t>(i * 4 + 0)] = static_cast<std::byte>((st[i] >> 24) & 0xFFu);
        d.bytes[static_cast<std::size_t>(i * 4 + 1)] = static_cast<std::byte>((st[i] >> 16) & 0xFFu);
        d.bytes[static_cast<std::size_t>(i * 4 + 2)] = static_cast<std::byte>((st[i] >> 8) & 0xFFu);
        d.bytes[static_cast<std::size_t>(i * 4 + 3)] = static_cast<std::byte>(st[i] & 0xFFu);
    }
    return d;
}

// Always kept as a portable fallback (and the LoongArch primary path). On x86+SHA-NI it may
// be unreferenced; silence -Wunused-function under -Werror.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
Digest sha256_scalar(const void* data, std::size_t len) noexcept {
    return sha256_with_block(data, len, sha256_block_scalar);
}

#if defined(GOBLIN_SHA256_X86)
Digest sha256_shani(const void* data, std::size_t len) noexcept {
    return sha256_with_block(data, len, sha256_block_shani);
}
#endif

#if defined(GOBLIN_SHA256_OPENSSL)
Digest sha256_openssl(const void* data, std::size_t len) noexcept {
    Digest d;
    static_assert(sizeof(d.bytes) == SHA256_DIGEST_LENGTH);
    ::SHA256(static_cast<const unsigned char*>(data), len,
             reinterpret_cast<unsigned char*>(d.bytes.data()));
    return d;
}
#endif

#if defined(GOBLIN_SHA256_X86) && defined(GOBLIN_SHA256_OPENSSL)
// A 183-byte input is 2 full blocks plus one padded block. At 184 bytes the 56-byte remainder
// needs two padding blocks. On the pre-SHA-NI Xeon E5-4657L v2, scalar wins decisively through
// 183 bytes and OpenSSL wins from the fourth compression block onward.
constexpr std::size_t kX86ScalarShortMaxBlocks = 3;
constexpr std::size_t kX86ScalarShortMaxBytes = kX86ScalarShortMaxBlocks * 64 - 1 - 8;

Digest sha256_x86_scalar_short_openssl_long(const void* data, std::size_t len) noexcept {
    if (len <= kX86ScalarShortMaxBytes) return sha256_scalar(data, len);
    return sha256_openssl(data, len);
}
#endif

using HashFn = Digest (*)(const void*, std::size_t) noexcept;

// Written once by the process-lifetime resolver; exposed through sha256_backend() for diagnostics.
const char* g_backend_name = "unresolved";

HashFn resolve_impl() noexcept {
#if defined(GOBLIN_SHA256_X86)
    if (cpu_has_sha_ni()) {
        g_backend_name = "x86-sha-ni";
        return sha256_shani;
    }
#if defined(GOBLIN_SHA256_OPENSSL)
    g_backend_name = "x86-scalar-short+openssl-long";
    return sha256_x86_scalar_short_openssl_long;
#endif
#endif
#if defined(__loongarch__) || defined(__loongarch64)
    // No SHA crypto ISA on Loongson 3A6000 (features: lsx lasx crc32 …). Prefer our unrolled
    // scalar over OpenSSL 1.1 generic C.
    g_backend_name = "scalar-loongarch";
    return sha256_scalar;
#endif
    // Other arches prefer OpenSSL when linked; x86 without SHA-NI returned the hybrid policy above.
#if defined(GOBLIN_SHA256_OPENSSL)
    g_backend_name = "openssl";
    return sha256_openssl;
#else
    g_backend_name = "scalar";
    return sha256_scalar;
#endif
}

HashFn impl() noexcept {
    static const HashFn fn = resolve_impl();
    return fn;
}

} // namespace

Digest sha256(const void* data, std::size_t len) noexcept { return impl()(data, len); }

std::uint64_t Digest::bucket() const noexcept {
    std::uint64_t v = 0;
    std::memcpy(&v, bytes.data(), sizeof(v));
    return v;
}

std::size_t Digest::write_hex(char* out) const noexcept {
    static constexpr char digits[] = "0123456789abcdef";
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto b = std::to_integer<unsigned>(bytes[i]);
        out[i * 2 + 0] = digits[(b >> 4) & 0xF];
        out[i * 2 + 1] = digits[b & 0xF];
    }
    return kHexLen;
}

std::string Digest::hex() const {
    std::string s(kHexLen, '0');
    write_hex(s.data());
    return s;
}

const char* sha256_backend() noexcept {
    (void)impl(); // ensure resolved
    return g_backend_name;
}

} // namespace goblin::crypto
