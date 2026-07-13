# ADR-0017: Buffer taxonomy — fixed head pool vs. small streaming I/O pool; zero-copy head

**Status:** Accepted (2026-06-09).

## Context
Two distinct kinds of RAM buffer were being conflated. They have different lifetimes and sizing,
and treating them the same is what makes the current GET allocate (and copy) a whole object.

## Decision
- **Payload head buffers — FIXED.** Object heads live in the fixed, `mlock`'d head-cache pool
  ([ADR-0008](0008-ram-allocator.md)), sized by config, bounded by S3-FIFO eviction
  ([ADR-0012](0012-multi-resource-eviction.md)). Long-lived (cached across requests).
- **I/O buffers — small, separate, fixed-size, reused.** A small slab of fixed chunk buffers for
  streaming the SSD/HDD portions (GET egress) and SET ingest. Transient: acquired per pipeline
  step, released immediately. **Not** proportional to object size. Size ≈
  `chunk × pipeline_depth × concurrent_streams`. Kept separate from the head pool so transient
  churn never fragments the head cache.
- **Zero-copy head send.** Head bytes go straight from the head-cache region to the socket — no
  intermediate userspace copy into a response buffer. Implemented as a scatter send (`writev` /
  io_uring send of `[header, head region, chunk…, trailer]`; `IORING_OP_SEND_ZC` later for NIC
  zero-copy). The head path therefore needs **zero** I/O buffers.
- **Streaming, not whole-file.** GET = header → head (zero-copy) → SSD/HDD in fixed chunks from the
  I/O pool, each sent as read, head-first ([ADR-0006](0006-positional-tiering-pipeline.md)), never
  buffering the whole value. SET = stream socket→disk in chunks. RAM per transfer ≈ a few chunks,
  independent of object size.
- `--memory` sizes the local head pool; `--sub-memory` optionally adds head blocks on every foreign
  NUMA node. Small I/O pools are separately bounded by `--io-chunk × --io-buffers` per worker (plus
  one write-staging pool). All are `mlock`'d unless `--no-mlock` is selected
  ([ADR-0016](0016-bounded-locked-memory.md)).

## Consequences
- ➕ RAM per request is O(chunks), not O(object size); head served with no userspace payload copy; the two pools have clean, independent lifetimes.
- ➖ Requires a streaming GET/SET path + scatter-send; replaces the current whole-file-in-one-block GET.

## Implementation status (2026-06-10)
Done: the fixed head pool, the small `IoBufferPool`, **streaming GET** (head sent zero-copy from the
pool + the remainder in fixed I/O chunks), and **streaming SET** (chunked ingest). `IORING_OP_SEND_ZC`
/ `writev` scatter-send is a later micro-opt; today each segment is a separate `send`.
