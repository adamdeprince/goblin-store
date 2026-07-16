#include "goblin/storage/drive_pool.hpp"

#include <algorithm>
#include <span>

namespace goblin::storage {

unsigned DrivePool::drive_of(std::uint64_t key_hash, Offset off) const noexcept {
    const std::uint64_t c = off / s_; // chunk index
    return static_cast<unsigned>((key_hash + c) % n_);
}

Offset DrivePool::file_offset_of(Offset off) const noexcept {
    const Offset c = off / s_;
    const Offset row = c / n_;
    return row * s_ + (off % s_);
}

unsigned DrivePool::files_used(Size tier_bytes) const noexcept {
    if (tier_bytes == 0) return 0;
    const Size chunks = (tier_bytes + s_ - 1) / s_;
    return static_cast<unsigned>(chunks < n_ ? chunks : n_);
}

Size DrivePool::file_extent(std::uint64_t key_hash, Size tier_bytes,
                            unsigned drive) const noexcept {
    if (tier_bytes == 0 || drive >= n_ || n_ == 0 || s_ == 0) return 0;
    const Size full_chunks = tier_bytes / s_;
    const Size tail = tier_bytes % s_;
    const Size full_rows = full_chunks / n_;
    const unsigned remainder = static_cast<unsigned>(full_chunks % n_);
    const unsigned first = static_cast<unsigned>(key_hash % n_);
    const unsigned slot = (drive + n_ - first) % n_;

    Size extent = full_rows * s_;
    if (slot < remainder) extent += s_;
    if (tail != 0 && slot == remainder) extent += tail;
    return extent;
}

std::size_t DrivePool::plan_reads(std::uint64_t key_hash, Offset off, Size len,
                                  std::span<ReadSegment> out) const noexcept {
    // One segment per chunk, in logical order. Each is contiguous in BOTH file and logical space,
    // so a caller gathers them into a contiguous buffer by accumulating lengths. We deliberately
    // do NOT coalesce same-drive chunks: for N>1 a drive's chunks (c, c+N, ...) are *strided* in
    // the logical buffer, so a flat coalesced read can't gather contiguously. Coalescing a drive's
    // chunks into one readv/writev (scatter/gather) is a later optimization (ADR-0009).
    std::size_t n = 0;
    Offset cur = off;
    Size remaining = len;
    while (remaining > 0 && n < out.size()) {
        const Size intra = cur % s_;
        const Size seg_len = std::min<Size>(remaining, s_ - intra);
        out[n++] = {drive_of(key_hash, cur), file_offset_of(cur), seg_len};
        cur += seg_len;
        remaining -= seg_len;
    }
    return n;
}

std::vector<ReadSegment> DrivePool::plan_reads(std::uint64_t key_hash, Offset off, Size len) const {
    // Upper bound: one segment per stripe-unit boundary crossed, +1 for a partial start.
    const Size max_segs = s_ ? (len / s_) + 2 : 1;
    std::vector<ReadSegment> segs(static_cast<std::size_t>(max_segs));
    const std::size_t n = plan_reads(key_hash, off, len, std::span<ReadSegment>(segs.data(), segs.size()));
    segs.resize(n);
    return segs;
}

} // namespace goblin::storage
