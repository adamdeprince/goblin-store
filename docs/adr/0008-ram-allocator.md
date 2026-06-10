# ADR-0008: RAM allocator — power-of-two block array + intra-block buddy

**Status:** Accepted (2026-06-09).

## Context
RAM is a sized latency tool, not a savings tool. We want fixed, bounded,
fragmentation-resistant RAM that also doubles as the io_uring **registered-buffer** region.

## Decision
- RAM is **one preallocated array of equal-size blocks**; block size is a **power of two**
  (operator-configured). Whole-block allocation/free is O(1) via a free-list.
- Allocations **smaller than a block** take one block and run **buddy allocation inside it**
  (power-of-two sub-blocks down to a minimum order); split/merge is O(log).
- The block array is a single large region whose total size is a **fixed,
  command-line-specified amount** — RAM is a sized latency tool, it never grows. The region is
  **`mlock`ed / `MAP_LOCKED` — never swapped** (never page the fast head out to the very disk
  we're trying to avoid), prefers hugepages (THP / `MAP_HUGETLB`), and is **registered once
  with io_uring as a fixed buffer** so reads DMA straight into it. The arena is **aligned to the
  device block size (≥ 4 KiB)** and blocks are power-of-two ≥ that — which hands us **O_DIRECT
  alignment for free** (see [ADR-0011](0011-odirect-bypass-page-cache.md)). *Ops:* raise
  `RLIMIT_MEMLOCK` (or grant `CAP_IPC_LOCK`) for multi-GB locked regions; fail fast at startup if
  the limit is too low.
- **Per-core arenas:** each core owns a batch of blocks drawn from the global array and refills
  in bulk → no per-allocation cross-core lock (matches thread-per-core, [ADR-0001](0001-thread-per-core-concurrency.md)).
- Size the block / buddy min-order so the RAM head size and the read chunk / stripe unit land
  on clean orders (minimize internal fragmentation).

## Consequences
- ➕ Bounded, predictable RAM (no unbounded growth); zero-copy into registered buffers; lock-light per core.
- ➖ Internal fragmentation up to the buddy min-order — mitigated by sizing blocks/orders to the workload.
