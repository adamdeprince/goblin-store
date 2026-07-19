# ADR-0017: Buffer taxonomy — fixed head pool, inline small responses, and streaming I/O pools

**Status:** Accepted (2026-06-09).

## Context
Two distinct kinds of RAM buffer were being conflated. They have different lifetimes and sizing,
and treating them the same is what makes the current GET allocate (and copy) a whole object.

## Decision
- **Resident payload buffers — FIXED.** Fixed-size large-object heads and packed RAM-only small
  objects live in fixed, unswappable data pools ([ADR-0008](0008-ram-allocator.md)), sized by
  config and bounded by S3-FIFO eviction ([ADR-0012](0012-multi-resource-eviction.md)). They are
  long-lived (cached across requests). The classes share blocks by default; explicit
  `--small-memory` gives the packed-small-object class a strict, non-borrowing pool.
- **I/O buffers — small, separate, fixed-size, reused.** A small slab of fixed chunk buffers for
  streaming the SSD/HDD portions (GET egress) and SET ingest. Transient: acquired per pipeline
  step, released immediately. **Not** proportional to object size. Size ≈
  `chunk × pipeline_depth × concurrent_streams`. Kept separate from the head pool so transient
  churn never fragments the head cache.
- **Inline or pinned head send.** If a complete RAM-resident response body is at most 16 KiB, copy
  it beside its protocol framing and issue one send. This removes extra completion round trips on
  the small-object path. Larger resident slices stay pinned and are sent directly from the head
  region, normally with the header through `sendmsg`, without an intermediate payload copy.
  `IORING_OP_SEND_ZC` remains a possible NIC-copy optimization. Neither head path borrows a disk
  I/O buffer.
- **Streaming, not whole-file.** GET = inline response or header → pinned head → SSD/HDD in fixed
  chunks from a per-worker read pool, head-first
  ([ADR-0006](0006-positional-tiering-pipeline.md)), never buffering a large whole value. SET
  streams socket→disk through the shared fixed write-staging pool. RAM per transfer is a small
  number of chunks, independent of object size.
- `--memory` sizes the local fixed-head budget; `--sub-memory` optionally adds fixed-head blocks on
  every foreign NUMA node. `--small-memory` opts into a dedicated local packed-small-object budget,
  with `--small-sub-memory` optionally adding the same-sized role on each foreign node. The two
  resident pools do not borrow; without `--small-memory`, both classes retain the legacy shared
  behavior. Read I/O pools are separately bounded by `--io-chunk × --io-buffers` per worker; the
  shared write-staging pool is bounded by `--write-io-chunk × --io-buffers`. On Linux all fixed
  pools first attempt explicit HugeTLB backing at the
  platform page order; `--block` is the common logical allocation unit and the fixed-head promotion
  unit, and may span multiple huge pages. Every resident budget is a whole `--block` multiple.
  Ordinary fallback memory is `mlock`'d unless `--no-mlock` is selected
  ([ADR-0016](0016-bounded-locked-memory.md)). Chunk and head geometry and split-pool isolation are
  unchanged by the backing choice. Packed-small-object blocks are not NUMA-promoted.

## Consequences
- ➕ RAM per request is O(chunks), not O(object size); larger heads avoid an intermediate
  userspace payload copy, while small responses avoid extra sends; the pools have clean,
  independent lifetimes.
- ➖ The inline threshold trades one small copy for fewer network completions. Larger transfers
  still require pin lifetime tracking and a streaming state machine.

## Implementation status (2026-07-15)
Done: the fixed head pool, per-worker read `IoBufferPool`s, one shared write-staging pool,
**streaming GET**, **streaming SET**, the ≤16 KiB inline fast path, and pinned `sendmsg` for larger
heads. `IORING_OP_SEND_ZC` and registered io_uring buffers remain possible measured follow-ups.
