# Full-filesystem writes: reservation, reclaim, and publication

Goblin Store treats filesystem capacity as a concurrent external resource. Several stores can be
in flight, other processes can consume the same filesystem, and unlinking a file does not release
its blocks until every open reader closes it. A cached free-byte count or `statvfs` sample cannot
make admission correct. The real `openat`, `fallocate`, or `pwrite` result is therefore
authoritative.

## Known-size admission

The memcache protocol supplies the value length before its body. Goblin first computes the RAM,
SSD, and optional HDD layout, opens a new private generation on every used drive as
`<64-hex-digest>.g<generation>` with `O_EXCL`, and reserves the RAM head and write-staging block.

On Linux, each pool computes the exact final extent of each stripe shard. This includes the final
4 KiB alignment padding needed for `O_DIRECT`, but does not reserve empty files. Goblin then calls:

```text
fallocate(fd, FALLOC_FL_KEEP_SIZE, 0, shard_extent)
```

The call reserves physical blocks on the filesystem that will actually hold that shard without
changing its apparent length. In three-tier mode, admission must succeed for both the SSD prefix and
HDD tail. If a later shard fails, aborting the unpublished generation unlinks all shards already
created or reserved for it.

`ENOSPC` and `EDQUOT` mean exhausted capacity. `EOPNOTSUPP`, `ENOSYS`, and an unsupported
`FALLOC_FL_KEEP_SIZE` reported as `EINVAL` mean that the filesystem cannot provide this guarantee;
Goblin continues with checked streaming writes instead. This fallback matters for some network and
older filesystems.

Other failures are not treated as invitations to evict. The failing device records the errno and
hardware-class write failures move it to a read-only or failed state. New disk-backed admissions
whose stripe layout touches that device then fail with `read_only`, rather than deleting healthy
cache entries in a futile retry loop. This keeps disk-full, unsupported reservation, and actual I/O
failure as three separate operational cases.

## EVICT and BLOCK under concurrency

The storage caller selects EVICT or BLOCK for each admission, and the resulting `StoreHandle`
retains that choice through every block flush. EVICT and BLOCK writers may therefore run
concurrently through the same `TierManager`. Memcache passes its configured mode (default EVICT);
startup `--source` preloads explicitly pass the configured HTTP write mode (default BLOCK). HTTP
serving itself is read-only. The choice controls only the response to filesystem exhaustion; it is
not stored in `ObjectMeta`.

The normal file-creation and allocation attempts do not take the capacity coordinator, so writers
remain concurrent while space is available. Only an out-of-space result enters the slow path.

A background maintenance thread uses the same coordinator and filesystem-local policy. It samples
live `fstatvfs` byte and inode availability and, once either use ratio reaches the configurable high
watermark, reclaims toward the low watermark. Work is bounded per pass and repeated at the
configured interval. This is proactive pressure control only: the real allocation/write result
remains authoritative because other processes and open-but-unlinked generations can change space
between samples.

Each pool directory is identified at startup by its `st_dev`. Directories backed by the same mount
share one capacity domain and one victim policy, even if they appear in different tiers; distinct
filesystems have distinct policies. A committed object is inserted into every capacity policy for
which it has a physical shard. RAM-only objects appear in none of them. The separate global
whole-object policy enforces the configured disk-backed-object count bound and is not used to
answer a filesystem-full event.

In **EVICT** mode, one `TierManager`-local coordinator serializes reclaiming writers. The failed
`openat`, `fallocate`, or `pwrite` identifies the `st_dev` of its drive. A writer takes the
coordinator and retries the real operation before evicting: another Goblin writer or an unrelated
filesystem user may have changed available space while it waited. If the retry still fails, Goblin
first prefers an expired disk-backed object that occupies that filesystem, otherwise asks that
filesystem's capacity policy for a victim. It removes one complete object and retries. Success ends
the loop; exhaustion of eligible victims on the failed filesystem returns the capacity error. A
full HDD mount therefore cannot evict an SSD-only object on another `st_dev`.

For a **BLOCK** admission, Goblin does not enter that eviction loop. File creation, reservation, or a
streaming write that reports out of space fails the new store. The current indexed object, if any,
remains unchanged; the unpublished generation and its private RAM head are discarded. The caller
receives `out_of_space`. A future writable HTTP surface should map that error to
`507 Insufficient Storage`.

BLOCK does not make an object durable or permanently non-evictable. RAM-head allocation and
compaction use the same policy for both admission modes, and the global `--max-objects` bound is
enforced independently. After commit there is no BLOCK marker on the object: a later EVICT
admission can choose a disk-backed object from the failed filesystem's capacity policy. The mode
means only “do not reclaim disk objects for this admission's `ENOSPC`.”

On a filesystem without usable `fallocate`, capacity can run out after part of a body has arrived.
Goblin keeps the complete aligned staging block until it is written successfully. An EVICT
admission pauses that writer at the failed block, enters the same serialized reclaim/retry loop, and
replays the whole block. Replaying cannot damage a visible value because the destination generation
is still private. A BLOCK admission fails and aborts it instead.

## Atomic visibility and delayed reclamation

The generation files are immutable after completion and are never renamed over a live object. When
all bytes are present, one in-memory Index swap publishes the new `file_generation`, metadata, and
RAM-head locator together. A reader captures that generation and opens its descriptors under the
same storage snapshot. Replacement then unlinks the old generation names, but readers that already
opened them keep the old inodes—and their blocks—until those descriptors close. Readers therefore
see one complete incarnation, while the allocation retry correctly observes whether eviction has
actually made enough space.

Capacity selection is local to the failed `st_dev`, but deletion remains whole-object: reclaiming an
object to free one full mount also removes its shards from any other mount it spans. This preserves
Goblin's one-key/one-object semantics and updates every capacity policy together. Open readers can
still delay the physical block release, which is why each retry remains the final authority.

The same generation identity makes tail-read quarantine safe. A failed or short asynchronous read
records the exact device error and removes only the generation captured by that reader. If a
concurrent store has already published a newer generation, quarantine observes the mismatch and
leaves it untouched. Existing readers retain their open descriptor while the poisoned pathname is
unlinked; new lookups miss instead of repeatedly serving a known-bad object.

See [ADR-0010](adr/0010-write-admission-modes.md) for the admission decision and
[ADR-0018](adr/0018-concurrency-model.md) for the read/write publication model.
