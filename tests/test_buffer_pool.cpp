#include "mini_test.hpp"

#include "goblin/core/buffer_pool.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

using namespace goblin;
using namespace goblin::core;

TEST("buddy: split allocates distinct sub-blocks; used() tracks demand") {
    BuddyAllocator b(64 * KiB, 4 * KiB);
    const auto a0 = b.allocate(4 * KiB);
    const auto a1 = b.allocate(4 * KiB);
    CHECK(a0.has_value());
    CHECK(a1.has_value());
    CHECK(*a0 != *a1);
    CHECK_EQ(b.used(), Size(8 * KiB));
}

TEST("buddy: whole-arena allocation, then exhausted") {
    BuddyAllocator b(64 * KiB, 4 * KiB);
    const auto big = b.allocate(64 * KiB);
    CHECK(big.has_value());
    CHECK_EQ(*big, Offset(0));
    CHECK(!b.allocate(4 * KiB).has_value());
}

TEST("buddy: rounds up to a power of two") {
    BuddyAllocator b(64 * KiB, 4 * KiB);
    CHECK(b.allocate(5 * KiB).has_value());
    CHECK_EQ(b.used(), Size(8 * KiB)); // 5 KiB -> 8 KiB block
}

TEST("buddy: free coalesces buddies back to the whole arena") {
    BuddyAllocator b(64 * KiB, 4 * KiB);
    const auto a0 = b.allocate(32 * KiB);
    const auto a1 = b.allocate(32 * KiB);
    CHECK(a0.has_value() && a1.has_value());
    CHECK(!b.allocate(4 * KiB).has_value()); // full
    b.deallocate(*a0, 32 * KiB);
    b.deallocate(*a1, 32 * KiB);
    CHECK_EQ(b.used(), Size(0));
    const auto whole = b.allocate(64 * KiB); // merged back into one block
    CHECK(whole.has_value());
    CHECK_EQ(*whole, Offset(0));
}

TEST("buddy: zero and too-big allocations fail") {
    BuddyAllocator b(64 * KiB, 4 * KiB);
    CHECK(!b.allocate(0).has_value());
    CHECK(!b.allocate(128 * KiB).has_value());
}

TEST("block_pool: acquire distinct, O_DIRECT-aligned blocks until exhausted") {
    auto pool = BlockPool::create(64 * KiB, 4, /*lock_memory=*/false);
    CHECK(pool.has_value());
    auto& p = *pool;
    CHECK_EQ(p.num_blocks(), Size(4));
    CHECK_EQ(p.free_blocks(), Size(4));

    bool seen[4] = {false, false, false, false};
    for (int i = 0; i < 4; ++i) {
        const auto idx = p.acquire();
        CHECK(idx.has_value());
        CHECK(*idx < 4u);
        seen[*idx] = true;
        CHECK_EQ(reinterpret_cast<std::uintptr_t>(p.block_data(*idx)) % kDeviceBlock,
                 std::uintptr_t(0));
    }
    CHECK(seen[0] && seen[1] && seen[2] && seen[3]);
    CHECK(!p.acquire().has_value());
    CHECK_EQ(p.free_blocks(), Size(0));
}

TEST("block_pool: release returns a block to the pool") {
    auto pool = BlockPool::create(64 * KiB, 2, false);
    CHECK(pool.has_value());
    auto& p = *pool;
    const auto a = p.acquire();
    const auto b = p.acquire();
    CHECK(a && b);
    CHECK(!p.acquire().has_value());
    p.release(*a);
    CHECK_EQ(p.free_blocks(), Size(1));
    CHECK(p.acquire().has_value());
}

