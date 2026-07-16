# ADR-0010: Write/upload admission — EVICT vs BLOCK on disk exhaustion

**Status:** Accepted (2026-06-09), revised 2026-07-15.

## Context
[ADR-0003](0003-cache-semantics-head-as-cache.md) makes the default a best-effort cache that
evicts to admit. Some producers should be allowed to reclaim disk objects when their own admission
hits `ENOSPC`; others, such as startup source loading, should report that the content did not fit.
This is a decision for one write attempt, not a persistent classification of the stored object.

## Decision
Every `begin_store`/`store` admission carries one **write mode**. `StoreHandle` captures it for the
entire write, including any late streaming retry, so EVICT and BLOCK admissions can safely coexist
in one `TierManager`. Memcache passes its configured mode (default EVICT); the startup `--source`
loader explicitly passes the configured HTTP write mode (default BLOCK). The HTTP serving surface
is read-only.

- **EVICT — cache admission (default on the memcache path).** On filesystem exhaustion, use the
  failed filesystem's capacity victim policy
  ([ADR-0007](0007-eviction-policy.md)/[ADR-0012](0012-multi-resource-eviction.md)) to make room.
  Best-effort; existing disk-backed objects may disappear.
- **BLOCK — no capacity reclaim (the configured `--source` default).** If file creation or physical
  allocation reports exhaustion, fail this store with `out_of_space` and remove its unpublished
  generation. A future writable HTTP surface should translate this to
  **`507 Insufficient Storage`**. Despite the name, BLOCK does not wait indefinitely and does not
  pin existing or newly stored objects.

### Cross-cutting rules (both modes)
- **Immutable generations and one publication point.** A disk-backed store creates its SSD/HDD
  shards as `<64-hex-digest>.g<generation>` with `O_EXCL`. These private files are never renamed
  over the current object. After the complete body and tail padding have been written, one Index
  metadata swap publishes the new `file_generation`. Readers capture the head locator and open
  generation descriptors together; descriptors already held by older readers keep that complete
  retired incarnation alive after its names are unlinked. An abort unlinks only the unpublished
  generation. Thus a failed or concurrent replacement cannot expose a mixed or partial object.
- **Reserve the known layout on the files that will hold it.** memcache supplies the complete value
  length before admission. On Linux, after opening the private generation, Goblin computes the
  exact final extent of every stripe shard from the object digest, pool stripe geometry, and tier
  length. It includes the final 4 KiB `O_DIRECT` padding where required, then calls
  `fallocate(fd, FALLOC_FL_KEEP_SIZE, 0, shard_extent)` on each actual shard. `KEEP_SIZE` reserves
  blocks without changing the file length during admission; the Index remains the authority for
  the protocol-visible logical length.
- **Unsupported reservation falls back safely.** `EOPNOTSUPP`, `ENOSYS`, and the unsupported-mode
  form of `EINVAL` mean that shard proceeds with checked streaming `pwrite`. `ENOSPC` and `EDQUOT`
  always mean real capacity exhaustion. On fallback filesystems a write can therefore fail between
  blocks. Goblin retains the complete aligned staging block and, in EVICT mode, replays that block
  after reclaim; replay is safe because the generation is private.
- **Capacity victims are filesystem-local.** At startup Goblin records `st_dev` for every pool
  directory and maintains one capacity victim policy per distinct filesystem. A committed object
  participates in every such policy whose drives contain one of its shards. The failing
  `openat`/`fallocate`/`pwrite` reports the exact `st_dev`, and capacity reclaim considers only
  expired objects and policy victims that occupy that filesystem. Thus a full HDD mount cannot
  evict an SSD-only object on another `st_dev`. The separate global whole-object policy exists only
  to enforce the configured object-count bound; it is not the disk-full victim source.
- **EVICT slow paths are serialized.** The first file creation, reservation, or `pwrite` attempt is
  concurrent. After an out-of-space result, one capacity coordinator serializes writers, retries
  before evicting, reclaims one expired object or filesystem-local victim, and retries after each
  victim. Other writers may have reclaimed space while this writer waited, and an unrelated
  filesystem user may allocate or release blocks at any time, so successful
  `openat`/`fallocate`/`pwrite` retry—not a free-space estimate—is the truth. The loop stops on
  success or when no eligible object occupies the failed filesystem.
- **BLOCK never enters the eviction loop.** It returns the first `ENOSPC`/`EDQUOT` result from
  file creation, reservation, or streaming write. The old indexed incarnation remains intact and
  the failed private generation is discarded.
- **The mode governs filesystem exhaustion only.** RAM-head allocation follows the ordinary head
  eviction/compaction policy in both modes. The explicit global `--max-objects` bound is also
  independent and may evict after a successful BLOCK admission. Once committed, a BLOCK-created
  disk-backed object carries no durable or pinned label; a later EVICT admission may select it as a
  capacity victim.
- **Check both pools (3-layer):** an object needs SSD-prefix space **and** HDD-tail space.
- **Crash durability (fsync) is a separate axis.** Neither mode is a durable store mode
  ([ADR-0003](0003-cache-semantics-head-as-cache.md)).

## Consequences
- ➕ Known-size writes normally discover insufficient SSD/HDD capacity before accepting body
  bytes, without racing a separate `statvfs` check.
- ➕ Immutable names plus a single Index swap keep same-key readers and concurrent writers on
  complete incarnations; no cross-drive rename transaction is required.
- ➕ EVICT coordinates only the exceptional full-filesystem path; ordinary writers remain
  concurrent. Per-admission BLOCK gives deterministic no-capacity-reclaim failure semantics without
  changing simultaneous admissions or the eventual eviction eligibility of committed objects.
- ➖ An unlinked retired generation still consumes blocks while a reader has it open. A retry is
  deliberately authoritative: it may need another victim or may still fail.
- ➖ Filesystems without usable `fallocate` cannot promise admission up front, so exhaustion can
  still surface during streaming. The private-block replay path is required for correctness there.

The operational flow and concurrency caveats are summarized in
[Full-filesystem writes](../full-filesystem-writes.md).
