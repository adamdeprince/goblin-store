# ADR-0019: Decayed access scores and NUMA block promotion

**Status:** Accepted (2026-07-13; single-owner atomic score revision 2026-07-14).

## Context

`--sub-memory` lets the fixed-head cache spill onto foreign NUMA nodes, but static local-first
admission is not enough. A once-cold head can become hot after local RAM is full, leaving every
serving thread to read it across the socket interconnect. The application knows which keys create
that traffic and can continuously keep the hottest complete blocks local.

The compactable small-object arena has different movement and pinning costs. Small heads also create
less cross-NUMA traffic, so this revision deliberately leaves arena blocks out of the policy.

## Decision

- Every indexed key owns one logical `double` access score. A new key starts at zero; replacing the
  value for the same key preserves its score, while deleting and recreating it starts again at zero.
- `--increment FLOAT` (default `1.0`) must be finite and positive. One successful logical read adds
  it exactly once. Streaming chunks and an internal retry while waiting for an I/O buffer do not add
  extra increments.
- `--decay FLOAT` (default `0.5`) must be finite and strictly inside `(0, 1)`. Once per minute the
  index scores and node-local dense score arrays atomically apply `score *= decay` to every key.
- A score has exactly one numeric owner. A fixed resident head stores it in a dense
  `std::atomic<double>` slot on the same NUMA node as the head bytes. The slot is derived directly
  from its allocation block and fixed-head offset; it is not a hash table. Fractional arena objects
  and disk-backed objects whose head has been evicted store their relaxed atomic score beside the
  metadata in the sharded key index. A canonical NaN in that index field marks external ownership;
  it is a tag, not a second score.
- Head eviction and value replacement move the numeric score between owners under TierManager's
  exclusive lock. Publication into an occupied dense slot and extraction from an empty one are
  rejected. Deletion discards the sole active owner. Consequently increment and decay visit one
  numeric value per key, never a canonical value plus a NUMA mirror.
- Relaxed atomic operations give increments, decay, and scalar ranking defined concurrent semantics
  without imposing ordering fences. Increment and decay use compare/exchange loops so no arithmetic
  update can be lost. Metadata/head lifetime and ownership changes remain protected by TierManager
  and Index shard locks.
- Dense scans use scalar atomic loads for now. The implementation does not alias atomic objects as a
  raw `double` vector for SIMD. C++ guarantees atomic correctness even on a target that implements
  `std::atomic<double>` with a lock; the 64-bit deployment targets are expected to provide lock-free
  eight-byte atomics, but that is a performance property rather than a startup requirement.
- Score decay and NUMA promotion share a priority gate. The decay thread marks a rescore pending
  before waiting for that gate, which prevents the promotion thread's tight success loop from
  starting another exchange. At most the exchange already in progress finishes; decay then traverses
  the entire index without promotion running. The pending marker is cleared only after decay releases
  the gate.
- The selected NUMA node receives a strict inherited
  [`set_mempolicy(MPOL_BIND | MPOL_F_STATIC_NODES)`](https://man7.org/linux/man-pages/man2/set_mempolicy.2.html)
  default before the index or workers are created. Index buckets/nodes, request
  metadata, I/O buffers, and ordinary serving/coordinator allocations therefore come from local
  memory. The fixed-head `--memory` / `--sub-memory` ranges retain their more specific per-range
  [`mbind()`](https://man7.org/linux/man-pages/man2/mbind.2.html) policies, including foreign
  `--sub-memory` ranges.
- When the fixed-head pool has foreign regions, one persistent worker per physical NUMA node binds
  to that node and scalar-scans its local dense atomic array. Each worker sums the fixed-head scores in every
  complete block and reduces them to one local extrema summary. The serving-node coordinator compares
  the coldest eligible preferred block with the hottest eligible foreign block. If the foreign score
  is strictly larger, it exchanges their bytes, dense score slices, and allocator metadata, then
  rewrites every affected `HeadLoc` so the one score continues to follow its key. The O(N) dense
  score and scratch arrays stay node-local; cross-node coordination is limited to command state,
  compact per-node pin inputs, and one directional-extremum result from each worker. If no inversion
  exists, the coordinator sleeps for one second before scanning again.
- An eligible block is a **completely occupied buddy block** whose published index entries account
  for its entire allocation footprint. Bump-arena blocks, partially occupied buddy blocks, pinned or
  orphaned heads, and blocks containing an uncommitted store are excluded. This makes the exchange
  safe under the existing exclusive TierManager lock and zero-copy head pinning rules. Objects
  exactly equal to `--ram-head` are RAM-only but deliberately use fixed buddy slots, so (with the
  defaults) eight 256 KiB objects fill one promotable 2 MiB block. Smaller fractional RAM-only
  objects remain in the non-promotable compact arena. If `--small-memory` creates a dedicated
  small-object pool, its local and `--small-sub-memory` regions remain outside the promotion scan;
  strict pool isolation does not change that policy.

## Consequences

- ➕ The local head budget converges toward the blocks producing the most read traffic, while aging
  lets a formerly hot key naturally fall back to foreign memory.
- ➕ Key metadata and coordination remain on the NIC-local node, while each O(N) dense-array scan and
  decay pass executes on the node that owns those score pages. Ranking exchanges only compact
  command/pin state and one extrema result per worker across the interconnect, never the O(N) arrays.
- ➕ The minute decay pass cannot be starved by a long sequence of successful promotions, and block
  ranking never observes a partially decayed key space.
- ➖ A promotion copies two whole blocks while holding the storage write lock. Block size therefore
  bounds the pause, and close scores can cause extra movement until the ordering stabilizes.
- ➖ Small-object arena promotion is deferred; it needs an arena-aware aggregate/movement policy and
  is lower priority because those heads generate less cross-node bandwidth.
- `--perverse` is a benchmark-only policy inversion: region zero is deliberately placed on the
  farthest node and still treated as preferred, while workers and ordinary allocations remain on the
  serving node. It permits a controlled remote-DRAM latency comparison and must not be interpreted as
  a production placement recommendation.
