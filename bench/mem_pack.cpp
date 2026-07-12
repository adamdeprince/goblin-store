// Quick memory-consumption microbenchmark for the RAM head allocator (ADR-0008). Packs a small-object
// size distribution into a fixed head arena three ways and reports capacity (objects held) + effective
// bytes/object -- where the whole small-object memory story lives:
//   * arena(16)   -- Phase 2: byte-granular bump packing (no rounding)
//   * buddy(16)   -- Phase 1: power-of-two rounding, 16 B floor
//   * buddy(4096) -- original: 4 KiB device-block floor (the ~12x memcached gap)
// No server, no io_uring -- pure allocator packing. Build: g++ -std=c++23 -O2 -I src bench/mem_pack.cpp
// src/goblin/core/buffer_pool.cpp src/goblin/common/error.cpp -o /tmp/mem_pack
#include "goblin/core/buffer_pool.hpp"

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace goblin;
using namespace goblin::core;

namespace {

// Objects held when packing `sizes` (cycled) into one `arena` region until it's full.
std::size_t arena_cap(Size arena, Size align, const std::vector<std::uint32_t>& sizes) {
    ArenaAllocator a(arena, align);
    std::size_t n = 0;
    while (a.allocate(sizes[n % sizes.size()]).has_value()) ++n;
    return n;
}
std::size_t buddy_cap(Size arena, Size min_block, const std::vector<std::uint32_t>& sizes) {
    BuddyAllocator b(arena, min_block);
    std::size_t n = 0;
    while (b.allocate(sizes[n % sizes.size()]).has_value()) ++n;
    return n;
}

void row(const std::string& label, Size arena, const std::vector<std::uint32_t>& sizes) {
    double avg = 0;
    for (auto s : sizes) avg += s;
    avg /= static_cast<double>(sizes.size());
    const auto a16 = arena_cap(arena, 16, sizes);
    const auto b16 = buddy_cap(arena, 16, sizes);
    const auto b4k = buddy_cap(arena, 4096, sizes);
    std::printf("  %-17s avg=%4.0fB | arena16 %8zu (%4.0f B/obj) | buddy16 %8zu (%4.0f B/obj, +%2.0f%%) | "
                "buddy4K %8zu (%4.1fx)\n",
                label.c_str(), avg, a16, static_cast<double>(arena) / static_cast<double>(a16), b16,
                static_cast<double>(arena) / static_cast<double>(b16),
                100.0 * (static_cast<double>(a16) / static_cast<double>(b16) - 1.0), b4k,
                static_cast<double>(a16) / static_cast<double>(b4k));
}

} // namespace

int main() {
    const Size arena = 256 * MiB; // one big head arena; capacity scales with it (power of two for buddy)
    std::printf("RAM head packing -- objects held in a %zu MiB head arena (more = less RAM per object)\n\n",
                static_cast<std::size_t>(arena / MiB));

    // Realistic small-object mix: uniform 96..1024 B.
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> d(96, 1024);
    std::vector<std::uint32_t> mix(8192);
    for (auto& s : mix) s = static_cast<std::uint32_t>(d(rng));
    row("uniform 96..1024", arena, mix);

    // Pure sizes to show where power-of-two rounding hurts (just above a power of two = worst).
    for (std::uint32_t s : {250u, 300u, 520u, 768u, 1000u, 1500u})
        row("fixed " + std::to_string(s) + "B", arena, std::vector<std::uint32_t>{s});

    std::printf("\narena16 = Phase 2 (byte-granular)   buddy16 = Phase 1 (power-of-two)   "
                "buddy4K = original (4 KiB floor)\n");
    std::printf("+%% = arena16 capacity gain over buddy16 (Phase 2 vs Phase 1)\n");
    return 0;
}
