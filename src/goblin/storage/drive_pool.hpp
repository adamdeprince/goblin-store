// RAID-0 striping of an object's tier-local bytes across all drives in a pool (ADR-0009).
// Goal: a single read fans across EVERY drive -> full aggregate bandwidth on one stream.
//
// Grid layout (pool of N drives, stripe unit S), for tier-local offset `off`:
//   chunk c = off / S,  row = c / N
//   drive       = (key_hash + c) % N      (consecutive chunks -> consecutive drives)
//   file_offset = row * S + (off % S)
#pragma once

#include "goblin/common/types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace goblin::storage {

// One contiguous read against one drive's per-object file.
struct ReadSegment {
    unsigned drive = 0;       // index in [0, num_drives)
    Offset file_offset = 0;   // offset within that drive's file for this object
    Size length = 0;
};

class DrivePool {
public:
    DrivePool(unsigned num_drives, Size stripe_unit) : n_(num_drives), s_(stripe_unit) {}

    unsigned num_drives() const noexcept { return n_; }
    Size stripe_unit() const noexcept { return s_; }
    Size row_bytes() const noexcept { return s_ * n_; } // N * S == one full stripe row

    // Drive holding tier-local offset `off` for object `key_hash`, and the in-file offset there.
    unsigned drive_of(std::uint64_t key_hash, Offset off) const noexcept;
    Offset file_offset_of(Offset off) const noexcept;

    // Number of constituent files an object of `tier_bytes` occupies: min(N, ceil(bytes/S)).
    unsigned files_used(Size tier_bytes) const noexcept;

    // Map tier-local range [off, off+len) to read segments: one per chunk, in logical order,
    // each contiguous in BOTH file and logical space (gather them by accumulating lengths).
    // A single row (N*S) -> one segment per drive (N-way parallel). Merging a drive's chunks into
    // one readv/writev for big sequential per-drive I/O is a later optimization — flat coalescing
    // is wrong for N>1 because a drive's chunks are strided in the logical buffer.
    std::vector<ReadSegment> plan_reads(std::uint64_t key_hash, Offset off, Size len) const;

private:
    unsigned n_;
    Size s_;
};

} // namespace goblin::storage
