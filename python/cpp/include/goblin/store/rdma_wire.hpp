#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>

namespace goblin::store::rdma_wire {

// Version 3 retains Packrat's version-2 control-ring prefix and adds a
// separately registered, fixed-slot bulk window. Descriptor integers use
// network byte order; established-connection frames use little endian.
inline constexpr std::uint32_t kDescriptorMagic = 0x47425244U; // "GBRD"
inline constexpr std::uint16_t kDescriptorVersion = 3;
inline constexpr std::size_t kDescriptorBytes = 56;
inline constexpr std::size_t kControlSlotStride = 256;
inline constexpr std::size_t kControlCommitOffset = 248;
inline constexpr std::size_t kControlRecordBytes = 192;
inline constexpr std::size_t kInlineBytes = kControlRecordBytes - 1;
inline constexpr std::size_t kControlRegionHeaderBytes = 128;
inline constexpr std::size_t kConsumedOffset = 0;
inline constexpr std::size_t kCreditReadbackOffset = 64;
inline constexpr std::size_t kBulkReadyBytes = 16;
inline constexpr std::size_t kBulkReleaseBytes = 16;

enum class FrameType : std::uint8_t {
    inline_bytes = 1,
    bulk_ready = 2,
    bulk_release = 3,
};

// CM private-data layout (all network byte order):
//   0 magic:u32, 4 version:u16, 6 control stride:u16,
//   8 control slots:u32, 12 control record bytes:u16,
//   14 log2(bulk slot bytes):u16,
//   16 ring address:u64, 24 ring rkey:u32, 28 ring bytes:u32,
//   32 nonce:u64, 40 bulk address:u64, 48 bulk rkey:u32,
//   52 total bulk bytes:u32.
//
// The bulk region is [TX slots][RX slots], with bulk_window_count slots in
// each half. A peer writes from its TX slot to the other side's RX slot.
struct Descriptor {
    std::uint64_t ring_address = 0;
    std::uint64_t nonce = 0;
    std::uint64_t bulk_address = 0;
    std::uint32_t ring_rkey = 0;
    std::uint32_t ring_region_bytes = 0;
    std::uint32_t ring_slot_count = 0;
    std::uint32_t bulk_rkey = 0;
    std::uint32_t bulk_region_bytes = 0;
    std::uint32_t bulk_window_bytes = 0;
    std::uint32_t bulk_window_count = 0; // derived while decoding
};

struct FrameView {
    FrameType type{};
    std::string_view inline_payload{};
    std::uint32_t length = 0;
    std::uint64_t sequence = 0;
};

namespace detail {

constexpr std::uint64_t byteswap64(std::uint64_t value) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap64(value);
#else
    return ((value & 0x00000000000000ffULL) << 56) |
           ((value & 0x000000000000ff00ULL) << 40) |
           ((value & 0x0000000000ff0000ULL) << 24) |
           ((value & 0x00000000ff000000ULL) << 8) |
           ((value & 0x000000ff00000000ULL) >> 8) |
           ((value & 0x0000ff0000000000ULL) >> 24) |
           ((value & 0x00ff000000000000ULL) >> 40) |
           ((value & 0xff00000000000000ULL) >> 56);
#endif
}

constexpr std::uint64_t to_be64(std::uint64_t value) noexcept {
    if constexpr (std::endian::native == std::endian::little) return byteswap64(value);
    return value;
}

constexpr std::uint64_t from_be64(std::uint64_t value) noexcept { return to_be64(value); }

constexpr std::uint64_t to_le64(std::uint64_t value) noexcept {
    if constexpr (std::endian::native == std::endian::big) return byteswap64(value);
    return value;
}

constexpr std::uint64_t from_le64(std::uint64_t value) noexcept { return to_le64(value); }

inline void put_be16(std::byte* out, std::uint16_t value) noexcept {
    out[0] = static_cast<std::byte>(value >> 8);
    out[1] = static_cast<std::byte>(value);
}

inline void put_be32(std::byte* out, std::uint32_t value) noexcept {
    out[0] = static_cast<std::byte>(value >> 24);
    out[1] = static_cast<std::byte>(value >> 16);
    out[2] = static_cast<std::byte>(value >> 8);
    out[3] = static_cast<std::byte>(value);
}

inline void put_be64(std::byte* out, std::uint64_t value) noexcept {
    value = to_be64(value);
    std::memcpy(out, &value, sizeof(value));
}

inline std::uint16_t get_be16(const std::byte* in) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(in[0]) << 8) |
        static_cast<std::uint16_t>(in[1]));
}

inline std::uint32_t get_be32(const std::byte* in) noexcept {
    return (static_cast<std::uint32_t>(in[0]) << 24) |
           (static_cast<std::uint32_t>(in[1]) << 16) |
           (static_cast<std::uint32_t>(in[2]) << 8) |
           static_cast<std::uint32_t>(in[3]);
}

