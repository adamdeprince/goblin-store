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

goblin-store implements a compatible subset of the memcache text and meta
protocols. It also serves read-only HTTP/1.1
for edge/CDN use, either from explicitly inserted objects or as a
streaming reverse cache for an HTTP(S) origin.


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

The preliminary HDD-only mirror-cache comparison with Vinyl Cache reduced median time to first byte
from 14.865 ms to 345.890 µs and raised median request bandwidth from 453.411 MB/s to 722.241 MB/s.
The four-socket NUMA, RAID, IPoIB, HugeTLB, IRQ-affinity, monitoring setup, and full results are in
[`docs/mirror-proxy-benchmark.md`](docs/mirror-proxy-benchmark.md); concurrent I/O tests are underway.

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

Start at **[`docs/adr/README.md`](docs/adr/README.md)** — 21 ADRs covering the full design.
For a focused explanation of the NUMA-aware head cache, HugeTLB geometry, and the two-R820
interconnect test, read **[NUMA-local RAM heads and interconnect bandwidth](docs/numa-interconnect-bandwidth.md)**.
The longer direct-link experiment—including nanosecond traces, tail percentiles, and probability of
superiority—is **[Looking for NUMA latency below the network noise floor](docs/numa-first-byte-latency.md)**.
The native data path's direct 40 Gbit/s InfiniBand measurements are in
**[Native RDMA over InfiniBand: 256 KiB latency and throughput](docs/native-rdma-256k-performance.md)**.
For the optional userspace TCP path—including ordinary memcache/HTTP wire compatibility and the
separately installable C++/Python memcache clients—see **[ExaSock transport](docs/exasock.md)**.
The small-object channel and short-key hashing results are summarized in
**[Small-object channel and short-key SHA-256 performance](docs/small-object-channel-performance.md)**.
For the storage-full path, including per-admission EVICT/BLOCK policy, exact shard reservation,
filesystem-local reclaim, and immutable publication, read
**[Full-filesystem writes: reservation, reclaim, and publication](docs/full-filesystem-writes.md)**.

## Protocols

- **memcache (TCP):** classic text retrieval, storage, arithmetic, touch, delete, delayed flush,
  stats, and CAS—including full multi-key `get`/`gets`, `gat`/`gats`, and atomic `add`/`replace`.
  The **meta** surface (`mn`/`mg`/`ms`/`md`/`ma`/`me`) includes CAS, binary keys, access metadata,
  conditional value retrieval, early recache, serve-stale, and `N`/`W`/`Z` anti-stampede
  coordination. TCP is IPv4/IPv6 capable and may be wrapped in TLS 1.3; the same protocol is also
  available on an optional Unix-domain socket. Standard memcached ASCII authentication can gate
  both TCP and Unix clients. The default async and fallback blocking TCP backends run the same semantics and a
  differential wire-transcript suite guards that contract. This remains a compatible subset: no
  binary protocol or UDP.
- **memcache (native InfiniBand/RoCE):** optional reliable-connected v3 endpoint for the classic
  operations. Commands and framing use a small one-sided control ring; every nonempty object body
  uses registered, credit-controlled bulk windows. With the defaults, 1 MiB is four bulk writes
  instead of roughly 5,462 192-byte inline writes. This is not TCP over IPoIB and does not fall back
  to the old inline-body ABI. The initial server owns one QP and progress thread per connection;
  TCP/HTTP retain their core-local multiplexed loops. See
  [ADR-0020](docs/adr/0020-native-rdma-bulk-windows.md) and the separately installable
  [C++/Python client](python/README.md).
- **memcache + HTTP (ExaSock-accelerated TCP):** optional, explicitly selected userspace TCP on a
  supported ExaNIC. The wire protocols do not change: ordinary memcache clients, browsers, and
  Ethernet peers still see standard TCP services. The standalone C++ library and its nanobind
  Python package provide a fail-closed ExaSock memcache client. ExaSock remains an external,
  system-installed dependency and is never vendored; see [the build and deployment guide](docs/exasock.md).
- **HTTP/1.1 (read-only):** `GET`/`HEAD`, byte ranges, conditional GET (ETag / `If-None-Match` → 304),
  `Content-Type`, `Accept-Ranges`. **No write surface by design** (edge-cache role; put a writer in
  front). **HTTPS** via OpenSSL + **kTLS** with SNI cert selection.
- **HTTP/HTTPS mirror cache (`--mirror URL`):** query-aware shared-cache keys, origin cache-control
  and validator handling, same-key miss coalescing, and one-chunk lockstep streaming to the client
  and RAM/SSD/HDD. A disk failure abandons only that fill; a client disconnect does not. Cached hits
  return through the ordinary head-first/disk-prefetch path. The default libcurl origin client
  supports HTTP(S); `--mirror-client uring` selects a strict persistent-HTTP/1.1 fast path for
  well-behaved plaintext origins. See
  [ADR-0021](docs/adr/0021-http-mirror-cache.md).

