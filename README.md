# Goblin Store (`goblincache`)

A pure **C++23, Linux-only** **large-object** key/value cache and HTTP object server. It keeps the
latency-sensitive *head* of each object in a RAM budget, streams the middle from SSD, and (optionally)
hoards the cold tail on cheap HDD — handing clients the first bytes immediately while the rest is
dragged out of the cave behind them.

**Mission: beat memcached on large objects.** Match memcached's hit latency at a fraction of the RAM,
beat extstore on the cold-read tail, and cost less per stored GB by spreading each object down a
**RAM → SSD → HDD** price pyramid. goblincache speaks the **memcache** text + meta protocols (drop-in
for memcache clients) and a read-only **HTTP/1.1** object path for edge/CDN use.

## Benchmarks — the short version

AWS, 2-core server vs a 16-vCPU load box over a real NIC; mixed **256 KiB–8 MiB** objects driven by
mutilate. Full methodology, rig, and tables in **[`BENCHMARKS.md`](BENCHMARKS.md)**.

| workload | vs | result |
|---|---|---|
| **memory-sized** (set fits RAM) | memcached | **same latency, 7.4× less RAM** (267 MiB vs 1.97 GiB) → ~6–7× cheaper |
| **disk-sized** (set ≫ RAM) | memcached + extstore | **+68% throughput, ~25% cheaper** on the *same* HDD — and goblincache holds 100% of the set while extstore **sheds 41%** under write load |
| **small objects** (sub-2 KB) | memcached | **memcached wins** — ~12× less RAM, ~2× faster. Small values are the slab allocator's turf; goblincache is a large-object engine and says so. |

The win is large objects: a hot **RAM head** + **SSD prefix** hide the ~5 ms HDD seek that extstore
pays in full on every cold GET, while a read-ahead pipeline keeps the cheap-tier bulk flowing near
spindle throughput.

## How it works

Each object is split **positionally** (ADR-0006) into a RAM head `[0, ram_head)`, an SSD prefix, and
an HDD tail — bytes flow down the tiers as they get colder and cheaper:

- **RAM head** — first `ram_head` bytes (default 256 KiB), served zero-copy for instant time-to-first-byte.
- **SSD prefix** — the warm middle (default up to 32 MiB/object) on fast storage.
- **HDD tail** — the cold remainder on cheap, throughput-optimized disk (3-layer mode).

**Small objects that fit in the head (`size ≤ ram_head`) are RAM-only** — no redundant disk copy, no
per-object file (ADR-0003, revised): the head is authoritative (a cache that blanks on restart, like
memcached), so head-evict = object-evict. (Trimming the head off *large* objects' disk copy too is a
planned follow-up.)

Underneath: **thread-per-core** shared-nothing loops on **io_uring** (ADR-0001/0002), **O_DIRECT**
backing store so the cache owns its RAM budget instead of fighting the page cache (ADR-0011), **atomic
copy-on-write publish** (readers never see a torn value, ADR-0018), a **buddy** RAM-head allocator
(ADR-0008), **s3fifo** + whole-object multi-resource eviction (ADR-0007/0012), keyless **SHA-256
digest identity** (ADR-0014), and a **bounded, mlock-able** memory model (ADR-0016).

Start at **[`docs/adr/README.md`](docs/adr/README.md)** — 18 ADRs covering the full design.

## Protocols

- **memcache (TCP):** classic text (`get`/`gets`/`set`/`add`/`replace`/`append`/`prepend`/`cas`/
  `delete`/`incr`/`decr`/`touch`/`stats`) + the **meta** protocol (`mn`/`mg`/`ms`/`md`), TTL, and
  **CAS** (= a per-store ETag, essentially free). No binary protocol, no UDP, no built-in auth.
- **HTTP/1.1 (read-only):** `GET`/`HEAD`, byte ranges, conditional GET (ETag / `If-None-Match` → 304),
  `Content-Type`, `Accept-Ranges`. **No write surface by design** (edge-cache role; put a writer in
  front). **HTTPS** via OpenSSL + **kTLS** with SNI cert selection.

> **Status:** working memcache + HTTP/HTTPS server on io_uring + O_DIRECT — 3-tier store, atomic
> publish, zero-copy head GET, read-ahead pipeline, TTL/CAS, graceful shutdown. **115 unit tests pass
> under Release, ASan, and TSan.** macOS is a non-goal (no io_uring / O_DIRECT analog); FreeBSD
> (kqueue/aio) is a planned port.

## Dependencies

**Build:** **C++23** compiler (GCC ≥ 14, developed on GCC 16; or Clang ≥ 18), **CMake ≥ 3.28** +
**Ninja**, **OpenSSL** (`libssl-dev`, for the HTTPS listener — always linked; the key-digest SHA-256 is
vendored so the storage path never links TLS, ADR-0014), **liburing** (`liburing-dev`, the io_uring
backend — without it the logic layers still compile but the reactor is stubbed and the server can't serve).

**Runtime:** Linux kernel ≥ 5.19 (io_uring multishot).

```sh
sudo apt-get install -y build-essential cmake ninja-build libssl-dev liburing-dev
```

## Build & run

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/goblincache --help

# A pool directory must be "blessed" before use — goblincache refuses to wipe an unblessed dir,
# so a typo'd --ssd-dir can never erase /var (ADR-0013). One-time, and it must be empty:
./build/goblin-store-path-prep /mnt/ssd/pool
./build/goblin-store-path-prep /mnt/hdd/pool          # optional cold tier (enables 3-layer)

# 3-tier: 4 GiB RAM heads, SSD prefix, HDD tail; memcache on 11211 + HTTP on 8080
./build/goblincache --memory 4G \
    --ssd-dir /mnt/ssd/pool --hdd-dir /mnt/hdd/pool \
    --memcache-port 11211 --http-port 8080
```

Key knobs (see `--help`): `--ram-head`, `--ssd-prefix` (positional tier sizes), `--io-buffers` /
`--io-chunk` (bounded streaming RAM), `--eviction`, `--max-objects`, `--no-mlock` (dev), `--tls-cert`/
`--tls-key` (HTTPS), `--source` (preload a directory tree).

## Layout
```
src/goblin/common/    types, error (std::expected), config + validation
src/goblin/core/      RAM block/buddy allocator, io_uring reactor, thread-per-core runtime
src/goblin/crypto/    vendored SHA-256 (keyless digest identity)
src/goblin/storage/   positional layout, drive-pool striping, index, tiering, eviction
src/goblin/net/       shared stream loop (memcache / HTTP / HTTPS subclasses)
src/goblin/protocol/  memcache text + meta front-end
src/goblin/http/      HTTP request/response, key derivation
src/goblin/tls/       OpenSSL + kTLS, SNI
src/goblin/tools/     goblin-store-path-prep, goblin-bench
tests/                dependency-free unit tests (Release + ASan + TSan)
docs/adr/             18 architecture decision records
```