inline std::uint64_t get_be64(const std::byte* in) noexcept {
    std::uint64_t value = 0;
    std::memcpy(&value, in, sizeof(value));
    return from_be64(value);
}

inline void put_le32(std::byte* out, std::uint32_t value) noexcept {
    out[0] = static_cast<std::byte>(value);
    out[1] = static_cast<std::byte>(value >> 8);
    out[2] = static_cast<std::byte>(value >> 16);
    out[3] = static_cast<std::byte>(value >> 24);
}

inline void put_le64(std::byte* out, std::uint64_t value) noexcept {
    value = to_le64(value);
    std::memcpy(out, &value, sizeof(value));
}

inline std::uint32_t get_le32(const std::byte* in) noexcept {
    return static_cast<std::uint32_t>(in[0]) |
           (static_cast<std::uint32_t>(in[1]) << 8) |
           (static_cast<std::uint32_t>(in[2]) << 16) |
           (static_cast<std::uint32_t>(in[3]) << 24);
}

inline std::uint64_t get_le64(const std::byte* in) noexcept {
    std::uint64_t value = 0;
    std::memcpy(&value, in, sizeof(value));
    return from_le64(value);
}

inline bool all_zero(std::span<const std::byte> bytes) noexcept {
    for (const auto byte : bytes) {
        if (byte != std::byte{0}) return false;
    }
    return true;
}

} // namespace detail

inline bool valid_bulk_geometry(std::uint32_t window_bytes,
                                std::uint32_t window_count) noexcept {
    // Page-sized slots make every slot suitable for direct I/O and preserve
    // page alignment throughout both halves of the mapping.
    if (window_bytes < 4096 || !std::has_single_bit(window_bytes) ||
        window_count == 0 || window_count > std::numeric_limits<std::uint16_t>::max())
        return false;
    const std::uint64_t total = std::uint64_t{2} * window_bytes * window_count;
    return total <= std::numeric_limits<std::uint32_t>::max();
}

inline std::uint64_t control_commit_to_wire(std::uint64_t sequence,
                                            std::size_t record_bytes) noexcept {
    return detail::to_le64((sequence << 8) | static_cast<std::uint64_t>(record_bytes));
}

inline std::uint64_t control_commit_from_wire(std::uint64_t word) noexcept {
    return detail::from_le64(word);
}

inline std::uint64_t sequence_to_wire(std::uint64_t sequence) noexcept {
    return detail::to_le64(sequence);
}

inline std::uint64_t sequence_from_wire(std::uint64_t sequence) noexcept {
    return detail::from_le64(sequence);
}

inline std::uint16_t bulk_window_shift(std::uint32_t window_bytes) noexcept {
    return valid_bulk_geometry(window_bytes, 1)
               ? static_cast<std::uint16_t>(std::countr_zero(window_bytes))
               : 0;
}

inline std::array<std::byte, kDescriptorBytes> encode_descriptor(
    const Descriptor& descriptor) noexcept {
    std::array<std::byte, kDescriptorBytes> bytes{};
    detail::put_be32(bytes.data() + 0, kDescriptorMagic);
    detail::put_be16(bytes.data() + 4, kDescriptorVersion);
    detail::put_be16(bytes.data() + 6, static_cast<std::uint16_t>(kControlSlotStride));
    detail::put_be32(bytes.data() + 8, descriptor.ring_slot_count);
    detail::put_be16(bytes.data() + 12, static_cast<std::uint16_t>(kControlRecordBytes));
    detail::put_be16(bytes.data() + 14, bulk_window_shift(descriptor.bulk_window_bytes));
    detail::put_be64(bytes.data() + 16, descriptor.ring_address);
    detail::put_be32(bytes.data() + 24, descriptor.ring_rkey);
    detail::put_be32(bytes.data() + 28, descriptor.ring_region_bytes);
    detail::put_be64(bytes.data() + 32, descriptor.nonce);
    detail::put_be64(bytes.data() + 40, descriptor.bulk_address);
    detail::put_be32(bytes.data() + 48, descriptor.bulk_rkey);
    detail::put_be32(bytes.data() + 52, descriptor.bulk_region_bytes);
    return bytes;
}

