#include "goblin/core/buffer_pool.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <string>
#include <sys/mman.h>
#include <utility>

#if defined(__linux__)
#include <linux/mempolicy.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace goblin::core {

// ---------------- BuddyAllocator ----------------

BuddyAllocator::BuddyAllocator(Size arena_size, Size min_block)
    : arena_size_(arena_size), min_block_(min_block), max_order_(0) {
    unsigned o = 0;
    for (Size bs = min_block_; bs < arena_size_; bs <<= 1) ++o;
    max_order_ = o;
    free_.resize(max_order_ + 1);
    free_[max_order_].push_back(0); // the whole arena starts as one free max-order block
}

unsigned BuddyAllocator::order_for(Size bytes) const noexcept {
    const Size need = bytes < min_block_ ? min_block_ : bytes;
    unsigned o = 0;
    for (Size bs = min_block_; bs < need; bs <<= 1) ++o;
    return o;
}

std::optional<Offset> BuddyAllocator::allocate(Size bytes) {
    if (bytes == 0 || bytes > arena_size_) return std::nullopt;
    const unsigned target = order_for(bytes);
    if (target > max_order_) return std::nullopt;

    unsigned o = target;
    while (o <= max_order_ && free_[o].empty()) ++o;
    if (o > max_order_) return std::nullopt; // no free block large enough

    Offset off = free_[o].back();
    free_[o].pop_back();
    while (o > target) { // split down, freeing each upper buddy
        --o;
        free_[o].push_back(off + block_size(o));
    }
    used_ += block_size(target);
    return off;
}

void BuddyAllocator::deallocate(Offset off, Size bytes) {
    const unsigned target = order_for(bytes);
    used_ -= block_size(target);

    unsigned order = target;
    Offset cur = off;
    while (order < max_order_) {
        const Offset buddy = cur ^ block_size(order);
        auto& fl = free_[order];
        const auto it = std::find(fl.begin(), fl.end(), buddy);
        if (it == fl.end()) break; // buddy not free -> stop coalescing
        fl.erase(it);
        cur = (cur < buddy) ? cur : buddy;
        ++order;
    }
    free_[order].push_back(cur);
}

// ---------------- ArenaAllocator ----------------

std::optional<Offset> ArenaAllocator::allocate(Size bytes) {
    if (bytes == 0) return std::nullopt;
    const Size a = round(bytes);
    if (a > arena_size_ - frontier_) return std::nullopt; // no room below the frontier (no overflow)
    const Offset off = static_cast<Offset>(frontier_);
    frontier_ += a;
    live_ += a;
    return off;
}

void ArenaAllocator::deallocate(Offset /*off*/, Size bytes) {
    // Bump arenas don't reuse individual slots; just drop the live count. When it hits 0 the block is
    // fully dead and the BufferPool returns it to the pool; short of that, compaction reclaims the holes.
    live_ -= round(bytes);
}

// ---------------- BlockPool ----------------

BlockPool::BlockPool(std::byte* base, Size block_bytes, Size num_blocks, bool locked, bool mapped,
                     Size mapped_bytes, std::vector<FreeRegion> regions)
    : base_(base), block_bytes_(block_bytes), num_blocks_(num_blocks), free_blocks_(num_blocks),
      locked_(locked), mapped_(mapped), mapped_bytes_(mapped_bytes), regions_(std::move(regions)) {}

Result<BlockPool> BlockPool::create(Size block_bytes, Size num_blocks, bool lock_memory) {
    if (!is_power_of_two(block_bytes) || block_bytes < kDeviceBlock)
        return err(Errc::invalid_argument, "block_bytes must be a power of two >= 4 KiB");
    if (num_blocks == 0) return err(Errc::invalid_argument, "num_blocks must be >= 1");
    if (num_blocks > std::numeric_limits<unsigned>::max())
        return err(Errc::invalid_argument, "num_blocks exceeds the block-index limit");
    if (block_bytes > std::numeric_limits<Size>::max() / num_blocks)
        return err(Errc::invalid_argument, "block-pool size overflows Size");
    const BlockPoolRegion region{block_bytes * num_blocks, std::nullopt};
    return create_regions(block_bytes, std::span<const BlockPoolRegion>(&region, 1), lock_memory);
}