TEST("block_pool: region priority survives out-of-order releases") {
    const std::array<BlockPoolRegion, 2> regions{{
        {128 * KiB, std::nullopt}, // preferred/local: blocks 0-1
        {128 * KiB, std::nullopt}, // subordinate: blocks 2-3
    }};
    auto pool = BlockPool::create_regions(64 * KiB, regions, false,
                                          /*try_hugetlb=*/false);
    CHECK(pool.has_value());
    if (!pool) return;
    CHECK_EQ(pool->region_count(), std::size_t(2));
    CHECK_EQ(pool->block_region(0), std::optional<std::size_t>(0));
    CHECK_EQ(pool->block_region(1), std::optional<std::size_t>(0));
    CHECK_EQ(pool->block_region(2), std::optional<std::size_t>(1));
    CHECK(!pool->block_region(4).has_value());
    CHECK(!pool->region_uses_hugetlb(0));
    CHECK(!pool->region_uses_hugetlb(1));

    const auto local0 = pool->acquire();
    const auto local1 = pool->acquire();
    const auto foreign0 = pool->acquire();
    CHECK(local0 && local1 && foreign0);
    if (!(local0 && local1 && foreign0)) return;
    CHECK_EQ(*local0, 0u);
    CHECK_EQ(*local1, 1u);
    CHECK_EQ(*foreign0, 2u);

    pool->release(*local0);
    pool->release(*foreign0); // released last, but must not jump ahead of a free local block
    const auto reacquired_local = pool->acquire();
    const auto reacquired_foreign = pool->acquire();
    CHECK(reacquired_local && reacquired_foreign);
    if (reacquired_local && reacquired_foreign) {
        CHECK_EQ(*reacquired_local, *local0);
        CHECK_EQ(*reacquired_foreign, *foreign0);
    }

    for (unsigned block = 0; block < 4; ++block) {
        CHECK_EQ(pool->block_index(pool->block_data(block)), std::optional<unsigned>(block));
        CHECK_EQ(pool->block_index(pool->block_data(block) + 17), std::optional<unsigned>(block));
    }
}

TEST("block_pool: unavailable hugetlb silently falls back to ordinary memory") {
    // 64 KiB is deliberately not the platform default huge-page size on supported production
    // targets. The pool must remain usable whether the kernel rejects that order or happens to have
    // such a pool configured.
    auto pool = BlockPool::create(64 * KiB, 2, /*lock_memory=*/false,
                                  /*try_hugetlb=*/true);
    CHECK(pool.has_value());
    if (!pool) return;
    const auto block = pool->acquire();
    CHECK(block.has_value());
    if (block) pool->block_data(*block)[0] = std::byte{0x5a};
}

TEST("block_pool: rejects bad geometry") {
    CHECK(!BlockPool::create(1000, 4, false).has_value());   // not a power of two
    CHECK(!BlockPool::create(1 * KiB, 4, false).has_value()); // below the 4 KiB device block
    CHECK(!BlockPool::create(64 * KiB, 0, false).has_value()); // zero blocks
}

TEST("buffer_pool: allocate distinct usable regions; write/read; addr matches") {
    auto poolr = BufferPool::create(/*total=*/256 * KiB, /*block=*/64 * KiB, /*min=*/4 * KiB, false);
    CHECK(poolr.has_value());
    auto& pool = *poolr;

    const auto a = pool.allocate(4 * KiB, 4 * KiB);
    const auto b = pool.allocate(8 * KiB, 4 * KiB);
    CHECK(a.has_value());
    CHECK(b.has_value());
    CHECK(a->data != b->data);
    CHECK_EQ(pool.addr(a->block, a->offset), a->data);

    for (std::uint32_t i = 0; i < a->len; ++i) a->data[i] = std::byte{0xAB};
    bool ok = true;
    for (std::uint32_t i = 0; i < a->len; ++i)
        if (a->data[i] != std::byte{0xAB}) ok = false;
    CHECK(ok);
}

TEST("buffer_pool: too-big alloc fails; exhaustion returns nullopt; free reuses the block") {
    auto poolr = BufferPool::create(128 * KiB, 64 * KiB, 4 * KiB, false); // 2 blocks
    CHECK(poolr.has_value());
    auto& pool = *poolr;

    CHECK(!pool.allocate(128 * KiB, 4 * KiB).has_value()); // larger than a block

    const auto a = pool.allocate(64 * KiB, 4 * KiB); // fills block 0's arena
    const auto b = pool.allocate(64 * KiB, 4 * KiB); // fills block 1's arena
    CHECK(a.has_value());
    CHECK(b.has_value());
    CHECK(!pool.allocate(4 * KiB, 4 * KiB).has_value()); // both blocks in use -> exhausted

    pool.deallocate(a->block, a->offset, a->len); // empties block 0 -> returned to pool
    CHECK(pool.allocate(64 * KiB, 4 * KiB).has_value());   // reused
}

