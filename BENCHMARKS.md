# goblin-store benchmarks

Goblin Store's mission is **"beat memcached on large objects"** — match it at less RAM, beat extstore on
the cold tail, cost less per stored GB. These are the measurements behind that claim.

## TL;DR

| workload | vs | result |
|---|---|---|
| **memory-sized** (set fits RAM) | memcached | same latency, **7.4× less RAM** → ~6–7× cheaper |
| **disk-sized** (set ≫ RAM) | memcached + extstore | **+68% throughput, ~25% cheaper** on the same HDD; goblin-store holds 100% of the set, extstore **sheds 41%** |
| **small objects** (sub-2 KB) | memcached | **memcached wins** (~12× less RAM, ~2× faster) — small values are not goblin-store's game |
| **small files on Optane** (mean 175 KiB) | Vinyl Cache | **+7.9% warm bandwidth, 64.7% less resident object data** with 256 KiB heads; 64 KiB heads cut mean TTFB 14.8% |
| **native RDMA, 256 KiB values** | nominal 40 Gbit/s InfiniBand | **38.232 Gbit/s payload** (95.58% of link rate); 7.659 us median response-header TTFB |

The win is large objects, and the mechanism is the tiering: a hot **RAM head** + **SSD prefix** answer
instantly while a **read-ahead pipeline** hides the ~5 ms HDD seek that extstore eats whole on every cold GET.

The current Optane small-file comparison—including the fixed 373,165-object corpus, NUMA placement,
cache population rate, head-size sweep, resident-memory calculation, CPU load, and warm-cache
latency—is in **[Small-object HTTP caching on Optane](docs/optane-small-object-benchmark.md)**.

The opt-in **[Goblin Store versus BlobDB harness](bench/GOBLIN_BLOBDB_BENCHMARK.md)** performs a
complete populate-and-read lifecycle for both engines under matched application-memory budgets. It
replays one fixed key schedule serially and records time to first byte and complete retrieval in
nanoseconds for three datasets: Goblin's embedded C++ O_DIRECT storage path, integrated BlobDB
buffered reads, and integrated BlobDB direct reads. Neither engine crosses a network or protocol
layer in this harness. Goblin materializes every value into a per-reader aligned buffer that starts
empty, grows only when the encountered object does not fit, and is reused between requests; disk
tails land directly in that final buffer.

## The rig

Two AWS instances, same VPC / same AZ (us-east-2c), driven over a real NIC on private IPs:

- **Server** — 2 vCPU, **3.7 GiB RAM**. Two disks: a fast root volume (**SSD**, ~0.58 ms random read,
  1713 IOPS) and `/mnt/slow` (**HDD/st1**, ~5.04 ms random read, 198 IOPS — **8.6× slower**). The ~5 ms
  HDD seek is the thing the RAM head exists to hide.
- **Load box** — c7g.4xlarge, **16 vCPU** (sized so the *server*, not the client, is the bottleneck).

