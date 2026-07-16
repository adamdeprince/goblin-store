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


We set up a 2-core server running goblin-store and pitted it against a
16-vCPU load box running mutilate across a real NIC, transferring mixed
256 KiB–8 MiB objects. Full methodology, rig, and tables are in
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
depends on the latency of its underlying storage and when the demand
for it will arrive across the wire. Traffic flows down the tiers to
cheaper, higher-latency storage, with earlier layers absorbing the
latency of later ones.

When a request arrives, the resident RAM head is delivered first. Once
that initial response send is queued, the first bounded disk-tail read
starts at the configured head boundary and runs while the head is in
flight; completed tail bytes remain ordered behind the head. Subsequent
io_uring reads overlap transmission of the current chunk. Bytes reached
later in a three-layer stream come from SSD and then cheaper HDD, so
their latency can be hidden behind earlier wire time. Small objects are
stored in RAM only. Medium objects use RAM plus SSD. Large objects use
RAM, SSD, and HDD when the HDD tier is configured.


- **RAM head** — first `ram_head` bytes (default 256 KiB), resident for immediate time-to-first-byte.
  Responses up to 16 KiB are framed inline for one send; larger resident heads use a pinned
  zero-copy send.
  Heads are packed inside larger allocation/promotion blocks (default 2 MiB on x86, so eight default
  heads per block); the allocation block is not a per-file reservation.
- **SSD bytes** — in three-layer mode, the warm prefix (default up to 32 MiB/object); in two-layer
  mode, SSD holds the complete backing value for every disk-backed object.
- **HDD tail** — the cold remainder on cheap, throughput-optimized disk (3-layer mode).



Underneath: **thread-per-core TCP/HTTP** network loops on **io_uring** over a shared locked store
([ADR-0001](docs/adr/0001-thread-per-core-concurrency.md)/[ADR-0002](docs/adr/0002-iouring-rings-shared-drives.md)); native RDMA currently uses a connection-owned progress thread. **O_DIRECT**
backing store so the cache owns its RAM budget instead of fighting the page cache ([ADR-0011](docs/adr/0011-odirect-bypass-page-cache.md)), **atomic
copy-on-write publish** (readers never see a torn value, [ADR-0018](docs/adr/0018-concurrency-model.md)), a **buddy** RAM-head allocator
([ADR-0008](docs/adr/0008-ram-allocator.md)), **s3fifo** + whole-object multi-resource eviction ([ADR-0007](docs/adr/0007-eviction-policy.md)/[ADR-0012](docs/adr/0012-multi-resource-eviction.md)), keyless **SHA-256
digest identity** ([ADR-0014](docs/adr/0014-keyless-digest-identity.md)), and **bounded,
mlock-able data pools** ([ADR-0016](docs/adr/0016-bounded-locked-memory.md)).

Start at **[`docs/adr/README.md`](docs/adr/README.md)** — 20 ADRs covering the full design.
For a focused explanation of the NUMA-aware head cache, HugeTLB geometry, and the two-R820
interconnect test, read **[NUMA-local RAM heads and interconnect bandwidth](docs/numa-interconnect-bandwidth.md)**.
The longer direct-link experiment—including nanosecond traces, tail percentiles, and probability of
superiority—is **[Looking for NUMA latency below the network noise floor](docs/numa-first-byte-latency.md)**.
The native data path's direct 40 Gbit/s InfiniBand measurements are in
**[Native RDMA over InfiniBand: 256 KiB latency and throughput](docs/native-rdma-256k-performance.md)**.
The small-object channel and short-key hashing results are summarized in
**[Small-object channel and short-key SHA-256 performance](docs/small-object-channel-performance.md)**.
For the storage-full path, including per-admission EVICT/BLOCK policy, exact shard reservation,
filesystem-local reclaim, and immutable publication, read
**[Full-filesystem writes: reservation, reclaim, and publication](docs/full-filesystem-writes.md)**.

## Protocols

- **memcache (TCP):** classic text (`get`/`gets`/`set`/`add`/`replace`/`cas`/`delete`/`stats`/
  `version`/`quit`) + the **meta** protocol (`mn`/`mg`/`ms`/`md`), TTL, and
  **CAS** (= a per-store ETag, essentially free). No binary protocol, no UDP, no built-in auth.
- **memcache (native InfiniBand/RoCE):** optional reliable-connected v3 endpoint for the classic
  operations. Commands and framing use a small one-sided control ring; every nonempty object body
  uses registered, credit-controlled bulk windows. With the defaults, 1 MiB is four bulk writes
  instead of roughly 5,462 192-byte inline writes. This is not TCP over IPoIB and does not fall back
  to the old inline-body ABI. The initial server owns one QP and progress thread per connection;
  TCP/HTTP retain their core-local multiplexed loops. See
  [ADR-0020](docs/adr/0020-native-rdma-bulk-windows.md) and the separately installable
  [C++/Python client](python/README.md).
