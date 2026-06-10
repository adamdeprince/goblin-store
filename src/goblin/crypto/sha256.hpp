// The one hash Goblin uses (ADR-0014): SHA-256 of the client key -> 32-byte binary Digest, the
// internal object identity. Stored binary; hex only for on-disk filenames (ADR-0009).
//
// `sha256()` is backed by a runtime-dispatched implementation. Today that's a correct, portable
// scalar block function (NIST-verified); the hardware SHA-NI / AVX2 path (the actually-fast one,
// ~4-6x scalar) drops in behind this same function — see ADR-0014. We hash keys, never values,
// and exactly once per request (carry the Digest through the pipeline).
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
    std::string hex() const;               // 64 lowercase hex chars (the on-disk filename)
};

Digest sha256(const void* data, std::size_t len) noexcept;

// Hash a client key to its internal identity. Hash ONCE per request; carry the Digest.
inline Digest hash_key(std::string_view key) noexcept {
    return sha256(key.data(), key.size());
}

} // namespace goblin::crypto
