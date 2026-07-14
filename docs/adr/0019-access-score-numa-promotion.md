# ADR-0019: Decayed access scores and NUMA block promotion

**Status:** Accepted (2026-07-13).

## Context

`--sub-memory` lets the head cache spill onto foreign NUMA nodes, but static local-first admission is
not enough. A once-cold head can become hot after local RAM is full, leaving every serving thread to
read it across the socket interconnect. The application knows which keys create that traffic and can
continuously keep the hottest complete blocks local.

The compactable small-object arena has different movement and pinning costs. Small heads also create
less cross-NUMA traffic, so this revision deliberately leaves arena blocks out of the policy.

## Decision

- Every indexed key owns a `double` access score. A new key starts at zero; replacing the value for
  the same key preserves its score, while deleting and recreating it starts again at zero.
- `--increment FLOAT` (default `1.0`) must be finite and positive. One successful logical read adds
  it exactly once. Streaming chunks and an internal retry while waiting for an I/O buffer do not add
  extra increments.
- `--decay FLOAT` (default `0.5`) must be finite and strictly inside `(0, 1)`. Once per minute a
  maintenance thread traverses the index and atomically applies `score *= decay` to every key.
- Scores are relaxed `std::atomic<double>` values adjacent to index metadata. Shard locks protect
  entry lifetime; the relaxed atomic makes worker increments, the decay pass, and NUMA ranking free
  of a data race without turning every hit into an exclusive index operation.
- Score decay and NUMA promotion share a priority gate. The decay thread marks a rescore pending
  before waiting for that gate, which prevents the promotion thread's tight success loop from
  starting another exchange. At most the exchange already in progress finishes; decay then traverses
  the entire index without promotion running. The pending marker is cleared only after decay releases
  the gate.
- The selected NUMA node receives a strict inherited
  [`set_mempolicy(MPOL_BIND | MPOL_F_STATIC_NODES)`](https://man7.org/linux/man-pages/man2/set_mempolicy.2.html)
  default before the index or workers are created. Index buckets/nodes, request
  metadata, I/O buffers, and maintenance-thread allocations therefore come from local memory. The
  fixed head-cache ranges retain their more specific per-range
  [`mbind()`](https://man7.org/linux/man-pages/man2/mbind.2.html) policies, including foreign
  `--sub-memory` ranges.
- When the head pool has foreign regions, one maintenance thread runs on the selected node and
  repeatedly:
  1. groups resident keys by backing block and sums their scores (aggregate traffic is the block's
     score);
  2. finds the coldest eligible local block and hottest eligible foreign block;
  3. if the foreign score is strictly larger, exchanges their bytes and allocator metadata, then
     rewrites every affected `HeadLoc` so scores remain attached to keys;
  4. otherwise sleeps for one second before scanning again.
- An eligible block is a **completely occupied buddy block** whose published index entries account
  for its entire allocation footprint. Bump-arena blocks, partially occupied buddy blocks, pinned or
  orphaned heads, and blocks containing an uncommitted store are excluded. This makes the exchange
  safe under the existing exclusive TierManager lock and zero-copy head pinning rules. Objects
  exactly equal to `--ram-head` are RAM-only but deliberately use fixed buddy slots, so (with the
  defaults) eight 256 KiB objects fill one promotable 2 MiB block. Smaller fractional RAM-only
  objects remain in the non-promotable compact arena.

## Consequences

- ➕ The local head budget converges toward the blocks producing the most read traffic, while aging
  lets a formerly hot key naturally fall back to foreign memory.
- ➕ Key metadata and all score-maintenance work remain on the NIC-local CPU and memory node.
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