- **HTTP/1.1 (read-only):** `GET`/`HEAD`, byte ranges, conditional GET (ETag / `If-None-Match` → 304),
  `Content-Type`, `Accept-Ranges`. **No write surface by design** (edge-cache role; put a writer in
  front). **HTTPS** via OpenSSL + **kTLS** with SNI cert selection.

> **Status:** working memcache + HTTP/HTTPS server on io_uring + O_DIRECT — 3-tier store, atomic
> publish, RAM-head GET, read-ahead pipeline, TTL/CAS, graceful shutdown. **213 unit-test
> cases, with Release, ASan/UBSan, and TSan coverage.** macOS is a non-goal (no io_uring / O_DIRECT
> analog); FreeBSD (kqueue/aio) is a planned port.

## Dependencies

**Build:** **C++23** compiler (GCC ≥ 14, developed on GCC 16; or Clang ≥ 18), **CMake ≥ 3.28** +
**Ninja**, **OpenSSL** (`libssl-dev`, for HTTPS and the long-input SHA-256 fallback where selected;
short-key and hardware SHA implementations are vendored, [ADR-0014](docs/adr/0014-keyless-digest-identity.md)), **liburing** (`liburing-dev`, the io_uring
backend — without it the logic layers still compile but the server cannot serve). Native RDMA is
optional and is built when **libibverbs** and **librdmacm** development headers are present.

**Runtime:** Linux kernel ≥ 5.19 (project io_uring support floor).

```sh
sudo apt-get install -y build-essential cmake ninja-build libssl-dev liburing-dev \
    libibverbs-dev librdmacm-dev
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

# 3-tier: 4 GiB shared resident-data RAM, SSD prefix, HDD tail; memcache + HTTP
./build/goblin-store --memory 4G \
    --ssd-dir /mnt/ssd/pool --hdd-dir /mnt/hdd/pool \
    --memcache-port 11211 --http-port 8080

# Four NUMA nodes, legacy shared mode: 100 GiB on the serving/NIC-local node and 20 GiB on
# each of the other three. Fixed heads and packed small objects share all 160 GiB.
./build/goblin-store --numa 0 --memory 100G --sub-memory 20G \
    --ssd-dir /mnt/ssd/pool --hdd-dir /mnt/hdd/pool

# Strict split mode: fixed heads get 100 GiB local + 20 GiB per remote node (160 GiB total).
# Packed small objects get their own 16 GiB local + 4 GiB per remote node (28 GiB total).
./build/goblin-store --numa 0 --memory 100G --sub-memory 20G \
    --small-memory 16G --small-sub-memory 4G \
    --ssd-dir /mnt/ssd/pool --hdd-dir /mnt/hdd/pool

# Native RDMA-only memcache. Without an explicit --numa, the exact address selects its HCA node.
./build/goblin-store --no-memcache --no-http --rdma 10.88.88.1 \
    --memory 4G --ssd-dir /mnt/ssd/pool
```

Key knobs (see `--help`): `--ram-head` (power-of-two per-object resident head, default 256 KiB),
`--ssd-prefix` (positional tier size), `--block` (power-of-two allocation/promotion block, default
2 MiB on x86 and 32 MiB on Arm/LoongArch), `--io-buffers` /
`--io-chunk` (bounded streaming RAM), `--eviction`, `--max-objects`, `--no-mlock` (dev), `--tls-cert`/
`--tls-key` (HTTPS), `--source` (preload a directory tree), `--numa NODE` (explicit NUMA
placement), `--memory SIZE` / `--sub-memory SIZE` (fixed-head RAM on the local / each non-local
NUMA node), `--small-memory SIZE` / `--small-sub-memory SIZE` (packed-small-object RAM on the local /
each non-local node), `--increment FLOAT` (score added per successful key read), `--decay FLOAT`
(per-minute score multiplier in `(0, 1)`), and `--perverse` (benchmark-only inversion of preferred
head-memory placement).

On Linux, every fixed pool—including both sides of an explicit head/small-object split—first
requests HugeTLB backing using the platform page order
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

