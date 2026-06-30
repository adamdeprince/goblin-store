# ADR-0003: Best-effort cache semantics; RAM head = write-through cache of an SSD-resident prefix

**Status:** Accepted (2026-06-09); **revised 2026-06-29** — for objects that fit in the head
(`size ≤ ram_head`) the RAM head is now the *sole* copy (no SSD duplicate); see the **Revision** section below.

## Context
The name says both "store" and "cache." Two questions decide the write path, durability, and
eviction: (1) cache or store? (2) is the RAM head the *sole* copy of bytes `[0, head)`, or a
*cache* of bytes also persisted below?

## Decision
**Best-effort CACHE semantics in v1.** Objects are evictable; **no fsync, no durability
guarantee**. (A durable "store" mode can be added later as an explicit option.)

**Persistent layout (the source of truth):**
- *2-layer:* the entire object `[0, size)` lives on the SSD pool.
- *3-layer:* `[0, ssd_prefix)` lives on the SSD pool; `[ssd_prefix, size)` lives on the HDD
  pool. `ssd_prefix = ram_head + ssd_middle` (operator-configured).

**The RAM head is a WRITE-THROUGH CACHE** of the first `ram_head` bytes, which also physically
reside at the front of the SSD prefix. Therefore:
- Evicting a RAM head is **free** — the bytes already exist on SSD.
- A RAM-head miss costs **one SSD read** of the head — still far better TTFB than reading a whole value.
- **Whole-object eviction** reclaims RAM + SSD + HDD together.
- Duplicate cost (head in RAM *and* on SSD) is `ram_head` per object (e.g. 256 KB) — negligible **for large
  objects**. *(Revised 2026-06-29: for objects ≤ `ram_head` the head IS the whole object, so the duplicate is
  100% — those are now stored RAM-only; see the Revision section.)*

## Consequences
- ➕ No fsync on the write path; eviction is cheap; matches the "cache" positioning; deterministic placement, no temperature migration.
- ➖ Not durable — a crash loses unflushed objects. Acceptable for a cache; revisit for a store mode. **v1 goes further: no persistence at all — blank slate every boot (see [ADR-0013](0013-ephemeral-wipe-on-start.md)).**

## Revision (2026-06-29): objects that fit in the head are RAM-only

The "negligible duplicate" reasoning in the Decision holds for *large* objects — a 256 KiB head on an 8 MiB
value is ~3% overhead. It breaks down when `size ≤ ram_head`: then the head **is** the whole object, so the
"duplicate" is 100% of it. Every byte sits in both RAM and SSD, and the SSD copy buys nothing — the object
already fits in RAM and the disk tiers add no capacity for it. Worse, each such object still cost a
per-object file, which made small-object ingest filesystem-bound.

**So for `size ≤ ram_head` the object is now stored RAM-only** — no SSD/HDD extent, no per-object file. The
RAM head is the **sole, authoritative** copy. Consequences specific to these objects:
- **head-evict = object-evict** — there is no disk copy to fall back to, so evicting the head removes the
  object (vs. the "free, re-read from SSD" eviction the Decision describes for larger objects).
- a head-only object that cannot obtain RAM **backpressures** (`would_block`) rather than indexing a
  body-less entry.

The write-through-cache model in the Decision is **unchanged for objects larger than `ram_head`**: their head
stays a cache of the SSD-resident prefix and evicts for free. (Trimming the head off *large* objects' disk
copy too — persisting only `[ram_head, size)` — was considered but **not** adopted: it would couple the
cacheable object count to RAM-head capacity. A possible future ADR.)

**Impact.** On the small-object benchmark this cut the SSD footprint from 546 MiB to 8 KiB and the per-object
file count from ~137 K to 1, with no latency change. It does **not** change goblin-store's standing vs
memcached on small objects (memcached's slab allocator still wins on RAM and throughput — see
[BENCHMARKS.md](../../BENCHMARKS.md)); it just stops wasting disk on objects that don't need it. Implemented
in `storage/layout.cpp` (`compute_layout`) and `storage/tier_manager.cpp`
(begin_store / write / commit / read / eviction); validated by 115/115 unit tests under Release, ASan, and TSan.
