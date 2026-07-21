# ADR-0012: Multi-resource eviction — head, object-count, and filesystem pressure

**Status:** Accepted (2026-06-09), revised for the implemented v1 on 2026-07-15. Complements
[ADR-0007](0007-eviction-policy.md) (the *value/recency* model); this ADR is the structure — which
resource, which valve.

## Context
Eviction is a **multi-resource (vector)** problem. Positional tiering ([ADR-0006](0006-positional-tiering-pipeline.md))
gives each object a structured footprint, and the per-object cost differs by **size regime**
(example `ram_head`=256 KB, `ssd_prefix`=32 MB):

| Regime | RAM | SSD | HDD |
|---|---|---|---|
| tiny (`size ≤ ram_head`) | whole object | 0 | 0 |
| small/med (`ram_head < size ≤ ssd_prefix`) | head | `size` — **partly fills** | 0 |
| large (`size > ssd_prefix`) | head | `ssd_prefix` (full) | `size − ssd_prefix` (the bulk) |

The table is the three-layer layout. In two-layer mode every disk-backed object's complete value is
on SSD. RAM-only objects have no disk file; evicting their head therefore evicts the object.

## Decision
The implemented v1 has three synchronous pressure paths:

1. **RAM-head pressure.** Head admission first compacts fragmented small-object arenas where useful,
   then asks the corresponding resident S3-FIFO policy for victims. With an explicit
   `--small-memory` split, fixed heads and packed small objects have independent victim policies;
   neither policy evicts across the pool boundary. A disk-backed object loses only its cached head
   and remains readable from disk. A RAM-only object's head is authoritative, so the entire object
   is removed. This path is independent of `WriteMode`.
2. **Global object-count pressure.** Explicit `--max-objects N` bounds committed disk-backed
   objects. After commit, the global object policy removes whole objects until it is within the
   bound. Zero means unbounded. This path is also independent of `WriteMode`; there is no derived
   SSD-capacity default.
3. **Filesystem-capacity pressure.** Each distinct pool-directory `st_dev` has its own victim
   policy. `openat`, `fallocate`, or `pwrite` identifies the failed capacity domain. An EVICT
   admission first tries expired objects on that filesystem, then removes filesystem-local whole
   objects one at a time and retries the real allocation. A BLOCK admission returns
   `out_of_space` without entering this reclaim loop. See
   [ADR-0010](0010-write-admission-modes.md).

TTL expiry is additionally reclaimed by the periodic reaper and lazy-hidden on reads. Live byte
and inode watermarks drive bounded background high-to-low reclaim through the same filesystem-local
policies. SSD-to-HDD demotion and hit-driven re-promotion remain possible refinements. Primary placement remains positional
([ADR-0006](0006-positional-tiering-pipeline.md)).

## Consequences
- ➕ RAM-head eviction can preserve disk-backed objects; filesystem-local policies do not evict
  objects that cannot relieve the failed mount.
- ➕ Real allocation retry remains correct in the presence of concurrent writers, unrelated
  filesystem users, and open-but-unlinked reader generations.
- ➖ Whole-object filesystem reclaim may free shards on additional mounts occupied by the same
  object. Tier demotion remains future work.
