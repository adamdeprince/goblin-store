# ADR-0008: RAM allocator — power-of-two block array + intra-block buddy

**Status:** Accepted (2026-06-09).

## Context
RAM is a sized latency tool, not a savings tool. We want fixed, bounded,
fragmentation-resistant RAM that also doubles as the io_uring **registered-buffer** region.

## Decision
- RAM is preallocated as regions of equal-size allocation blocks; `--block` is a **power of
  two** (default 2 MiB on x86 and 32 MiB on Arm/LoongArch). It is distinct from the per-object
  `--ram-head` (default 256 KiB): heads pack exactly inside it, so the x86 defaults hold eight heads
  per block. By default there is one shared logical pool for fixed heads and packed small objects.
  Explicit `--small-memory` creates two dedicated reservations in the same block/address namespace;
  those reservations never lend blocks to one another. Each reservation has one ordered free-list
  per NUMA region. Whole-block allocation/free is O(1) within a region, and each reservation searches
  its own regions local-first.
- Fixed-size heads use **buddy allocation inside each block**. `--ram-head` is a power of two no
  larger than `--block`; split/merge is O(log). Fractional RAM-only objects smaller than
  `--ram-head` use the compact bump arena described below.
- Every reservation has a **fixed, command-line-specified size** — RAM is a sized latency tool; the
  reservations never grow. The fixed-head budget is `--memory`, plus
  `(other online nodes × --sub-memory)` with explicit NUMA placement. When `--small-memory` is
  present, the dedicated packed-small-object budget is `--small-memory`, plus
  `(other online nodes × --small-sub-memory)`. Every range is
  **resident and unswappable** — never page the fast head out to the very disk we're trying to
  avoid. On Linux, every fixed block pool first attempts an explicit `MAP_HUGETLB` mapping using the
  platform's physical page order (2 MiB on x86, 32 MiB on Arm/LoongArch). `--block` must be a
  power-of-two multiple of that size; a 4 MiB x86 allocation block is backed by two 2 MiB huge pages,
  not a nonexistent 4 MiB huge-page order. Streaming pools keep the smaller `--io-chunk` allocation
  granule within that backing mapping. Each NUMA region is attempted independently against that
  node's pool, for both fixed-head and dedicated small-object reservations. An unavailable page
  order, insufficient pages for the complete region, or failed
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

`--numa NODE` makes that node local to the serving threads. `--memory` fixed-head bytes are mapped
there; optional `--sub-memory` maps that many fixed-head bytes on every other online node.
`--sub-memory` is invalid without explicit `--numa`, because automatic NIC selection is
intentionally not enough authority to commit memory across the entire machine.

Omitting `--small-memory` preserves the original shared behavior: fixed heads and packed small
objects both consume the `--memory` / `--sub-memory` regions. Supplying `--small-memory` creates a
strict non-borrowing split. Packed small objects then consume only `--small-memory` bytes on the
local node and, when configured, `--small-sub-memory` bytes on each other node; fixed heads consume
only their original regions. `--small-sub-memory` requires both `--small-memory` and explicit
`--numa`. Every nonzero local or subordinate budget is at least one `--block` and a whole multiple
of `--block`, so the physical HugeTLB-page multiple and ordinary-memory fallback rules apply to
both reservations without rounding.

The allocator owns one independently mapped range per region. It attempts to populate each range
from the selected node's explicit hugetlb pool and verifies residency and strict physical placement;
if that attempt cannot be satisfied, it maps the same bytes normally, installs
`mbind(MPOL_BIND | MPOL_F_STATIC_NODES)`, and `mlock`s them. Region zero is local. Allocation searches
compatible local arenas and unused local blocks before visiting foreign regions. Separate per-region
free lists preserve that ordering even when local and foreign blocks are returned in an arbitrary
order. A global block ID resolves through a block-to-region table, so physical mappings need not be
virtually contiguous.

Once foreign fixed-head blocks are in use, static local-first admission is augmented by score-based
promotion
([ADR-0019](0019-access-score-numa-promotion.md)). The maintenance pass may exchange a completely
occupied local buddy block with a hotter completely occupied foreign buddy block. It swaps both the
bytes and per-block allocator state, then rewrites every affected `HeadLoc`. Partial blocks and the
small-object bump arena are not promotion candidates in this revision, whether their blocks come
from the legacy shared reservation or a dedicated split reservation.

The benchmark-only `--perverse` flag separates CPU/NIC locality from head-memory locality. Serving
threads, the promotion coordinator, key index, streaming pools, and inherited default policy stay on
the selected serving node. Dense-score workers still bind beside the score arrays they own, while
the preferred `--memory` region—and the preferred `--small-memory` region, if configured—is mapped
on the online node with the greatest NUMA distance from the serving node. The regions remain first
in their respective allocation orders, deliberately inverting locality; only fixed-head blocks
participate in promotion. This produces a normal/perverse latency A/B with the same serving CPU and
PCI path. It is invalid with `--no-numa` or on a machine with fewer than two online NUMA nodes.

## Revision (2026-07): per-class allocators — buddy for large heads, byte-granular arena for small

The buddy min-order was `kDeviceBlock` (4 KiB) so every head stayed O_DIRECT-aligned. For the
**large-object** workload that's free — a `ram_head`-sized head is already a power of two. But once
small objects became **RAM-only** (head is the only copy — [ADR-0003](0003-cache-semantics-head-as-cache.md)),
a ~250 B value burned a whole 4 KiB leaf: **~12× worse than memcached** on small values. Two facts
unlock the fix: a RAM-only head **never DMAs** (it's memcpy-filled and io_uring-*sent*, so it needs
no 4 KiB alignment), and a head is **movable** — reachable only through the index `HeadLoc`, so its
bytes can be relocated as long as that locator is rewritten under the write lock ([ADR-0018](0018-concurrency-model.md)).

In legacy shared mode a block's **class is fixed on first use**, so the two classes never
simultaneously occupy one block. In explicit split mode every block's class is fixed by its pool at
startup, and spare capacity cannot cross the pool boundary:

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
    block with a pinned head** (a reader streaming it zero-copy — [ADR-0017](0017-buffer-taxonomy.md))
    or an unpublished fractional `StoreHandle` reservation, runs under the exclusive storage lock,
    and is triggered from the **admission path before eviction** when old dead space suffices. If one
    eviction creates enough new dead space, admission compacts before choosing another victim, so a
    full arena is not needlessly drained. Eviction remains the backstop when there is genuinely more
    live data than RAM. This
    is the "beat slab" move — memcached can't relocate objects,
    so emptied slabs calcify; we compact.

### Consequences
- ➕ Small-object RAM approaches memcached parity; byte-granular packing removes power-of-two waste; compaction defeats slab-style calcification without a size-class rebalance.
- ➕ Operators can reserve independent fixed-head and packed-small-object capacity; omitting the new
  option keeps existing deployments' shared-pool admission behavior.
- ➖ A strict split can strand free blocks in one class while the other class evicts under pressure;
  that isolation is the requested policy, not an allocator fallback.
- ➖ Compaction is an **O(live heads) index walk** per pass (bounded by triggering only under small-pool pressure, and gated on there being ≥ one head's worth of reclaimable dead space); a block with a pinned head defers to a later pass.
