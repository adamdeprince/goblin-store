# ADR-0008: RAM allocator — power-of-two block array + intra-block buddy

**Status:** Accepted (2026-06-09).

## Context
RAM is a sized latency tool, not a savings tool. We want fixed, bounded,
fragmentation-resistant RAM that also doubles as the io_uring **registered-buffer** region.

## Decision
- RAM is **one preallocated logical array of equal-size allocation/promotion blocks**; `--block` is
  a **power of two** (default 2 MiB on x86 and 32 MiB on Arm/LoongArch). It is distinct from the
  per-object `--ram-head` (default 256 KiB): heads pack exactly inside it, so the x86 defaults hold
  eight heads per block. The array has one ordered free-list per NUMA region. Whole-block
  allocation/free is O(1) within a region, and regions are searched local-first.
- Fixed-size heads use **buddy allocation inside each block**. `--ram-head` is a power of two no
  larger than `--block`; split/merge is O(log). Fractional RAM-only objects smaller than
  `--ram-head` use the compact bump arena described below.
- The block array has a **fixed, command-line-specified size** — RAM is a sized latency tool, it
  never grows. Without subordinate memory its size is `--memory`; with explicit NUMA placement it is
  `--memory + (other online nodes × --sub-memory)`. The range is
  **resident and unswappable** — never page the fast head out to the very disk we're trying to
  avoid. On Linux, every fixed block pool first attempts an explicit `MAP_HUGETLB` mapping using the
  platform's physical page order (2 MiB on x86, 32 MiB on Arm/LoongArch). `--block` must be a
  power-of-two multiple of that size; a 4 MiB x86 allocation block is backed by two 2 MiB huge pages,
  not a nonexistent 4 MiB huge-page order. Streaming pools keep the smaller `--io-chunk` allocation
  granule within that backing mapping. Each NUMA region is attempted independently against that
  node's pool. An unavailable page order, insufficient pages for the complete region, or failed
  placement check silently falls back to ordinary memory plus the configured `mlock`. The logical
  block/head geometry and configured capacity never change on fallback. Registering the arena with
  io_uring as a fixed buffer remains a design target. A pool's total size must be a whole multiple
  of the physical HugeTLB page size; no hidden rounding is allowed because that would violate its
  configured memory bound. The arena is **aligned to the
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

The allocator owns one independently mapped range per region. It attempts to populate each range
from the selected node's explicit hugetlb pool and verifies residency and strict physical placement;
if that attempt cannot be satisfied, it maps the same bytes normally, installs
`mbind(MPOL_BIND | MPOL_F_STATIC_NODES)`, and `mlock`s them. Region zero is local. Allocation searches
compatible local arenas and unused local blocks before visiting foreign regions. Separate per-region
free lists preserve that ordering even when local and foreign blocks are returned in an arbitrary
order. A global block ID resolves through a block-to-region table, so physical mappings need not be
virtually contiguous.

Once foreign blocks are in use, static local-first admission is augmented by score-based promotion
([ADR-0019](0019-access-score-numa-promotion.md)). The maintenance pass may exchange a completely
occupied local buddy block with a hotter completely occupied foreign buddy block. It swaps both the
bytes and per-block allocator state, then rewrites every affected `HeadLoc`. Partial blocks and the
small-object bump arena are not promotion candidates in this revision.

The benchmark-only `--perverse` flag separates CPU/NIC locality from head-memory locality. Threads,
the key index, streaming pools, and the inherited default policy stay on the selected serving node,
while region zero (`--memory`) is mapped on the online node with the greatest NUMA distance from it.
The region remains first in allocation and promotion order, deliberately inverting the policy. This
produces a normal/perverse latency A/B with the same serving CPU and PCI path. It is invalid with
`--no-numa` or on a machine with fewer than two online NUMA nodes.

## Revision (2026-07): per-class allocators — buddy for large heads, byte-granular arena for small

The buddy min-order was `kDeviceBlock` (4 KiB) so every head stayed O_DIRECT-aligned. For the
**large-object** workload that's free — a `ram_head`-sized head is already a power of two. But once
small objects became **RAM-only** (head is the only copy — [ADR-0003](0003-cache-semantics-head-as-cache.md)),
a ~250 B value burned a whole 4 KiB leaf: **~12× worse than memcached** on small values. Two facts
unlock the fix: a RAM-only head **never DMAs** (it's memcpy-filled and io_uring-*sent*, so it needs
no 4 KiB alignment), and a head is **movable** — reachable only through the index `HeadLoc`, so its
bytes can be relocated as long as that locator is rewritten under the write lock ([ADR-0018](0018-atomic-publish-cow.md)).

A block's **class is fixed on first use**, and the two classes never share a block:

- **Fixed heads (objects `>= ram_head`)** use an exact `ram_head`-sized intra-block **buddy** slot —
  power-of-two, O_DIRECT-aligned, and self-coalescing. An object exactly equal to `ram_head` is still
  RAM-only, but uses this class so a complete block can participate in NUMA promotion.
- **Fractional RAM-only heads (objects `< ram_head`)** use a **byte-granular bump arena**: heads packed
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
