# Goblin Store versus BlobDB object-cache benchmark

`bench_goblin_and_blobdb` compares Goblin Store's embedded C++ storage API with RocksDB's integrated
BlobDB C++ API under explicit application-memory budgets on the same machine and backing filesystem.
Neither side crosses a socket or parses a network protocol. The default budget is
1640 MiB (approximately 1.6 GiB) of Goblin packed-small-object RAM plus 4300 MiB (approximately
4.2 GiB) of Goblin fixed-head RAM. BlobDB receives their 5940 MiB sum as one combined block cache,
blob cache, and memtable budget. The benchmark does not impose a host-memory or cgroup limit. The
host must have an adequate `RLIMIT_MEMLOCK` for Goblin's pools.

The benchmark is opt-in because the RocksDB library that provides integrated BlobDB is not a Goblin
Store dependency:

```sh
sudo apt install librocksdb-dev
cmake -S . -B build-rocks-bench -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DGOBLIN_BUILD_BLOBDB_BENCHMARK=ON
cmake --build build-rocks-bench --target bench_goblin_and_blobdb
```

Run it with an EBS-backed source tree, an empty-space parent for disposable databases, a unique
list of keys to preload, and the ordered list of keys to retrieve:

```sh
./build-rocks-bench/bench_goblin_and_blobdb \
    --data /mnt/ebs/source \
    --scratch /mnt/ebs/benchmark-scratch \
    --prefetch /mnt/ebs/prefetch.txt \
    --schedule /mnt/ebs/order.txt \
    --goblin-small-memory 1640M \
    --goblin-large-memory 4300M \
    --goblin-head-size 16K \
    --goblin-read-chunk 4M \
    --goblin-file-handles 262144 \
    --blobdb-buffer 5940M \
    --blobdb-min-blob-size 16K \
    --blobdb-cache-shard-bits 4
```

`--source` aliases `--data`, and `--keys` aliases `--schedule`. A leading `/` in either key-list row
is removed. Keys are storage keys rather than memcache wire keys, so the memcache protocol's
250-byte and character restrictions do not apply. Every listed key must name a regular file under
`--data`, every scheduled
key must occur in `--prefetch`, and repeated schedule keys retain their exact order. Duplicate
prefetch keys are rejected so population counts and timing remain unambiguous.

`--goblin-head-size` controls the resident head used for this run; it does not change Goblin Store.
`--goblin-read-chunk` controls the embedded reader's aligned disk-read quantum and defaults to 4 MiB
for this comparison. `--goblin-file-handles` controls Goblin's shared immutable-generation descriptor
cache and defaults to 262,144 entries—large enough to avoid descriptor churn in the reference
373,165-object corpus. Both values must be powers of two. Before the Goblin case, the harness raises
its soft `RLIMIT_NOFILE` to the cache capacity plus 4,096 descriptors. It fails with the required
`ulimit -n` command if the account's hard limit is lower; it never changes the hard limit.
The harness also counts prefetched objects larger than the Goblin head and refuses to run the Goblin
case when the cache cannot hold all of their descriptors.
Before starting an engine, the harness computes the compact small-object footprint and fixed-head
footprint of the complete prefetch list. It refuses a configuration that cannot keep every
prefetched key resident or disk-addressable within the two requested Goblin pools. This prevents an
undersized head geometry from silently turning scheduled reads into cache misses.

## What one invocation does

The harness inventories all regular source files, resolves `--prefetch`, and gives both products
the exact population order from that file. It produces three read datasets:

1. Goblin Store using its normal `O_DIRECT` backing-file path.
2. Integrated BlobDB with buffered SST and blob-file reads.
3. Integrated BlobDB with `use_direct_reads=true`.

