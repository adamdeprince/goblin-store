# Goblin Store (`goblin-store`)

Goblin store is a high-performance large-object key/value cache and
HTTP object server. It combines RAM, SSD, and optionally HDD to
provide low-latency, high-throughput storage for large values at
significantly lower per-MiB storage cost than an all-RAM cache.

It keeps the latency-sensitive head of each object in RAM, streams the
middle from SSD, and optionally hoards the cold tail on cheap HDD —
handing clients the first bytes immediately while the rest is dragged
out of the cave behind them.

The goal of goblin-store is to beat memcached on large objects: match
memcached’s hit latency at a fraction of the RAM, outperform memcached
+ extstore on cold reads, and cost less per stored GiB by spreading
each object down a RAM → SSD → HDD price pyramid.

goblin-store speaks the memcache text and meta protocols, making it a
drop-in replacement for memcached. It also serves read-only HTTP/1.1
for edge/CDN use.


## Benchmarks — the short version


We setup a 2-core server running goblin-store and pitted ig against a
16-vCPU load box running mutilate across a real NIC transfering mixed
256KiB-8MiB objects.  Full methodology, rig, and tables in
**[`BENCHMARKS.md`](BENCHMARKS.md)**.

| workload | vs | result |
|---|---|---|
| **memory-sized** (set fits RAM) | memcached | **same latency, 7.4× less RAM** (267 MiB vs 1.97 GiB) → ~6–7× cheaper |
| **disk-sized** (set ≫ RAM) | memcached + extstore | **+68% throughput, ~25% cheaper** on the *same* HDD — and goblin-store holds 100% of the set while extstore **sheds 41%** under write load |
| **small objects** (sub-2 KB) | memcached | **memcached wins** — ~12× less RAM, ~2× faster. Small values are the slab allocator's turf; goblin-store is a large-object engine and says so. |

The win is large objects: a hot **RAM head** + **SSD prefix** hide the ~5 ms HDD seek that extstore
pays in full on every cold GET, while a read-ahead pipeline keeps the cheap-tier bulk flowing near
spindle throughput.

## How it works

Each object is split
([ADR-0006](docs/adr/0006-positional-tiering-pipeline.md)) into three
parts: RAM, SSD and optionally an HDD tail. The position of each part
depends on the latecy of its underlying storage and when the demand
for it across the wire will actually arive.  Traffic flows down the
teirs to cheaper and higher latency storage, latency absorbed by the
laters beore it.

When a request arrives all three layers are dispatched at once, but
not every byte is delivered to the wire at once.  Data to be delivered
immediately comes from ram.  Later data comes at a later time on the
wire; this data comes from SSD where the latency can be hidden by
transmission time on the wire.  Even later data comes from cheaper
HDD.  Samll objects are stored in RAM only.  Medium sized objects in
SSD only.  Large objects across RAM, SSD and HDD.


- **RAM head** — first `ram_head` bytes (default 256 KiB), served zero-copy for instant time-to-first-byte.
  Heads are packed inside larger allocation/promotion blocks (default 2 MiB on x86, so eight default
  heads per block); the allocation block is not a per-file reservation.
- **SSD prefix** — the warm middle (default up to 32 MiB/object) on fast storage.
- **HDD tail** — the cold remainder on cheap, throughput-optimized disk (3-layer mode).



Underneath: **thread-per-core** shared-nothing loops on **io_uring** ([ADR-0001](docs/adr/0001-thread-per-core-concurrency.md)/[ADR-0002](docs/adr/0002-iouring-rings-shared-drives.md)), **O_DIRECT**
backing store so the cache owns its RAM budget instead of fighting the page cache ([ADR-0011](docs/adr/0011-odirect-bypass-page-cache.md)), **atomic
copy-on-write publish** (readers never see a torn value, [ADR-0018](docs/adr/0018-concurrency-model.md)), a **buddy** RAM-head allocator
([ADR-0008](docs/adr/0008-ram-allocator.md)), **s3fifo** + whole-object multi-resource eviction ([ADR-0007](docs/adr/0007-eviction-policy.md)/[ADR-0012](docs/adr/0012-multi-resource-eviction.md)), keyless **SHA-256
digest identity** ([ADR-0014](docs/adr/0014-keyless-digest-identity.md)), and a **bounded, mlock-able** memory model ([ADR-0016](docs/adr/0016-bounded-locked-memory.md)).

