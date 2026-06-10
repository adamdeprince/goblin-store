# ADR-0012: Multi-resource eviction — per-resource relief valves & the demotion ladder

**Status:** Accepted (2026-06-09) — revisable. Complements [ADR-0007](0007-eviction-policy.md)
(the *value/recency* model); this ADR is the *structure* — which resource, which valve.

## Context
Eviction is a **multi-resource (vector)** problem. Positional tiering ([ADR-0006](0006-positional-tiering-pipeline.md))
gives each object a structured footprint, and the per-object cost differs by **size regime**
(example `ram_head`=256 KB, `ssd_prefix`=32 MB):

| Regime | RAM | SSD | HDD |
|---|---|---|---|
| tiny (`size ≤ ram_head`) | whole object | `size` (backing copy) | 0 |
| small/med (`ram_head < size ≤ ssd_prefix`) | head | `size` — **partly fills** | 0 |
| large (`size > ssd_prefix`) | head | `ssd_prefix` (full) | `size − ssd_prefix` (the bulk) |

So **SSD bytes = `min(size, ssd_prefix)`** — uniform only for large objects; the *many* small
files use little SSD each. RAM ≈ a free-to-drop uniform knob; HDD ≈ the variable bulk.

## Decision
**Three independent, watermarked pressure controllers sharing one value model (ADR-0007).
Reclaim in the background** (never on the serving path), from high-watermark down to low.

**Relief valves** (cheapest → most destructive):
1. **Drop RAM head** (RAM→∅) — free, reversible; the RAM controller's only valve.
2. **Demote SSD prefix → HDD** (object goes fully cold) — frees SSD, spends HDD + migration I/O; reversible (a hit re-promotes); **3-layer only**.
3. **Evict whole object** — frees RAM+SSD+HDD; irreversible; the HDD controller's main valve.

**Demotion ladder:** `hot (RAM+SSD+HDD) → [RAM pressure] cool → [SSD pressure] cold (HDD only)
→ [HDD pressure] evicted`; a hit promotes back up. Primary placement stays positional
(ADR-0006) — this is only the pressure-relief overlay.

**Per controller:**
- **RAM** → valve 1; free; rank heads by recency/freq (SIEVE/CLOCK); optimize TTFB hit-rate.
- **SSD → count-bound for v1** (operator decision 2026-06-09). Trigger on **object count**, not
  byte-precise accounting. Default limit `= ssd_capacity / ssd_prefix` (safe — never overflows
  even if every object is large). Reuse the per-pool **byte counter already kept for BLOCK-mode
  reservation ([ADR-0010](0010-write-admission-modes.md)) as a free hard ceiling.** On trip:
  **demote** (valve 2) if HDD has room, else **evict** (valve 3); victims = lowest value.
- **HDD** → valve 3, **size-aware** (`value / hdd_bytes` → big cold tails first).

**Coupling:** valve 2 spends HDD, so under simultaneous SSD+HDD pressure the SSD controller
falls through to eviction. In **BLOCK mode** (ADR-0010) write-side refusal replaces eviction.
**TTL-expired objects are swept first** (free relief).

## Consequences
- ➕ Each pressure source relieved by the right valve at minimum value-loss; count-bound SSD is simple and ships now.
- ➖ **Count-bound under-utilizes SSD when many objects are small** (the partly-fill case): the count cap can trip with SSD bytes mostly free. Accepted for v1; the byte ceiling prevents the opposite (overflow). Revisit with byte accounting / small-object packing.
