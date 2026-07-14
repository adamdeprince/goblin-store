// RAM allocator (ADR-0008): a fixed array of power-of-two blocks + buddy allocation inside a
// block. Two pieces:
//   * BuddyAllocator — subdivides ONE power-of-two arena into power-of-two sub-blocks. It is
//     offset-based and memory-free, so it can sit over any real block and be unit-tested with
//     no allocation at all.
//   * BlockPool — owns aligned, optionally mlock'd mappings of equal blocks. Allocation regions are
//     ordered local-first and each has its own Linux memory policy and hugetlb fallback decision.
#pragma once

#include "goblin/common/error.hpp"
#include "goblin/common/types.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace goblin::core {

class BuddyAllocator {
public:
    // arena_size and min_block must be powers of two with arena_size >= min_block.
    BuddyAllocator(Size arena_size, Size min_block);

    std::optional<Offset> allocate(Size bytes);  // nullopt if too big or no space
    void deallocate(Offset off, Size bytes);      // pass the same `bytes` given to allocate()

    Size capacity() const noexcept { return arena_size_; }
    Size used() const noexcept { return used_; }
    Size min_block() const noexcept { return min_block_; }
    Size allocation_bytes(Size bytes) const noexcept {
        return bytes == 0 || bytes > arena_size_ ? 0 : block_size(order_for(bytes));
    }

private:
    unsigned order_for(Size bytes) const noexcept;
    Size block_size(unsigned order) const noexcept { return min_block_ << order; }

    Size arena_size_;
    Size min_block_;
    unsigned max_order_;
    Size used_ = 0;
    std::vector<std::vector<Offset>> free_; // free_[o] = stack of free block offsets at order o
};

// Byte-granular bump arena over ONE block (ADR-0008 Phase 2): packs variable-size RAM-only heads
// contiguously with NO power-of-two rounding -- unlike BuddyAllocator, which rounds every head up to
// a power-of-two leaf. A free only marks bytes dead; the bump frontier never rewinds, so a block's
// dead space is reclaimed by compaction (heads are movable -- TierManager rewrites HeadLoc) or when
// the block empties. Offset-based and memory-free like BuddyAllocator, so it unit-tests over any block.
class ArenaAllocator {
public:
    // arena_size is the block size; align rounds each slot (8 B keeps heads word-aligned, <=7 B waste).
    explicit ArenaAllocator(Size arena_size, Size align = 8)
        : arena_size_(arena_size), align_(align) {}

    std::optional<Offset> allocate(Size bytes); // nullopt if it won't fit below the bump frontier
    void deallocate(Offset off, Size bytes);     // marks the slot dead (pass the same bytes as allocate)

    Size capacity() const noexcept { return arena_size_; }
    Size used() const noexcept { return live_; }              // live (still-referenced) bytes
    Size dead() const noexcept { return frontier_ - live_; }  // reclaimable-by-compaction bytes
    Size frontier() const noexcept { return frontier_; }      // high-water; room left = capacity-frontier
    Size align() const noexcept { return align_; }            // slot granularity; the small-class key
    Size slot_size(Size bytes) const noexcept { return round(bytes); } // rounded footprint of a request
    void set_frontier(Size f) noexcept { frontier_ = f; }     // compaction rewind after sliding live down

private:
    Size round(Size b) const noexcept { return (b + align_ - 1) & ~(align_ - 1); }
    Size arena_size_;
    Size align_;
    Size frontier_ = 0; // next free offset; never rewinds (compaction drains the whole block instead)
    Size live_ = 0;     // sum of live slot sizes (rounded); block is reclaimable when this hits 0
};

// One physical part of a BlockPool. Regions are ordered by allocation preference. A NUMA node
// selects that node's hugetlb pool first and otherwise applies MPOL_BIND to a normal mapping;
// nullopt is useful for the ordinary allocator and tests.
struct BlockPoolRegion {
    Size bytes = 0;
    std::optional<unsigned> numa_node;
};

class BlockPool {
public:
    // hugetlb_bytes == 0 selects block_bytes. A separate value lets streaming pools keep their
    // smaller chunk allocation geometry inside a platform-sized hugetlb mapping.
    static Result<BlockPool> create(Size block_bytes, Size num_blocks, bool lock_memory,
                                    bool try_hugetlb = true, Size hugetlb_bytes = 0);
    static Result<BlockPool> create_regions(Size block_bytes,
                                            std::span<const BlockPoolRegion> regions,
                                            bool lock_memory, bool try_hugetlb = true,
                                            Size hugetlb_bytes = 0);
    ~BlockPool();
    BlockPool(BlockPool&&) noexcept;
    BlockPool& operator=(BlockPool&&) noexcept;
    BlockPool(const BlockPool&) = delete;
    BlockPool& operator=(const BlockPool&) = delete;

    std::optional<unsigned> acquire(); // first free block from the highest-priority nonempty region
    std::optional<unsigned> acquire_from_region(std::size_t region);
    void release(unsigned index);
    std::byte* block_data(unsigned index) const noexcept;

