# ADR-0008: RAM allocator — power-of-two block array + intra-block buddy

**Status:** Accepted (2026-06-09).

## Context
RAM is a sized latency tool, not a savings tool. We want fixed, bounded,
fragmentation-resistant RAM that also doubles as the io_uring **registered-buffer** region.

## Decision
- RAM is **one preallocated virtual array of equal-size blocks**; block size is a **power of two**
  (operator-configured). The array has one ordered free-list per NUMA region. Whole-block
  allocation/free is O(1) within a region, and regions are searched local-first.
- Allocations **smaller than a block** take one block and run **buddy allocation inside it**
  (power-of-two sub-blocks down to a minimum order); split/merge is O(log).
- The block array has a **fixed, command-line-specified size** — RAM is a sized latency tool, it
  never grows. Without subordinate memory its size is `--memory`; with explicit NUMA placement it is
  `--memory + (other online nodes × --sub-memory)`. The range is
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

## Revision (2026-07): local-first NUMA regions

`--numa NODE` makes that node local to the serving threads. `--memory` bytes are mapped there;
optional `--sub-memory` maps that many bytes on every other online node. `--sub-memory` is invalid
without explicit `--numa`, because automatic NIC selection is intentionally not enough authority to
commit memory across the entire machine.

The allocator reserves one contiguous virtual range, applies a strict physical-node
`mbind(MPOL_BIND | MPOL_F_STATIC_NODES)` policy to each local/foreign subrange, then `mlock`s the
whole range. Region zero is local. Allocation searches compatible local arenas and unused local
blocks before visiting foreign regions. Separate per-region free lists preserve that ordering even
when local and foreign blocks are returned in an arbitrary order.

## Revision (2026-07): per-class allocators — buddy for large heads, byte-granular arena for small

The buddy min-order was `kDeviceBlock` (4 KiB) so every head stayed O_DIRECT-aligned. For the
**large-object** workload that's free — a `ram_head`-sized head is already a power of two. But once
small objects became **RAM-only** (head is the only copy — [ADR-0003](0003-cache-semantics-head-as-cache.md)),
a ~250 B value burned a whole 4 KiB leaf: **~12× worse than memcached** on small values. Two facts
unlock the fix: a RAM-only head **never DMAs** (it's memcpy-filled and io_uring-*sent*, so it needs
no 4 KiB alignment), and a head is **movable** — reachable only through the index `HeadLoc`, so its
bytes can be relocated as long as that locator is rewritten under the write lock ([ADR-0018](0018-atomic-publish-cow.md)).

A block's **class is fixed on first use**, and the two classes never share a block:

- **Large heads (`>= kDeviceBlock`)** keep intra-block **buddy** allocation — power-of-two, O_DIRECT-aligned, self-coalescing. Unchanged.
- **Small RAM-only heads (`< kDeviceBlock`)** use a **byte-granular bump arena**: heads packed
  contiguously at an 8/16 B granularity (`--small-min-alloc`, default 16 B), **no power-of-two
  rounding**. A free only marks bytes dead; the bump frontier never rewinds.
  - *Phase 1* pared the floor from 4 KiB to the min-order (kills the 4 KiB leaf).
  - *Phase 2* replaces buddy-in-block with the bump arena (kills the remaining ~power-of-two rounding),
    and adds **sliding compaction** to reclaim the dead holes bump can't coalesce: within a fragmented
    block, live heads slide down to squeeze out the holes, each `HeadLoc` is rewritten, and the
    frontier rewinds. It is **in-place** (needs no spare block, so it works at 100 % RAM), **skips any
    block with a pinned head** (a reader streaming it zero-copy — [ADR-0017](0017-streaming-io-buffers.md)),
    runs under the exclusive storage lock, and is triggered from the **admission path before eviction**
    (reclaim dead space rather than drop live objects). Eviction remains the backstop when there is
    genuinely more live data than RAM. This is the "beat slab" move — memcached can't relocate objects,
    so emptied slabs calcify; we compact.

### Consequences
- ➕ Small-object RAM approaches memcached parity; byte-granular packing removes power-of-two waste; compaction defeats slab-style calcification without a size-class rebalance.
- ➖ Compaction is an **O(live heads) index walk** per pass (bounded by triggering only under small-pool pressure, and gated on there being ≥ one head's worth of reclaimable dead space); a block with a pinned head defers to a later pass.
