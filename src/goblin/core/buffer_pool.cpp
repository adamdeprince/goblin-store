#include "goblin/core/buffer_pool.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <utility>

#if defined(__linux__)
#include <linux/mempolicy.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace goblin::core {

namespace {

#if defined(__linux__)

Status bind_mapping(void* base, Size bytes, unsigned node, unsigned flags = 0) {
    constexpr Size bits_per_word = sizeof(unsigned long) * 8;
    const Size words = static_cast<Size>(node) / bits_per_word + 1;
    std::vector<unsigned long> mask(static_cast<std::size_t>(words), 0);
    mask[node / bits_per_word] |= 1UL << (node % bits_per_word);
    const unsigned long maxnode = static_cast<unsigned long>(words * bits_per_word);
    if (::syscall(SYS_mbind, base, static_cast<unsigned long>(bytes),
                  MPOL_BIND | MPOL_F_STATIC_NODES, mask.data(), maxnode, flags) != 0)
        return err(Errc::out_of_memory,
                   "mbind NUMA node " + std::to_string(node) + ": " + std::strerror(errno));
    return {};
}

bool hugetlb_mapping_is_resident(void* base, Size bytes, Size hugepage_bytes) {
    for (Size offset = 0; offset < bytes; offset += hugepage_bytes) {
        unsigned char resident = 0;
        if (::mincore(static_cast<std::byte*>(base) + offset, 1, &resident) != 0 ||
            (resident & 1u) == 0)
            return false;
    }
    return true;
}

void* try_hugetlb_mapping(Size bytes, Size hugepage_bytes, std::optional<unsigned> node) {
    if (bytes == 0 || hugepage_bytes < kDeviceBlock || !is_power_of_two(hugepage_bytes) ||
        bytes % hugepage_bytes != 0)
        return MAP_FAILED;

    void* mapping = MAP_FAILED;
    const auto map = [&]() noexcept {
        try {
            if (node) {
                constexpr Size bits_per_word = sizeof(unsigned long) * 8;
                const Size words = static_cast<Size>(*node) / bits_per_word + 1;
                std::vector<unsigned long> mask(static_cast<std::size_t>(words), 0);
                mask[*node / bits_per_word] |= 1UL << (*node % bits_per_word);
                const unsigned long maxnode = static_cast<unsigned long>(words * bits_per_word);
                if (::syscall(SYS_set_mempolicy, MPOL_BIND | MPOL_F_STATIC_NODES, mask.data(),
                              maxnode) != 0)
                    return;
            }
            const unsigned huge_shift = std::countr_zero(hugepage_bytes);
            const int huge_flags = static_cast<int>(huge_shift << MAP_HUGE_SHIFT);
            mapping = ::mmap(nullptr, static_cast<std::size_t>(bytes), PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE | huge_flags,
                             -1, 0);
        } catch (...) {
            mapping = MAP_FAILED;
        }
    };

    if (node) {
        // set_mempolicy is per-thread. A short-lived mapper thread can select the target node for
        // MAP_POPULATE without disturbing the serving thread's inherited local-memory policy.
        try {
            std::thread mapper(map);
            mapper.join();
        } catch (...) {
            return MAP_FAILED;
        }
    } else {
        map();
    }
    if (mapping == MAP_FAILED) return MAP_FAILED;

    // MAP_POPULATE deliberately hides population failures. mincore keeps an undersized per-node
    // hugetlb pool from turning into a later SIGBUS; MPOL_MF_STRICT also verifies NUMA placement.
    bool usable = false;
    try {
        usable = hugetlb_mapping_is_resident(mapping, bytes, hugepage_bytes) &&
                 (!node || static_cast<bool>(bind_mapping(mapping, bytes, *node, MPOL_MF_STRICT)));
    } catch (...) {
        // The mapping is not published into a FreeRegion yet, so clean it up locally before the
        // ordinary-memory fallback handles an allocation failure in validation metadata.
        ::munmap(mapping, static_cast<std::size_t>(bytes));
        return MAP_FAILED;
    }
    if (!usable) {
        ::munmap(mapping, static_cast<std::size_t>(bytes));
        return MAP_FAILED;
    }
    return mapping;
}

#endif

} // namespace

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

std::size_t BuddyAllocator::free_blocks(Size bytes) const noexcept {
    if (bytes < min_block_ || bytes > arena_size_ || !is_power_of_two(bytes) ||
        bytes % min_block_ != 0)
        return 0;
    const unsigned order = order_for(bytes);
    return order <= max_order_ && block_size(order) == bytes ? free_[order].size() : 0;
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
    requested_ += bytes;
    return off;
}