    Size block_bytes() const noexcept { return block_bytes_; }
    Size num_blocks() const noexcept { return num_blocks_; }
    Size free_blocks() const noexcept { return free_blocks_; }
    bool locked() const noexcept { return locked_; }
    std::size_t region_count() const noexcept { return regions_.size(); }
    unsigned region_first_block(std::size_t region) const noexcept;
    unsigned region_end_block(std::size_t region) const noexcept;
    std::optional<unsigned> region_numa_node(std::size_t region) const noexcept;
    std::optional<std::size_t> block_region(unsigned block) const noexcept;
    std::optional<unsigned> block_numa_node(unsigned block) const noexcept;
    bool region_uses_hugetlb(std::size_t region) const noexcept;
    std::optional<unsigned> block_index(const std::byte* address) const noexcept;

private:
    struct FreeRegion {
        unsigned first_block = 0;
        unsigned end_block = 0; // exclusive
        std::optional<unsigned> numa_node;
        std::vector<unsigned> free;
        std::byte* base = nullptr;
        Size bytes = 0;
        bool mapped = false;
        bool mlocked = false;
        bool hugetlb = false;
    };

    BlockPool(Size block_bytes, Size num_blocks, bool locked, std::vector<FreeRegion> regions,
              std::vector<std::byte*> block_ptrs);
    void destroy() noexcept;

    Size block_bytes_ = 0;
    Size num_blocks_ = 0;
    Size free_blocks_ = 0;
    bool locked_ = false;
    std::vector<FreeRegion> regions_;
    std::vector<std::byte*> block_ptrs_;
};

// Allocator facade over the block pool (ADR-0008): hands out sub-block buffers via buddy
// allocation inside blocks, pulling a fresh block when arenas are full and returning a block to
// the pool when its arena empties. This is the RAM the head cache lives in.
class BufferPool {
public:
    static Result<BufferPool> create(Size total_bytes, Size block_bytes, Size min_alloc,
                                     bool lock_memory, bool try_hugetlb = true,
                                     Size hugetlb_bytes = 0);
    static Result<BufferPool> create_regions(std::span<const BlockPoolRegion> regions,
                                             Size block_bytes, Size min_alloc, bool lock_memory,
                                             bool try_hugetlb = true, Size hugetlb_bytes = 0);

    struct Region {
        unsigned block = 0;       // BlockPool block index
        std::uint32_t offset = 0; // buddy offset within the block
        std::uint32_t len = 0;    // requested length
        std::byte* data = nullptr;
    };

    // `min_alloc` selects the block class (buddy min-order); a block is typed on first use and only
    // reused by allocations of the same class. nullopt if no room or bytes > block.
    std::optional<Region> allocate(std::uint32_t bytes, Size min_alloc);
    void deallocate(unsigned block, std::uint32_t offset, std::uint32_t bytes);
    std::byte* addr(unsigned block, std::uint32_t offset) const noexcept;

    Size block_bytes() const noexcept { return blocks_.block_bytes(); }
    Size free_blocks() const noexcept { return blocks_.free_blocks(); }
    std::size_t region_count() const noexcept { return blocks_.region_count(); }
    bool block_is_local(unsigned block) const noexcept;
    std::optional<std::size_t> block_region(unsigned block) const noexcept {
        return blocks_.block_region(block);
    }
    std::optional<unsigned> block_numa_node(unsigned block) const noexcept {
        return blocks_.block_numa_node(block);
    }
    bool region_uses_hugetlb(std::size_t region) const noexcept {
        return blocks_.region_uses_hugetlb(region);
    }

    // NUMA promotion uses only completely occupied buddy blocks. indexed_bytes lets TierManager
    // prove that every allocation belongs to a published index entry (no in-flight StoreHandle or
    // orphaned pinned head). Arena blocks are deliberately ineligible for now.
    std::optional<Size> buddy_allocation_bytes(unsigned block, Size bytes) const noexcept;
    bool full_buddy_block(unsigned block, Size indexed_bytes) const noexcept;
    bool swap_blocks(unsigned first, unsigned second);

    // Compaction support (ADR-0008 Phase 2): the small-class bump arena backing a block (nullptr if the
    // block is large/buddy or unused), and the total reclaimable-by-compaction dead bytes across small
    // arenas -- the admission path compacts before evicting when this covers the head it needs.
    ArenaAllocator* small_arena(unsigned block) noexcept {
        return block < arenas_.size() ? std::get_if<ArenaAllocator>(&arenas_[block]) : nullptr;
    }
    Size small_dead_total() const noexcept {
        Size total = 0;
        for (const auto& a : arenas_)
            if (const auto* ar = std::get_if<ArenaAllocator>(&a)) total += ar->dead();
        return total;
    }

private:
    explicit BufferPool(BlockPool blocks)
        : blocks_(std::move(blocks)), arenas_(blocks_.num_blocks()) {}
    BlockPool blocks_;
    // One sub-allocator per block index, chosen on first use: large (>= kDeviceBlock) heads use buddy
    // (power-of-two, O_DIRECT-aligned); small heads use a byte-granular bump arena (compactable, ADR-0008).
    std::vector<std::variant<std::monostate, BuddyAllocator, ArenaAllocator>> arenas_;
};

// Small pool of fixed-size chunk buffers for streaming I/O (ADR-0017) — distinct from the head
// pool. Transient: acquire a chunk for a transfer step, release it when sent. Thin wrapper over a
// BlockPool whose "block" is one I/O chunk.
class IoBufferPool {
public:
    static Result<IoBufferPool> create(Size chunk_bytes, unsigned count, bool lock_memory,
                                       bool try_hugetlb = true, Size hugetlb_bytes = 0);

    std::optional<MutBytes> acquire(); // one chunk, or nullopt if all are in use
    void release(MutBytes chunk);

    Size chunk_bytes() const noexcept { return blocks_.block_bytes(); }
    Size free_count() const noexcept { return blocks_.free_blocks(); }

private:
    explicit IoBufferPool(BlockPool b) : blocks_(std::move(b)) {}
    BlockPool blocks_;
};

} // namespace goblin::core
