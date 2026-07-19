# Small-object HTTP caching on Optane: Goblin Store versus Vinyl Cache

On July 18, 2026, we compared Goblin Store with Vinyl Cache on a completely warmed, 373,165-file
HTTP workload backed by an Intel Optane SSD 900P. Goblin Store's best population run loaded the
cache at **12.659 Gbit/s**, 72.5% faster than Vinyl's 7.341 Gbit/s. Once warm, 256 KiB heads reached
**14.611 Gbit/s**, 7.9% above Vinyl, while requiring 22.176 GiB of resident object data rather than
Vinyl's 62.847 GiB page-rounded working set.

Head size changes the best result depending on what matters. A 64 KiB head produced the lowest
first-byte latency—162.589 microseconds on average, 14.8% below Vinyl—while 128 KiB was the practical
knee in the aggregate-throughput curve. A 256 KiB head delivered the highest bandwidth, lowest
mean completion latency, and least Optane read traffic.

This is a small-*file* cache test, not the sub-2-KiB memcache-value case. The mean object is about
175 KiB, and the distribution extends beyond 1 MiB. It exercises both Goblin's packed RAM-only
object arena and its RAM-head-plus-Optane-tail path.

## Data set and fixed request sequence

The corpus was copied to the Optane filesystem before the benchmark. Size-aware manifests were
generated once and used unchanged by every server and head size.

| Property | Value |
|---|---:|
| Objects | 373,165 |
| Payload bytes | 66,693,472,653 bytes |
| Payload size | 62.113 GiB |
| Mean object size | 178,723.8 bytes |
| Prewarm requests | 373,165—every object exactly once |
| Measured requests | 6,141,630 |
| Measured payload | 285,757,364,203 bytes / 266.132 GiB |
| Measured concurrency | 8 persistent HTTP connections |
| Request ordering | Fixed manifest order, identical for every case |

The corpus shape is important when interpreting the head sweep:

| Maximum object size | Objects at or below size | Measured requests entirely resident at this head |
|---:|---:|---:|
| 16 KiB | 116,720 | 52.36% |
| 32 KiB | 200,196 | 75.12% |
| 64 KiB | 245,409 | 84.06% |
| 128 KiB | 319,623 | 94.50% |
| 256 KiB | 343,728 | 97.93% |

## Software and transport

| Component | Tested version or identity |
|---|---|
| Vinyl Cache | 9.0.1, revision `423648c4cb6b225b3268ffc337354ea938f5efee` |
| Goblin Store | 0.0.2 development benchmark build |
| Origin | nginx 1.18.0, 24 workers |

Both caches served ordinary HTTP/1.1 over TCP/IPoIB. Goblin Store's native verbs/RDMA protocol was
not used. The remote client therefore saw two wire-compatible HTTP caches at the same address and
port. Goblin's empty-cache population used its purpose-built persistent HTTP/1.1 io_uring origin
client; Vinyl used its normal backend fetch path.

## Hardware and topology

### Cache server

The cache server was a Dell PowerEdge R820, a four-socket Ivy Bridge-EP machine with 1.5 TiB-class
RAM:

| Component | Configuration |
|---|---|
| CPU | 4 x Intel Xeon E5-4657L v2 at 2.40 GHz |
| Cores | 12 cores/socket, 48 physical cores, 96 logical CPUs |
| CPU caches | 1.5 MiB L1d, 1.5 MiB L1i, 12 MiB L2, 120 MiB L3 across the machine |
| NUMA | Four nodes, one per socket; distances `10/20/30/20` |
| RAM | 1,623,299,801,088 bytes reported by Linux—1.476 TiB usable |
| RAM per node | Approximately 387,000 MiB / 378 GiB |
| Kernel | Linux `5.15.0-157-generic`, Ubuntu build `#167` |

The server's Mellanox HCA was attached to NUMA node 1. Both cache implementations were restricted
to eight physical node-1 cores and both SMT siblings:

```text
1,5,9,13,17,21,25,29,49,53,57,61,65,69,73,77
```

