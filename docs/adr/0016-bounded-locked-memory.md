# ADR-0016: Bounded, locked memory across NUMA regions

**Status:** Accepted (2026-06-09) — invariant; **partially implemented** (see status below).

## Context
Operator requirement: specify a local head-cache budget (`--memory`), optionally add a smaller
budget on each foreign NUMA node (`--sub-memory`), have it **locked** (no swap of the hot head), and
**never OOM**. RAM is a sized latency tool ([ADR-0008](0008-ram-allocator.md)) — its footprint must
be a hard, predictable cap, not best-effort.

## Decision
- **The resident head `BufferPool` is fixed:** `--memory` bytes on the explicit/automatic local NUMA
  node, plus `--sub-memory` bytes on every other online node when explicitly requested. Each region
  first attempts explicit hugetlb backing and otherwise uses ordinary `mlock`'d memory. Using
  `--sub-memory` requires `--numa NODE`. Head allocation is local-first, then foreign, and never grows.
- **Streaming I/O pools are separate and fixed:** read pools are `--io-chunk × --io-buffers` per
  worker; one write-staging pool uses the same geometry. Pool exhaustion produces backpressure
  ([ADR-0006](0006-positional-tiering-pipeline.md)), never heap growth.
- **No operation buffers a whole object.** SET streams socket→disk in fixed chunks; GET streams
  disk→socket head-first in fixed chunks. RAM per transfer = a few pool chunks, *independent of
  object size* (so a 1 GB GET/SET uses a handful of chunks, not 1 GB).
- **The index is bounded by the SSD object-count cap** ([ADR-0012](0012-multi-resource-eviction.md)),
  so its size is `O(count_limit)`. Locking the pool already covers the latency-critical data (heads
  + buffers never swap); the index is bounded-by-count and may be locked later if needed.
- **Connection state uses bounded fixed buffers** (cap the command line; stream the body).
- Size every node budget below the corresponding physical RAM and reserve room for I/O pools and
  metadata ⇒ the locked pools cannot swap and all growth is bounded. Head-cache capacity is exactly
  `--memory + (other_nodes × --sub-memory)`.

## Consequences
- ➕ Hard, predictable footprint; the resident pools never swap; local heads stay local until that
  region is exhausted; no OOM when every node is sized with headroom.
- ➖ Requires **streaming I/O everywhere** (no whole-value buffers) and enforcing the index count-bound.

## Implementation status (2026-07-13)
- ✅ Fixed, unswappable, per-NUMA-region pool backs the RAM-head cache, with best-effort explicit
  hugetlb, ordinary-memory fallback, strict placement checks, and strict local-first
  allocation and **S3-FIFO eviction** (ADR-0007/0012).
- ✅ **Streaming GET** (head zero-copy + chunked egress) and **streaming SET** (chunked ingest) —
  no whole-value buffers; RAM per transfer = O(chunk).
- ✅ **Object-count bound** (`--max-objects`) caps the index/store via whole-object eviction + unlink.
- Remaining polish: auto-derive the default `--max-objects` from SSD `statvfs` (today it's opt-in,
  `0` = unbounded); ordinary fallback memory needs `RLIMIT_MEMLOCK` raised (or `--no-mlock` for dev).
