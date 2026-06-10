# ADR-0002: Per-core io_uring rings, shared drive fds, striped parallel reads

**Status:** Accepted (2026-06-09) — revisable.

## Context
Early framing spoke of "per-drive queues." That conflates two things. An io_uring ring is a
**per-thread** submission/completion structure; a read is `(ring, fd, offset, len, buffer)`.
A drive is just an `fd`. There is no hardware reason a ring must belong to a drive.

## Decision
- Each core owns its own io_uring ring(s). Drive `fd`s (SSD/HDD) are opened **once and shared
  globally**; they are read-mostly and safe to share across cores.
- The connection-owning core (see [ADR-0001](0001-thread-per-core-concurrency.md)) submits all
  reads for its objects against any drive `fd` **on its own ring**. `blk-mq` maps per-core
  submissions onto per-core NVMe hardware queues automatically.
- **Striping:** an object's bytes are striped across the SSD pool (and HDD pool) in
  fixed-size chunks; the owning core **fans out parallel reads** to every relevant drive at
  once — multi-drive parallelism falls out of fan-out on a single ring.
- Use io_uring **registered (fixed) buffers** and **registered files** where they measurably
  help; **multishot accept/recv** on the network side.

## liburing availability
liburing is **optional at the skeleton stage** (not installed on the dev box). Code compiles
with a **stub reactor** when `GOBLIN_HAVE_URING` is unset. The real io_uring backend is the
first implementation milestone — needs `liburing-dev` and a kernel ≥ 5.19 for multishot (we
have 6.17).

## Consequences
- ➕ No per-drive lock and no cross-core I/O routing; parallel pool reads are free.
- ➖ Shared device queues/page cache mean fairness must be managed by **bounded per-tier queues** ([ADR-0006](0006-positional-tiering-pipeline.md)), not by ring ownership.
