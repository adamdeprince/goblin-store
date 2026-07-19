# Optane small-object mirror-cache benchmark notes

Work log for the benchmark running on July 17, 2026. This file records the durable setup and
measurements while large trace artifacts remain on the benchmark host.

## Hardware and placement

- Four-socket Intel Xeon E5-4657L v2 at 2.40 GHz: 48 physical cores, 96 hardware threads, and four
  NUMA nodes.
- 1.5 TiB RAM, approximately 387 GiB on each NUMA node.
- `/mnt/local` is XFS on an Intel Optane SSD 900P (`INTEL SSDPED1D960GAY`), PCI `0000:04:00.0`,
  NUMA node 0. It is not the operating-system RAID filesystem.
- The 40 Gbit/s Connect-IB IPoIB adapter is on NUMA node 1. The Intel X540 and ExaNIC X100 are on
  NUMA node 0.
- The measured client is a second four-socket host on the same Connect-IB fabric. Both IPoIB
  interfaces use connected mode and MTU 65,520. The server is `10.88.88.3` and the client is
  `10.88.88.2`; host names are for the work log and can be removed from a public report.
- Vinyl and the eventual Goblin Store server run on NUMA node 1 with the HCA. The Optane device is
  on node 0. The C++ client is pinned to CPU 45 on the client's HCA-local node 1 and writes its
  trace to `/dev/shm`, not NFS or a client disk.
- Benchmark data, cache files, tools, and results use `/mnt/local`; `/var/tmp` is excluded.

## Corpus and request sequence

- Payload files: 373,165.
- Payload bytes: 66,693,472,653 bytes (62.113 GiB).
- Mean object size: 178,723.8 bytes.
- Warm order: `/mnt/local/prefetch.txt`, exactly 373,165 rows.
- Measured order: `/mnt/local/order.txt`, exactly 6,141,630 rows and 285,757,364,203 requested
  bytes (266.132 GiB).
- Generated size-aware manifests live under
  `/mnt/local/goblin-small-bench/manifests/` on the benchmark host.

Object counts at or below selected sizes:

| Maximum size | Objects |
|---:|---:|
| 2 KiB | 18,466 |
| 4 KiB | 25,099 |
| 8 KiB | 41,995 |
| 16 KiB | 116,720 |
| 32 KiB | 200,196 |
| 64 KiB | 245,409 |
| 128 KiB | 319,623 |
| 256 KiB | 343,728 |
| 512 KiB | 355,275 |
| 1 MiB | 362,675 |

## Direct Optane latency

Read-only fio 3.28 test against a 150 MiB corpus object: direct I/O, random reads, libaio,
queue depth 1, one job, node-0 CPU and memory binding, a two-second ramp, and ten measured seconds
per block size. Raw output is in
`/mnt/local/goblin-small-bench/results/optane-latency/` on the benchmark host.

| Read size | p50 | p90 | p99 | p99.9 | Bandwidth |
|---:|---:|---:|---:|---:|---:|
| 4 KiB | 16.512 µs | 16.768 µs | 24.704 µs | 42.752 µs | 139 MiB/s |
| 8 KiB | 17.024 µs | 17.536 µs | 25.984 µs | 44.288 µs | 258 MiB/s |
| 16 KiB | 21.632 µs | 22.656 µs | 31.104 µs | 49.408 µs | 415 MiB/s |
| 32 KiB | 29.824 µs | 31.104 µs | 42.752 µs | 59.136 µs | 618 MiB/s |
| 64 KiB | 41.728 µs | 43.776 µs | 64.256 µs | 73.216 µs | 1,029 MiB/s |

## Planned cache configurations

- Vinyl Cache: local file storage on `/mnt/local`, random access advice, completely warmed before
  the one-pass measured sequence. Small objects require 4 KiB file-storage granularity; an initial
  2 MiB-granularity attempt retained only a fraction of the set, produced zero cache hits, and was
  discarded before completion.
- Goblin Store: 48 GiB fixed-head pool and 48 GiB packed-small-object pool, tails on `/mnt/local`,
  completely warmed before measurement.