Result<BlockPool> BlockPool::create_regions(Size block_bytes,
                                             std::span<const BlockPoolRegion> requested_regions,
                                             bool lock_memory) {
    if (!is_power_of_two(block_bytes) || block_bytes < kDeviceBlock)
        return err(Errc::invalid_argument, "block_bytes must be a power of two >= 4 KiB");
    if (requested_regions.empty())
        return err(Errc::invalid_argument, "block pool needs at least one memory region");

    Size total = 0;
    Size num_blocks = 0;
    bool numa_bound = false;
    std::vector<FreeRegion> free_regions;
    free_regions.reserve(requested_regions.size());
    for (const auto& requested : requested_regions) {
        if (requested.bytes == 0 || requested.bytes % block_bytes != 0)
            return err(Errc::invalid_argument,
                       "each block-pool region must be a nonzero multiple of block_bytes");
        if (requested.bytes > std::numeric_limits<Size>::max() - total)
            return err(Errc::invalid_argument, "block-pool size overflows Size");
        const Size count = requested.bytes / block_bytes;
        if (count > std::numeric_limits<unsigned>::max() - num_blocks)
            return err(Errc::invalid_argument, "num_blocks exceeds the block-index limit");

        FreeRegion region;
        region.first_block = static_cast<unsigned>(num_blocks);
        num_blocks += count;
        region.end_block = static_cast<unsigned>(num_blocks);
        region.numa_node = requested.numa_node;
        region.free.reserve(static_cast<std::size_t>(count));
        for (unsigned i = region.end_block; i-- > region.first_block;) region.free.push_back(i);
        free_regions.push_back(std::move(region));
        total += requested.bytes;
        numa_bound = numa_bound || requested.numa_node.has_value();
    }
    if (total > std::numeric_limits<std::size_t>::max())
        return err(Errc::invalid_argument, "block pool exceeds the addressable size");

    void* p = nullptr;
    bool mapped = false;
    if (numa_bound) {
#if defined(__linux__)
        const long page_size = ::sysconf(_SC_PAGESIZE);
        if (page_size <= 0)
            return err(Errc::io_error, "sysconf(_SC_PAGESIZE) failed");
        const Size page = static_cast<Size>(page_size);
        for (const auto& requested : requested_regions)
            if (requested.bytes % page != 0)
                return err(Errc::invalid_argument,
                           "each NUMA memory region must be a multiple of the system page size");

        p = ::mmap(nullptr, static_cast<std::size_t>(total), PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED)
            return err(Errc::out_of_memory,
                       std::string("mmap NUMA head arena: ") + std::strerror(errno));
        mapped = true;

        auto* cursor = static_cast<std::byte*>(p);
        constexpr Size bits_per_word = sizeof(unsigned long) * 8;
        for (const auto& requested : requested_regions) {
            if (requested.numa_node) {
                const unsigned node = *requested.numa_node;
                const Size words = static_cast<Size>(node) / bits_per_word + 1;
                std::vector<unsigned long> mask(static_cast<std::size_t>(words), 0);
                mask[node / bits_per_word] |= 1UL << (node % bits_per_word);
                const unsigned long maxnode = static_cast<unsigned long>(node) + 1UL;
                if (::syscall(SYS_mbind, cursor, static_cast<unsigned long>(requested.bytes),
                              MPOL_BIND | MPOL_F_STATIC_NODES, mask.data(), maxnode, 0UL) != 0) {
                    const std::string detail = "mbind NUMA node " + std::to_string(node) + ": " +
                                               std::strerror(errno);
                    ::munmap(p, static_cast<std::size_t>(total));
                    return err(Errc::out_of_memory, detail);
                }
            }
            cursor += requested.bytes;
        }
#else
        return err(Errc::unsupported, "NUMA memory binding is supported only on Linux");
#endif
    } else {
        p = std::aligned_alloc(kDeviceBlock, static_cast<std::size_t>(total));
        if (!p) return err(Errc::out_of_memory, "aligned_alloc failed");
    }

    bool locked = false;
    if (lock_memory) {
        if (::mlock(p, static_cast<std::size_t>(total)) != 0) {
            const std::string detail =
                std::string("mlock head arena: ") + std::strerror(errno) +
                " (need sufficient per-node RAM and RLIMIT_MEMLOCK/CAP_IPC_LOCK)";
            if (mapped)
                ::munmap(p, static_cast<std::size_t>(total));
            else
                std::free(p);
            return err(Errc::out_of_memory, detail);
        }
        locked = true;
    }
    return BlockPool(static_cast<std::byte*>(p), block_bytes, num_blocks, locked, mapped, total,
                     std::move(free_regions));
}

