# ADR-0009: Drive pools — per-object files, round-robin striping, short-read EOF

**Status:** Accepted (2026-06-09) — revisable. (The one open fork: **per-object files** vs a
**packed-extent** allocator. Defaulting to per-object files — it's what the short-read-EOF
mechanic implies and it's the simplest thing that works.)

## Context
How object bytes map onto a multi-drive tier pool, how reads parallelize, and how we detect
end-of-object.

## Decision
- A tier pool (the SSD pool, the HDD pool) is a **list of drive directories** — one per drive.
- Each object occupies **one file per directory** in the pool (named by key hash). Bytes are
  **round-robin striped** across those `N` files by a **stripe unit `S`**:

  > logical chunk `i = offset / S`  →  file `i mod N`,  at in-file position `(i / N) * S`.

  (RAID-0 layout: chunk 0 → file 0, chunk 1 → file 1, …, chunk N → file 0 row 2, …)
- **Stripe unit `S` is configurable, `≥ 1 page`.** Recommend **larger units on the HDD pool**
  (e.g. 1–8 MB) to amortize seeks — ties directly to the shock-absorber law in [ADR-0006](0006-positional-tiering-pipeline.md);
  the SSD pool can use small units (page–128 KB) since SSDs don't seek.
- **Parallel reads:** the connection-owning core ([ADR-0001](0001-thread-per-core-concurrency.md)/[ADR-0002](0002-iouring-rings-shared-drives.md))
  issues one read per file for a full **stripe row** at once on its own ring → N-way parallelism.
- **EOF detection:** the **first constituent file that returns a short read** (in round-robin
  scan order) marks end-of-object; later files in that row read empty. Cross-checked against the
  size recorded in the index.
- **Delete = `unlink` the files.** In 3-layer, an object has SSD-pool files for `[0, ssd_prefix)`
  and HDD-pool files for the tail; each pool is striped independently.

## Consequences
- ➕ Dead-simple placement, parallel reads, trivial delete, self-describing EOF; resurrects junk drives with no NAS/SAN.
- ➖ Many small files at scale + per-object `open()` cost → revisit packed-extent allocation if file count or open latency bites.

## Refinements (2026-06-09 — many objects only partly fill the pool)
- **Rotate the start drive per object:** `chunk i → file (hash(key) + i) mod N`. Without
  rotation every object's chunk 0 lands on drive 0 — and when most objects are sub-stripe (see
  [ADR-0012](0012-multi-resource-eviction.md)), drive 0 becomes a hotspot. Rotation spreads
  small objects across the pool.
- **Sub-stripe-unit objects are stored as a single, unstriped file** — no value in striping data
  smaller than one stripe unit, and it avoids creating empty constituent files.
- **Create only `min(N, ceil(size / S))` files**, never empty ones. The index records exact
  per-file lengths, so reads go only to files that hold data; short-read EOF remains the cross-check.

## Goal: one read saturates the whole pool (2026-06-09)
**Priority (operator):** a single object read must use the full *aggregate* bandwidth of all
drives in its tier. So every object is striped RAID-0 across **all** N drives in its pool, and
the read-ahead pipeline ([ADR-0006](0006-positional-tiering-pipeline.md)) prefetches in **full
stripe rows**.

Grid layout for an object's tier-local bytes (pool of N drives, stripe unit S):
- `chunk c = offset / S`,  `row = c / N`
- **drive = `(hash(key) + c) % N`**  — consecutive chunks land on consecutive drives
- **in-file offset = `row * S + (offset % S)`**

A read of one row (`N × S` bytes) therefore issues N parallel per-drive reads and lights up
every drive at once. `drive_pool` exposes `plan_reads(key, tier_offset, len) → [{drive,
file_offset, length}]`; the pipeline calls it in `N × S` rows.

**Stripe unit is per-pool and does NOT fight [ADR-0006](0006-positional-tiering-pipeline.md):**
SSD pool uses a small S (e.g. 64 KB) so even modest reads span all drives; HDD pool uses a large
S (MBs) for seek amortization — a full-row read is still `N × S`, so each HDD gets a coarse
sequential chunk. Across a whole object stream both pools run concurrently (SSD serves the prefix
while HDD prefetches the tail). Tiny objects (≤ `ram_head`) are served from RAM, so
striping-for-bandwidth applies to the medium/large objects that actually hit disk.

**Trade-off:** striping every object across every spindle maximizes single-stream bandwidth at
the cost of concurrent-stream isolation (every read touches every drive). The large HDD stripe
unit + SSD buffering bound the resulting seek pressure; stripe unit is the knob if concurrent
throughput must ever be traded against single-stream latency.

## File names & the open path (2026-06-09)
- Per-object files are named by the **hex** of the digest
  ([ADR-0014](0014-keyless-digest-identity.md)): the same `<64-hex>` name in each drive directory
  (the directory identifies the drive). Because we have **few, large** objects, a **flat
  directory** is fine — no git-style `ab/cd/` fan-out — so resolution is a single component.
- **Open via a long-lived per-drive `dirfd` + `openat`:** open each drive directory once at
  startup (`O_DIRECTORY`), then `openat(drive_dirfd, hex, O_RDONLY|O_DIRECT)` per object. The
  kernel starts at the pinned directory and looks up only the final hex component — no re-walking
  the `/data/ssdN/goblin-store/…` parent chain on every open. (Modern path-walk is RCU-lockless,
  so the win is *less work per open*, not lock relief; biggest for frequent opens / small objects,
  negligible for a 1 GB stream — but free.)
- **With io_uring:** `IORING_OP_OPENAT` (async open via the dirfd) + **registered fixed-files**
  (`IOSQE_FIXED_FILE`, [ADR-0002](0002-iouring-rings-shared-drives.md)) so reads skip the per-op fd
  refcount lookup; a bounded **hot-object fd cache** (feasible because objects are few) avoids
  re-opening on repeat reads. `openat2(RESOLVE_NO_SYMLINKS)` is cheap defense-in-depth (our names
  are never symlinks).