- Goblin head-size experiment: 4, 8, 16, and 32 KiB, retaining the normal 2 MiB allocation block.
- The benchmark client replays the supplied order exactly and records expected and received bytes,
  response/body TTFB, TTLB, and body-transfer time for every request.
- A loopback Vinyl trace was stopped at the operator's request and is excluded. Final measurements
  use ordinary HTTP/TCP over connected-mode 40 Gbit/s IPoIB.

## Empty-cache warm-up result

The valid Vinyl and 8 KiB-head Goblin runs each began with an empty cache and fetched the same
373,165 objects (66,693,472,653 bytes, 62.113 GiB) from the local Python origin. The remote C++
client used concurrency 32 over connected-mode IPoIB. Both warm-ups completed with zero failed
requests. This phase measures cache population while simultaneously returning each response to the
client; it is not the warmed-cache latency pass.

| Cache | Elapsed | Aggregate throughput | Objects/s | Failures |
|---|---:|---:|---:|---:|
| Vinyl Cache 9.0.1, 96 GiB file cache, 4 KiB granularity | 656.367 s | 0.81288 Gbit/s | 568.5 | 0 |
| Goblin Store, 8 KiB heads, 48 GiB head + 48 GiB small-object pools | 253.976 s | 2.10078 Gbit/s | 1,469.3 | 0 |

Goblin populated the cache **2.584x faster**, reducing warm-up time by **61.3%** and saving
402.391 seconds (6 minutes 42.391 seconds). Its aggregate warm-up throughput was 158.4% higher.
After each warm-up, a 1,000-request concurrency-1 probe completed with zero failures and generated
no additional origin requests before the full measured pass began.

The Vinyl artifacts are in
`/mnt/local/goblin-small-bench/results/vinyl-baseline-20260718T000049Z`; the 8 KiB Goblin artifacts
are being written to
`/mnt/local/goblin-small-bench/results/goblin-8k-baseline-20260718T004944Z`.

## Thermal telemetry for the 4 KiB run

The 4 KiB-head run will sample the Optane controller's Linux hwmon sensor once per second and save
the line-buffered readings as `nvme-temperature.csv` beside the other run artifacts. The sensor is
explicitly tied to `nvme0`, model `INTEL SSDPED1D960GAY`, PCI `0000:04:00.0`, so it cannot be
confused with the RMS-200 exposed as `nvme1`. A spot reading during the 8 KiB measured pass was
36.85 degrees Celsius; that single reading does not establish whether the device throttled earlier
or later in the run.

## Nginx origin for follow-up runs

The completed head-size and concurrency comparisons used the Python `ThreadingHTTPServer` origin
described above. Follow-up runs replace it with Ubuntu nginx 1.18.0 so origin implementation and
per-request Python overhead are less likely to constrain cache population. The isolated nginx
instance listens only on `127.0.0.1:18000`, uses 24 workers pinned to origin NUMA node 0, serves the
same `/mnt/local/data` corpus with `sendfile`, and writes a buffered access log that the runner
flushes before checking miss counts. The system nginx service and its default port-80 site remain
disabled; each benchmark owns the origin process and stops it during cleanup.

A short origin-only validation retrieved the first 10,000 manifest objects (756,346,266 bytes,
0.704 GiB) at concurrency 32. It completed in 0.437092 seconds at 13.8433 Gbit/s with zero failures
and exactly 10,000 access-log rows. This is a capacity check, not a cache result, but it is well above
the previous 2.09 Gbit/s Goblin population rate. Warming results from nginx-origin follow-up runs
must be identified separately from the earlier Python-origin results.

## Concurrency-12 result

The matched concurrency-12 comparison used the 16 KiB Goblin head selected by the single-connection
head-size sweep. Both cache servers were allowed all 24 logical CPUs on the IPoIB-local NUMA node.
The C++ client was also allowed all 24 logical CPUs on its IPoIB-local NUMA node, instead of the
single client CPU used by the earlier concurrency-1 and concurrency-8 runs. Prewarming remained at
concurrency 32, the cache-hit gate at concurrency 1, and the measured exact-order pass used 12
persistent HTTP connections.

### Cache loading

Both empty caches loaded the same 373,165 objects and 62.113 GiB with zero failures.