void BuddyAllocator::deallocate(Offset off, Size bytes) {
    const unsigned target = order_for(bytes);
    used_ -= block_size(target);
    requested_ -= bytes;

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
    requested_ += bytes;
    return off;
}

void ArenaAllocator::deallocate(Offset /*off*/, Size bytes) {
    // Bump arenas don't reuse individual slots; just drop the live count. When it hits 0 the block is
    // fully dead and the BufferPool returns it to the pool; short of that, compaction reclaims the holes.
    live_ -= round(bytes);
    requested_ -= bytes;
}

// ---------------- BlockPool ----------------

BlockPool::BlockPool(Size block_bytes, Size num_blocks, bool locked,
                     std::vector<FreeRegion> regions, std::vector<std::byte*> block_ptrs)
    : block_bytes_(block_bytes), num_blocks_(num_blocks), free_blocks_(num_blocks), locked_(locked),
      regions_(std::move(regions)), block_ptrs_(std::move(block_ptrs)) {}

Result<BlockPool> BlockPool::create(Size block_bytes, Size num_blocks, bool lock_memory,
                                    bool try_hugetlb, Size hugetlb_bytes) {
    if (!is_power_of_two(block_bytes) || block_bytes < kDeviceBlock)
        return err(Errc::invalid_argument, "block_bytes must be a power of two >= 4 KiB");
    if (num_blocks == 0) return err(Errc::invalid_argument, "num_blocks must be >= 1");
    if (num_blocks > std::numeric_limits<unsigned>::max())
        return err(Errc::invalid_argument, "num_blocks exceeds the block-index limit");
    if (block_bytes > std::numeric_limits<Size>::max() / num_blocks)
        return err(Errc::invalid_argument, "block-pool size overflows Size");
    const BlockPoolRegion region{block_bytes * num_blocks, std::nullopt};
    return create_regions(block_bytes, std::span<const BlockPoolRegion>(&region, 1), lock_memory,
                          try_hugetlb, hugetlb_bytes);
}

Result<BlockPool> BlockPool::create_regions(Size block_bytes,
                                             std::span<const BlockPoolRegion> requested_regions,
                                             bool lock_memory, bool try_hugetlb,
                                             Size hugetlb_bytes) {
    if (!is_power_of_two(block_bytes) || block_bytes < kDeviceBlock)
        return err(Errc::invalid_argument, "block_bytes must be a power of two >= 4 KiB");
    if (requested_regions.empty())
        return err(Errc::invalid_argument, "block pool needs at least one memory region");
    if (hugetlb_bytes == 0) hugetlb_bytes = block_bytes;

    Size num_blocks = 0;
    std::vector<FreeRegion> free_regions;
    free_regions.reserve(requested_regions.size());
    for (const auto& requested : requested_regions) {
        if (requested.bytes == 0 || requested.bytes % block_bytes != 0)
            return err(Errc::invalid_argument,
                       "each block-pool region must be a nonzero multiple of block_bytes");
        if (requested.bytes > std::numeric_limits<std::size_t>::max())
            return err(Errc::invalid_argument,
                       "a block-pool region exceeds this process's address-size limit");
        const Size count = requested.bytes / block_bytes;
        if (count > std::numeric_limits<unsigned>::max() - num_blocks)
            return err(Errc::invalid_argument, "num_blocks exceeds the block-index limit");

        FreeRegion region;
        region.first_block = static_cast<unsigned>(num_blocks);
        num_blocks += count;
        region.end_block = static_cast<unsigned>(num_blocks);
        region.numa_node = requested.numa_node;
        region.allocation_class = requested.allocation_class;
        region.bytes = requested.bytes;
        region.free.reserve(static_cast<std::size_t>(count));
        for (unsigned i = region.end_block; i-- > region.first_block;) region.free.push_back(i);
        free_regions.push_back(std::move(region));
    }

#if !defined(__linux__)
    (void)try_hugetlb;
    (void)hugetlb_bytes;
    for (const auto& requested : requested_regions)
        if (requested.numa_node)
            return err(Errc::unsupported, "NUMA memory binding is supported only on Linux");
#endif

    // Allocate all metadata before taking ownership of kernel mappings. This keeps an allocation
    // failure while building the address table from leaking already-created hugetlb/anonymous VMAs.
    std::vector<std::byte*> block_ptrs(static_cast<std::size_t>(num_blocks));

    struct MappingGuard {
        std::vector<FreeRegion>* regions;

        ~MappingGuard() {
            if (!regions) return;
            for (auto& region : *regions) {
                if (!region.base) continue;
                if (region.mlocked)
                    ::munlock(region.base, static_cast<std::size_t>(region.bytes));
                if (region.mapped)
                    ::munmap(region.base, static_cast<std::size_t>(region.bytes));
                else
                    std::free(region.base);
                region.base = nullptr;
            }
        }
        void dismiss() noexcept { regions = nullptr; }
    } mapping_guard{&free_regions};

    for (auto& region : free_regions) {
        void* p = nullptr;
#if defined(__linux__)
        if (try_hugetlb)
            p = try_hugetlb_mapping(region.bytes, hugetlb_bytes, region.numa_node);
        if (p != MAP_FAILED && p != nullptr) {
            region.base = static_cast<std::byte*>(p);
            region.mapped = true;
            region.hugetlb = true;
            continue; // explicit hugetlb pages are resident and cannot be swapped
        }

        if (region.numa_node) {
            p = ::mmap(nullptr, static_cast<std::size_t>(region.bytes), PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p == MAP_FAILED) {
                return err(Errc::out_of_memory,
                           std::string("mmap NUMA RAM arena: ") + std::strerror(errno));
            }
            region.base = static_cast<std::byte*>(p);
            region.mapped = true;
            if (auto bound = bind_mapping(p, region.bytes, *region.numa_node); !bound) {
                Error detail = std::move(bound.error());
                return std::unexpected(std::move(detail));
            }
        } else
#endif
        {
            p = std::aligned_alloc(kDeviceBlock, static_cast<std::size_t>(region.bytes));
            if (!p) {
                return err(Errc::out_of_memory, "aligned_alloc failed");
            }
            region.base = static_cast<std::byte*>(p);
        }

        if (lock_memory && ::mlock(region.base, static_cast<std::size_t>(region.bytes)) != 0) {
            const std::string detail = std::string("mlock RAM arena: ") + std::strerror(errno) +
                                       " (need sufficient per-node RAM and "
                                       "RLIMIT_MEMLOCK/CAP_IPC_LOCK)";
            return err(Errc::out_of_memory, detail);
        }
        region.mlocked = lock_memory;
    }

    for (const auto& region : free_regions)
        for (unsigned block = region.first_block; block < region.end_block; ++block)
            block_ptrs[block] =
                region.base + static_cast<Size>(block - region.first_block) * block_bytes;
    mapping_guard.dismiss();
    return BlockPool(block_bytes, num_blocks, lock_memory, std::move(free_regions),
                     std::move(block_ptrs));
}