The pairs `(1,49)` through `(29,77)` are SMT siblings, giving each cache 16 allowed logical CPUs.
Goblin was explicitly started with `--cores 8`: eight asynchronous workers, each owning an
`SO_REUSEPORT` listener and an io_uring loop. A connection remained on its accepting loop for its
entire lifetime. Vinyl received the identical external CPU and NUMA affinity.

The nginx origin was isolated on NUMA node 0 and allowed all 24 node-0 logical CPUs. It listened
only on server loopback, so origin population traffic never crossed the InfiniBand fabric.

### Optane storage

| Property | Value |
|---|---|
| Device | Intel Optane SSD 900P |
| Model | `INTEL SSDPED1D960GAY` |
| Firmware | `E2010435` |
| PCI identity | `0000:04:00.0`, Intel `8086:2700` |
| NUMA node | 0 |
| Filesystem | XFS on `/dev/nvme0n1`, mounted at `/mnt/local` |
| Filesystem geometry | 4 KiB data blocks, four allocation groups, internal log |
| Usable filesystem size | 894 GiB |

The Optane controller was on node 0 while the cache workers and HCA were on node 1. Goblin's
O_DIRECT tail reads therefore crossed one socket boundary before reaching the network-local
workers. Vinyl's fully warmed file-backed cache produced no physical reads in its measured window;
Goblin continued reading nonresident tails directly from Optane by design.

Before the cache comparison, queue-depth-one direct-I/O measurements characterized the device:

| Read size | p50 latency | p99 latency | Bandwidth |
|---:|---:|---:|---:|
| 4 KiB | 16.512 us | 24.704 us | 139 MiB/s |
| 8 KiB | 17.024 us | 25.984 us | 258 MiB/s |
| 16 KiB | 21.632 us | 31.104 us | 415 MiB/s |
| 32 KiB | 29.824 us | 42.752 us | 618 MiB/s |
| 64 KiB | 41.728 us | 64.256 us | 1,029 MiB/s |

### Client and network

The client was a second Dell PowerEdge R820 with the same four-socket CPU topology:

| Component | Configuration |
|---|---|
| CPU | 4 x Intel Xeon E5-4657L v2 at 2.40 GHz |
| Cores | 48 physical cores, 96 logical CPUs |
| NUMA | Four nodes; the HCA was on node 1 |
| RAM | 473,323,020,288 bytes / 440.8 GiB reported by Linux |
| Kernel | Linux `5.15.0-185-generic`, Ubuntu build `#195` |

The io_uring benchmark client and its memory were pinned to logical CPU 45 on the HCA-local node 1.
It multiplexed all eight persistent measured connections from that one CPU and retained the
nanosecond trace in `/dev/shm`, avoiding NFS or local-disk logging during a run.

Both endpoints used Mellanox Technologies MT27600 Connect-IB adapters (`15b3:1011`) at 40 Gbit/s.
The server HCA was PCI `0000:42:00.0`; the client HCA was `0000:41:00.0`; both were local to node 1.
IPoIB used connected mode and MTU 65,520.

```text
server node 0: nginx origin + Optane 900P
         |
         | one NUMA hop for Goblin tail I/O
         v
server node 1: cache workers + Connect-IB HCA
         |
         | 40 Gbit/s connected-mode IPoIB
         v
client node 1: Connect-IB HCA + io_uring client on CPU 45
```

## Cache configurations

### Vinyl Cache

Vinyl received a fully allocated 96 GiB file on the Optane XFS filesystem, with 4 KiB granularity
and random access advice. The VCL made origin responses cacheable for seven days, disabled grace
and keep, and streamed cache fills to the requesting client.

```text
numactl --physcpubind=<eight node-1 cores and SMT siblings> --membind=1 \
  vinyld -F -j none -a 10.88.88.3:8080 -f benchmark.vcl \
  -s file,vinyl.bin,96G,4K,random \
  -p default_grace=0 -p default_keep=0
```