| Cache | Load time | Aggregate throughput |
|---|---:|---:|
| Vinyl Cache | 519.236 s | 1.02756 Gbit/s |
| Goblin Store, 16 KiB heads | **254.804 s** | **2.09395 Gbit/s** |

Goblin loaded **2.04x faster**, reducing population time by **50.9%**.

### Completely warmed reads

Each implementation served the same 6,141,630 requests and 285,757,364,203 bytes (266.132 GiB).
The target CPU figures are one-second samples restricted to the exact measured-pass wall-clock
window. CPU percentages are process-wide, so 100% represents one fully utilized logical CPU.

| Metric | Vinyl Cache | Goblin Store, 16 KiB heads |
|---|---:|---:|
| Elapsed time | 351.626 s | **293.021 s** |
| Aggregate throughput | 6.5014 Gbit/s | **7.8017 Gbit/s** |
| Average server CPU | 346.6% | **193.7%** |
| Peak server CPU | 492.0% | **250.0%** |
| Physical reads during measured window | 0 GiB | 208.823 GiB |
| Failed requests | 0 | 0 |

Goblin delivered **20.0% higher throughput** and completed in **16.7% less time**, saving 58.605
seconds. It used **44.1% less average server CPU** and delivered about **2.15x as much throughput per
utilized server CPU core**. This is particularly notable because Goblin deliberately bypassed the
page cache and physically read 208.823 GiB of object tails from Optane; Vinyl's completely warmed
file cache generated no physical reads during its measured pass.

Both implementations passed a 1,000-request hit probe, made exactly 373,165 origin requests during
prewarming and none afterward, produced complete 6,141,631-line measured CSV files including their
headers, and recorded result checksums. The Optane temperature during Goblin ranged from 34.85 to
38.85 degrees Celsius, with no evidence of thermal stress.

Absolute concurrency-12 throughput was lower than the earlier concurrency-8 run. Those cases are
not directly comparable because concurrency 8 pinned the entire client to one CPU, while concurrency
12 allowed the client across all 24 local CPUs. The concurrency-12 pair is internally matched and
valid, but the absolute regression indicates a client-scheduling or concurrency knee that warrants
the planned concurrency-6 measurement before drawing a scaling curve.

Artifacts:

- Vinyl: `/mnt/local/goblin-small-bench/results/vinyl-baseline-20260718T034312Z-c12-vinyl`
- Goblin: `/mnt/local/goblin-small-bench/results/goblin-16k-baseline-20260718T034312Z-c12-goblin16k`
- Pair controller: `/mnt/local/goblin-small-bench/results/concurrency12-pair-20260718T034312Z`

## Eight-core nginx/io_uring comparison

This follow-up removes two earlier harness limits. The origin is nginx 1.18.0 rather than Python's
HTTP server, and the client is the single-event-loop io_uring implementation. Each cache server is
restricted to eight physical cores on the IPoIB-local NUMA node; both SMT siblings are allowed, for
16 logical CPUs in the affinity mask. Goblin is also explicitly configured with `--cores 8`.
Prewarming uses concurrency 32 and the measured pass uses eight persistent connections. The client
remains pinned to one HCA-local CPU, and both implementations replay exactly the same manifests.

### Empty-cache population

Both implementations fetched all 373,165 objects (62.113 GiB) from nginx with zero failures.

| Cache | Load time | Aggregate throughput | Objects/s |
|---|---:|---:|---:|
| Vinyl Cache | **72.684 s** | **7.34069 Gbit/s** | **5,134.1** |
| Goblin Store, 16 KiB heads | 86.268 s | 6.18481 Gbit/s | 4,325.8 |

Goblin achieved 84.25% of Vinyl's population throughput. It was 15.75% slower by throughput and
took 18.69% longer. This is a cache-miss and admission-path result; follow-up work should profile
the origin-fetch, disk-write, index-publication, and response-streaming path separately.

### Completely warmed reads

Each cache served 6,141,630 requests and 266.132 GiB with zero failures.

| Metric | Vinyl Cache | Goblin Store, 16 KiB heads |
|---|---:|---:|
| Elapsed time | **168.828 s** | 183.003 s |
| Aggregate throughput | **13.5408 Gbit/s** | 12.4919 Gbit/s |
| Average server CPU | 214.0% | **195.1%** |
| Peak server CPU | 416.0% | **317.0%** |
| Physical reads during measured window | **0 GiB** | 126.29 GiB |
| Failed requests | 0 | 0 |

