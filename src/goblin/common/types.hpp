// Fundamental types and bit/size helpers shared across Goblin Store.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace goblin {

using ByteView = std::span<const std::byte>; // read-only view over bytes
using MutBytes = std::span<std::byte>;       // writable view
using Offset   = std::uint64_t;              // byte offset within an object
using Size     = std::uint64_t;              // byte length
using Key      = std::string;                // object key (memcache key / HTTP path)
using KeyView  = std::string_view;

inline constexpr Size KiB = 1024;
inline constexpr Size MiB = 1024 * KiB;
inline constexpr Size GiB = 1024 * MiB;

// Largest object we serve. Size and Offset are 64-bit; this guard bounds admission and catches
// unreasonable protocol lengths before any filesystem work begins.
inline constexpr Size kMaxObjectSize = 4 * GiB;
// Device-block / O_DIRECT alignment (ADR-0011) and arena alignment floor (ADR-0008).
inline constexpr Size kDeviceBlock = 4 * KiB;

// An unresolved RFC 7233 single byte-range (resolved against the object size at serve time). Lives
// here, not in http/, so the shared streaming loop can carry one without depending on the protocol.
struct ByteRange {
    enum class Kind { closed, from, suffix };
    Kind kind = Kind::closed;
    Offset a = 0; // closed/from: first byte; suffix: number of trailing bytes
    Offset b = 0; // closed: last byte, inclusive; unused for from/suffix
};

constexpr bool is_power_of_two(Size v) noexcept {
    return v != 0 && (v & (v - 1)) == 0;
}
// `a` must be a power of two.
constexpr Size align_up(Size v, Size a) noexcept { return (v + a - 1) & ~(a - 1); }
constexpr Size align_down(Size v, Size a) noexcept { return v & ~(a - 1); }
constexpr bool is_aligned(Size v, Size a) noexcept { return (v & (a - 1)) == 0; }

} // namespace goblin