Start at **[`docs/adr/README.md`](docs/adr/README.md)** — 19 ADRs covering the full design.
For a focused explanation of the NUMA-aware head cache, HugeTLB geometry, and the two-R820
interconnect test, read **[NUMA-local RAM heads and interconnect bandwidth](docs/numa-interconnect-bandwidth.md)**.
The longer direct-link experiment—including nanosecond traces, tail percentiles, and probability of
superiority—is **[Looking for NUMA latency below the network noise floor](docs/numa-first-byte-latency.md)**.

## Protocols

- **memcache (TCP):** classic text (`get`/`gets`/`set`/`add`/`replace`/`append`/`prepend`/`cas`/
  `delete`/`incr`/`decr`/`touch`/`stats`) + the **meta** protocol (`mn`/`mg`/`ms`/`md`), TTL, and
  **CAS** (= a per-store ETag, essentially free). No binary protocol, no UDP, no built-in auth.
- **HTTP/1.1 (read-only):** `GET`/`HEAD`, byte ranges, conditional GET (ETag / `If-None-Match` → 304),
  `Content-Type`, `Accept-Ranges`. **No write surface by design** (edge-cache role; put a writer in
  front). **HTTPS** via OpenSSL + **kTLS** with SNI cert selection.

> **Status:** working memcache + HTTP/HTTPS server on io_uring + O_DIRECT — 3-tier store, atomic
> publish, zero-copy head GET, read-ahead pipeline, TTL/CAS, graceful shutdown. **153 unit-test cases
> run under Release, ASan, and TSan.** macOS is a non-goal (no io_uring / O_DIRECT analog); FreeBSD
> (kqueue/aio) is a planned port.

## Dependencies

