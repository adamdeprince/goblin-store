# ADR-0009: Drive pools — immutable per-object files and rotated round-robin striping

**Status:** Accepted (2026-06-09) — revisable. (The one open fork: **per-object files** vs a
**packed-extent** allocator. Defaulting to per-object files because it is the simplest thing that
works.)

## Context
How object bytes map onto a multi-drive tier pool, how reads parallelize, and how a live immutable
generation is opened.

## Decision
- A tier pool (the SSD pool, the HDD pool) is a **list of drive directories** — one per drive.
- Each disk incarnation occupies **one immutable generation file per used directory** in the pool,
  named `<64-hex-digest>.g<generation>`. Bytes are
  **round-robin striped** across those `N` files by a **stripe unit `S`**:

  > logical chunk `i = offset / S` → drive `(hash(key) + i) mod N`, at in-file position
  > `(i / N) * S`.

  (For start drive `h = hash(key) mod N`: chunk 0 → drive `h`, chunk 1 → drive
  `(h + 1) mod N`, …, chunk N → drive `h` in row 2.)
- **Stripe unit `S` is configurable as a nonzero multiple of 4 KiB.** Recommend **larger units on
  the HDD pool** (e.g. 1–8 MB) to amortize seeks — ties directly to the shock-absorber law in
  [ADR-0006](0006-positional-tiering-pipeline.md); the SSD pool can use small units (page–128 KB)
  since SSDs don't seek.
- **Parallel reads:** for each bounded streaming piece, the connection-owning core
  ([ADR-0001](0001-thread-per-core-concurrency.md)/[ADR-0002](0002-iouring-rings-shared-drives.md))
  plans every stripe segment and queues those reads together on its own ring. A piece that spans
  several drives therefore overlaps them.
- **Length and short reads:** `ObjectMeta::size` is authoritative. The reader plans only the
  requested logical range; a constituent-file short read is an I/O error, not an EOF-discovery
  mechanism.
- **Delete = `unlink` that generation's files.** In 3-layer, an object generation has SSD-pool
  files for `[0, ssd_prefix)` and HDD-pool files for the tail; each pool is striped independently.
  Open descriptors continue to pin a retired incarnation for readers already using it.

## Consequences
- ➕ Dead-simple placement, parallel reads, and trivial delete; resurrects junk drives with no NAS/SAN.
- ➖ Many small files at scale + per-object `open()` cost → revisit packed-extent allocation if file count or open latency bites.

## Refinements (2026-06-09 — many objects only partly fill the pool)
- **Rotate the start drive per object:** `chunk i → file (hash(key) + i) mod N`. Without
  rotation every object's chunk 0 lands on drive 0 — and when most objects are sub-stripe (see
  [ADR-0012](0012-multi-resource-eviction.md)), drive 0 becomes a hotspot. Rotation spreads
  small objects across the pool.
- **Sub-stripe-unit objects are stored as a single, unstriped file** — no value in striping data
  smaller than one stripe unit, and it avoids creating empty constituent files.
- **Create only `min(N, ceil(size / S))` files**, never empty ones. Exact per-drive extents are
  recomputed in O(1) from the digest, logical tier length, and stripe geometry; they are not stored
  separately in the index.

## Striping goal: one large stream can use the whole pool (2026-06-09)
**Priority (operator):** a sufficiently large object should use the full *aggregate* bandwidth of
all drives in its tier. Objects with at least `N` stripe chunks occupy all `N` drives; smaller
objects deliberately use fewer files. The bounded read pipeline
([ADR-0006](0006-positional-tiering-pipeline.md)) queues every segment in its current piece, so
`--io-chunk`, stripe size, and drive count determine how many drives one batch reaches.

Grid layout for an object's tier-local bytes (pool of N drives, stripe unit S):
- `chunk c = offset / S`,  `row = c / N`
- **drive = `(hash(key) + c) % N`**  — consecutive chunks land on consecutive drives
- **in-file offset = `row * S + (offset % S)`**

A range covering one row (`N × S` bytes) therefore plans N per-drive reads. `DrivePool` exposes
`plan_reads(key, tier_offset, len) → [{drive, file_offset, length}]`; the current pipeline calls it
for bounded `--io-chunk` pieces rather than forcing every piece to one full row.

**Stripe unit is per-pool and does NOT fight [ADR-0006](0006-positional-tiering-pipeline.md):**
SSD pool uses a small S (e.g. 64 KB) so modest pieces can span several drives; HDD pool uses a large
S (MBs) for seek amortization. The stream remains positional: it sends the RAM head, then reads the
SSD bytes, then the optional HDD tail. Two read buffers can overlap reading the next piece with
sending the current one; they do not dispatch the SSD and HDD tiers simultaneously. Tiny objects
(≤ `ram_head`) are served from RAM, so
striping-for-bandwidth applies to the medium/large objects that actually hit disk.

**Trade-off:** striping sufficiently large objects across every spindle maximizes their available
single-stream bandwidth at the cost of concurrent-stream isolation when a read batch spans the
whole pool. The large HDD stripe unit bounds seek pressure; stripe and I/O-chunk sizes are the knobs
if concurrent throughput must ever be traded against single-stream bandwidth.

## File names & the open path (revised 2026-07-15)
- Per-object files use an immutable **digest + generation** name
  ([ADR-0014](0014-keyless-digest-identity.md)): the same
  `<64-hex-digest>.g<file_generation>` name in each used drive directory (the directory identifies
  the drive). `ObjectMeta` records the live generation; the stripe layout and set of used drives
  are recomputed from the digest and logical size. Because we have **few, large** objects, a **flat
  directory** is fine — no git-style `ab/cd/` fan-out — so resolution is a single component.
- **Open via a long-lived per-drive `dirfd` + `openat`:** open each drive directory once at
  startup (`O_DIRECTORY`), then `openat(drive_dirfd, generation_name, O_RDONLY)` per object, adding
  `O_DIRECT` in the default cache-bypass mode. New generations use `O_CREAT|O_EXCL`; they are never
  opened as an old file and never renamed over one. The kernel starts at the pinned directory and
  looks up only the final component — no re-walking the `/data/ssdN/goblin-store/…` parent chain on
  every open. (Modern path-walk is RCU-lockless,
  so the win is *less work per open*, not lock relief; biggest for frequent opens / small objects,
  negligible for a 1 GB stream — but free.)
- **Publish is one in-memory operation:** after every shard is complete, one Index metadata swap
  changes the live `file_generation`. The retired generation names are then unlinked. A reader that
  opened the old names before the swap keeps a complete old incarnation through its descriptors.
- **Current open/read path:** `openat` is synchronous under the storage snapshot lock; the opened
  generation descriptors then live with that read and its data reads use the worker's io_uring.
  `IORING_OP_OPENAT`, registered fixed files, a bounded hot-object fd cache, and
  `openat2(RESOLVE_NO_SYMLINKS)` remain possible follow-up optimizations.