Each dataset starts with a fresh database. The harness loads every object named by `--prefetch`
itself: the embedded streaming writer for Goblin and `DB::Put` for each BlobDB configuration. In other words, both
products are preloaded by the benchmark, and the preload time is measured rather than inferred.
Goblin population ends after the last embedded writer commits. BlobDB uses no WAL because both stores are
disposable caches; each BlobDB population measurement includes a final synchronous `Flush`, so the
subsequent read phase is genuinely reading SST and blob files.
The harness then waits for currently running RocksDB background work and pauses new compactions for
the read phase, keeping background-job timing from contaminating one engine's latency distribution.
After population, one reusable Goblin reader or one BlobDB handle retrieves schedule keys strictly
one at a time. Goblin's per-reader materialization buffer starts empty, grows only when an encountered
object does not fit, and is retained between requests; the benchmark does not preallocate for the
largest corpus object. The RAM head is copied into that final buffer and disk reads target their final
offsets directly. The harness records the interval from immediately before the storage lookup to the
first completed in-place piece and to completion of the full materialized value.
BlobDB's `DB::Get` is synchronous and materializes the complete value before returning, so its
observable first-byte time is necessarily the same as its complete-value time. That distinction is
intentional: Goblin can expose byte zero while BlobDB is still waiting for the final byte. Requests
never overlap.

The Goblin reader owns an aligned reusable buffer and a per-reader `io_uring` when the build and
kernel permit it; otherwise it falls back to aligned `pread`. The first callback receives a pinned
RAM head before tail I/O is awaited. Read-only per-generation file descriptors are reused through
Goblin's bounded CLOCK cache rather than reopened for every scheduled GET. Population likewise
streams source chunks into a bounded
Goblin writer instead of materializing the complete object.

BlobDB's point-write API accepts a complete value, so the harness materializes one source object at
a time while populating it. Peak process memory can therefore exceed `--blobdb-buffer` by roughly
the largest source object plus allocator overhead. This is normally negligible for a small-object
corpus; it must be included in the memory plan for multi-gigabyte individual values.

The embedded Goblin handle is destroyed before BlobDB starts, and its database is deleted before BlobDB receives the
machine's memory. Values of at least `--blobdb-min-blob-size` are separated into blob files. One
strict-capacity cache is shared by SST data, blobs, indexes, and filters. A `WriteBufferManager`
charges memtable allocations against that cache, avoiding an accidental second BlobDB memory
allowance. The default 16 cache shards give each shard enough room for this corpus's largest value;
the former automatic 64-shard geometry did not. The harness validates this relationship before
starting BlobDB and reports a configuration error if the largest selected object cannot fit one
strict cache shard. This follows RocksDB's documented distinction
between [block-cache memory](https://github.com/facebook/rocksdb/wiki/Block-Cache) and
[memtable memory](https://github.com/facebook/rocksdb/wiki/Memory-usage-in-RocksDB).

By default the harness applies `POSIX_FADV_DONTNEED` to every selected source file before each
population phase and again after consuming each file. This prevents the first engine from inheriting
an accidentally warm source corpus and prevents later engines from receiving a source-data
page-cache advantage. Use `--keep-source-cache` to disable this. Buffered database reads
intentionally use the kernel page cache and can therefore consume memory outside either explicit
application budget; direct mode is the controlled-cache comparison.

## Artifacts

The default output is a timestamped directory in the current working directory. Override it with
`--output DIR`. It contains:

- `reads.csv`: engine, I/O mode, schedule sequence, key, expected/returned bytes, time to first byte
  in nanoseconds, time to complete response/value in nanoseconds, and status.
- `summary.csv`: population time, object/byte counts, complete read-phase time, requests, and bytes.
- `metadata.txt`: resolved budgets, BlobDB settings, paths, clock, RocksDB library version, and
  source/schedule dimensions.

Disposable databases live only in fixed `goblin-*` and `blobdb-*` children of `--scratch` and are
removed after their case. `--keep-databases` retains them for inspection.

`--case goblin`, `--case blobdb-buffered`, and `--case blobdb-direct` run one fresh case in
isolation. This is useful for retrying a failed engine without repeating already valid measurements;
the default `--case all` retains the three-case lifecycle above.
