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

    // Final byte extent of one constituent file after striping `tier_bytes`. This is O(1) even for
    // multi-terabyte objects and includes a possible partial last stripe. Summing this over all
    // drives equals tier_bytes exactly. Used by fallocate admission before streaming starts.
    Size file_extent(std::uint64_t key_hash, Size tier_bytes, unsigned drive) const noexcept;

    // Map tier-local range [off, off+len) to read segments: one per chunk, in logical order,
    // each contiguous in BOTH file and logical space (gather them by accumulating lengths).
    // Writes up to `out.size()` segments into `out`; returns the count written. If the range needs
    // more segments than `out` has room, fills what fits and returns out.size() (caller should size
    // for worst-case: ~ceil(len/stripe_unit) entries). Prefer this over the vector form on hot paths.
    std::size_t plan_reads(std::uint64_t key_hash, Offset off, Size len,
                           std::span<ReadSegment> out) const noexcept;
    // Heap form for callers that need a dynamic list (tests, rare paths).
    std::vector<ReadSegment> plan_reads(std::uint64_t key_hash, Offset off, Size len) const;

private:
    unsigned n_;
    Size s_;
};

} // namespace goblin::storage