TEST("buffer_pool: reserved allocation classes share indexes but never borrow blocks") {
    const std::array<BlockPoolRegion, 5> regions{{
        {64 * KiB, std::nullopt, BufferPoolClass::fixed_head},   // block 0
        {64 * KiB, std::nullopt, BufferPoolClass::small_object}, // block 1
        {64 * KiB, std::nullopt, BufferPoolClass::fixed_head},   // block 2
        {64 * KiB, std::nullopt, BufferPoolClass::small_object}, // block 3
        {64 * KiB, std::nullopt},                                // block 4: legacy shared
    }};
    auto poolr = BufferPool::create_regions(regions, 64 * KiB, 4 * KiB, false,
                                            /*try_hugetlb=*/false);
    CHECK(poolr.has_value());
    if (!poolr) return;
    auto& pool = *poolr;

    // The old two-argument API remains a shared-region allocation. It sees neither reservation,
    // even while both are empty, and retains the one global block/address namespace.
    const auto shared = pool.allocate(64 * KiB, 4 * KiB);
    CHECK(shared.has_value());
    if (!shared) return;
    CHECK_EQ(shared->block, 4u);
    CHECK(!pool.allocate(4 * KiB, 4 * KiB));

    const auto head0 =
        pool.allocate(64 * KiB, 4 * KiB, BufferPoolClass::fixed_head);
    const auto head1 =
        pool.allocate(64 * KiB, 4 * KiB, BufferPoolClass::fixed_head);
    CHECK(head0 && head1);
    if (!(head0 && head1)) return;
    CHECK_EQ(head0->block, 0u);
    CHECK_EQ(head1->block, 2u);
    CHECK(!pool.allocate(4 * KiB, 4 * KiB, BufferPoolClass::fixed_head));

    const auto small0 =
        pool.allocate(64 * KiB, 16, BufferPoolClass::small_object);
    const auto small1 =
        pool.allocate(64 * KiB, 16, BufferPoolClass::small_object);
    CHECK(small0 && small1);
    if (!(small0 && small1)) return;
    CHECK_EQ(small0->block, 1u);
    CHECK_EQ(small1->block, 3u);
    CHECK(!pool.allocate(16, 16, BufferPoolClass::small_object));

    // Releasing a fixed-head block does not make it visible to the exhausted small-object class.
    pool.deallocate(head0->block, head0->offset, head0->len);
    CHECK(!pool.allocate(16, 16, BufferPoolClass::small_object));

    CHECK_EQ(pool.region_allocation_class(0), BufferPoolClass::fixed_head);
    CHECK_EQ(pool.region_allocation_class(1), BufferPoolClass::small_object);
    CHECK_EQ(pool.region_allocation_class(4), BufferPoolClass::shared);
    CHECK(pool.block_is_local(head0->block));
    CHECK(!pool.block_is_local(head1->block));
    CHECK(pool.block_is_local(small0->block));
    CHECK(!pool.block_is_local(small1->block));
    CHECK(pool.block_is_local(shared->block));
    CHECK(head0->data != small0->data);
    CHECK_EQ(pool.addr(head0->block, head0->offset), head0->data);
    CHECK_EQ(pool.addr(small0->block, small0->offset), small0->data);
}

TEST("buffer_pool: block swaps cannot cross allocation-class reservations") {
    const std::array<BlockPoolRegion, 2> regions{{
        {64 * KiB, std::nullopt, BufferPoolClass::fixed_head},
        {64 * KiB, std::nullopt, BufferPoolClass::shared},
    }};
    auto poolr = BufferPool::create_regions(regions, 64 * KiB, 4 * KiB, false,
                                            /*try_hugetlb=*/false);
    CHECK(poolr.has_value());
    if (!poolr) return;
    auto& pool = *poolr;
    const auto head =
        pool.allocate(64 * KiB, 4 * KiB, BufferPoolClass::fixed_head);
    const auto shared = pool.allocate(64 * KiB, 4 * KiB);
    CHECK(head && shared);
    if (head && shared) CHECK(!pool.swap_blocks(head->block, shared->block));
}

TEST("buffer_pool: a freed local block wins over space in an existing foreign arena") {
    const std::array<BlockPoolRegion, 2> regions{{
        {64 * KiB, std::nullopt}, // one local block
        {64 * KiB, std::nullopt}, // one foreign block
    }};
    auto poolr = BufferPool::create_regions(regions, 64 * KiB, 4 * KiB, false);
    CHECK(poolr.has_value());
    if (!poolr) return;
    auto& pool = *poolr;

    const auto local = pool.allocate(64 * KiB, 4 * KiB); // exhaust local region
    const auto foreign = pool.allocate(32 * KiB, 4 * KiB); // opens a half-full foreign arena
    CHECK(local && foreign);
    if (!(local && foreign)) return;
    CHECK_EQ(local->block, 0u);
    CHECK_EQ(foreign->block, 1u);

    pool.deallocate(local->block, local->offset, local->len); // local is available again
    const auto next = pool.allocate(4 * KiB, 4 * KiB);
    CHECK(next.has_value());
    if (next) CHECK_EQ(next->block, 0u);
}

