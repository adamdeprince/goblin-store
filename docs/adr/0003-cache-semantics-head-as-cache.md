# ADR-0003: Best-effort cache semantics; RAM head = write-through cache of an SSD-resident prefix

**Status:** Accepted (2026-06-09) — revisable.

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
- Duplicate cost (head in RAM *and* on SSD) is `ram_head` per object (e.g. 256 KB) — negligible.

## Consequences
- ➕ No fsync on the write path; eviction is cheap; matches the "cache" positioning; deterministic placement, no temperature migration.
- ➖ Not durable — a crash loses unflushed objects. Acceptable for a cache; revisit for a store mode. **v1 goes further: no persistence at all — blank slate every boot (see [ADR-0013](0013-ephemeral-wipe-on-start.md)).**
