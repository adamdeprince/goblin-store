# ADR-0007: Eviction — size-aware, FIFO-based (not LRU/LFU)

**Status:** Accepted (2026-06-09) — revisable; the policy is pluggable.

## Context
LRU and LFU are the wrong frame here for two reasons:
1. They optimize object **count**, but we evict to reclaim **bytes**, and objects span
   1 MB–1 GB (1000×) — so **value-per-byte** is what matters, not recency alone.
2. LRU's per-hit list surgery is a lock convoy under thread-per-core ([ADR-0001](0001-thread-per-core-concurrency.md)).

We also have **few** objects (thousands of big ones, not memcached's tens of millions of
tiny ones), which lets us afford a richer per-object policy than memcached can.

## Decision
- **Base signal — FIFO-based, not LRU/LFU.** Default **S3-FIFO** (SOSP'23: small/main/ghost
  queues); **SIEVE** (NSDI'24: one queue + a hand) as the simpler fallback. A "hit" sets a
  *visited bit* (one atomic) — no list reorder, no global lock — and both are scan-resistant.
- **Size-aware retention — GreedyDual-Size-Frequency (GDSF).** Rank whole-object retention by
  value ≈ `(frequency × recency) / bytes` with an aging *inflation clock*. The usual GDSF
  objection (an O(log n) priority queue) is a non-issue: few objects → a heap is cheap.
- **Two eviction tiers** (because tiering is positional — [ADR-0003](0003-cache-semantics-head-as-cache.md)/[ADR-0006](0006-positional-tiering-pipeline.md)):
  - *RAM-head cache:* cheap SIEVE/CLOCK over heads; eviction is **free** (bytes are on SSD); optimize for TTFB hit-rate.
  - *Whole-object capacity* (SSD+HDD full): size-aware GDSF; `unlink` reclaims SSD+HDD together.
- **Admission control**, not just eviction: a size-aware gate so a 1 GB one-hit-wonder can't
  flush the hot set (the GDSF comparison + S3-FIFO's ghost queue / SIEVE's lazy promotion).
- **TTL/expiry is separate:** memcache `exptime` is authoritative — expired objects are dropped
  on access and swept by a timer wheel, independent of the capacity policy.
- **Pluggable `EvictionPolicy`, selectable at runtime** via `--eviction {sieve|s3fifo|tinylfu}`
  (default **s3fifo**) — so policies are A/B-benchmarkable on real workloads (a stated project
  value). All three are FIFO/clock-based and lock-light (a hit sets a visited/frequency bit, no
  list surgery); **W-TinyLFU** adds a Count-Min frequency sketch as an admission filter. Size-aware
  GDSF (ADR-0012) layers on top for the whole-object tiers.
  **(Status 2026-06-10: S3-FIFO is implemented and wired into both the RAM-head cache and the
  object-count bound; SIEVE / W-TinyLFU are postponed — `--eviction` accepts only `s3fifo` for now.)**

## Consequences
- ➕ Lock-light hits; scan resistance; byte-correct value; benchmarkable; exploits "few large items."
- ➖ More moving parts than LRU — mitigated by shipping SIEVE as a simple baseline.

## References
S3-FIFO (Yang et al., SOSP 2023) · SIEVE (Zhang et al., NSDI 2024) · GreedyDual-Size / GDSF
(Cao & Irani 1997; Cherkasova 1998) · W-TinyLFU (Caffeine) as an alternative admission filter.
