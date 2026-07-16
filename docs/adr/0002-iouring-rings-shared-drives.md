# ADR-0002: Per-worker io_uring rings and striped parallel reads

**Status:** Accepted (2026-06-09), revised 2026-07-15 — revisable.

## Context
Early framing spoke of "per-drive queues." That conflates two things. An io_uring ring is a
**per-thread** submission/completion structure; a read is `(ring, fd, offset, len, buffer)`.
A drive is just an `fd`. There is no hardware reason a ring must belong to a drive.

## Decision
- Each serving worker owns its own io_uring `Reactor`. The connection-owning worker (see
  [ADR-0001](0001-thread-per-core-concurrency.md)) submits that connection's network operations and
  disk reads on its own ring; connection state and streaming buffers do not move between workers.
- Each pool opens one long-lived `O_DIRECTORY` fd per configured drive directory. Object data fds
  are not global: a read snapshot opens and owns the descriptors for one immutable generation, and
  a `StoreHandle` owns private descriptors for the generation it is writing. Already-open reader
  fds therefore keep a retired incarnation alive after replacement unlinks its names
  ([ADR-0018](0018-concurrency-model.md)).
- **Striping:** an object's bytes are striped across the SSD pool (and HDD pool) in
  fixed-size chunks. For each bounded streaming piece, the owning worker submits every relevant
  segment before flushing the ring, so reads from several drives can overlap on that one ring.
- Network accept, recv, send/sendmsg, poll, and disk read are currently one-shot operations.
  Registered buffers/files, multishot operations, and async writes remain measurement-driven
  follow-ups. Stores currently perform striped synchronous `pwrite` outside the storage lock;
  separate handles can write concurrently.

## liburing availability
Code still compiles with a **stub reactor** when `GOBLIN_HAVE_URING` is unset, which supports logic
tests but cannot serve traffic. A serving build needs `liburing-dev`; Linux 5.19 is the project's
current io_uring support floor.

## Consequences
- ➕ No cross-worker I/O routing; immutable snapshot fds make replacement safe; one piece can
  overlap reads across several pool drives.
- ➖ Device queues remain shared hardware resources. Bounded per-worker read pools and the
  positional pipeline ([ADR-0006](0006-positional-tiering-pipeline.md)) provide backpressure; ring
  ownership alone is not a fairness policy.
