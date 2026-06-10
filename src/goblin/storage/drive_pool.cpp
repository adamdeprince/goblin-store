#include "goblin/storage/drive_pool.hpp"

#include <algorithm>

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

std::vector<ReadSegment> DrivePool::plan_reads(std::uint64_t key_hash, Offset off, Size len) const {
    // One segment per chunk, in logical order. Each is contiguous in BOTH file and logical space,
    // so a caller gathers them into a contiguous buffer by accumulating lengths. We deliberately
    // do NOT coalesce same-drive chunks: for N>1 a drive's chunks (c, c+N, ...) are *strided* in
    // the logical buffer, so a flat coalesced read can't gather contiguously. Coalescing a drive's
    // chunks into one readv/writev (scatter/gather) is a later optimization (ADR-0009).
    std::vector<ReadSegment> segs;
    Offset cur = off;
    Size remaining = len;
    while (remaining > 0) {
        const Size intra = cur % s_;
        const Size seg_len = std::min<Size>(remaining, s_ - intra);
        segs.push_back({drive_of(key_hash, cur), file_offset_of(cur), seg_len});
        cur += seg_len;
        remaining -= seg_len;
    }
    return segs;
}

} // namespace goblin::storage