void BlockPool::destroy() noexcept {
    if (base_) {
        if (locked_) ::munlock(base_, static_cast<std::size_t>(mapped_bytes_));
        if (mapped_)
            ::munmap(base_, static_cast<std::size_t>(mapped_bytes_));
        else
            std::free(base_);
        base_ = nullptr;
    }
}

BlockPool::~BlockPool() { destroy(); }

BlockPool::BlockPool(BlockPool&& o) noexcept
    : base_(o.base_), block_bytes_(o.block_bytes_), num_blocks_(o.num_blocks_),
      free_blocks_(o.free_blocks_), locked_(o.locked_), mapped_(o.mapped_),
      mapped_bytes_(o.mapped_bytes_), regions_(std::move(o.regions_)) {
    o.base_ = nullptr;
}

BlockPool& BlockPool::operator=(BlockPool&& o) noexcept {
    if (this != &o) {
        destroy();
        base_ = o.base_;
        block_bytes_ = o.block_bytes_;
        num_blocks_ = o.num_blocks_;
        free_blocks_ = o.free_blocks_;
        locked_ = o.locked_;
        mapped_ = o.mapped_;
        mapped_bytes_ = o.mapped_bytes_;
        regions_ = std::move(o.regions_);
        o.base_ = nullptr;
    }
    return *this;
}

std::optional<unsigned> BlockPool::acquire() {
    for (std::size_t region = 0; region < regions_.size(); ++region)
        if (auto block = acquire_from_region(region)) return block;
    return std::nullopt;
}

std::optional<unsigned> BlockPool::acquire_from_region(std::size_t region) {
    if (region >= regions_.size() || regions_[region].free.empty()) return std::nullopt;
    const unsigned idx = regions_[region].free.back();
    regions_[region].free.pop_back();
    --free_blocks_;
    return idx;
}

void BlockPool::release(unsigned index) {
    for (auto& region : regions_) {
        if (index < region.first_block || index >= region.end_block) continue;
        region.free.push_back(index);
        ++free_blocks_;
        return;
    }
}

std::byte* BlockPool::block_data(unsigned index) const noexcept {
    return base_ + static_cast<Size>(index) * block_bytes_;
}

unsigned BlockPool::region_first_block(std::size_t region) const noexcept {
    return region < regions_.size() ? regions_[region].first_block : 0;
}

unsigned BlockPool::region_end_block(std::size_t region) const noexcept {
    return region < regions_.size() ? regions_[region].end_block : 0;
}

std::optional<unsigned> BlockPool::region_numa_node(std::size_t region) const noexcept {
    return region < regions_.size() ? regions_[region].numa_node : std::nullopt;
}

// ---------------- BufferPool ----------------

Result<BufferPool> BufferPool::create(Size total_bytes, Size block_bytes, Size min_alloc,
                                      bool lock_memory) {
    const BlockPoolRegion region{total_bytes, std::nullopt};
    return create_regions(std::span<const BlockPoolRegion>(&region, 1), block_bytes, min_alloc,
                          lock_memory);
}

Result<BufferPool> BufferPool::create_regions(std::span<const BlockPoolRegion> regions,
                                              Size block_bytes, Size min_alloc,
                                              bool lock_memory) {
    if (!is_power_of_two(min_alloc) || min_alloc < kDeviceBlock)
        return err(Errc::invalid_argument, "min_alloc must be a power of two >= 4 KiB");
    if (block_bytes < min_alloc)
        return err(Errc::invalid_argument, "block_bytes must be >= min_alloc");

    auto bp = BlockPool::create_regions(block_bytes, regions, lock_memory);
    if (!bp) return std::unexpected(bp.error());
    return BufferPool(std::move(*bp), min_alloc);
}