Vinyl's object-memory comparison below is the corpus rounded to its 4 KiB storage granularity:
62.847 GiB. It excludes cache metadata but represents the data pages needed to keep this working
set completely warm.

### Goblin Store

Every Goblin case used the same pool and I/O geometry; only `--ram-head` changed:

```text
numactl --physcpubind=<eight node-1 cores and SMT siblings> --membind=1 \
  goblin-store --mirror http://127.0.0.1:18000 --mirror-client uring \
  --listen-address 10.88.88.3 --http-port 8080 --no-memcache --net async \
  --numa 1 --cores 8 --memory 48G --small-memory 48G \
  --block 2M --ram-head <16K|32K|64K|128K|256K> \
  --io-chunk 256K --write-io-chunk 1M --no-numa-promotion \
  --ssd-dir <fresh Optane pool>
```

The 48 GiB fixed-head reservation and 48 GiB packed-small-object reservation were strictly local to
node 1 and did not borrow from one another. There was no `--sub-memory` or
`--small-sub-memory`. Promotion was disabled because all allocated RAM was already local.

The configured 96 GiB was a deliberately oversized, constant ceiling, not the amount required by
the corpus. Objects smaller than the selected head were stored in a compactable arena at 16-byte
alignment; a 1 KiB object therefore consumed about 1 KiB, not an entire 256 KiB head. Objects at
least as large as the selected head consumed one full fixed-head slot. Allocation blocks remained
2 MiB and attempted HugeTLB backing.

## Benchmark procedure

Each result followed the same controlled sequence:

1. Stop prior cache, origin, client, and monitoring processes. Require `numad` to be inactive and
   verify that the cache and origin ports are unused.
2. Trim `/mnt/local` with `fstrim`. Create a unique, empty cache pool for the case; Vinyl's 96 GiB
   storage file was freshly allocated rather than reused.
3. Before each Goblin start, write `1` to `/proc/sys/vm/compact_memory`, then request 49,152 2 MiB
   HugeTLB pages from node 1—exactly 96 GiB. All reported head runs obtained the requested pages.
   The prior HugeTLB setting was restored during cleanup.
4. Start the 24-worker nginx origin on node 0 and the cache on node 1 with the affinity shown above.
   Verify the server/client route, IPoIB interface, connected mode, MTU, and NUMA locality.
5. From the remote client, fetch all 373,165 objects once at concurrency 32. This is the population
   measurement. The runner required exactly 373,165 nginx access-log records.
6. Make a 1,000-request, concurrency-one hit probe. Any additional origin request invalidated the
   run.
7. Replay the exact 6,141,630-row measured manifest at concurrency 8. The client recorded wall and
   monotonic nanosecond timestamps before the request, at first response byte, at first body byte,
   and at the last declared body byte. Results stayed in client RAM until the pass ended.
8. Flush and recount the origin log. The count had to remain 373,165: every measured request was a
   cache hit. Capture process CPU, RSS, `/proc` I/O counters, VM and NUMA state, pressure metrics,
   and Optane temperature once per second; then checksum the result traces.

The same prewarm and measured manifests were copied into client tmpfs and identified by SHA-256.
Every reported run completed all requests with HTTP 200, exact body length, zero client errors, and
no post-prewarm origin traffic.

## Cache population

Population simultaneously fetched each object from local nginx, returned it over IPoIB, and
admitted it to the empty cache. This measures the origin-fetch and storage-admission path as well
as client delivery.

| Server/head | Elapsed | Aggregate bandwidth | Bandwidth versus Vinyl | Time reduction |
|---|---:|---:|---:|---:|
| Vinyl | 72.684 s | 7.341 Gbit/s | baseline | baseline |
| Goblin 16 KiB | 49.981 s | 10.675 Gbit/s | +45.4% | 31.2% |
| Goblin 32 KiB | 48.297 s | 11.047 Gbit/s | +50.5% | 33.6% |
| Goblin 64 KiB | 47.381 s | 11.261 Gbit/s | +53.4% | 34.8% |
| Goblin 128 KiB | 42.392 s | 12.586 Gbit/s | +71.5% | 41.7% |
| Goblin 256 KiB | **42.146 s** | **12.659 Gbit/s** | **+72.5%** | **42.0%** |

