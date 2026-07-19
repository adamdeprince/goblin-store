# ADR-0016: Bounded, locked memory across NUMA regions

**Status:** Accepted (2026-06-09) — invariant; **partially implemented** (see status below).

## Context
Operator requirement: specify a local fixed-head budget (`--memory`), optionally add a smaller
budget on each foreign NUMA node (`--sub-memory`), and optionally give packed small objects their
own local (`--small-memory`) and per-foreign-node (`--small-sub-memory`) budgets. These pools must be
**locked** and must **never OOM**. RAM is a sized latency tool
([ADR-0008](0008-ram-allocator.md)) — its footprint must be a hard, predictable cap, not best-effort.

## Decision
- **Resident data pools are fixed.** `--memory` bytes on the explicit/automatic local NUMA node,
  plus `--sub-memory` bytes on every other online node when explicitly requested, are the
  fixed-head budget. Omitting `--small-memory` preserves legacy behavior: packed small objects share
  that budget. Supplying `--small-memory` activates a strict split and gives packed small objects
  that many local bytes plus `--small-sub-memory` bytes on each other node; neither class may borrow
  from the other. Using either subordinate option requires explicit `--numa NODE`, and
  `--small-sub-memory` additionally requires `--small-memory`. Allocation within each pool is
  local-first, then foreign, and never grows.
- **HugeTLB and geometry rules apply independently to both resident pools.** Every region first
  attempts explicit HugeTLB backing using the platform's physical page size and otherwise uses
  ordinary `mlock`'d memory. Every nonzero configured capacity is a whole multiple of the logical
  `--block`, which may span several HugeTLB pages. Fallback preserves both pools' sizes, class isolation, and
  packing exactly.
- **Streaming I/O pools are separate and fixed:** read pools are `--io-chunk × --io-buffers` per
  worker; one write-staging pool is `--write-io-chunk × --io-buffers`. The independent geometry
  lets cache-miss/SET admission amortize writes without forcing warmed reads to use the same
  quantum. Pool exhaustion produces backpressure
  ([ADR-0006](0006-positional-tiering-pipeline.md)), never heap growth.
- **No operation buffers a whole object.** SET streams socket→disk in fixed chunks; GET streams
  disk→socket head-first in fixed chunks. RAM per transfer = a few pool chunks, *independent of
  object size* (so a 1 GB GET/SET uses a handful of chunks, not 1 GB).
- **The fixed pools are bounded; the index is only partially bounded today.** `--max-objects`
  limits committed disk-backed objects ([ADR-0012](0012-multi-resource-eviction.md)), not RAM-only
  entries. Non-empty RAM-only objects consume the fixed head arena and are therefore indirectly
  count-limited, but zero-length entries and node-based map metadata still lack one explicit global
  cap. The index is ordinary local-node memory and may be locked or moved to a bounded allocator
  later.
- **Connection state uses bounded fixed buffers** (cap the command line; stream the body).
- Size every node budget below the corresponding physical RAM and reserve room for I/O pools and
  metadata. The locked pools cannot swap. Fixed-head capacity is exactly
  `--memory + (other_nodes × --sub-memory)`. In split mode, packed-small-object capacity is exactly
  `--small-memory + (other_nodes × --small-sub-memory)`; in legacy mode it shares the fixed-head
  capacity. The remaining index-metadata bound is tracked above.

## Consequences
- ➕ Hard, predictable fixed-pool footprint; resident pools never swap and local heads stay local
  until that region is exhausted.
- ➕ Explicit split mode guarantees one object class cannot consume the other class's reserved RAM;
  omission preserves the established shared-pool behavior.
- ➖ Strict isolation can leave capacity unused in one pool while the other is under pressure.
- ➖ Requires **streaming I/O everywhere** (no whole-value buffers); a complete index metadata
  bound remains necessary before claiming a hard cap for the whole process.

## Implementation status (2026-07-15)
- ✅ Fixed, unswappable, per-NUMA-region pools back fixed heads and, when explicitly split, packed
  small objects, with best-effort explicit hugetlb, ordinary-memory fallback, strict placement
  checks, local-first allocation, and **S3-FIFO eviction** (ADR-0007/0012).
- ✅ **Streaming GET** (small RAM-only responses framed inline; larger heads pinned; disk egress
  chunked) and **streaming SET** (chunked ingest) — no whole-value buffers; RAM per transfer =
  O(chunk).
- ✅ **Disk-backed object-count bound** (`--max-objects`) evicts and unlinks whole disk-backed
  objects over the explicit limit; `0` is unbounded.
- Remaining work: bound zero-length/RAM-only index metadata explicitly; ordinary fallback memory
  needs `RLIMIT_MEMLOCK` raised (or `--no-mlock` for dev).
