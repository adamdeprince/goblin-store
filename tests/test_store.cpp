#include "mini_test.hpp"

#include "goblin/store.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace goblin;

TEST("embedded store: streaming writer and head-first reader round-trip without networking") {
    const fs::path base = fs::temp_directory_path() /
        ("goblin-embedded-" + std::to_string(::getpid()));
    const fs::path disk = base / "disk";
    fs::remove_all(base);
    CHECK(Store::prepare_directory(disk.string()).has_value());

    StoreOptions options;
    options.ssd.dirs = {disk.string()};
    options.ssd.stripe_unit = 4 * KiB;
    options.tiers.ram_head = 4 * KiB;
    options.tiers.ssd_prefix = 64 * KiB;
    options.memory.total_bytes = 2 * MiB;
    options.memory.block_bytes = 2 * MiB;
    options.memory.lock_memory = false;
    options.memory.use_hugepages = false;
    options.read_chunk_bytes = 8 * KiB;
    options.write_chunk_bytes = 8 * KiB;
    options.write_buffers = 2;
    options.direct_io = false;

    auto opened = Store::open(options);
    CHECK(opened.has_value());
    if (!opened) {
        fs::remove_all(base);
        return;
    }
    Store store = std::move(*opened);

    std::vector<std::byte> expected(37 * KiB);
    for (std::size_t i = 0; i < expected.size(); ++i)
        expected[i] = static_cast<std::byte>((i * 29 + 7) & 0xff);
    auto writer = store.begin_put("large", expected.size(), PutOptions{.flags = 42});
    CHECK(writer.has_value());
    if (writer) {
        CHECK(writer->write(ByteView(expected.data(), 3 * KiB)).has_value());
        CHECK(writer->write(ByteView(expected.data() + 3 * KiB,
                                    expected.size() - 3 * KiB)).has_value());
        auto committed = writer->commit();
        CHECK(committed.has_value());
        if (committed) {
            CHECK_EQ(committed->size, Size(expected.size()));
            CHECK_EQ(committed->flags, std::uint32_t(42));
        }
    }

    auto reader = store.make_reader();
    CHECK(reader.has_value());
    if (reader) {
        std::vector<std::byte> actual;
        std::vector<std::size_t> chunks;
        auto streamed = reader->stream("large", [&](ByteView piece) -> Status {
            chunks.push_back(piece.size());
            actual.insert(actual.end(), piece.begin(), piece.end());
            return {};
        });
        CHECK(streamed.has_value());
        CHECK(!chunks.empty());
        if (!chunks.empty()) CHECK_EQ(chunks.front(), std::size_t(4 * KiB));
        CHECK(actual == expected);

        std::size_t ready_bytes = 0;
        std::vector<std::size_t> ready_chunks;
        auto loaded = reader->load("large", [&](ByteView piece) -> Status {
            ready_bytes += piece.size();
            ready_chunks.push_back(piece.size());
            return {};
        });
        CHECK(loaded.has_value());
        CHECK_EQ(ready_bytes, expected.size());
        CHECK(!ready_chunks.empty());
        if (!ready_chunks.empty()) CHECK_EQ(ready_chunks.front(), std::size_t(4 * KiB));
        if (loaded) {
            CHECK_EQ(loaded->bytes.size(), expected.size());
            if (loaded->bytes.size() == expected.size())
                CHECK(std::equal(loaded->bytes.begin(), loaded->bytes.end(), expected.begin()));
        }
        const auto first_capacity = reader->materialization_capacity();
        CHECK_EQ(first_capacity, align_up(Size(expected.size()), kDeviceBlock));
        const std::byte* const retained = loaded ? loaded->bytes.data() : nullptr;

        std::vector<std::byte> small(1 * KiB, std::byte{0x5a});
        CHECK(store.put("small", ByteView(small.data(), small.size())).has_value());
        auto small_loaded = reader->load("small");
        CHECK(small_loaded.has_value());
        if (small_loaded) {
            CHECK_EQ(small_loaded->bytes.data(), retained);
            CHECK_EQ(small_loaded->bytes.size(), small.size());
            if (small_loaded->bytes.size() == small.size())
                CHECK(std::equal(small_loaded->bytes.begin(), small_loaded->bytes.end(),
                                 small.begin()));
        }
        CHECK_EQ(reader->materialization_capacity(), first_capacity);

        std::vector<std::byte> larger(75 * KiB + 17);
        for (std::size_t i = 0; i < larger.size(); ++i)
            larger[i] = static_cast<std::byte>((i * 11 + 3) & 0xff);
        CHECK(store.put("larger", ByteView(larger.data(), larger.size())).has_value());
        auto larger_loaded = reader->load("larger");
        CHECK(larger_loaded.has_value());
        if (larger_loaded) {
            CHECK_EQ(larger_loaded->bytes.size(), larger.size());
            if (larger_loaded->bytes.size() == larger.size())
                CHECK(std::equal(larger_loaded->bytes.begin(), larger_loaded->bytes.end(),
                                 larger.begin()));
        }
        CHECK_EQ(reader->materialization_capacity(),
                 align_up(Size(larger.size()), kDeviceBlock));

        auto materialized = reader->get("large");
        CHECK(materialized.has_value());
        if (materialized) CHECK(*materialized == expected);
        auto missing = reader->get("missing");
        CHECK(!missing.has_value());
        if (!missing) CHECK_EQ(missing.error().code, Errc::not_found);
    }

    CHECK(store.erase("large"));
    CHECK(!store.info("large").has_value());
    fs::remove_all(base);
}