The native io_uring origin client and 1 MiB admission/write chunks make this a materially different
miss path from the earlier libcurl population baseline. The three repeated 16 KiB native-client
runs ranged from 9.959 to 10.675 Gbit/s, supporting the direction of the population result rather
than treating its best run as an isolated sample.

## Completely warmed performance

The warmed pass transferred the same 266.132 GiB through every cache. Average CPU is process-wide:
100% means one completely utilized logical CPU. Object RAM is the computed resident data required
by this corpus, not the deliberately oversized 96 GiB configured capacity.

| Server/head | Packed small objects | Fixed heads | Total object RAM | RAM saved vs. Vinyl | Aggregate bandwidth | Bandwidth vs. Vinyl | Average CPU | Mean TTFB | TTFB vs. Vinyl | Mean TTLB | TTLB vs. Vinyl |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Vinyl | — | — | 62.847 GiB | baseline | 13.541 Gbit/s | baseline | 353.7% | 190.824 us | baseline | 216.261 us | baseline |
| Goblin 16 KiB | 1.042 GiB | 3.913 GiB | **4.955 GiB** | 92.1% | 12.483 Gbit/s | -7.8% | 275.2% | 204.096 us | -7.0% | 235.118 us | -8.7% |
| Goblin 32 KiB | 2.741 GiB | 5.279 GiB | **8.020 GiB** | 87.2% | 13.666 Gbit/s | +0.9% | 286.8% | 182.533 us | +4.3% | 214.485 us | +0.8% |
| Goblin 64 KiB | 4.775 GiB | 7.798 GiB | **12.573 GiB** | 80.0% | 14.209 Gbit/s | +4.9% | 282.9% | **162.589 us** | **+14.8%** | 206.117 us | +4.7% |
| Goblin 128 KiB | 10.998 GiB | 6.540 GiB | **17.538 GiB** | 72.1% | 14.413 Gbit/s | +6.4% | 272.2% | 166.453 us | +12.8% | 203.170 us | +6.1% |
| Goblin 256 KiB | 14.984 GiB | 7.193 GiB | **22.176 GiB** | 64.7% | **14.611 Gbit/s** | **+7.9%** | **271.6%** | 166.978 us | +12.5% | **200.241 us** | **+7.4%** |

The Goblin footprint calculation sums each sub-head object rounded to 16 bytes and one full head
for each object at least as large as the configured head. Index metadata and transient I/O buffers
are excluded. Rounding the separate small and fixed pools up to complete 2 MiB allocation blocks
adds less than 3 MiB in every case.

The CPU values are one-second samples restricted to each exact measured-client wall-clock window.
They include all cache worker threads but exclude nginx and the remote-client SSH wrapper. The
256 KiB case delivered roughly 5.38 Gbit/s per utilized logical CPU, versus 3.83 for Vinyl—about
40.5% more payload bandwidth per unit of measured cache-process CPU.

### First-byte latency distribution

The means above summarize all requests; the distribution shows why 64 KiB is the latency choice.
Percentiles are nearest-rank and were computed from all 6,141,630 requests in each trace.

| Server/head | p50 TTFB | p90 TTFB | p99 TTFB | p99.9 TTFB |
|---|---:|---:|---:|---:|
| Vinyl | 170 us | 263 us | 599 us | 1,326 us |
| Goblin 16 KiB | 176 us | 335 us | 595 us | 795 us |
| Goblin 32 KiB | **151 us** | 305 us | 502 us | 721 us |
| Goblin 64 KiB | **151 us** | **231 us** | **367 us** | **508 us** |
| Goblin 128 KiB | 154 us | 239 us | 379 us | 531 us |
| Goblin 256 KiB | 155 us | 238 us | 388 us | 553 us |

