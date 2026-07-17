# Preliminary: Vinyl Cache versus Goblin Store on a 2.69 TB warm cache

On July 17, 2026, we benchmarked Vinyl Cache and Goblin Store as HTTP reverse caches for
multi-gigabyte objects. In this preliminary, concurrency-one test, Goblin Store reduced median
time to first byte from **14.865 ms to 345.890 µs** and raised median request bandwidth from
**453.411 MB/s to 722.241 MB/s**. Across the entire measured transfer, aggregate payload bandwidth
rose from **3.638 Gbit/s to 5.760 Gbit/s**.

These are single-client-thread numbers: one persistent HTTP connection, one request outstanding,
and no overlap between requests. They show first-byte behavior and serial large-object streaming,
not the maximum throughput of either server. The servers retained their normal worker and I/O
threads; “single-thread” describes the load offered by the client. Concurrent I/O tests are
currently underway.

Goblin Store's relevant design is positional caching. It keeps the beginning of each object in a
bounded RAM pool and stores the remainder on cheaper media. When a client asks for an object, the
RAM head can be returned immediately while the disk tail is read ahead and placed behind it on the
same response stream. Its `--mirror` mode presents that design as an ordinary HTTP cache in front
of an origin.

## The 2.69 TB data set

The corpus consisted of 1,639 gzip-compressed stock-trade files. It was deliberately much larger
than RAM and contained no small objects:

| Property | Value |
|---|---:|
| Objects | 1,639 |
| Corpus bytes | 2,686,979,075,088 bytes |
| Corpus size | 2.687 TB / 2.444 TiB |
| Smallest object | 594,761,572 bytes |
| Median object | 1,446,536,448 bytes |
| Largest object | 4,237,599,802 bytes |
| Objects larger than 1 GiB | 1,495 |
| Objects larger than 2 GiB | 263 |

The measured case made three independently shuffled passes through the same manifest, using the
fixed seed `0x243f6a8885a308d3`. Each cache therefore served 4,917 requests and
8,060,937,225,264 payload bytes: **8.061 TB / 7.331 TiB**. The request sequence, object sizes, and
paths matched exactly between the two traces.

## Versions and transport

| Component | Tested version or identity |
|---|---|
| Vinyl Cache | 9.0.1, revision `423648c4cb6b225b3268ffc337354ea938f5efee` |
| Goblin Store | preliminary 0.0.2 benchmark build |
| Vinyl binary SHA-256 | `a00f61953a656ffcb283a0be47f2ad09e50405a04fe04cf710950d7556e99139` |
| Goblin Store binary SHA-256 | `933a0ff7477391f2f7951eac7fc98f9ed499a2c51e7f8ee15ca67d1d12393688` |
| Benchmark client SHA-256 | `df9712f6f6fe6f0beb5ac96b9a2501970fd3b269f03f0c827f1f2fad8610aa28` |

Both products served ordinary HTTP over TCP/IPoIB. Goblin Store's native RDMA protocol was not
used, so the client saw two normal HTTP caches on the same address, port, network, and storage
machine. The two valid result sets are run `20260717T021836Z` for Vinyl and
`20260717T091202Z` for Goblin Store.

## A pure warm-cache exercise

This comparison was designed to exercise the caches, not their origins. Each product received a
fresh, empty local cache directory. A local Python origin was then started, and a prewarm client
fetched every one of the 1,639 objects exactly once at concurrency 11. The runner required the
origin access log to contain exactly 1,639 requests.

After prewarm, a bodyless HEAD request checked a warmed key. The measured client then read the
entire corpus three times. The origin request count was checked again after the measured case and
was not allowed to increase. A miss would have invalidated the run. Thus every timed byte came
from the cache under test: Vinyl's local cache file, or Goblin Store's RAM heads and local RAID
tails. The shared source filesystem was involved in prewarm but not in measured data delivery.

The measured phase used one client thread and one persistent connection. The client took
nanosecond timestamps immediately before sending each request, upon receiving the first response
byte, upon receiving the first body byte, and after receiving the last declared body byte. Results
remained in client RAM until the case finished, avoiding benchmark-log I/O during the run.

## Vinyl Cache configuration

Vinyl Cache was bound to the server's storage-and-network NUMA node and given a fully allocated
2,800 GiB file on the local XFS cache filesystem:

```text
numactl --cpunodebind=0 vinyld -F -j none \
  -a 10.88.88.20:8080 \
  -f benchmark.vcl \
  -s file,/var/tmp/test/vinyl.bin,2800G,2M,sequential \
  -p default_grace=0 -p default_keep=0
```