On Linux, Goblin Store binds the main thread before allocating its fixed RAM arenas; serving and
coordinator threads inherit that affinity, and `--cores 0` uses the number of CPUs available on the
selected NUMA node as the worker count for each enabled protocol. Score-table workers instead bind
to the node whose score memory they scan. A strict inherited local memory policy keeps the dynamic
key index, I/O buffers, and ordinary thread allocations on the serving node. Without `--numa`, the
serving node is derived from the UP Ethernet interfaces covered by the wildcard listeners. If those
interface addresses belong to different nodes—or locality is unknown on a multi-node host—startup
stops and reports each Linux interface name, listening address, NUMA node, and the corresponding
`--numa NODE` override.

`--perverse` leaves the serving threads, key index, I/O pools, and inherited default memory policy on
that selected NIC-local node, but maps the preferred region-zero `--memory` head arena—and an
explicit `--small-memory` arena—on the online node with the greatest Linux NUMA distance. Equal
distances choose the lowest node ID. This is a benchmark control for comparing local and remote DRAM
with an otherwise identical CPU/NIC path; it is rejected with `--no-numa` and on a single-node
machine.

`--memory` is the local fixed-head budget. With explicit `--numa`, optional `--sub-memory` adds the
stated fixed-head budget on **each** other online NUMA node. By default, packed small objects continue
to use those same blocks, preserving the legacy shared-pool policy and capacity. Supplying
`--small-memory` opts into a strict split: fixed heads may allocate only from the `--memory` /
`--sub-memory` pool, while packed small objects may allocate only from their dedicated local
`--small-memory` pool and optional per-remote-node `--small-sub-memory` regions. Neither class borrows
unused blocks from the other. `--small-sub-memory` requires both `--small-memory` and explicit
`--numa`, just as `--sub-memory` requires explicit `--numa`.

Every nonzero configured budget must be a whole multiple of `--block` (and therefore of the platform
HugeTLB page size). Each configured range is bound to its physical node with Linux `mbind(MPOL_BIND)`, attempts
HugeTLB independently, and falls back to ordinary locked memory without changing its size or block
geometry. Each pool searches its local blocks first and falls through to its own foreign regions
only when local capacity is exhausted. In split mode, fixed-head capacity is
`--memory + (other_nodes × --sub-memory)` and packed-small-object capacity is
`--small-memory + (other_nodes × --small-sub-memory)`. Bounded streaming read-I/O pools remain
additional per-worker memory, while the write-staging pool is shared by the store. In benchmark-only
perverse mode, "local" for both resident pools means the deliberately far preferred region;
subordinate regions include the real serving node, so fixed-head promotion is intentionally inverted
as well. Packed small-object blocks are never NUMA-promotion candidates.

Each key starts with a score of zero. A successful logical read adds `--increment` (default `1.0`),
and once per minute every score is multiplied by `--decay` (default `0.5`). A fixed resident head's
score lives only in a dense `std::atomic<double>` array on the same NUMA node as its bytes; fractional
or headless objects keep their atomic score in the key index. Per-node workers scan and decay their
own dense arrays, returning only compact extrema summaries to the local coordinator. While the
hottest eligible foreign block is hotter than the coldest eligible local block, their contents,
score slices, and allocator state are exchanged and the affected key locators are rewritten. Pinned,
partially filled, in-flight, and compactable small-object arena blocks are not moved. When no safe
score inversion exists, the coordinator waits one second before scanning again. The minute rescore
has priority: once it announces that it is pending, no new promotion starts; an active block exchange
finishes and then the entire decay traversal runs without promotion
([ADR-0019](docs/adr/0019-access-score-numa-promotion.md)).

FPGA NICs and direct userspace networking are on the product development path. They will allow NIC
queues, workers, RAM heads, and hardware timestamps to share one NUMA node while removing the kernel
TCP path from first-payload-byte measurements.

## Layout
```
src/goblin/common/    types, error (std::expected), config + validation
src/goblin/core/      RAM block/buddy allocator, io_uring reactor, thread-per-core runtime
src/goblin/crypto/    multi-backend SHA-256 (keyless digest identity)
src/goblin/storage/   positional layout, drive-pool striping, index, tiering, eviction
src/goblin/net/       shared stream loop (memcache / HTTP / HTTPS subclasses)
src/goblin/protocol/  memcache text + meta front-end
src/goblin/http/      HTTP request/response, key derivation
src/goblin/tls/       OpenSSL + kTLS, SNI
src/goblin/tools/     goblin-store-path-prep, goblin-bench
tests/                dependency-free unit tests (Release + ASan/UBSan + TSan)
docs/adr/             20 architecture decision records
```

## License

Copyright 2026 Adam DePrince. Licensed under the **Apache License, Version 2.0** — see
[LICENSE](LICENSE) (or <https://www.apache.org/licenses/LICENSE-2.0>). Source and issues:
<https://github.com/adamdeprince/goblin-store>.
