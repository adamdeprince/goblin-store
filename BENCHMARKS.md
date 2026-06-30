# goblin-store benchmarks

Goblin Store's mission is **"beat memcached on large objects"** — match it at less RAM, beat extstore on
the cold tail, cost less per stored GB. These are the measurements behind that claim.

## TL;DR

| workload | vs | result |
|---|---|---|
| **memory-sized** (set fits RAM) | memcached | same latency, **7.4× less RAM** → ~6–7× cheaper |
| **disk-sized** (set ≫ RAM) | memcached + extstore | **+68% throughput, ~25% cheaper** on the same HDD; goblin-store holds 100% of the set, extstore **sheds 41%** |
| **small objects** (sub-2 KB) | memcached | **memcached wins** (~12× less RAM, ~2× faster) — small values are not goblin-store's game |

The win is large objects, and the mechanism is the tiering: a hot **RAM head** + **SSD prefix** answer
instantly while a **read-ahead pipeline** hides the ~5 ms HDD seek that extstore eats whole on every cold GET.

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

Working set 400 K `fb_value` objects (sub-2 KB, ~typical memcache). 0 misses.

| run | QPS | lat p99 | RAM | SSD / files |
|---|---|---|---|---|
| memcached | **152 K** | 1.9 ms | **152 MiB** | — |
| goblin-store 2-tier | 72 K | 2.3 ms | 1785 MiB | ~0 / 1 file |

**memcached wins decisively** — ~12× less RAM (152 MiB vs 1.78 GiB; confirmed real — at a 1 GiB budget
goblin-store sheds 34%) and ~2× faster. With objects too small to tier, the data lives entirely in RAM,
where goblin-store's **buddy allocator** (power-of-2 rounding) + rich per-object index cost ~3× the raw
bytes vs memcached's lean **slab allocator**. This is positioning, not a bug: **goblin-store is a
large-object engine.** A finer small-object allocator is the lever if that ever needs to change.

---

## Notes & caveats

- Small absolute QPS reflects the **2-core server** — these are relative results; the architecture
  conclusions scale.
- goblin-store uses ~20% more *total* disk than extstore on large objects (1 MiB `--block` rounding) —
  tunable; the RAM + throughput wins dominate cost regardless.
- Numbers are point-in-time on modest hardware; reproduce with the drivers and your own object mix.