The backing file was created with `fallocate` before Vinyl started. Its recorded length was
3,006,477,107,200 bytes, all physically allocated, with 21 XFS extents. `sequential` advice is
important for multi-gigabyte objects on rotating storage: random advice had previously reduced the
workload to a storm of 4 KiB major faults and was rejected as an invalid configuration.

The benchmark VCL pointed at the local origin, assigned cacheable responses a seven-day TTL,
disabled grace and keep, and enabled streaming with `beresp.do_stream = true`.

## Goblin Store configuration

Goblin Store was constrained to the same server NUMA node both outside and inside the process:

```text
numactl --cpunodebind=0 --membind=0 goblin-store \
  --mirror http://127.0.0.1:18000 \
  --listen-address 10.88.88.20 --http-port 8080 \
  --no-memcache --net async \
  --numa 0 --memory 64G \
  --block 32M --ram-head 32M \
  --no-numa-promotion \
  --ssd-dir /var/tmp/test/goblin
```

There was no remote NUMA memory pool. The `--no-numa-promotion` setting kept the already local
heads stationary. Before startup, the runner compacted memory and reserved 32,768 2 MiB HugeTLB
pages on node 0, matching the 64 GiB resident pool. Startup and post-prewarm audits verified that
application threads, allocation policy, and the 24 live io_uring instances remained local to that
node.

The option is named `--ssd-dir` because it identifies Goblin Store's first disk tier. In this
experiment it pointed to the HDD RAID. No `--hdd-dir` was configured, so there was exactly one disk
level and no SSD.

### Why the heads were 32 MiB

Thirty-two MiB is much larger than Goblin Store's usual 256 KiB default. The server had no SSD
layer to bridge the latency between RAM and the sixteen-disk RAID, so the RAM head had to cover a
rotational seek and the beginning of the RAID read directly.

At an ideal 40 Gbit/s, transmitting 32 MiB takes about 6.71 ms. That gives the asynchronous disk
read several milliseconds to produce the tail before the client drains the head. One 32 MiB head
for each of 1,639 objects consumes 51.219 GiB, which fits inside the 64 GiB node-local pool and
leaves about 12.8 GiB for unused head slots and allocator capacity. The 32 MiB allocation block and
32 MiB head were deliberately matched for this all-large-object workload.

## Hardware and locality

### Server

The server was a four-socket Dell PowerEdge R820 running Ubuntu 26.04 LTS with Linux
`7.0.0-28-generic`:

| Component | Server configuration |
|---|---|
| CPU | 4 x Intel Xeon E5-4657L v2 at 2.40 GHz |
| Cores | 12 cores/socket, 48 physical cores, 96 logical CPUs |
| NUMA | Four nodes, one per socket |
| RAM | 512 GiB installed; 499.6 GiB reported by Linux |
| Cache filesystem | XFS on LVM |
| Storage controller | Dell PERC H710 at PCI `0000:02:00.0`, NUMA node 0 |
| Cache drives | 16 x 300 GB 15K RPM disks in RAID 6 |
| SSD tier | None |

The server HCA and PERC controller were both attached to NUMA node 0. Goblin Store's threads,
memory, and io_uring submission/completion work were kept there. The PERC and HCA MSI-X vectors
were also pinned to node-0 CPUs, preventing storage or network interrupts from being delivered on
another socket.

### Client

The client was another four-socket Dell PowerEdge R820 with the same 4 x Xeon E5-4657L v2 CPU
layout: 48 physical cores and 96 logical CPUs across four NUMA nodes. Its benchmark-time hardware
monitor reported 440.8 GiB as `MemTotal`; that number is total usable RAM, not free RAM. A current
DIMM inventory explains it: fourteen 32 GiB modules, or 448 GiB installed, rather than 512 GiB.
There were no HugeTLB pages reserved during the benchmark. The benchmark process and its memory
were pinned to NUMA node 1, where the client HCA was installed.

### InfiniBand and IPoIB

Both endpoints used **Mellanox Technologies MT27600 Connect-IB** adapters, reported by the verbs
stack as CA type **MT4113**, with firmware `10.16.1200`. The ports ran at 40 Gbit/s FDR10 over an
InfiniBand SwitchX fabric.

| Role | Linux interface | Address | PCI / NUMA | IPoIB mode | MTU | Link |
|---|---|---|---|---|---:|---:|
| Server | `ibp3s0` | `10.88.88.20/24` | `0000:03:00.0` / node 0 | connected | 65,520 | 40 Gbit/s FDR10 |
| Client | `ibp65s0` | `10.88.88.2/24` | `0000:41:00.0` / node 1 | connected | 65,520 | 40 Gbit/s FDR10 |

Connected IPoIB mode permits the 65,520-byte MTU used here. Routes were checked before each run,
and a 65,520-byte no-fragment ICMP packet crossed the fabric. The client also ran the active OpenSM
subnet manager. Again, this was TCP/IP over IPoIB—not Goblin Store's verbs/RDMA transport.

