#include "goblin/store/rdma_wire.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace wire = goblin::store::rdma_wire;
using namespace std::string_view_literals;

#define CHECK(expression)        \
    do {                         \
        if (!(expression))       \
            return __LINE__;     \
    } while (false)

int main() {
    CHECK(wire::valid_bulk_geometry(4096, 1));
    CHECK(!wire::valid_bulk_geometry(2048, 1));
    CHECK(!wire::valid_bulk_geometry(6144, 1));
    CHECK(!wire::valid_bulk_geometry(4096, 0));
    CHECK(wire::valid_bulk_geometry(std::uint32_t{1} << 30, 1));
    CHECK(!wire::valid_bulk_geometry(std::uint32_t{1} << 30, 2));
    const auto commit = wire::control_commit_to_wire(0x010203040506ULL, 16);
    CHECK(wire::control_commit_from_wire(commit) == 0x01020304050610ULL);

    const wire::Descriptor descriptor{
        .ring_address = 0x0102030405060708ULL,
        .nonce = 0x1112131415161718ULL,
        .bulk_address = 0x2122232425262728ULL,
        .ring_rkey = 0x0a0b0c0dU,
        .ring_region_bytes = 0x00102030U,
        .ring_slot_count = 0x00000102U,
        .bulk_rkey = 0x31323334U,
        .bulk_region_bytes = 0x00200000U,
        .bulk_window_bytes = 0x00040000U,
        .bulk_window_count = 4,
    };
    const std::array<std::byte, wire::kDescriptorBytes> golden{
        std::byte{0x47}, std::byte{0x42}, std::byte{0x52}, std::byte{0x44},
        std::byte{0x00}, std::byte{0x03}, std::byte{0x01}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x02},
        std::byte{0x00}, std::byte{0xc0}, std::byte{0x00}, std::byte{0x12},
        std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
        std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08},
        std::byte{0x0a}, std::byte{0x0b}, std::byte{0x0c}, std::byte{0x0d},
        std::byte{0x00}, std::byte{0x10}, std::byte{0x20}, std::byte{0x30},
        std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14},
        std::byte{0x15}, std::byte{0x16}, std::byte{0x17}, std::byte{0x18},
        std::byte{0x21}, std::byte{0x22}, std::byte{0x23}, std::byte{0x24},
        std::byte{0x25}, std::byte{0x26}, std::byte{0x27}, std::byte{0x28},
        std::byte{0x31}, std::byte{0x32}, std::byte{0x33}, std::byte{0x34},
        std::byte{0x00}, std::byte{0x20}, std::byte{0x00}, std::byte{0x00},
    };
    const auto encoded = wire::encode_descriptor(descriptor);
    CHECK(encoded == golden);

    wire::Descriptor decoded;
    CHECK(wire::decode_descriptor(std::span(encoded), decoded));
    CHECK(!wire::decode_descriptor(
        std::span(encoded).first(wire::kDescriptorBytes - 1), decoded));
    CHECK(decoded.ring_address == descriptor.ring_address);
    CHECK(decoded.bulk_address == descriptor.bulk_address);
    CHECK(decoded.ring_slot_count == descriptor.ring_slot_count);
    CHECK(decoded.bulk_window_bytes == descriptor.bulk_window_bytes);
    CHECK(decoded.bulk_window_count == descriptor.bulk_window_count);
    std::array<std::byte, 64> provider_padded{};
    std::copy(encoded.begin(), encoded.end(), provider_padded.begin());
    CHECK(wire::decode_descriptor(std::span(provider_padded), decoded));

    std::array<std::byte, wire::kControlRecordBytes> inline_frame{};
    CHECK(wire::encode_inline({}, inline_frame) == 0);
    CHECK(wire::encode_inline("g\0b"sv, inline_frame) == 4);
    wire::FrameView frame;
    CHECK(wire::decode_frame(
        std::string_view(reinterpret_cast<const char*>(inline_frame.data()), 4), frame));
    CHECK(frame.type == wire::FrameType::inline_bytes);
    CHECK(frame.inline_payload == "g\0b"sv);

    const auto ready = wire::encode_bulk_ready(0x01020304U, 0x0102030405060708ULL);
    const std::array<std::byte, wire::kBulkReadyBytes> ready_golden{
        std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x04}, std::byte{0x03}, std::byte{0x02}, std::byte{0x01},
        std::byte{0x08}, std::byte{0x07}, std::byte{0x06}, std::byte{0x05},
        std::byte{0x04}, std::byte{0x03}, std::byte{0x02}, std::byte{0x01},
    };
    CHECK(ready == ready_golden);
    CHECK(wire::decode_frame(
        std::string_view(reinterpret_cast<const char*>(ready.data()), ready.size()), frame));
    CHECK(frame.type == wire::FrameType::bulk_ready);
    CHECK(frame.length == 0x01020304U);
    CHECK(frame.sequence == 0x0102030405060708ULL);
    auto zero_ready = wire::encode_bulk_ready(0, 1);
    CHECK(!wire::decode_frame(
        std::string_view(reinterpret_cast<const char*>(zero_ready.data()), zero_ready.size()),
        frame));
    auto bad_ready = ready;
    bad_ready[1] = std::byte{1};
    CHECK(!wire::decode_frame(
        std::string_view(reinterpret_cast<const char*>(bad_ready.data()), bad_ready.size()),
        frame));

    auto release = wire::encode_bulk_release(9);
    CHECK(wire::decode_frame(
        std::string_view(reinterpret_cast<const char*>(release.data()), release.size()), frame));
    CHECK(frame.type == wire::FrameType::bulk_release && frame.sequence == 9);
    release[2] = std::byte{1};
    CHECK(!wire::decode_frame(
        std::string_view(reinterpret_cast<const char*>(release.data()), release.size()), frame));

    auto overflowing = descriptor;
    overflowing.bulk_address = std::numeric_limits<std::uint64_t>::max() - 1;
    const auto overflowing_bytes = wire::encode_descriptor(overflowing);
    CHECK(!wire::decode_descriptor(std::span(overflowing_bytes), decoded));
    overflowing = descriptor;
    overflowing.ring_address = std::numeric_limits<std::uint64_t>::max() - 1;
    const auto overflowing_ring = wire::encode_descriptor(overflowing);
    CHECK(!wire::decode_descriptor(std::span(overflowing_ring), decoded));
}

#undef CHECK
