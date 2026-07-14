# NUMA-local RAM heads and interconnect bandwidth

Goblin Store keeps the latency-sensitive beginning of every object in RAM and starts the slower
tiers at the same time. On a multi-socket server, however, "in RAM" is not precise enough: a worker
can repeatedly fetch a hot head across the socket interconnect even though free memory exists beside
the worker and its NIC. The NUMA head-cache work makes that placement explicit and then adapts it to
the observed workload.

On matching four-socket Dell R820s, the production-geometry test served 512 GiB in each of four
memcache/HTTP cases at the same 8.7 Gib/s network ceiling. A 3.906 GiB node-local working set used
about 9 MiB of estimated remote cache-line data per GiB of payload, while a 15.625 GiB set spread
across all four nodes with promotion disabled used about 771 MiB/GiB. In this deliberately
different-working-set comparison, the local layout carried 98.8% less measured remote-memory burden
without changing wire throughput.

## Allocation geometry is not object geometry

The default resident object head is 256 KiB. It is packed into a larger allocation and promotion
block, which defaults to 2 MiB on x86. Eight default heads therefore share one block; Goblin Store
does **not** reserve 2 MiB for every object.

The logical block and the physical HugeTLB page are separate settings with related constraints:

- `--ram-head` is a power-of-two resident head size and must divide `--block` exactly.
- `--block` is a power-of-two allocator and promotion unit. It must be a multiple of the platform's
  HugeTLB page size.
- The physical HugeTLB page size is 2 MiB on x86 and 32 MiB on Arm/LoongArch. A larger logical block
  spans several huge pages.
- Each fixed pool first attempts an explicit HugeTLB mapping on its intended NUMA node. If that
  node's pool has the wrong page order or too few pages, allocation falls back to ordinary memory
  without changing capacity or allocator geometry.

Huge pages reduce translation pressure and make the backing geometry predictable, while the smaller
head size avoids wasting SSD-latency-sized RAM reservations. Fractional small objects use the compact
arena and are not currently NUMA-promoted.

With `--numa NODE`, `--memory` belongs to the serving node and `--sub-memory` is allocated on each
other online node. Workers, maintenance threads, dynamic I/O allocations, and the key index remain on
the serving node. Allocation uses local blocks first, then consumes foreign capacity. Successful GETs
increase per-key scores; once per minute the scores decay. A maintenance thread exchanges a hot full
foreign block with a cold full local block, while the rescore traversal has priority over promotion.

For controlled testing, `--perverse` keeps the serving threads, NIC path, key index, and ordinary
allocations on the selected node but maps the preferred region-zero head arena on the node with the
greatest NUMA distance. It deliberately inverts allocation and promotion preference. Normal versus
perverse therefore changes remote DRAM placement without also changing the CPU or PCI path.

This is primarily an interconnect-load feature. Moving a block once is worthwhile when it prevents
the same hot bytes from crossing sockets on every subsequent request.

## Dell R820 test

The test used two matching Dell PowerEdge R820 servers, `dopey` and `rain`. Each system has four
sockets/four NUMA nodes, 48 physical cores (96 logical CPUs), and Intel Xeon E5-4657L v2 processors.
They communicated through a quiet commodity 10 GbE switch using ordinary 10 GbE adapters; no direct
cable was required. On `dopey`, the active NIC belongs to node 0 and its NUMA distance row is
`10 20 30 20`, making node 2 the farthest node.

`rain` was the client and its process was confined to the NUMA node local to its NIC. `dopey` was the
server. The controlled runs used one worker on logical CPU 92 on NIC-local node 0.

The dataset was 1,024 objects of exactly 256 KiB: 256 MiB total, all resident heads, with no SSD
payload. The measured hot set was 128 objects, or 32 MiB. That fits in one 64 MiB local region with
32 MiB of slack. The pool lived under `/var/tmp`; after every population its only file was the
23-byte safety marker.

Two placements separated the promotion mechanism:

1. **NUMA placement, no promotion:** the worker and NIC were on N0, the hot set was initially on N2,
   and `--no-numa-promotion` kept it there.
2. **NUMA placement and promotion:** the same initial layout, then the hot set was read until all 16
   full 2 MiB blocks had moved to N0.