TEST("buffer_pool: swaps full buddy blocks across logical NUMA regions") {
    const std::array<BlockPoolRegion, 2> regions{{
        {64 * KiB, std::nullopt}, // preferred/local block 0
        {64 * KiB, std::nullopt}, // foreign block 1
    }};
    auto poolr = BufferPool::create_regions(regions, 64 * KiB, 4 * KiB, false);
    CHECK(poolr.has_value());
    if (!poolr) return;
    auto& pool = *poolr;
    const auto local = pool.allocate(64 * KiB, 4 * KiB);
    const auto remote_a = pool.allocate(32 * KiB, 4 * KiB);
    const auto remote_b = pool.allocate(32 * KiB, 4 * KiB);
    CHECK(local && remote_a && remote_b);
    if (!(local && remote_a && remote_b)) return;
    CHECK(pool.block_is_local(local->block));
    CHECK(!pool.block_is_local(remote_a->block));
    CHECK_EQ(remote_a->block, remote_b->block);
    CHECK(pool.full_buddy_block(local->block, 64 * KiB));
    CHECK(pool.full_buddy_block(remote_a->block, 64 * KiB));
    CHECK(!pool.full_buddy_block(local->block, 32 * KiB)); // incomplete index accounting is unsafe

    std::fill_n(local->data, local->len, std::byte{0x11});
    std::fill_n(remote_a->data, remote_a->len, std::byte{0x77});
    std::fill_n(remote_b->data, remote_b->len, std::byte{0x55});
    CHECK(pool.swap_blocks(local->block, remote_a->block));
    CHECK(std::all_of(local->data + remote_a->offset,
                      local->data + remote_a->offset + remote_a->len,
                      [](std::byte b) { return b == std::byte{0x77}; }));
    CHECK(std::all_of(local->data + remote_b->offset,
                      local->data + remote_b->offset + remote_b->len,
                      [](std::byte b) { return b == std::byte{0x55}; }));
    CHECK(std::all_of(remote_a->data - remote_a->offset,
                      remote_a->data - remote_a->offset + local->len,
                      [](std::byte b) { return b == std::byte{0x11}; }));

    // Allocator metadata moves with the bytes: the two former-remote halves now free block 0,
    // while the former-local whole allocation now frees block 1.
    pool.deallocate(local->block, remote_a->offset, remote_a->len);
    pool.deallocate(local->block, remote_b->offset, remote_b->len);
    pool.deallocate(remote_a->block, local->offset, local->len);
    CHECK_EQ(pool.free_blocks(), Size(2));
}

TEST("buffer_pool: bump-arena blocks are excluded from full-block NUMA swaps") {
    auto poolr = BufferPool::create(64 * KiB, 64 * KiB, 4 * KiB, false);
    CHECK(poolr.has_value());
    if (!poolr) return;
    const auto small = poolr->allocate(1024, /*min_alloc=*/16);
    CHECK(small.has_value());
    if (!small) return;
    CHECK(!poolr->buddy_allocation_bytes(small->block, small->len).has_value());
    CHECK(!poolr->full_buddy_block(small->block, 64 * KiB));
}

TEST("arena: byte-granular bump packing; dead accounting; drains to empty") {
    ArenaAllocator a(4 * KiB, /*align=*/8);
    const auto x = a.allocate(100); // rounds to 104
    const auto y = a.allocate(200); // 200
    const auto z = a.allocate(30);  // 32
    CHECK(x.has_value()); CHECK(y.has_value()); CHECK(z.has_value());
    CHECK_EQ(*x, Offset{0});
    CHECK_EQ(*y, Offset{104});      // packed right after x -- no power-of-two rounding
    CHECK_EQ(*z, Offset{304});      // 104 + 200
    CHECK_EQ(a.used(), Size{336});
    CHECK_EQ(a.frontier(), Size{336});
    a.deallocate(*y, 200);          // y dies: dead space grows, frontier does not rewind
    CHECK_EQ(a.used(), Size{136});
    CHECK_EQ(a.dead(), Size{200});
    CHECK_EQ(a.frontier(), Size{336});
    a.deallocate(*x, 100);
    a.deallocate(*z, 30);
    CHECK_EQ(a.used(), Size{0});     // fully dead -> BufferPool would reclaim the block
}

