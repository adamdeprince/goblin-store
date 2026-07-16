// The one hash Goblin uses (ADR-0014): SHA-256 of the client key -> 32-byte binary Digest, the
// internal object identity. Stored binary; hex only for on-disk filenames (ADR-0009).
//
// `sha256()` is behind one function seam (ADR-0014). Runtime-selected policies:
//   x86 SHA-NI when CPUID reports it; scalar for short keys then OpenSSL for longer inputs on
//   pre-SHA-NI x86; optimized scalar on LoongArch; otherwise OpenSSL when linked or scalar.
// Known-answer and padding-boundary vectors gate the policy selected on each build host. We hash
// keys, never values, once per request.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace goblin::crypto {

struct Digest {
    std::array<std::byte, 32> bytes{};

    bool operator==(const Digest&) const noexcept = default;

    std::uint64_t bucket() const noexcept; // low 64 bits: index bucket / drive-stripe seed
    // 64 lowercase hex chars into `out` (no NUL). Returns 64. Used for on-disk names (no heap).
    static constexpr std::size_t kHexLen = 64;
    std::size_t write_hex(char* out /*[kHexLen]*/) const noexcept;
    std::string hex() const; // heap form; prefer write_hex on hot paths
};

Digest sha256(const void* data, std::size_t len) noexcept;

// Name of the resolved backend policy (for example "x86-sha-ni",
// "x86-scalar-short+openssl-long", "scalar-loongarch", "openssl", or "scalar").
const char* sha256_backend() noexcept;

// Hash a client key to its internal identity. Hash ONCE per request; carry the Digest.
inline Digest hash_key(std::string_view key) noexcept {
    return sha256(key.data(), key.size());
}

} // namespace goblin::crypto