For the NUMA layouts, `numa_maps` confirmed four independent 64 MiB mappings, each backed by exactly
32 x 2 MiB HugeTLB pages on its requested node. Promotion exchanged 16 block pairs: 32 MiB of hot
data moved local, 32 MiB of cold data moved out, and the server reported 64 MiB of logical copy
traffic. The exchanges took 10.90 ms in total and at most 0.750 ms for one pair; the promotion count
then remained stable.

## First-byte latency: honest inconclusive result

The benchmark client was corrected for this run to report two distinct measurements: arrival of the
first protocol response byte and arrival of the first object payload byte. The latter is the relevant
head-cache metric. Each row below is the median result from four one-connection, eight-second runs;
the throughput column is also a median across those runs.

| placement | response p50 | payload p50 | payload p99 | completion p50 | throughput |
|---|---:|---:|---:|---:|---:|
| N0 worker reading N2, promotion disabled | 94.7 us | 166.1 us | 207.0 us | 371.6 us | 671.1 MiB/s |
| N0 worker, hot blocks promoted to N0 | 94.2 us | 163.0 us | 207.7 us | 367.0 us | 682.1 MiB/s |

There was too much run-to-run jitter to claim a latency difference. Payload p50 ranged from
149-179 us with far memory and from 142-187 us after promotion. The roughly 3 us median movement had
no corresponding p99 improvement and is smaller than the uncontrolled variation. These data neither
prove nor disprove a one-microsecond locality effect; Ethernet/TCP,
kernel scheduling, frequency state, and host noise obscure a signal of that size.

An earlier far-pinned `--no-numa` row was discarded from the comparison. It moved CPU and RAM
together, leaving DRAM local to the CPU and moving the PCI/NIC path across the fabric—the wrong
topology for a remote-DRAM latency test. That mistake motivated the longer
[direct-link normal/perverse experiment](numa-first-byte-latency.md), which keeps the CPU and NIC
fixed while changing only head-memory placement and covers both memcache and HTTP.

## Interconnect load: clear result

At 24 connections, every placement reached the same network ceiling:

| placement | throughput | object payload in 10 s |
|---|---:|---:|
| N0 worker reading N2, promotion disabled | 1,119.6 MiB/s (8.75 Gib/s) | 10.94 GiB |
| hot blocks promoted to N0 | 1,119.9 MiB/s (8.75 Gib/s) | 10.94 GiB |

Throughput cannot rise once 10 GbE is full, but Intel PCM's per-core NUMA events show what happened
underneath. The following figures average nine complete one-second samples on the pinned server core
during matched 8.75 Gib/s runs:

| hot-head placement | local DRAM events/s | remote DRAM events/s | remote share |
|---|---:|---:|---:|
| far N2, promotion disabled | 10.06 million | 11.69 million | 53.73% |
| promoted to local N0 | 28.88 million | 0.216 million | 0.742% |

Promotion reduced measured remote DRAM events by **98.15%** without changing wire throughput. If the
off-core response events are expressed as 64-byte cache-line fills, that is approximately 713 MiB/s
before promotion versus 13 MiB/s afterward. This conversion is an estimate from the event semantics,
not a direct QPI byte counter: PCM reported the R820's QPI link-layer monitoring devices as missing.
The raw event counts are the primary result.

## Full-arena interconnect accounting

On July 14, 2026, a second interconnect experiment used the direct 10 GbE cable and the
production-sized allocator geometry: `--memory 4G --sub-memory 4G --block 2M --ram-head 256K` on
four NUMA nodes. All four 4 GiB head arenas consumed the intended per-node 2 MiB HugeTLB pools. Four
server workers and eight client connections were enough to sustain the link ceiling in smoke tests.

This experiment asks how much remote-memory traffic the optimizer keeps off the fabric per GiB sent
to the client. It compares two deliberately different working-set shapes:

1. **normal:** 16,000 x 256 KiB objects (3.90625 GiB), which fit in the preferred node-0 arena;
   promotion remained enabled;
2. **distributed/adverse:** 64,000 x 256 KiB objects (15.625 GiB), which filled the local arena and
   then spread across the other nodes; `--no-numa-promotion` froze that initial placement.