TEST("arena: zero fails; exact fit succeeds; overflow past the frontier fails") {
    ArenaAllocator a(256, 8);
    CHECK(a.allocate(0) == std::nullopt);
    const auto p = a.allocate(250);          // rounds to 256 -- exact fit
    CHECK(p.has_value());
    CHECK_EQ(a.frontier(), Size{256});
    CHECK(a.allocate(8) == std::nullopt);     // frontier full
    CHECK(a.allocate(1000) == std::nullopt);  // larger than the arena
}

TEST("buffer_pool: small min-order packs many objects/block; a large class gets its own block") {
    auto poolr = BufferPool::create(/*total=*/256 * KiB, /*block=*/64 * KiB, /*min=*/4 * KiB, false);
    CHECK(poolr.has_value());
    auto& pool = *poolr;
    // 200 x 100 B at a 64 B min-order round to 128 B (~25 KiB) -> one 64 KiB block. At the 4 KiB min
    // they'd need 200 x 4 KiB = 800 KiB and blow the 256 KiB pool -- that gap is the Phase-1 win.
    unsigned small_block = ~0u;
    int packed = 0;
    for (int i = 0; i < 200; ++i) {
        const auto r = pool.allocate(100, /*min_alloc=*/64);
        if (!r) break;
        ++packed;
        if (small_block == ~0u) small_block = r->block;
    }
    CHECK_EQ(packed, 200);
    // A 4 KiB-class allocation must not share the small (64 B) block.
    const auto big = pool.allocate(8 * KiB, /*min_alloc=*/4 * KiB);
    CHECK(big.has_value());
    if (big) CHECK(big->block != small_block);
}

TEST("io_buffer_pool: fixed chunks, distinct + usable, exhaustion, release reuses") {
    auto pr = IoBufferPool::create(/*chunk=*/64 * KiB, /*count=*/3, false);
    CHECK(pr.has_value());
    auto& p = *pr;
    CHECK_EQ(p.chunk_bytes(), Size(64 * KiB));

    const auto a = p.acquire();
    const auto b = p.acquire();
    const auto c = p.acquire();
    CHECK(a.has_value());
    CHECK(b.has_value());
    CHECK(c.has_value());
    CHECK(!p.acquire().has_value()); // only 3 chunks
    CHECK_EQ(a->size(), std::size_t(64 * KiB));
    CHECK(a->data() != b->data());

    (*a)[0] = std::byte{0x11}; // usable
    CHECK(std::to_integer<int>((*a)[0]) == 0x11);

    p.release(*a);
    CHECK(p.acquire().has_value()); // reused
}

TEST("io_buffer_pool: backing hugepage order does not change chunk geometry") {
    constexpr unsigned chunks = 32;
    auto pr = IoBufferPool::create(/*chunk=*/64 * KiB, chunks, /*lock_memory=*/false,
                                   /*try_hugetlb=*/true, /*hugetlb_bytes=*/2 * MiB);
    CHECK(pr.has_value());
    if (!pr) return;
    CHECK_EQ(pr->chunk_bytes(), Size(64 * KiB));
    for (unsigned i = 0; i < chunks; ++i) CHECK(pr->acquire().has_value());
    CHECK(!pr->acquire().has_value());
}

TEST("buffer_pool: logical allocation block may span multiple HugeTLB pages") {
    auto pr = BufferPool::create(/*total=*/8 * MiB, /*block=*/4 * MiB,
                                 /*min=*/4 * KiB, /*lock_memory=*/false,
                                 /*try_hugetlb=*/true, /*hugetlb_bytes=*/2 * MiB);
    CHECK(pr.has_value()); // also covers graceful ordinary-memory fallback when no pool is reserved
    if (!pr) return;
    CHECK_EQ(pr->block_bytes(), Size(4 * MiB));

    const auto first = pr->allocate(256 * KiB, 4 * KiB);
    CHECK(first.has_value());
    if (first) CHECK_EQ(first->len, std::uint32_t(256 * KiB));
}