inline bool decode_descriptor(std::span<const std::byte> bytes,
                              Descriptor& descriptor) noexcept {
    if (bytes.size() < kDescriptorBytes ||
        detail::get_be32(bytes.data() + 0) != kDescriptorMagic ||
        detail::get_be16(bytes.data() + 4) != kDescriptorVersion ||
        detail::get_be16(bytes.data() + 6) != kControlSlotStride ||
        detail::get_be16(bytes.data() + 12) != kControlRecordBytes)
        return false;

    const std::uint16_t shift = detail::get_be16(bytes.data() + 14);
    if (shift >= 32) return false;
    const std::uint32_t window_bytes = std::uint32_t{1} << shift;

    descriptor.ring_slot_count = detail::get_be32(bytes.data() + 8);
    descriptor.ring_address = detail::get_be64(bytes.data() + 16);
    descriptor.ring_rkey = detail::get_be32(bytes.data() + 24);
    descriptor.ring_region_bytes = detail::get_be32(bytes.data() + 28);
    descriptor.nonce = detail::get_be64(bytes.data() + 32);
    descriptor.bulk_address = detail::get_be64(bytes.data() + 40);
    descriptor.bulk_rkey = detail::get_be32(bytes.data() + 48);
    descriptor.bulk_region_bytes = detail::get_be32(bytes.data() + 52);
    descriptor.bulk_window_bytes = window_bytes;

    const std::uint64_t required_ring = kControlRegionHeaderBytes +
        static_cast<std::uint64_t>(descriptor.ring_slot_count) * kControlSlotStride;
    const std::uint64_t bytes_per_pair = std::uint64_t{2} * window_bytes;
    if (descriptor.ring_slot_count < 2 || descriptor.ring_address == 0 ||
        required_ring > descriptor.ring_region_bytes || descriptor.bulk_address == 0 ||
        bytes_per_pair == 0 || descriptor.bulk_region_bytes % bytes_per_pair != 0)
        return false;
    if (descriptor.ring_address >
            std::numeric_limits<std::uint64_t>::max() - descriptor.ring_region_bytes ||
        descriptor.bulk_address >
            std::numeric_limits<std::uint64_t>::max() - descriptor.bulk_region_bytes)
        return false;

    const std::uint64_t count = descriptor.bulk_region_bytes / bytes_per_pair;
    if (count == 0 || count > std::numeric_limits<std::uint16_t>::max()) return false;
    descriptor.bulk_window_count = static_cast<std::uint32_t>(count);
    return valid_bulk_geometry(descriptor.bulk_window_bytes,
                               descriptor.bulk_window_count);
}

inline std::size_t encode_inline(std::string_view payload,
                                 std::array<std::byte, kControlRecordBytes>& out) noexcept {
    if (payload.empty() || payload.size() > kInlineBytes) return 0;
    out[0] = static_cast<std::byte>(FrameType::inline_bytes);
    std::memcpy(out.data() + 1, payload.data(), payload.size());
    return payload.size() + 1;
}

// READY is {type:u8, flags:u8=0, reserved:u16=0, length:le32,
// sequence:le64}. It is posted after the corresponding bulk write on the same
// RC QP and therefore acts as that write's publication/doorbell.
inline std::array<std::byte, kBulkReadyBytes> encode_bulk_ready(
    std::uint32_t length, std::uint64_t sequence) noexcept {
    std::array<std::byte, kBulkReadyBytes> out{};
    out[0] = static_cast<std::byte>(FrameType::bulk_ready);
    detail::put_le32(out.data() + 4, length);
    detail::put_le64(out.data() + 8, sequence);
    return out;
}

// RELEASE is {type:u8, reserved[7]=0, consumed_sequence:le64}. Credits are
// cumulative, so an implementation may coalesce unsent releases.
inline std::array<std::byte, kBulkReleaseBytes> encode_bulk_release(
    std::uint64_t sequence) noexcept {
    std::array<std::byte, kBulkReleaseBytes> out{};
    out[0] = static_cast<std::byte>(FrameType::bulk_release);
    detail::put_le64(out.data() + 8, sequence);
    return out;
}

inline bool decode_frame(std::string_view record, FrameView& frame) noexcept {
    if (record.empty()) return false;
    const auto* bytes = reinterpret_cast<const std::byte*>(record.data());
    frame = {};
    frame.type = static_cast<FrameType>(static_cast<std::uint8_t>(bytes[0]));
    switch (frame.type) {
        case FrameType::inline_bytes:
            if (record.size() < 2 || record.size() > kControlRecordBytes) return false;
            frame.inline_payload = record.substr(1);
            return true;
        case FrameType::bulk_ready:
            if (record.size() != kBulkReadyBytes || bytes[1] != std::byte{0} ||
                !detail::all_zero(std::span(bytes + 2, std::size_t{2})))
                return false;
            frame.length = detail::get_le32(bytes + 4);
            frame.sequence = detail::get_le64(bytes + 8);
            return frame.length != 0 && frame.sequence != 0;
        case FrameType::bulk_release:
            if (record.size() != kBulkReleaseBytes ||
                !detail::all_zero(std::span(bytes + 1, std::size_t{7})))
                return false;
            frame.sequence = detail::get_le64(bytes + 8);
            return frame.sequence != 0;
        default:
            return false;
    }
}

} // namespace goblin::store::rdma_wire