## Preliminary results

Each result below contains 4,917 successful HTTP 200 responses, zero body-length mismatches, and
zero recorded client errors. Response TTFB and body TTFB were identical at every reported
percentile because the first socket receive contained both the HTTP headers and initial payload.

### Aggregate serial transfer

| Metric | Vinyl Cache | Goblin Store | Goblin improvement |
|---|---:|---:|---:|
| Requests | 4,917 | 4,917 | -- |
| Payload | 8.061 TB | 8.061 TB | -- |
| Elapsed time | 17,724.138 s | 11,195.883 s | 36.83% shorter |
| Aggregate payload bandwidth | 3.638 Gbit/s | 5.760 Gbit/s | 58.31% higher |

### Time to first byte

TTFB is `response_first_byte_ns - query_start_ns`. Lower is better.

| Percentile | Vinyl Cache | Goblin Store | Goblin improvement |
|---:|---:|---:|---:|
| P50 | 14.865 ms | 345.890 µs | 97.67% lower |
| P75 | 19.646 ms | 399.030 µs | 97.97% lower |
| P90 | 25.714 ms | 474.817 µs | 98.15% lower |
| P95 | 29.678 ms | 1.295 ms | 95.64% lower |
| P99 | 39.138 ms | 2.928 ms | 92.52% lower |
| P99.9 | 106.793 ms | 3.224 ms | 96.98% lower |

Goblin Store's TTFB was lower at every reported percentile, including the slow tail.

### Request-to-last-byte bandwidth

For every request, bandwidth is:

```text
received payload bytes / (complete_ns - query_start_ns)
```

MB/s is decimal: one MB is 1,000,000 bytes. Throughput percentiles use a degradation ordering so
they point in the same direction as latency percentiles. P99.9 is therefore the lower 0.1% speed
tail—the fifth-slowest request in this 4,917-request sample—not the fastest 0.1%.

| Percentile | Vinyl Cache | Goblin Store | Goblin improvement |
|---:|---:|---:|---:|
| P50 | 453.411 MB/s | 722.241 MB/s | 59.29% higher |
| P75 | 437.534 MB/s | 696.183 MB/s | 59.12% higher |
| P90 | 422.119 MB/s | 668.677 MB/s | 58.41% higher |
| P95 | 412.963 MB/s | 653.005 MB/s | 58.13% higher |
| P99 | 393.523 MB/s | 619.887 MB/s | 57.52% higher |
| P99.9 | 306.397 MB/s | 567.673 MB/s | 85.27% higher |

Goblin Store delivered greater request bandwidth at every reported percentile, including the slow
tail.

## What these preliminary numbers establish

For this single-stream, already-warm, HDD-only large-object workload, Goblin Store reached the
client sooner and sustained substantially more payload bandwidth than Vinyl Cache. The first-byte
result is consistent with serving a resident object head immediately while starting the disk-tail
read behind it. The full-transfer result shows that the gain was not purchased by slowing the rest
of the object.

The scope remains deliberately narrow. This is not yet a maximum-throughput comparison, an origin
fill comparison, an SSD-tier comparison, or a small-object comparison. Concurrent I/O tests are
currently underway to measure scaling with multiple outstanding requests and to expose contention
that a single client stream cannot create.

## Reproduction and analysis

- [`bench/run_mirror_proxy_benchmark.sh`](https://github.com/adamdeprince/goblin-store/blob/master/bench/run_mirror_proxy_benchmark.sh)
  configures the cache phases, topology gates, prewarm, and origin-miss checks.
- [`bench/mirror_proxy_benchmark.cpp`](https://github.com/adamdeprince/goblin-store/blob/master/bench/mirror_proxy_benchmark.cpp)
  generates the fixed-seed request sequence and records nanosecond timestamps.
- [`bench/analyze_mirror_proxy.py`](https://github.com/adamdeprince/goblin-store/blob/master/bench/analyze_mirror_proxy.py)
  validates the matched traces and calculates the percentile tables.
- [`bench/monitor_benchmark_resources.py`](https://github.com/adamdeprince/goblin-store/blob/master/bench/monitor_benchmark_resources.py)
  records CPU, process memory, pressure, VM, and per-NUMA-node state.
- [`bench/audit_process_numa.sh`](https://github.com/adamdeprince/goblin-store/blob/master/bench/audit_process_numa.sh)
  verifies application, mapping, and io_uring locality.
- [`bench/pin_pci_irqs_to_numa.sh`](https://github.com/adamdeprince/goblin-store/blob/master/bench/pin_pci_irqs_to_numa.sh)
  discovers and pins current PCI interrupt vectors.