The adverse row is called `perverse` in the artifact names for continuity, but the server did **not**
use `--perverse`: workers and the preferred arena remained on node 0. Using the real flag would have
tested one far arena, while this test is meant to represent a uniformly read multi-node cache whose
optimizer has been disabled. Keeping roughly 4 GiB of active heads per populated node also keeps
per-CPU TLB pressure comparable to the normal 4 GiB case.

For each server shape, memcache and HTTP each read exactly 2,097,152 fixed-seed random objects:
512 GiB of object payload per case and 2 TiB over the four measured cases. Driver counters on
`dopey:eno2` and `rain:eno2` were allowed to quiesce before and after every case, avoiding the delayed
counter publication found during calibration. The denominator is actual server NIC bytes rather
than the nominal payload. Intel PCM sampled local and remote DRAM events on every server CPU only
during the measured read window. `remote events x 64 bytes` is reported explicitly as an estimated
cache-line data volume, not as a direct QPI link-layer byte count.

| head layout | protocol | 512 GiB time | payload rate | server TX | remote event share | estimated remote data | remote MiB / GiB payload | remote MiB / GiB server TX |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| local 3.906 GiB set | memcache | 468.91 s | 8.735 Gib/s | 537.107 GiB | 0.465% | 4.518 GiB | 9.037 | 8.614 |
| local 3.906 GiB set | HTTP | 469.91 s | 8.717 Gib/s | 537.310 GiB | 0.485% | 4.674 GiB | 9.348 | 8.908 |
| distributed 15.625 GiB set, promotion off | memcache | 468.76 s | 8.738 Gib/s | 537.115 GiB | 50.638% | 385.282 GiB | 770.565 | 734.534 |
| distributed 15.625 GiB set, promotion off | HTTP | 468.25 s | 8.748 Gib/s | 537.310 GiB | 48.811% | 385.354 GiB | 770.707 | 734.403 |

Every dopey TX delta exactly matched rain's RX delta. Payload throughput stayed in a narrow
8.717-8.748 Gib/s band, as expected for a network-capped test. The local layout reduced estimated
remote cache-line data per GiB by **98.83% for memcache** and **98.79% for HTTP**. In absolute terms,
the distributed server fetched about 385.3 GiB remotely while delivering 512 GiB of objects; the
local server's all-CPU PCM background plus serving work totaled only 4.5-4.7 GiB remotely.

This is intentionally not a same-dataset A/B measurement of the promotion thread: the working-set
shapes differ, so it should not be presented as a 98.8% causal estimate for promotion alone. It does
quantify the interconnect burden the NUMA policy is designed to avoid while holding per-node working
set, allocator geometry, HugeTLB backing, CPU/NIC placement, protocol, random-read volume, and wire
rate explicit. The full artifacts are under
`/home/adam/goblin-numa-results/20260714T191926Z-numa-bandwidth-512g`; the reusable runner is
[`bench/numa_bandwidth.sh`](https://github.com/adamdeprince/goblin-store/blob/master/bench/numa_bandwidth.sh).

## Development path: FPGA NICs and userspace networking

The commodity switch was sufficient to establish the interconnect result. The subsequent
[latency experiment](numa-first-byte-latency.md) used a direct 10 GbE cable to remove the switch as
one variable and normal/perverse placement to isolate DRAM locality. Host-side jitter still matters,
so its full distributions and limitations are reported rather than reduced to a single best number.

FPGA NICs and direct userspace networking are on Goblin Store's development path. Bypassing the
kernel TCP path, pinning NIC queues and workers to the same NUMA node, and using NIC or FPGA
timestamps from request arrival to first payload DMA should make a microsecond-scale locality effect
measurable. Higher line rates will also stop 10 GbE from hiding throughput differences. That
environment is where Goblin Store's application-owned head placement, HugeTLB-backed pools, and
object-aware read-ahead should be evaluated together.

See [ADR-0008](adr/0008-ram-allocator.md), [ADR-0016](adr/0016-bounded-locked-memory.md), and
[ADR-0019](adr/0019-access-score-numa-promotion.md) for the allocator, bounded-memory, and promotion
decisions.