void BlockPool::destroy() noexcept {
    for (auto& region : regions_) {
        if (!region.base) continue;
        if (region.mlocked) ::munlock(region.base, static_cast<std::size_t>(region.bytes));
        if (region.mapped)
            ::munmap(region.base, static_cast<std::size_t>(region.bytes));
        else
            std::free(region.base);
        region.base = nullptr;
    }
    block_ptrs_.clear();
}

BlockPool::~BlockPool() { destroy(); }

BlockPool::BlockPool(BlockPool&& o) noexcept
    : block_bytes_(o.block_bytes_), num_blocks_(o.num_blocks_), free_blocks_(o.free_blocks_),
      locked_(o.locked_), regions_(std::move(o.regions_)), block_ptrs_(std::move(o.block_ptrs_)) {
    o.regions_.clear();
    o.block_ptrs_.clear();
    o.num_blocks_ = o.free_blocks_ = 0;
}

BlockPool& BlockPool::operator=(BlockPool&& o) noexcept {
    if (this != &o) {
        destroy();
        block_bytes_ = o.block_bytes_;
        num_blocks_ = o.num_blocks_;
        free_blocks_ = o.free_blocks_;
        locked_ = o.locked_;
        regions_ = std::move(o.regions_);
        block_ptrs_ = std::move(o.block_ptrs_);
        o.regions_.clear();
        o.block_ptrs_.clear();
        o.num_blocks_ = o.free_blocks_ = 0;
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
    return index < block_ptrs_.size() ? block_ptrs_[index] : nullptr;
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

BufferPoolClass BlockPool::region_allocation_class(std::size_t region) const noexcept {
    return region < regions_.size() ? regions_[region].allocation_class
                                    : BufferPoolClass::shared;
}

std::optional<std::size_t> BlockPool::block_region(unsigned block) const noexcept {
    for (std::size_t region = 0; region < regions_.size(); ++region)
        if (block >= regions_[region].first_block && block < regions_[region].end_block)
            return region;
    return std::nullopt;
}

std::optional<unsigned> BlockPool::block_numa_node(unsigned block) const noexcept {
    const auto region = block_region(block);
    return region ? regions_[*region].numa_node : std::nullopt;
}

bool BlockPool::region_uses_hugetlb(std::size_t region) const noexcept {
    return region < regions_.size() && regions_[region].hugetlb;
}

std::optional<unsigned> BlockPool::block_index(const std::byte* address) const noexcept {
    const auto needle = reinterpret_cast<std::uintptr_t>(address);
    for (const auto& region : regions_) {
        const auto begin = reinterpret_cast<std::uintptr_t>(region.base);
        const auto end = begin + static_cast<std::uintptr_t>(region.bytes);
        if (needle < begin || needle >= end) continue;
        return region.first_block + static_cast<unsigned>((needle - begin) / block_bytes_);
    }
    return std::nullopt;
}

// ---------------- BufferPool ----------------

Result<BufferPool> BufferPool::create(Size total_bytes, Size block_bytes, Size min_alloc,
                                      bool lock_memory, bool try_hugetlb, Size hugetlb_bytes) {
    const BlockPoolRegion region{total_bytes, std::nullopt};
    return create_regions(std::span<const BlockPoolRegion>(&region, 1), block_bytes, min_alloc,
                          lock_memory, try_hugetlb, hugetlb_bytes);
}

Result<BufferPool> BufferPool::create_regions(std::span<const BlockPoolRegion> regions,
                                              Size block_bytes, Size min_alloc,
                                              bool lock_memory, bool try_hugetlb,
                                              Size hugetlb_bytes) {
    if (!is_power_of_two(min_alloc) || min_alloc < kDeviceBlock)
        return err(Errc::invalid_argument, "min_alloc must be a power of two >= 4 KiB");
    if (block_bytes < min_alloc)
        return err(Errc::invalid_argument, "block_bytes must be >= min_alloc");

    auto bp = BlockPool::create_regions(block_bytes, regions, lock_memory, try_hugetlb,
                                        hugetlb_bytes);
    if (!bp) return std::unexpected(bp.error());
    return BufferPool(std::move(*bp));
}

std::optional<BufferPool::Region> BufferPool::allocate(std::uint32_t bytes, Size min_alloc,
                                                       BufferPoolClass allocation_class) {
    if (bytes == 0 || static_cast<Size>(bytes) > blocks_.block_bytes()) return std::nullopt;
    const bool small = min_alloc < kDeviceBlock; // small heads never DMA -> byte-granular bump arena;
                                                 // large heads stay buddy (power-of-two, O_DIRECT-aligned)

    // Search one NUMA region at a time. Existing foreign arenas must not steal an allocation while
    // the local region still has either compatible arena space or an unused block.
    for (std::size_t region = 0; region < blocks_.region_count(); ++region) {
        if (blocks_.region_allocation_class(region) != allocation_class) continue;
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

bool BufferPool::block_is_local(unsigned block) const noexcept {
    const auto region = blocks_.block_region(block);
    if (!region) return false;
    const auto allocation_class = blocks_.region_allocation_class(*region);
    for (std::size_t prior = 0; prior < *region; ++prior)
        if (blocks_.region_allocation_class(prior) == allocation_class) return false;
    return true;
}

std::optional<Size> BufferPool::buddy_allocation_bytes(unsigned block, Size bytes) const noexcept {
    if (block >= arenas_.size()) return std::nullopt;
    const auto* buddy = std::get_if<BuddyAllocator>(&arenas_[block]);
    if (!buddy) return std::nullopt;
    const Size footprint = buddy->allocation_bytes(bytes);
    if (footprint == 0) return std::nullopt;
    return footprint;
}

bool BufferPool::full_buddy_block(unsigned block, Size indexed_bytes) const noexcept {
    if (block >= arenas_.size()) return false;
    const auto* buddy = std::get_if<BuddyAllocator>(&arenas_[block]);
    return buddy && buddy->used() == buddy->capacity() && indexed_bytes == buddy->capacity();
}

bool BufferPool::swap_blocks(unsigned first, unsigned second) {
    if (first == second || first >= arenas_.size() || second >= arenas_.size()) return false;
    const auto first_region = blocks_.block_region(first);
    const auto second_region = blocks_.block_region(second);
    if (!first_region || !second_region ||
        blocks_.region_allocation_class(*first_region) !=
            blocks_.region_allocation_class(*second_region))
        return false;
    if (!std::holds_alternative<BuddyAllocator>(arenas_[first]) ||
        !std::holds_alternative<BuddyAllocator>(arenas_[second]))
        return false;
    std::swap_ranges(addr(first, 0), addr(first, 0) + blocks_.block_bytes(), addr(second, 0));
    std::swap(arenas_[first], arenas_[second]);
    return true;
}

std::vector<BufferPool::ClassUsage> BufferPool::usage() const {
    std::array<ClassUsage, 3> totals{};
    for (std::size_t i = 0; i < totals.size(); ++i)
        totals[i].allocation_class = static_cast<BufferPoolClass>(i);
    for (unsigned block = 0; block < arenas_.size(); ++block) {
        const auto region = blocks_.block_region(block);
        if (!region) continue;
        auto& total = totals[static_cast<std::size_t>(blocks_.region_allocation_class(*region))];
        total.capacity_bytes += blocks_.block_bytes();
        if (const auto* buddy = std::get_if<BuddyAllocator>(&arenas_[block])) {
            total.used_bytes += buddy->requested();
            total.fragmented_bytes += buddy->fragmented();
            total.free_bytes += buddy->capacity() - buddy->used();
        } else if (const auto* arena = std::get_if<ArenaAllocator>(&arenas_[block])) {
            total.used_bytes += arena->requested();
            total.fragmented_bytes += arena->frontier() - arena->requested();
            total.free_bytes += arena->capacity() - arena->frontier();
        } else {
            total.free_bytes += blocks_.block_bytes();
        }
    }
    std::vector<ClassUsage> out;
    for (const auto& total : totals)
        if (total.capacity_bytes != 0) out.push_back(total);
    return out;
}

std::vector<BufferPool::MappingUsage> BufferPool::mappings() const {
    std::vector<MappingUsage> out;
    out.reserve(blocks_.region_count());
    for (std::size_t region = 0; region < blocks_.region_count(); ++region) {
        const Size blocks = blocks_.region_end_block(region) - blocks_.region_first_block(region);
        out.push_back({region, blocks_.region_numa_node(region),
                       blocks_.region_allocation_class(region),
                       blocks * blocks_.block_bytes(), blocks_.region_uses_hugetlb(region)});
    }
    return out;
}

std::vector<BufferPool::BuddyFreeBlocks> BufferPool::buddy_free_blocks() const {
    constexpr std::size_t classes = 3;
    std::vector<Size> sizes;
    for (Size bytes = kDeviceBlock;; bytes <<= 1) {
        sizes.push_back(bytes);
        if (bytes == blocks_.block_bytes()) break;
    }
    std::array<std::vector<std::uint64_t>, classes> counts;
    std::array<bool, classes> present{};
    for (auto& values : counts) values.resize(sizes.size());

    for (unsigned block = 0; block < arenas_.size(); ++block) {
        const auto region = blocks_.block_region(block);
        if (!region) continue;
        const std::size_t allocation_class =
            static_cast<std::size_t>(blocks_.region_allocation_class(*region));
        present[allocation_class] = true;
        if (const auto* buddy = std::get_if<BuddyAllocator>(&arenas_[block])) {
            for (std::size_t i = 0; i < sizes.size(); ++i)
                counts[allocation_class][i] += buddy->free_blocks(sizes[i]);
        } else if (std::holds_alternative<std::monostate>(arenas_[block])) {
            ++counts[allocation_class].back();
        }
    }

    std::vector<BuddyFreeBlocks> out;
    for (std::size_t allocation_class = 0; allocation_class < classes; ++allocation_class) {
        if (!present[allocation_class]) continue;
        out.reserve(out.size() + sizes.size());
        for (std::size_t i = 0; i < sizes.size(); ++i)
            out.push_back({static_cast<BufferPoolClass>(allocation_class), sizes[i],
                           counts[allocation_class][i]});
    }
    return out;
}

// ---------------- IoBufferPool ----------------

Result<IoBufferPool> IoBufferPool::create(Size chunk_bytes, unsigned count, bool lock_memory,
                                          bool try_hugetlb, Size hugetlb_bytes) {
    auto bp = BlockPool::create(chunk_bytes, count, lock_memory, try_hugetlb, hugetlb_bytes);
    if (!bp) return std::unexpected(bp.error());
    return IoBufferPool(std::move(*bp));
}

std::optional<MutBytes> IoBufferPool::acquire() {
    const auto idx = blocks_.acquire();
    if (!idx) return std::nullopt;
    return MutBytes(blocks_.block_data(*idx), static_cast<std::size_t>(blocks_.block_bytes()));
}

void IoBufferPool::release(MutBytes chunk) {
    const auto block = blocks_.block_index(chunk.data());
    if (block && chunk.data() == blocks_.block_data(*block) &&
        chunk.size() == static_cast<std::size_t>(blocks_.block_bytes()))
        blocks_.release(*block);
}

} // namespace goblin::core