The 128 and 256 KiB rows give up only a few microseconds of first-byte latency relative to 64 KiB,
while making a larger fraction of the trace entirely resident. The difference is small enough that
replication is warranted before treating the few-microsecond ordering as universal; the much larger
improvement from 16 to 64 KiB is not subtle.

## Optane traffic and the head-size knee

Goblin bypasses the kernel page cache for disk-backed tails. Increasing the resident head reduced
the process's physical reads during the 266.132 GiB warmed pass:

| Head | Physical Optane reads | Reduction versus 16 KiB | Warm bandwidth |
|---:|---:|---:|---:|
| 16 KiB | 208.332 GiB | baseline | 12.483 Gbit/s |
| 32 KiB | 175.693 GiB | 15.7% | 13.666 Gbit/s |
| 64 KiB | 136.946 GiB | 34.3% | 14.209 Gbit/s |
| 128 KiB | 101.999 GiB | 51.0% | 14.413 Gbit/s |
| 256 KiB | 77.978 GiB | 62.6% | 14.611 Gbit/s |

The curve has three useful operating points:

- **64 KiB for first-byte latency.** It had the best mean, p90, p99, and p99.9 TTFB while using
  80.0% less resident object RAM than Vinyl.
- **128 KiB for balance.** It retained 94.5% of measured requests completely in RAM and delivered
  98.7% of the 256 KiB row's aggregate bandwidth with a 17.538 GiB object footprint.
- **256 KiB for aggregate bandwidth and completion time.** It produced the highest warm throughput,
  lowest mean TTLB, and lowest Optane traffic while still using 64.7% less object RAM than Vinyl.

Optane temperature did not explain the results. Across the head sweep the composite sensor remained
between 35.85 and 38.85 degrees Celsius. The 256 KiB measured window averaged 37.08 degrees.

## Scope and limitations

The Vinyl row and each new head size are one complete run, while the native 16 KiB population path
has three repetitions. Small differences—especially the few-microsecond gap between 128 and
256 KiB TTFB—should be treated as point estimates until replicated.

This test deliberately held server affinity, pool capacity, transport, origin, client CPU,
concurrency, and request order constant. It does not establish maximum 40 Gbit/s throughput, because
the one-CPU client and cache path reached 14.6 Gbit/s rather than saturating the fabric. It also does
not represent tiny memcache values, a multi-SSD tier, concurrent cache misses during the measured
phase, or a workload larger than the configured cache.

The object-RAM comparison excludes index metadata for both products. It answers the narrower policy
question: how many bytes of this corpus must remain resident under Vinyl's page-rounded whole-object
model and Goblin's packed-small-object plus fixed-head model?

## Reproduction

- [`bench/run_optane_vinyl_baseline.sh`](https://github.com/adamdeprince/goblin-store/blob/master/bench/run_optane_vinyl_baseline.sh) creates the Vinyl
  cache, starts nginx, checks origin counts, and runs the measured trace.
- [`bench/run_optane_goblin_baseline.sh`](https://github.com/adamdeprince/goblin-store/blob/master/bench/run_optane_goblin_baseline.sh) configures Goblin,
  compacts memory, reserves HugeTLB pages, and runs one complete head-size case.
- [`bench/run_optane_head_sweep.sh`](https://github.com/adamdeprince/goblin-store/blob/master/bench/run_optane_head_sweep.sh) trims the filesystem and
  invokes the complete Goblin runner for 32, 64, 128, and 256 KiB heads.
- [`bench/mirror_proxy_benchmark.cpp`](https://github.com/adamdeprince/goblin-store/blob/master/bench/mirror_proxy_benchmark.cpp) is the io_uring HTTP
  client that records the per-request nanosecond trace.
- [`bench/monitor_benchmark_resources.py`](https://github.com/adamdeprince/goblin-store/blob/master/bench/monitor_benchmark_resources.py) records CPU,
  process memory, pressure, VM, I/O, and NUMA counters.
- [`bench/monitor_nvme_temperature.py`](https://github.com/adamdeprince/goblin-store/blob/master/bench/monitor_nvme_temperature.py) records the Optane
  composite temperature once per second.
