# ADR-0006: Positional tiering & the request-triggered read-ahead pipeline

**Status:** Accepted (2026-06-09).

## Context
The core serving idea, and the sizing constraint it hides.

## Decision
- **Tiering is POSITIONAL (by byte offset), not temperature-based.** Every object is split
  `RAM head | SSD middle | (optional) HDD tail`. Placement is deterministic at write time —
  **no promotion/demotion**, no hotness tracking.
- **Request arrival triggers the whole pipeline at once.** Return the RAM head immediately
  while async reads for the SSD/HDD ranges are *already in flight*, staged **ahead of** the
  wire position via bounded per-tier queues + explicit backpressure. The wire path should
  almost always find the next chunk already resident.
- **The win is the head-first *sequential* path** (the common case — a memcache `GET` returns the
  whole value from byte 0): the RAM head gives the instant first byte and the pipeline prefetches
  forward. Arbitrary mid-object byte-range reads (e.g. an HTTP `Range` into the middle) hit
  SSD/HDD *cold* with no head/prefetch benefit — that's inherent; Goblin's latency magic is
  byte-0-first streaming, not random access. `tier_manager.read(offset,len)` is the primitive;
  the GET path always streams from offset 0.
- **Read-ahead sizing law — the SSD middle is a shock absorber for HDD seeks.** An HDD only
  delivers sequential throughput when read in large gulps; under *N* concurrent streams it
  degrades toward IOPS-bound. Therefore:

  > `ssd_middle ≥ hdd_readahead_chunk × concurrent_streams_per_spindle`

  HDD reads are issued in **coarse chunks**, and config validation **must reject** an SSD
  middle smaller than the HDD chunk size. The 32 MB example is *derived*, not magic.
- **Index is small** (few, large objects) → cheap sharded-lock map; **whole-object eviction**.

## Consequences
- ➕ Deterministic, explainable placement; trivial index; cheap eviction; the SSD-middle size is a *derived* quantity.
- ➖ HDD seek contention under concurrency is the dominant risk — mitigated by big chunks, striping across spindles, and a **cap on concurrent HDD streams** (a bounded queue).