Goblin reached 92.25% of Vinyl's throughput. It was 7.75% slower by throughput and took 8.40%
longer. The server was not compute-saturated: Goblin averaged approximately two logical CPUs of
work. Across all 96 host CPUs, the measured interval averaged 5.59% I/O wait, compared with
effectively zero for Vinyl. Vinyl's warmed file-backed mmap was resident in the host page cache;
Goblin deliberately bypassed the page cache and read object tails from Optane with O_DIRECT.

The following means split the identical trace at the 16 KiB resident-head boundary. Objects no
larger than the head require no tail read.

| Request class | Requests | Goblin mean TTFB | Goblin mean TTLB | Vinyl mean TTFB | Vinyl mean TTLB |
|---|---:|---:|---:|---:|---:|
| Entirely resident, <=16 KiB | 3,215,724 | **138.749 us** | **138.749 us** | 174.524 us | 174.524 us |
| Requires a tail, >16 KiB | 2,925,906 | 274.706 us | 340.796 us | **208.740 us** | **262.133 us** |
| All requests | 6,141,630 | 203.519 us | 235.005 us | **190.824 us** | **216.261 us** |

Goblin is faster on the fully resident class. The aggregate deficit comes from requests that cross
the head boundary. A disk-tail request currently opens the immutable per-object file before the
resident head is sent, so filesystem lookup/open latency also appears in TTFB even though the head
bytes themselves are already in RAM.

### Head-size implication

The configured 48 GiB small-object pool plus 48 GiB fixed-head pool is substantially larger than
the resident footprint produced by a 16 KiB head. The fixed trace gives the following coverage and
head-memory estimates; estimates include 16-byte packing for objects smaller than the head and a
full fixed head for larger objects.

| Head size | Measured requests entirely in RAM | Estimated resident head bytes |
|---:|---:|---:|
| 16 KiB | 52.36% | 4.96 GiB |
| 32 KiB | 75.12% | 8.02 GiB |
| 64 KiB | 84.06% | 12.57 GiB |
| 128 KiB | 94.50% | 17.54 GiB |
| 256 KiB | 97.93% | 22.18 GiB |

The immediate follow-up should test 128 and 256 KiB heads. Both fit comfortably inside the
existing split pools, and 256 KiB would remove the per-request file open and O_DIRECT tail read for
nearly 98% of this trace. Longer term, an adaptive policy could retain a modest guaranteed
first-byte head while spending otherwise-idle RAM to make hot or reasonably small objects fully
resident.

Artifacts:

- Vinyl: `/mnt/local/goblin-small-bench/results/vinyl-baseline-20260718T123937Z-8core-uring-nginx-c8-vinyl`
- Goblin: `/mnt/local/goblin-small-bench/results/goblin-16k-baseline-20260718T123937Z-8core-uring-nginx-c8-goblin16k`
- Pair controller: `/mnt/local/goblin-small-bench/results/concurrency8-pair-20260718T123937Z-8core-uring-nginx`

## Native io_uring origin-client follow-up

The mirror miss path now has an optional purpose-built HTTP/1.1 origin client selected with
`--mirror-client uring`. It uses persistent connections and submits connect, send, receive, and
timeout operations through the server's existing io_uring reactor. The common `Content-Length`
path receives payload bytes directly into the chunk handed to the downstream client and cache
writer, avoiding libcurl's callback layer and an intermediate body copy.

This is deliberately a strict client for controlled, well-behaved origins. It accepts valid
HTTP/1.1 responses framed by `Content-Length` or chunked transfer encoding, uses bounded headers
and reasonable connect/I/O timeouts, and rejects ambiguous framing, protocol upgrades,
close-delimited bodies, malformed chunks, and unsolicited bytes. A protocol failure is logged and
returned as an origin error; the uring mode does not silently fall back to libcurl. HTTPS remains
on the libcurl path.

