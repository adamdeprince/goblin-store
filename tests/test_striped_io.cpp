#include "mini_test.hpp"

#include "goblin/core/reactor.hpp"
#include "goblin/storage/drive_pool.hpp"
#include "goblin/storage/striped_io.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fcntl.h>
#include <filesystem>
#include <print>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace goblin;
using namespace goblin::storage;

TEST("striped_io: scatter a 40 KiB object across 4 drives, gather it back through io_uring") {
    if (!core::Reactor::available()) {
        std::println("    (skipped: built without liburing)");
        return;
    }
    auto reactor = core::Reactor::create();
    if (!reactor) {
        std::println("    (skipped: io_uring unavailable: {})", reactor.error().detail);
        return;
    }

    constexpr unsigned N = 4;
    const DrivePool pool(N, 4 * KiB);
    const std::uint64_t key_hash = 0x0123456789abcdefULL;

    const std::string base =
        (fs::temp_directory_path() / ("goblin-stripe-" + std::to_string(::getpid()))).string();
    std::array<int, N> fds{};
    std::array<std::string, N> paths{};
    for (unsigned i = 0; i < N; ++i) {
        paths[i] = base + "-d" + std::to_string(i);
        fds[i] = ::open(paths[i].c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        CHECK(fds[i] >= 0);
    }
    const std::span<const int> fdspan(fds.data(), N);

    const Size sz = 40 * KiB;
    std::vector<std::byte> data(sz);
    for (Size i = 0; i < sz; ++i) data[i] = static_cast<std::byte>((i * 31 + 7) & 0xFF);

    CHECK(striped_pwrite(pool, key_hash, fdspan, 0, ByteView(data.data(), sz)).has_value());

    // Full read-back: every byte should match, fanned across all 4 drives.
    std::vector<std::byte> got(sz);
    const auto n = striped_read(*reactor, pool, key_hash, fdspan, 0, MutBytes(got.data(), sz));
    CHECK(n.has_value());
    if (n) {
        CHECK_EQ(*n, std::size_t(sz));
        CHECK(got == data);
    }

    // Sub-range read [8 KiB, 24 KiB) must equal data[8 KiB, 24 KiB) (offset handling).
    const Size suboff = 8 * KiB, sublen = 16 * KiB;
    std::vector<std::byte> sub(sublen);
    const auto m = striped_read(*reactor, pool, key_hash, fdspan, suboff, MutBytes(sub.data(), sublen));
    CHECK(m.has_value());
    if (m) {
        CHECK_EQ(*m, std::size_t(sublen));
        CHECK(std::equal(sub.begin(), sub.end(), data.begin() + static_cast<std::ptrdiff_t>(suboff)));
    }

    for (unsigned i = 0; i < N; ++i) {
        ::close(fds[i]);
        fs::remove(paths[i]);
    }
}