> **Status:** working memcache + HTTP/HTTPS server on io_uring + O_DIRECT — 3-tier store, atomic
> publish, RAM-head GET, read-ahead pipeline, TTL/CAS, graceful shutdown. **281 unit-test
> cases, with Release, ASan/UBSan, and TSan coverage.** macOS is a non-goal (no io_uring / O_DIRECT
> analog); FreeBSD (kqueue/aio) is a planned port.

## Dependencies

**Build:** **C++23** compiler (GCC ≥ 14, developed on GCC 16; or Clang ≥ 18), **CMake ≥ 3.28** +
**Ninja**, **OpenSSL** (`libssl-dev`, for HTTPS and the long-input SHA-256 fallback where selected;
short-key and hardware SHA implementations are vendored, [ADR-0014](docs/adr/0014-keyless-digest-identity.md)), **liburing** (`liburing-dev`, the io_uring
backend — without it the logic layers still compile but the server cannot serve), and **libcurl**
(`libcurl4-openssl-dev`, optional unless `--mirror` is used). Native RDMA is
optional and is built when **libibverbs** and **librdmacm** development headers are present.
ExaSock is a separate Linux-only opt-in (`GOBLIN_ENABLE_EXASOCK` for the server and
`GOBLIN_STORE_CLIENT_ENABLE_EXASOCK` for the client); it must already be installed under its own
license, and default builds do not discover or link it.

**Runtime:** Linux kernel ≥ 5.19 (project io_uring support floor).

```sh
sudo apt-get install -y build-essential cmake ninja-build libssl-dev liburing-dev \
    libcurl4-openssl-dev libibverbs-dev librdmacm-dev
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

# Secure remote memcache: an external bind is explicit, TLS replaces plaintext on port 11211,
# and every connection must complete the memcached ASCII auth exchange before any other command.
# Supplying certs for memcache TLS alone does not also open the HTTPS listener; add --https to do so.
./build/goblin-store --listen-address :: --memcache-tls --auth-file /etc/goblin-store/users \
    --tls-cert /etc/goblin-store/server.crt --tls-key /etc/goblin-store/server.key \
    --no-http --memory 4G --ssd-dir /mnt/ssd/pool

# Local-only memcache over a mode-0600 Unix socket. --no-memcache disables only TCP.
./build/goblin-store --no-memcache --no-http --memcache-socket /run/goblin-store/memcache.sock \
    --memory 4G --ssd-dir /mnt/ssd/pool

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

# Reverse HTTP cache. /a/b.html?v=2 misses to
# https://origin.example/z/a/b.html?v=2 and later hits use the normal RAM-head pipeline.
./build/goblin-store --mirror https://origin.example/z \
    --memory 4G --ssd-dir /mnt/ssd/pool --http-port 8080

# Linux native-origin fast path: strict HTTP/1.1, persistent connections, Content-Length/chunked.
./build/goblin-store --mirror http://origin.example/z --mirror-client uring \
    --memory 4G --ssd-dir /mnt/ssd/pool --http-port 8080
```

Key knobs (see `--help`): `--ram-head` (power-of-two per-object resident head, default 256 KiB),
`--ssd-prefix` (positional tier size), `--block` (power-of-two allocation/promotion block, default
2 MiB on x86 and 32 MiB on Arm/LoongArch), `--io-buffers` /
`--io-chunk` (warmed-read quantum), `--write-io-chunk` (admission/write quantum), `--eviction`,
`--max-objects`, `--max-object-size`, `--no-mlock` (dev), `--tls-cert`/
`--tls-key` (HTTPS and memcache TLS), `--memcache-tls`, `--auth-file`, `--memcache-socket`,
`--disk-high-watermark` / `--disk-low-watermark` / `--disk-reclaim-interval`,
`--source` (preload a directory tree), `--numa NODE` (explicit NUMA
placement), `--mirror URL` (streaming HTTP(S) reverse-cache origin; incompatible with virtual-host
mode), `--mirror-client curl|uring`, `--memory SIZE` / `--sub-memory SIZE` (fixed-head RAM on the local / each non-local
NUMA node), `--small-memory SIZE` / `--small-sub-memory SIZE` (packed-small-object RAM on the local /
each non-local node), `--increment FLOAT` (score added per successful key read), `--decay FLOAT`
(per-minute score multiplier in `(0, 1)`), and `--perverse` (benchmark-only inversion of preferred
head-memory placement).

### Network security

TCP binds to `127.0.0.1` by default. Use an exact numeric IPv4/IPv6 address—or `0.0.0.0`/`::`
for a wildcard—only when remote access is intended. `::` is configured as a dual-stack wildcard
where the kernel permits it. IPv6 endpoints are logged in bracketed form.

`--memcache-tls` replaces plaintext on `--memcache-port` with TLS 1.3 and requires at least one
paired `--tls-cert`/`--tls-key`. The first certificate is the memcache default because ordinary
memcache clients need not send SNI. The async streaming path requires Linux kTLS so encrypted
resident heads and disk tails can keep using the normal io_uring send pipeline. Memcache TLS is
therefore rejected with the blocking and ExaSock backends rather than silently serving plaintext.
Certificate options used with `--memcache-tls` do not implicitly open HTTPS; add `--https` when both
TLS services are intended.