Read and admission I/O geometry were also separated. These runs use 256 KiB warmed-read chunks and
1 MiB cache-admission/write chunks. The larger write operation amortizes per-operation overhead
while a miss is being streamed and stored, without forcing warmed object-tail reads to use the same
geometry. Ordinary cache hits also avoid copying the request key and digest into mirror state;
those values are retained only for a miss or rejected stale snapshot.

All three native runs used the same corpus, request order, nginx origin, NUMA placement, eight
physical server cores with both SMT siblings, concurrency-32 prewarm, concurrency-8 measured pass,
16 KiB heads, 48 GiB fixed-head and 48 GiB small-object pools, and a client pinned to one
HCA-local CPU. Each made exactly 373,165 origin requests during population, none afterward, and
completed without request failures.

### Empty-cache population

| Cache/client path | Load time | Aggregate throughput | Relative to Vinyl |
|---|---:|---:|---:|
| Vinyl Cache | 72.684 s | 7.34069 Gbit/s | baseline |
| Goblin/libcurl, preceding optimized run | 69.174 s | 7.71318 Gbit/s | +5.1% |
| Goblin/native uring, replicate 1 | 50.077 s | 10.6545 Gbit/s | +45.1% |
| Goblin/native uring, replicate 2 | 53.572 s | 9.95945 Gbit/s | +35.7% |
| Goblin/native uring, final code | **49.981 s** | **10.6750 Gbit/s** | **+45.4%** |

The final native run completed population 31.2% sooner than Vinyl and 27.7% sooner than the
preceding libcurl-path run. Across the three native replicates, population throughput ranged from
9.959 to 10.675 Gbit/s. The libcurl comparison is a consecutive benchmark from the preceding code
revision rather than a same-binary selector A/B, but it used the same hardware, data, manifests,
CPU placement, origin, concurrency, and 1 MiB mirror-admission geometry. The three native
replicates establish that the population gain is not a single-run outlier.

During the final population window, Goblin averaged 555.5% process CPU, peaked at 614.0%, wrote
60.585 GiB according to `/proc`, and reached 0.748 GiB RSS. The process write counter understates
the 62.113 GiB payload slightly because the one-second monitor samples fall inside the exact
client wall-clock endpoints.

### Completely warmed reads

| Cache/client path | Elapsed time | Aggregate throughput | Relative to Vinyl |
|---|---:|---:|---:|
| Vinyl Cache | **168.828 s** | **13.5408 Gbit/s** | baseline |
| Goblin/native uring, replicate 1 | 175.843 s | 13.0006 Gbit/s | -4.0% |
| Goblin/native uring, replicate 2 | 180.085 s | 12.6943 Gbit/s | -6.3% |
| Goblin/native uring, final code | 183.130 s | 12.4833 Gbit/s | -7.8% |

The origin client is not involved in a warmed hit, so this phase is primarily a regression check
for the adjacent streaming changes. The final 12.4833 Gbit/s result is effectively equal to the
12.4919 Gbit/s pre-refactor Goblin result; the 0.07% difference is below run-to-run noise. Native
replicates ranged from 12.483 to 13.001 Gbit/s. The final measured window averaged 275.2% Goblin
CPU, peaked at 292.0%, physically read 208.332 GiB from Optane, and reached 0.754 GiB RSS.

The Optane composite sensor ranged from 34.85 to 37.85 degrees Celsius during final population and
36.85 to 38.85 degrees during the warmed pass. There is no thermal-throttling signal in these runs.
The Linux build passes all 237 tests, including persistent-connection reuse, valid chunked
responses, strict rejection of close-delimited responses, and linked io_uring timeout behavior.

Artifacts:

- Preceding libcurl path: `/mnt/local/goblin-small-bench/results/goblin-16k-baseline-20260718T132525Z-missopt-8core-uring-nginx-c8`
- Native replicate 1: `/mnt/local/goblin-small-bench/results/goblin-16k-baseline-20260718T140922Z-native-uring-8core-nginx-c8`
- Native replicate 2: `/mnt/local/goblin-small-bench/results/goblin-16k-baseline-20260718T142838Z-native-uring-final-8core-nginx-c8`
- Final native code: `/mnt/local/goblin-small-bench/results/goblin-16k-baseline-20260718T144408Z-native-uring-final2-8core-nginx-c8`
