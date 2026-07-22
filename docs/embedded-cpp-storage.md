# Embedded C++ storage API

Goblin Store can be used as an in-process C++23 storage engine without starting a listener or
speaking memcache/HTTP. Add the repository with `add_subdirectory`, include `<goblin/store.hpp>`, and
link `GoblinStore::storage`. The concrete archive is `libgoblin-store-storage.a`.

The boundary retains the properties that matter to the server data path:

- `StoreWriter` admits a known-size object and accepts bounded sequential pieces, then publishes the
  new generation atomically on `commit()`.
- `StoreReader` is reusable and owns an aligned scratch buffer plus a per-reader `io_uring` when the
  build and kernel support one. Queue creation failure falls back to aligned `pread`.
- `StoreReader::load()` materializes into a separate aligned buffer whose capacity starts at zero,
  grows only when the encountered object does not fit, and is retained between calls. Disk tails are
  read directly into their final offsets; only the resident RAM head is copied.
- A resident object's first streaming callback receives its pinned RAM head before the reader waits
  for disk-tail data. The chunk view is borrowed and remains valid only during that callback.
- `Store` is shared and thread-safe. A `StoreReader` is single-threaded by design; create one per
  application worker so rings and scratch buffers are never contended.

Concurrent disk-backed writers share the configured bounded staging-buffer pool. If every buffer is
busy, `begin_put()` returns `Errc::would_block`; an embedded scheduler can retry or queue that write
without allowing admission memory to grow without bound. Destroying an uncommitted `StoreWriter`
aborts its scratch generation and leaves the previously published value untouched.

## Minimal use

```cpp
#include <goblin/store.hpp>

using namespace goblin;

Store::prepare_directory("/mnt/ssd/my-cache"); // one time; directory must be empty

StoreOptions options;
options.ssd.dirs = {"/mnt/ssd/my-cache"};
options.memory.total_bytes = 4 * GiB;
options.tiers.ram_head = 256 * KiB;
options.read_chunk_bytes = 4 * MiB;
options.file_handle_cache = 8192;

auto opened = Store::open(options); // opens an empty store; wipes only marked pool directories
if (!opened) return 1;
Store store = std::move(*opened);

auto writer = store.begin_put("large-object", object_size);
while (auto piece = next_piece()) {
    if (!writer->write(piece)) return 1;
}
if (!writer->commit()) return 1;

auto made_reader = store.make_reader();
if (!made_reader) return 1;
StoreReader reader = std::move(*made_reader);
auto read = reader.stream("large-object", [](ByteView piece) -> Status {
    consume(piece);
    return {};
});

// Or materialize without allocating again for every object. `bytes` is borrowed until the next
// load() on this reader. An optional progress callback observes pieces as they become ready.
auto loaded = reader.load("large-object");
consume_complete(loaded->bytes);
```

`StoreOptions` exposes the same tier, memory, eviction, access-score, read/write I/O-chunk,
file-handle-cache, write-buffer, maximum-object, and `O_DIRECT` choices used by the server.
The descriptor-cache capacity defaults to 128 and must be a nonzero power of two. Applications using
a larger cache must raise `RLIMIT_NOFILE` before `Store::open()`; allow additional descriptors for
application sockets and files. `small_total_bytes` selects distinct
packed-small-object and fixed-head pools. An empty HDD directory list creates the common two-layer
RAM-head/SSD store; adding HDD directories enables the cold tail.

Pool safety is unchanged. `Store::prepare_directory()` only marks an empty directory, and
`Store::open()` refuses to clear a directory without that marker. Opening is destructive by design:
the in-memory index is not persistent, so every embedded open starts a new empty cache just like the
server process.

`StoreReader::load()` is the reusable complete-object path. Its returned view borrows the reader's
retained buffer until the next `load()` (moving or destroying the reader also invalidates it). The
buffer is not sized from corpus metadata or a maximum-object setting in advance: the request that
first exceeds capacity pays for an aligned allocation sized to that object, and later fitting
requests reuse it. The optional progress callback receives in-place pieces and therefore preserves
TTFB measurement or progressive consumption while the remainder is still arriving.

`StoreReader::get()` remains a convenience for callers that require ownership in a fresh
`vector<byte>`; it copies the completed retained view into that vector. `stream()` remains the
lowest-memory interface because it needs only the configured read chunk rather than a complete-value
buffer.

The [Goblin Store versus BlobDB benchmark](../bench/GOBLIN_BLOBDB_BENCHMARK.md) uses this interface
on both population and retrieval, making the comparison storage-engine-to-storage-engine rather
than memcache-over-loopback-to-in-process.
