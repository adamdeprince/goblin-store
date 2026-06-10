# ADR-0016: Bounded, locked total memory (RSS ≈ `--memory`)

**Status:** Accepted (2026-06-09) — invariant; **partially implemented** (see status below).

## Context
Operator requirement: specify a memory budget (`--memory`), have it **locked** (no swap of the
hot head), and **never OOM**. RAM is a sized latency tool ([ADR-0008](0008-ram-allocator.md)) — its
footprint must be a hard, predictable cap, not best-effort.

## Decision
- **The one `mlock`'d `BufferPool` (`--memory`) is the only home for large/streaming RAM:** RAM
  heads **and** all I/O chunk buffers (SET ingest, GET egress, read-ahead staging). Allocation from
  it is bounded; when full → **backpressure** ([ADR-0006](0006-positional-tiering-pipeline.md)),
  never heap growth.
- **No operation buffers a whole object.** SET streams socket→disk in fixed chunks; GET streams
  disk→socket head-first in fixed chunks. RAM per transfer = a few pool chunks, *independent of
  object size* (so a 1 GB GET/SET uses a handful of chunks, not 1 GB).
- **The index is bounded by the SSD object-count cap** ([ADR-0012](0012-multi-resource-eviction.md)),
  so its size is `O(count_limit)`. Locking the pool already covers the latency-critical data (heads
  + buffers never swap); the index is bounded-by-count and may be locked later if needed.
- **Connection state uses bounded fixed buffers** (cap the command line; stream the body).
- Size `--memory` below physical RAM ⇒ the locked pool can't swap, everything else is bounded ⇒
  **no OOM**. Result: `RSS ≈ --memory + bounded index + small fixed overhead`.

## Consequences
- ➕ Hard, predictable footprint; the `mlock`'d pool never swaps (no major-fault latency on the head); no OOM when `--memory` is sized with headroom.
- ➖ Requires **streaming I/O everywhere** (no whole-value buffers) and enforcing the index count-bound.

## Implementation status (2026-06-10)
- ✅ Fixed, `mlock`'d pool backs the RAM-head cache, **bounded by S3-FIFO eviction** (ADR-0007/0012).
- ✅ **Streaming GET** (head zero-copy + chunked egress) and **streaming SET** (chunked ingest) —
  no whole-value buffers; RAM per transfer = O(chunk).
- ✅ **Object-count bound** (`--max-objects`) caps the index/store via whole-object eviction + unlink.
- Remaining polish: auto-derive the default `--max-objects` from SSD `statvfs` (today it's opt-in,
  `0` = unbounded); `mlock`-by-default needs `RLIMIT_MEMLOCK` raised (or `--no-mlock` for dev).