**Build:** **C++23** compiler (GCC ≥ 14, developed on GCC 16; or Clang ≥ 18), **CMake ≥ 3.28** +
**Ninja**, **OpenSSL** (`libssl-dev`, for the HTTPS listener — always linked; the key-digest SHA-256 is
vendored so the storage path never links TLS, [ADR-0014](docs/adr/0014-keyless-digest-identity.md)), **liburing** (`liburing-dev`, the io_uring
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
./build/goblin-store --help

# A pool directory must be "blessed" before use — goblin-store refuses to wipe an unblessed dir,
# so a typo'd --ssd-dir can never erase /var ([ADR-0013](docs/adr/0013-ephemeral-wipe-on-start.md)). One-time, and it must be empty:
./build/goblin-store-path-prep /mnt/ssd/pool
./build/goblin-store-path-prep /mnt/hdd/pool          # optional cold tier (enables 3-layer)

# 3-tier: 4 GiB RAM heads, SSD prefix, HDD tail; memcache on 11211 + HTTP on 8080
./build/goblin-store --memory 4G \
    --ssd-dir /mnt/ssd/pool --hdd-dir /mnt/hdd/pool \
    --memcache-port 11211 --http-port 8080

# Four NUMA nodes: 100 GiB on the serving/NIC-local node, 20 GiB on each of the other three.
# Total head-cache capacity = 100 + (3 x 20) = 160 GiB.
./build/goblin-store --numa 0 --memory 100G --sub-memory 20G \
    --ssd-dir /mnt/ssd/pool --hdd-dir /mnt/hdd/pool
```

Key knobs (see `--help`): `--ram-head` (power-of-two per-object resident head, default 256 KiB),
`--ssd-prefix` (positional tier size), `--block` (power-of-two allocation/promotion block, default
2 MiB on x86 and 32 MiB on Arm/LoongArch), `--io-buffers` /
`--io-chunk` (bounded streaming RAM), `--eviction`, `--max-objects`, `--no-mlock` (dev), `--tls-cert`/
`--tls-key` (HTTPS), `--source` (preload a directory tree), `--numa NODE` (explicit NUMA
placement), `--sub-memory SIZE` (head-cache RAM on each non-local NUMA node), `--increment FLOAT`
(score added per successful key read), `--decay FLOAT` (per-minute score multiplier in `(0, 1)`),
and `--perverse` (benchmark-only inversion of preferred head-memory placement).

On Linux, every fixed pool first requests explicit HugeTLB backing using the platform page order
(2 MiB on x86, 32 MiB on Arm/LoongArch). `--block` is a logical allocator and promotion unit: it must
be a power-of-two multiple of that page size, and a larger block spans several real HugeTLB pages.
Streaming pools retain their smaller `--io-chunk` allocation granule within the same kind of backing
mapping. Each NUMA region is attempted independently against that node's pool. If the page order,
page count, or requested placement is unavailable, that region silently falls back to ordinary
memory; allocator geometry and capacity do not change. Explicit huge pages are resident and
unswappable; fallback head and streaming pools retain the configured `mlock` behavior. A pool must
total a whole number of HugeTLB pages; for example, the default 16 MiB streaming pool on 32 MiB-page
Arm/LoongArch falls back unless it is resized (such as `--io-buffers 128` with the default 256 KiB
chunks).

On Linux, Goblin Store binds the main thread before allocating its fixed RAM arenas; the worker and
maintenance threads inherit that affinity, and `--cores 0` uses the number of CPUs available on the
selected NUMA node as the worker count for each enabled protocol. A strict inherited local memory
policy also keeps the dynamic key index, I/O buffers, and thread allocations on that node. Without
`--numa`, the node is derived from the UP Ethernet interfaces covered by the wildcard listeners. If
those interface addresses belong to different nodes—or locality is unknown on a multi-node
host—startup stops and reports each Linux interface name, listening address, NUMA node, and the
corresponding `--numa NODE` override.

`--perverse` leaves the serving threads, key index, I/O pools, and inherited default memory policy on
that selected NIC-local node, but maps the preferred region-zero `--memory` head arena on the online
node with the greatest Linux NUMA distance. Equal distances choose the lowest node ID. This is a
benchmark control for comparing local and remote DRAM with an otherwise identical CPU/NIC path; it
is rejected with `--no-numa` and on a single-node machine.

With explicit `--numa`, `--memory` is the head-cache budget on that local node. Optional
`--sub-memory` adds the stated budget on **each** other online NUMA node and is rejected without an
explicit `--numa`. Each arena range is bound to its physical node with Linux `mbind(MPOL_BIND)`. The
allocator always searches local blocks first—including local blocks returned after foreign memory
has been used—and falls through to foreign regions only when the local region cannot satisfy the
allocation. Total head-cache capacity is `--memory + (other_nodes × --sub-memory)`; bounded streaming
I/O pools remain additional per-worker memory. In benchmark-only perverse mode, "local" in this
allocator policy means the deliberately far preferred region; subordinate regions include the real
serving node, so promotion is intentionally inverted as well.

Each key starts with a score of zero. A successful logical read adds `--increment` (default `1.0`),
and once per minute every score is multiplied by `--decay` (default `0.5`). With foreign head-memory
regions, a local maintenance thread compares the summed key score of complete buddy blocks: while the
hottest eligible foreign block is hotter than the coldest eligible local block, their contents and
allocator state are exchanged and the affected key locators are rewritten. Pinned, partially filled,
in-flight, and compactable small-object arena blocks are not moved. When no safe score inversion
exists, the thread waits one second before scanning again. The minute rescore has priority: once it
announces that it is pending, no new promotion starts; an active block exchange finishes and then the
entire decay traversal runs without promotion ([ADR-0019](docs/adr/0019-access-score-numa-promotion.md)).

FPGA NICs and direct userspace networking are on the product development path. They will allow NIC
queues, workers, RAM heads, and hardware timestamps to share one NUMA node while removing the kernel
TCP path from first-payload-byte measurements.

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
docs/adr/             19 architecture decision records
```

## License

Copyright 2026 Adam DePrince. Licensed under the **Apache License, Version 2.0** — see
[LICENSE](LICENSE) (or <https://www.apache.org/licenses/LICENSE-2.0>). Source and issues:
<https://github.com/adamdeprince/goblin-store>.