std::optional<BufferPool::Region> BufferPool::allocate(std::uint32_t bytes, Size min_alloc) {
    if (bytes == 0 || static_cast<Size>(bytes) > blocks_.block_bytes()) return std::nullopt;
    const bool small = min_alloc < kDeviceBlock; // small heads never DMA -> byte-granular bump arena;
                                                 // large heads stay buddy (power-of-two, O_DIRECT-aligned)

    // Search one NUMA region at a time. Existing foreign arenas must not steal an allocation while
    // the local region still has either compatible arena space or an unused block.
    for (std::size_t region = 0; region < blocks_.region_count(); ++region) {
        for (unsigned b = blocks_.region_first_block(region);
             b < blocks_.region_end_block(region); ++b) {
            std::optional<Offset> off;
            if (small) {
                if (auto* ar = std::get_if<ArenaAllocator>(&arenas_[b]);
                    ar && ar->align() == min_alloc)
                    off = ar->allocate(bytes);
            } else {
                if (auto* bd = std::get_if<BuddyAllocator>(&arenas_[b]);
                    bd && bd->min_block() == min_alloc)
                    off = bd->allocate(bytes);
            }
            if (off) {
                const auto o = static_cast<std::uint32_t>(*off);
                return Region{b, o, bytes, addr(b, o)};
            }
        }

        const auto blk = blocks_.acquire_from_region(region);
        if (!blk) continue;
        std::optional<Offset> off;
        if (small)
            off = arenas_[*blk]
                      .emplace<ArenaAllocator>(blocks_.block_bytes(), min_alloc)
                      .allocate(bytes);
        else
            off = arenas_[*blk]
                      .emplace<BuddyAllocator>(blocks_.block_bytes(), min_alloc)
                      .allocate(bytes);
        if (!off) {
            arenas_[*blk] = std::monostate{};
            blocks_.release(*blk);
            return std::nullopt;
        }
        const auto o = static_cast<std::uint32_t>(*off);
        return Region{*blk, o, bytes, addr(*blk, o)};
    }
    return std::nullopt; // every local and foreign region is exhausted for this allocation class
}

void BufferPool::deallocate(unsigned block, std::uint32_t offset, std::uint32_t bytes) {
    if (block >= arenas_.size()) return;
    bool empty = false;
    if (auto* ar = std::get_if<ArenaAllocator>(&arenas_[block])) {
        ar->deallocate(offset, bytes);
        empty = ar->used() == 0;
    } else if (auto* bd = std::get_if<BuddyAllocator>(&arenas_[block])) {
        bd->deallocate(offset, bytes);
        empty = bd->used() == 0;
    } else {
        return; // monostate: block not in use
    }
    if (empty) { // fully dead -> hand the block back to the pool
        arenas_[block] = std::monostate{};
        blocks_.release(block);
    }
}

std::byte* BufferPool::addr(unsigned block, std::uint32_t offset) const noexcept {
    return blocks_.block_data(block) + offset;
}

// ---------------- IoBufferPool ----------------

Result<IoBufferPool> IoBufferPool::create(Size chunk_bytes, unsigned count, bool lock_memory) {
    auto bp = BlockPool::create(chunk_bytes, count, lock_memory);
    if (!bp) return std::unexpected(bp.error());
    return IoBufferPool(std::move(*bp));
}

std::optional<MutBytes> IoBufferPool::acquire() {
    const auto idx = blocks_.acquire();
    if (!idx) return std::nullopt;
    return MutBytes(blocks_.block_data(*idx), static_cast<std::size_t>(blocks_.block_bytes()));
}

void IoBufferPool::release(MutBytes chunk) {
    const auto off = static_cast<std::size_t>(chunk.data() - blocks_.data());
    blocks_.release(static_cast<unsigned>(off / blocks_.block_bytes()));
}

} // namespace goblin::core