`--auth-file FILE` uses the memcached ASCII authentication format: one `user:password` record per
line, up to 256 bytes. The file must be a regular file with no group/other permission bits (normally
mode `0600`). A client authenticates with memcached's initial fake storage request—`set` followed by
a `username password` value—and may retry after `CLIENT_ERROR authentication failure`. Authentication
also covers `--memcache-socket`; it deliberately cannot be combined with the current native RDMA
endpoint, which does not yet carry the ASCII login exchange. `stats` exposes `auth_cmds` and
`auth_errors`, while `stats settings` reports `auth_enabled_ascii`, `memcache_tls`, and the Unix
socket path when configured.

`--memcache-socket PATH` creates an AF_UNIX listener with `--memcache-socket-mode 0600` by default.
A stale socket node is replaced, but startup refuses to remove a live socket or a non-socket at that
path. The socket is removed on shutdown. It shares the process-wide connection budget and overload
counters with the TCP listeners.

TCP overload is contained by a process-wide `--max-connections` budget shared by memcache, HTTP,
HTTPS, and ExaSock, plus a configurable `--listen-backlog`. Idle keepalives expire after
`--idle-timeout`; GET and SET buffer wait queues have independent per-worker bounds
(`--max-get-waiters` / `--max-set-waiters`) and a `--queue-timeout`. Set a timeout to zero only when
explicitly opting out. Memcache `stats` reports `rejected_connections`, `listen_disabled_num`,
`idle_drops`, and `queue_drops` alongside the existing transfer/backpressure counters.
It also reports each distinct backing filesystem once per tier as
`goblin_{ssd,hdd}_filesystem_N_*`: byte capacity, used, free, and unprivileged-available space;
total, used, free, and unprivileged-available inodes; and the numeric device ID. The accompanying
`goblin_{ssd,hdd}_filesystem_count` fields make the indexed series straightforward to discover.
These are live `fstatvfs()` gauges and do not scan cached objects.
The same response includes a live buddy free-list histogram named
`goblin_{ram,ram_head,small_pool}_buddy_free_SIZE_blocks`, where `SIZE` is the power-of-two block
size in bytes. Counts describe the allocator's currently coalesced blocks: a free larger block is
reported once at its current size rather than multiplied into every smaller size it could satisfy.
Fractional small objects use compactable bump arenas, so their unused and fragmented space is not
misreported as buddy blocks.

### Disk health, quarantine, and readiness

Every backing filesystem is tracked by its device ID even when several configured pool directories
share the same mount. Disk read, write, and capacity errors are counted per device, with the last
errno and error time. A device becomes `degraded` after a hardware-class read error; a
hardware-class write error makes it `read_only`, while disappearance errors such as `ENODEV` make
it `failed`. Degraded devices may continue serving cached data, but read-only and failed storage
makes the process not ready and rejects new disk-backed stores before body admission. `ENOSPC` and
`EDQUOT` remain capacity events rather than hardware failures: memcache reports `SERVER_ERROR out
of space`, while a hardware failure reports `SERVER_ERROR storage I/O failure` and an already
read-only device reports `SERVER_ERROR storage is read-only`.

A failed or short tail read still truncates the response and closes its connection once response
bytes have begun; no replacement status can be sent honestly at that point. Before closing, Goblin
quarantines the exact immutable generation that failed. It removes that object from the index and
all eviction policies and unlinks its shards. A concurrent replacement is protected by the
generation check and remains live. `quarantined_objects` and `quarantine_failures` expose the result.

The background storage-health thread checks live byte and inode availability every
`--disk-reclaim-interval` milliseconds (default 1000). At `--disk-high-watermark` (default 0.90),
it uses the existing filesystem-local victim policy to reclaim whole objects toward
`--disk-low-watermark` (default 0.80). Each pass is bounded and later passes continue the work, so
maintenance cannot monopolize serving threads. Set the interval to zero to disable proactive
reclaim; foreground `fallocate`/`pwrite` results remain authoritative either way.

Memcache `stats` exposes `storage_state`, `storage_ready`, quarantine and watermark counters, plus
`storage_device_N_{id,paths,state,read_errors,write_errors,capacity_errors,last_errno,last_error_unix}`.
HTTP and HTTPS reserve `GET`/`HEAD /__goblin/ready`: it returns JSON with HTTP 200 for healthy or
degraded-but-serving storage and HTTP 503 for read-only or failed storage. The endpoint is handled
locally even in mirror mode and is never sent to the origin.

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
key index, I/O buffers, and ordinary thread allocations on the serving node. For a loopback/Unix-only
deployment without `--numa`, Goblin selects the first online node allowed by the inherited
taskset/cgroup. For an external exact or wildcard bind, the serving node is derived from the covered
UP interfaces. If those interface addresses belong to different nodes—or locality is unknown on a
multi-node host—startup stops and reports each Linux interface name, listening address, NUMA node,
and the corresponding `--numa NODE` override.

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
