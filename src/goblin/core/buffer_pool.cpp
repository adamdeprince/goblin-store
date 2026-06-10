#include "goblin/core/buffer_pool.hpp"

#include <algorithm>
#include <cstdlib>
#include <sys/mman.h>
#include <utility>

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

// ---------------- BlockPool ----------------

BlockPool::BlockPool(std::byte* base, Size block_bytes, Size num_blocks, bool locked)
    : base_(base), block_bytes_(block_bytes), num_blocks_(num_blocks), locked_(locked) {
    free_.reserve(num_blocks_);
    for (Size i = num_blocks_; i-- > 0;) free_.push_back(static_cast<unsigned>(i));
}

Result<BlockPool> BlockPool::create(Size block_bytes, Size num_blocks, bool lock_memory) {
    if (!is_power_of_two(block_bytes) || block_bytes < kDeviceBlock)
        return err(Errc::invalid_argument, "block_bytes must be a power of two >= 4 KiB");
    if (num_blocks == 0) return err(Errc::invalid_argument, "num_blocks must be >= 1");

    const Size total = block_bytes * num_blocks;
    void* p = std::aligned_alloc(kDeviceBlock, total); // total is a multiple of kDeviceBlock
    if (!p) return err(Errc::out_of_memory, "aligned_alloc failed");

    bool locked = false;
    if (lock_memory) {
        if (::mlock(p, total) != 0) {
            std::free(p);
            return err(Errc::out_of_memory, "mlock failed (raise RLIMIT_MEMLOCK or grant CAP_IPC_LOCK)");
        }
        locked = true;
    }
    return BlockPool(static_cast<std::byte*>(p), block_bytes, num_blocks, locked);
}

void BlockPool::destroy() noexcept {
    if (base_) {
        if (locked_) ::munlock(base_, block_bytes_ * num_blocks_);
        std::free(base_);
        base_ = nullptr;
    }
}

BlockPool::~BlockPool() { destroy(); }

BlockPool::BlockPool(BlockPool&& o) noexcept
    : base_(o.base_), block_bytes_(o.block_bytes_), num_blocks_(o.num_blocks_), locked_(o.locked_),
      free_(std::move(o.free_)) {
    o.base_ = nullptr;
}

BlockPool& BlockPool::operator=(BlockPool&& o) noexcept {
    if (this != &o) {
        destroy();
        base_ = o.base_;
        block_bytes_ = o.block_bytes_;
        num_blocks_ = o.num_blocks_;
        locked_ = o.locked_;
        free_ = std::move(o.free_);
        o.base_ = nullptr;
    }
    return *this;
}

std::optional<unsigned> BlockPool::acquire() {
    if (free_.empty()) return std::nullopt;
    const unsigned idx = free_.back();
    free_.pop_back();
    return idx;
}

void BlockPool::release(unsigned index) { free_.push_back(index); }

std::byte* BlockPool::block_data(unsigned index) const noexcept {
    return base_ + static_cast<Size>(index) * block_bytes_;
}

// ---------------- BufferPool ----------------

Result<BufferPool> BufferPool::create(Size total_bytes, Size block_bytes, Size min_alloc,
                                      bool lock_memory) {
    if (!is_power_of_two(min_alloc) || min_alloc < kDeviceBlock)
        return err(Errc::invalid_argument, "min_alloc must be a power of two >= 4 KiB");
    if (block_bytes < min_alloc)
        return err(Errc::invalid_argument, "block_bytes must be >= min_alloc");
    const Size num_blocks = total_bytes / block_bytes;
    if (num_blocks == 0) return err(Errc::invalid_argument, "total_bytes < block_bytes");

    auto bp = BlockPool::create(block_bytes, num_blocks, lock_memory);
    if (!bp) return std::unexpected(bp.error());
    return BufferPool(std::move(*bp), min_alloc);
}

std::optional<BufferPool::Region> BufferPool::allocate(std::uint32_t bytes) {
    if (bytes == 0 || static_cast<Size>(bytes) > blocks_.block_bytes()) return std::nullopt;

    for (unsigned b = 0; b < arenas_.size(); ++b) {
        if (!arenas_[b]) continue;
        if (const auto off = arenas_[b]->allocate(bytes)) {
            const auto o = static_cast<std::uint32_t>(*off);
            return Region{b, o, bytes, addr(b, o)};
        }
    }
    const auto blk = blocks_.acquire();
    if (!blk) return std::nullopt; // RAM full
    arenas_[*blk].emplace(blocks_.block_bytes(), min_alloc_);
    const auto off = arenas_[*blk]->allocate(bytes); // fresh arena, bytes <= block -> succeeds
    const auto o = static_cast<std::uint32_t>(*off);
    return Region{*blk, o, bytes, addr(*blk, o)};
}

void BufferPool::deallocate(unsigned block, std::uint32_t offset, std::uint32_t bytes) {
    if (block >= arenas_.size() || !arenas_[block]) return;
    arenas_[block]->deallocate(offset, bytes);
    if (arenas_[block]->used() == 0) {
        arenas_[block].reset();
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
