# ADR-0006: Positional tiering & the request-triggered read-ahead pipeline

**Status:** Accepted (2026-06-09).

## Context
The core serving idea, and the sizing constraint it hides.

## Decision
- **Tiering is POSITIONAL (by byte offset), not temperature-based.** Every object is split
  positionally as `RAM head | SSD bytes | (optional) HDD tail`. Objects no larger than the
  configured RAM head are RAM-only. In two-layer mode SSD holds the complete backing value of
  every larger object; in three-layer mode it holds `[0, ssd_prefix)` and HDD holds the remainder.
  Disk placement is deterministic at write time — there is no SSD/HDD temperature promotion or
  demotion. NUMA promotion may move complete RAM-head allocation blocks without changing this
  disk layout ([ADR-0019](0019-access-score-numa-promotion.md)).
- **Read-ahead stays bounded and follows wire order.** Return the resident RAM head first. The disk
  remainder is planned in `--io-chunk` pieces. As soon as the initial response send is queued, the
  first tail read is issued behind the actual configured head boundary; a completed tail lane waits
  until the head send finishes, preserving wire order. With read-ahead enabled, at most two
  per-worker buffers overlap that first read with the head and then reading piece N+1 with sending
  piece N; without it, one buffer serializes later tail pieces. Only the next positional pieces are
  issued, rather than dispatching every tier at once. Pool exhaustion parks the request instead of
  allocating an object-sized buffer.
- **The win is the head-first *sequential* path** (the common case — a memcache `GET` returns the
  whole value from byte 0): the RAM head gives the instant first byte and the pipeline prefetches
  forward. Arbitrary mid-object byte-range reads (e.g. an HTTP `Range` into the middle) hit
  SSD/HDD *cold* with no head/prefetch benefit — that's inherent; Goblin's latency magic is
  byte-0-first streaming, not random access. `TierManager::ReadStream::plan(offset, ...)` is the
  streaming primitive; memcache GETs begin at zero, while HTTP may start at a selected range.
- **Read-ahead sizing law — the SSD middle is a shock absorber for HDD seeks.** An HDD only
  delivers sequential throughput when read in large gulps; under *N* concurrent streams it
  degrades toward IOPS-bound. Therefore:

  > `ssd_middle ≥ hdd_readahead_chunk × concurrent_streams_per_spindle`

  The current validation enforces the minimum useful form of that rule: the SSD middle
  (`ssd_prefix - ram_head`) must be at least one configured HDD stripe unit. Operators expecting
  many concurrent streams should provision a larger middle according to the full relationship.
  The 32 MB example is *derived*, not magic.
- **Index is small** (few, large objects) → cheap sharded-lock map; **whole-object eviction**.

## Consequences
- ➕ Deterministic, explainable placement; bounded streaming memory; the SSD-middle size is a
  *derived* quantity.
- ➖ HDD seek contention under concurrency remains the dominant risk; stripe geometry, I/O chunk
  size, SSD-middle depth, and the bounded per-worker buffer pools are the current controls.