**Load generator:** [mutilate](https://github.com/leverich/mutilate), patched to handle multi-MB and
variable value sizes (stock mutilate's 2 MiB value buffer segfaults on a distribution; enlarged to 34 MiB).

**Object sizes:** a realistic, bounded, right-skewed **mixed distribution** spanning **256 KiB – 8 MiB**
(generalized Pareto, mean ≈ 1 MiB) — i.e. "binary artifacts," not uniform blobs. Small-object runs use
mutilate's `fb_value` (the Facebook/Atikoglu memcache size distribution).

**Cost model (illustrative; adjust to your prices):** RAM **$4**, gp3-SSD **$0.08**, st1-HDD **$0.045**
per GiB-month. RAM dominates, so the RAM ratio drives the cost story at any realistic price ratio.

---

## 1. Memory-sized — same speed, far less RAM

Working set ~2 GiB (fits RAM). At a fixed 16 connections everyone is server-throughput-bound, so latency
is ~equal across the board — this is a **RAM-cost** contest, and goblin-store wins it. (0 misses everywhere.)

| run | QPS | lat p99 | **RAM** | SSD | HDD | ~$/mo |
|---|---|---|---|---|---|---|
| memcached standard | 640 | 78 ms | **1974 MiB** | — | — | **$7.7** |
| goblin-store 2-tier (RAM + SSD) | 615 | 79 ms | **267 MiB** | 1951 MiB | — | **$1.2** |
| goblin-store 3-tier (RAM + SSD + HDD) | 618 | 84 ms | **267 MiB** | 486 MiB | 1627 MiB | **$1.1** |

**Same latency, 7.4× less RAM** (267 MiB vs 1.97 GiB): memcached holds the whole set in RAM; goblin-store
holds only the hot heads and pushes the bulk to cheap disk. ~6–7× cheaper for identical performance.

## 2. Disk-sized — faster *and* cheaper than extstore on the same spindle

Working set ~12 GiB (≫ RAM → cold reads hit disk). extstore given a 48 GiB backing file so it holds the
set (≤0.4% miss). Latency = 16 ÷ QPS at fixed concurrency, so **throughput is the metric**.

| run | QPS | lat avg | RAM | SSD | HDD (bulk) | ~$/mo |
|---|---|---|---|---|---|---|
| memcached + extstore | 70 | 226 ms | 2188 MiB | — | 9.5 GiB | **$9.0** |
| goblin-store 2-tier (bulk on SSD) | 93 | 170 ms | 1520 MiB | 10.7 GiB | — | $6.9 |
| **goblin-store 3-tier (bulk on HDD)** | **118** | **135 ms** | **1520 MiB** | 2.84 GiB | 8.9 GiB | **$6.6** |

**goblin-store 3-tier sustains +68% throughput (118 vs 70 QPS) on the *same* HDD bulk, using less RAM, at
~25% lower cost.** extstore does synchronous ~5 ms seeks (≈110 MB/s, seek-bound); goblin-store answers
head/SSD-prefix instantly and **read-ahead pipelines the HDD tail** (≈210 MB/s, near spindle throughput).

**Durability bonus:** at a full-speed 20 GiB load, extstore **shed 41.5% of the data** — it ingests at
RAM speed but flushes to HDD at ~200 MB/s, so under pressure memcached *evicts* rather than backpressuring.
goblin-store queues-on-admission and **dropped nothing**.

## 3. Small objects — memcached's turf, and goblin-store says so

> Historical result: this run predates the compact fractional-object arena. Current Goblin Store no
> longer rounds every sub-head object to a 4 KiB buddy leaf; rerun before citing these exact ratios.

Working set 400 K `fb_value` objects (sub-2 KB, ~typical memcache). 0 misses.

| run | QPS | lat p99 | RAM | SSD / files |
|---|---|---|---|---|
| memcached | **152 K** | 1.9 ms | **152 MiB** | — |
| goblin-store 2-tier | 72 K | 2.3 ms | 1785 MiB | ~0 / 1 file |

**memcached wins decisively** — ~12× less RAM (152 MiB vs 1.78 GiB; confirmed real — at a 1 GiB budget
goblin-store sheds 34%) and ~2× faster. With objects too small to tier, the data lives entirely in RAM,
where the then-current **buddy allocator** (power-of-2 rounding) + rich per-object index cost ~3× the
raw bytes vs memcached's lean **slab allocator**. Fractional RAM-only objects now use a compactable
byte-granular arena, but small values remain outside Goblin Store's primary large-object workload.

## 4. NUMA head placement — isolate promotion from disk

Use objects exactly equal to `--ram-head`. They are RAM-only, make no SSD object files, and use fixed
buddy slots: with the defaults, eight 256 KiB objects fill each 2 MiB allocation/promotion block.
This measures the NUMA head layer without benchmarking a NAS or local disk.

On a four-node host, the following layout stores 256 keys per node in sequential population order:

```sh
goblin-store --numa 0 --memory 64M --sub-memory 64M \
  --block 2M --ram-head 256K --io-buffers 2 --no-mlock --cores 1 --no-http \
  --memcache-port 12131 --ssd-dir /var/tmp/goblin-numa/ssd
```

Populate 1,024 keys once from the load host, then drive a 32 MiB window in the third region. The
128-key hot set fits in the 64 MiB local arena with 32 MiB of slack:

```sh
goblin-bench --host 192.168.1.42 --port 12131 --size 256K --keys 1024 --populate-only
goblin-bench --host 192.168.1.42 --port 12131 --size 256K --keys 1024 \
  --hot-start 512 --hot-keys 128 --no-store --conns 24 --duration 15
```

For the controlled A/B, restart and repopulate for each variant. Compare the command above with the
same command plus `--no-numa-promotion`; both variants retain identical CPU affinity and physical
local/foreign placement, so the only changed mechanism is hot-block promotion. Warm the promoted run
until `stats numa_promotions` stabilizes before recording it, alternate variant order, and take at
least four repetitions. Report throughput, TTFB/completion tails, promotion count/bytes/time, and
verify that the SSD pool contains only its safety marker.

Use `/var/tmp` for every benchmark pool that may receive disk extents. `/tmp` is acceptable only for
strictly RAM-only checks that verify no payload file was created; it must not be used to characterize
the storage path.

Do not use a far-pinned `--no-numa` process as the remote-DRAM latency control. Moving both its CPU
and RAM to the far node keeps DRAM local to that CPU and instead forces the PCI/NIC path across the
fabric. Use matched `--numa NODE` runs with and without `--perverse`: serving CPUs and NIC locality
stay fixed while only region-zero head-memory placement changes.

### 2026-07-14 Dell R820 result: network-capped, interconnect load down 98%

The detailed narrative is **[NUMA-local RAM heads and interconnect bandwidth](docs/numa-interconnect-bandwidth.md)**.
Server `dopey` and client `rain` are matching four-socket/four-NUMA-node Dell R820s, each with 48
physical cores (96 logical CPUs), connected through a quiet commodity 10 GbE switch. Both client and
server work were confined to their NIC-local node. On `dopey`, the NIC is on N0 and its distance row
is `10 20 30 20`.

The run used 1,024 x 256 KiB objects (256 MiB resident at once) and a 128-object/32 MiB hot set.
Kernel `numa_maps` confirmed four 64 MiB mappings with exactly 32 x 2 MiB HugeTLB pages on each of
N0/N1/N2/N3. The `/var/tmp` SSD pool contained only its 23-byte safety marker.

`goblin-bench` reports first protocol byte and first object payload byte separately. Four
one-connection, eight-second runs produced the following medians:

| placement | response p50 | payload p50 | payload p99 | completion p50 |
|---|---:|---:|---:|---:|
| N0 worker reading N2, promotion disabled | 94.7 us | 166.1 us | 207.0 us | 371.6 us |
| N0 worker, hot blocks promoted to N0 | 94.2 us | 163.0 us | 207.7 us | 367.0 us |

There was too much jitter to claim a latency difference: per-run payload p50 ranged from 149-179 us
with far memory and 142-187 us after promotion. The 3 us movement in the medians had no p99
improvement. A signal around one microsecond is below the noise from Ethernet/TCP, kernel scheduling,
frequency state, and a non-isolated host.

An earlier `--no-numa` row is intentionally omitted: pinning both CPU and RAM to far N2 measured a
cross-fabric NIC/PCI path, not remote DRAM. The completed normal-versus-`--perverse` rerun kept the
NIC and CPU fixed, used a direct 10 GbE link, and collected more than 68 million serial memcache/HTTP
samples; see **[Looking for NUMA latency below the network noise floor](docs/numa-first-byte-latency.md)**.

Across all 17,166,137 requests per placement and protocol, local RAM reduced p50 payload TTFB by
10.63% for memcache and 20.12% for HTTP; p50 complete-download time improved by 5.20% and 10.13%.
Vargha–Delaney probability of superiority favored local payload TTFB with `A=0.927` for memcache and
`A=0.848` for HTTP, and favored local completion with `A=0.924` and `A=0.843`. All four 96-block
bootstrap intervals excluded 0.5. Some response-TTFB tail percentiles moved in the opposite
direction, and the multi-hour placements ran sequentially, so the result establishes a clear
difference between the recorded traces without pretending to isolate an exact cross-socket latency.

Both controlled 24-connection variants filled 10 GbE at about 1,120 MiB/s (8.75 Gib/s), transferring
10.94 GiB of object payload per ten-second run. Throughput therefore could not show the benefit.
Matched Intel PCM captures on the pinned N0 server core did:

| hot-head placement | local DRAM events/s | remote DRAM events/s | remote share |
|---|---:|---:|---:|
| far N2, promotion disabled | 10.06 million | 11.69 million | 53.73% |
| promoted to local N0 | 28.88 million | 0.216 million | 0.742% |

Promotion cut measured remote DRAM events by **98.15%** at unchanged wire throughput. Interpreting
the off-core events as 64-byte cache-line fills gives approximately 713 MiB/s before versus 13 MiB/s
after, but PCM could not expose direct QPI link-byte counters on these hosts; the raw event counts are
the primary measurement.

The production-geometry follow-up used four HugeTLB-backed 4 GiB arenas, the direct cable, and
512 GiB of random 256 KiB reads per case. A 3.906 GiB local set was compared with a 15.625 GiB set
spread across all nodes and frozen with `--no-numa-promotion` (the artifact's `perverse` label did
not pass `--perverse` to the server):

| protocol | local payload rate | local remote MiB/GiB payload | distributed remote MiB/GiB payload | local reduction |
|---|---:|---:|---:|---:|
| memcache | 8.735 Gib/s | 9.037 | 770.565 | 98.83% |
| HTTP | 8.717 Gib/s | 9.348 | 770.707 | 98.79% |

The distributed rows also ran at 8.738 and 8.748 Gib/s: 10 GbE hid the memory-placement difference
in throughput while PCM exposed it in interconnect load. These intentionally different working-set
shapes quantify the burden local placement avoids, not an isolated causal effect size for the
promotion thread. Full method and counters are in the
**[NUMA interconnect-bandwidth report](docs/numa-interconnect-bandwidth.md)**.

Sixteen 2 MiB hot blocks moved local, exchanging 32 MiB hot for 32 MiB cold and reporting 64 MiB of
logical copy traffic, 10.90 ms total swap time, and a 0.750 ms maximum. FPGA NICs and direct userspace
networking are on the development path. NIC/FPGA timestamps and kernel-bypass queues should make the
first-payload-byte latency effect measurable without the host-side jitter that obscured it here.

## 5. Native RDMA over InfiniBand — two QPs nearly fill a 40 Gbit/s link

The full method, latency distributions, QP sweep, and timing semantics are in
**[Native RDMA over InfiniBand: 256 KiB latency and throughput](docs/native-rdma-256k-performance.md)**.
On July 15, 2026, `rain` read a fixed-seed uniform 4 GiB working set from `dopey` over their direct
40 Gbit/s Connect-IB link. Both HCAs, the server's resident heads, and the client/server CPU sets were
local to NUMA node 1. Objects were exactly 256 KiB—one resident head and one registered bulk
window—so the test measured RAM plus native verbs rather than disk streaming.

Five 200,000-request serial repetitions produced one million measured samples. Median
application-visible response-header TTFB was 7.659 us; the complete 256 KiB bulk window was visible
at 98.837 us; and the memcache response completed at 99.740 us. `BULK_READY` follows the complete
RDMA write, so the payload timestamp is full-window readiness rather than literal first-body-byte
arrival.

One closed-loop QP delivered 20.911 Gbit/s of object payload. Two reached 37.957 Gbit/s, and the
12-QP sweep maximum was 38.232 Gbit/s—95.58% of the nominal link rate, excluding framing and fabric
overhead. Three additional 30-second two-QP runs averaged 37.858 Gbit/s with a 0.193 Gbit/s sample
standard deviation. The benchmark is reproducible with `bench/run_rdma_256k_ib.sh` and
`bench/analyze_rdma_256k.py`.

---

## Notes & caveats

- Small absolute QPS reflects the **2-core server** — these are relative results; the architecture
  conclusions scale.
- Disk footprint is governed by object tier extents, stripe geometry, and device alignment. `--block`
  is only the RAM allocation/promotion unit and does not round disk object extents.
- Numbers are point-in-time on modest hardware; reproduce with the drivers and your own object mix.
