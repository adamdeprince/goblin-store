# Looking for NUMA latency below the network noise floor

Goblin Store's NUMA policy is meant to keep repeated object-head reads off a multi-socket machine's
RAM interconnect. The bandwidth effect is easy to observe with hardware counters. The latency effect
is harder: one remote DRAM traversal is small beside Ethernet, TCP, kernel scheduling, and userspace
socket noise. This experiment collected more than 68 million serial requests to look for that signal
without pretending that a large sample count makes the surrounding system deterministic.

The comparison is deliberately **local head RAM versus remote head RAM**. The serving CPU and NIC
remain together on the same NUMA node in both cases. An earlier attempt pinned a non-NUMA server to a
far CPU, which moved the PCI path as well as the CPU and therefore answered the wrong question.

## Test rig

The two hosts were matching Dell PowerEdge R820s, `dopey` and `rain`:

- four sockets and four NUMA nodes per host;
- 48 physical Ivy Bridge cores / 96 logical CPUs per host (Xeon E5-4657L v2);
- 512 GiB RAM and 120 MiB aggregate L3 per host; and
- ordinary dual-port 10 GbE copper adapters.

One otherwise-unused 10 GbE port on each machine formed a direct `/30` link: `dopey` used
`eno2` at `10.77.77.1`, and `rain` used `eno2` at `10.77.77.2`. Management traffic and the default
route remained on `eno1`, so the test could not accidentally strand either host and the measured
traffic did not cross a switch. Both direct-link ports belong to NUMA node 0.

`dopey` served on logical CPU 4 on node 0. Every server thread was confined to that CPU after
startup. The single client process on `rain` was also pinned to logical CPU 4 on its NIC-local node
0. The request stream used one persistent connection and did not issue a second request until the
previous 256 KiB response had been consumed completely.

## Memory geometry and the controlled placement

Each server allocated four 4 GiB head arenas:

```text
--numa 0 --memory 4G --sub-memory 4G --block 2M --ram-head 256K
--cores 1 --no-numa-promotion
```

The 2 MiB blocks were backed by the reserved 2 MiB HugeTLB pools on their intended nodes, and each
block packed eight 256 KiB heads. The dataset contained 16,000 objects of exactly 256 KiB, or
4,194,304,000 bytes (3.90625 GiB). An object exactly the resident-head size is RAM-only, so there were
no SSD object extents and the `/var/tmp` backing pool contained only its safety marker. The working
set is more than 30 times the R820's aggregate L3 and roughly 128 times one socket's L3; this was not
a cache-resident 64 MiB test masquerading as a DRAM test.

Allocation always consumes the preferred arena first. Because the dataset fit in one 4 GiB arena,
the two server placements were:

1. **normal/local:** preferred arena on serving node 0;
2. **perverse/remote:** `--perverse` put the preferred arena on farthest node 2, whose Linux NUMA
   distance from node 0 is 30 rather than the local distance 10.

Promotion was disabled in both placements, so heads could not migrate during the run. `--perverse`
changed only the preferred head-arena mapping; workers, the key index, I/O buffers, ordinary dynamic
allocations, and the NIC stayed on node 0.

## Four long, serial traces

The placement was the outer loop to require only one server switch. One normal server ran memcache
and then HTTP; one perverse server then ran memcache and HTTP. Each protocol received a 200,000-read
warmup followed by exactly **17,166,137 measured GETs**. That count is the number of 256 KiB payloads
that a nominal 10,000,000,000-bit/s link would carry in one hour, although strict request/response
serialization made each case take longer.

| order | placement | protocol | measured wall time | object throughput |
|---:|---|---|---:|---:|
| 1 | normal/local | memcache | 98 min 25 s | 728.5 MiB/s (5.69 Gib/s) |
| 2 | normal/local | HTTP | 101 min 15 s | 708.1 MiB/s (5.53 Gib/s) |
| 3 | perverse/remote | memcache | 106 min 9 s | 675.3 MiB/s (5.28 Gib/s) |
| 4 | perverse/remote | HTTP | 109 min 15 s | 656.1 MiB/s (5.13 Gib/s) |

The client used seed `0x243f6a8885a308d3`. All four traces completed the same key-index sequence
with hash `0xf1691d253d2f550e`. Raw samples were accumulated in prefaulted client RAM during the
measurement, then written under `rain:/var/tmp`, compressed, and copied to the shared result
directory only after the server placement's measurements stopped. NFS was not in the timestamping
path.

The preserved run is `20260714T104600Z-outer-ns`. Each case has 17,166,137 rows and uses
`CLOCK_MONOTONIC_RAW`, whose reported resolution was one nanosecond. The four compressed raw files
total about 2.08 GiB.

## What “first byte” means

Each row records the timestamp immediately before the request send and three elapsed values:

- **response TTFB:** query start to the first byte returned by `recv()`;
- **payload TTFB:** query start to the first byte of the 256 KiB object rather than its protocol
  header; and
- **total download:** query start until the complete response has been consumed.

Memcache headers are read to their exact length before the payload read. HTTP parses the header
terminator and records whether the first receive already contained body bytes. These definitions
make the local/remote comparison valid within each protocol; absolute payload-TTFB values should not
be used as a protocol shootout because their parsing and receive boundaries differ.

## Percentile results

### memcache

| metric | percentile | local | remote | local improvement |
|---|---:|---:|---:|---:|
| response TTFB | p50 | 76.905 us | 79.720 us | +3.531% |
| response TTFB | p90 | 82.012 us | 97.307 us | +15.718% |
| response TTFB | p99 | 120.460 us | 110.081 us | -9.429% |
| response TTFB | p99.9 | 135.241 us | 131.901 us | -2.532% |
| response TTFB | p99.99 | 146.593 us | 182.470 us | +19.662% |
| payload TTFB | p50 | 135.446 us | 151.563 us | +10.634% |
| payload TTFB | p90 | 142.362 us | 181.987 us | +21.774% |
| payload TTFB | p99 | 196.628 us | 208.741 us | +5.803% |
| payload TTFB | p99.9 | 217.761 us | 295.515 us | +26.311% |
| payload TTFB | p99.99 | 308.094 us | 319.121 us | +3.455% |
| total download | p50 | 338.105 us | 356.646 us | +5.199% |
| total download | p90 | 346.588 us | 400.328 us | +13.424% |
| total download | p99 | 414.575 us | 456.998 us | +9.283% |
| total download | p99.9 | 467.726 us | 491.569 us | +4.850% |
| total download | p99.99 | 503.469 us | 515.245 us | +2.286% |

### HTTP

| metric | percentile | local | remote | local improvement |
|---|---:|---:|---:|---:|
| response TTFB | p50 | 79.623 us | 88.317 us | +9.844% |
| response TTFB | p90 | 102.487 us | 101.444 us | -1.028% |
| response TTFB | p99 | 140.014 us | 110.103 us | -27.166% |
| response TTFB | p99.9 | 150.374 us | 119.907 us | -25.409% |
| response TTFB | p99.99 | 162.134 us | 144.697 us | -12.051% |
| payload TTFB | p50 | 137.963 us | 172.717 us | +20.122% |
| payload TTFB | p90 | 181.901 us | 187.648 us | +3.063% |
| payload TTFB | p99 | 215.644 us | 214.707 us | -0.436% |
| payload TTFB | p99.9 | 299.905 us | 301.951 us | +0.678% |
| payload TTFB | p99.99 | 326.985 us | 320.861 us | -1.909% |
| total download | p50 | 340.480 us | 378.841 us | +10.126% |
| total download | p90 | 391.955 us | 407.112 us | +3.723% |
| total download | p99 | 439.032 us | 467.832 us | +6.156% |
| total download | p99.9 | 491.811 us | 498.385 us | +1.319% |
| total download | p99.99 | 519.259 us | 518.096 us | -0.224% |

Positive improvement means local RAM was faster and is calculated as
`(remote - local) / remote × 100%`. A negative value means the local trace had the slower percentile.
The central distribution and the extreme tail need not move together: the latter includes scheduler
stalls and other host events much larger than one cross-socket DRAM access.

## Probability of superiority

| protocol | metric | full-trace A: local faster | magnitude | blocked A (95% CI) | paired fixed-sequence probability (95% CI) |
|---|---|---:|---|---:|---:|
| memcache | response TTFB | 0.612381 | small | 0.612148 [0.581601, 0.643022] | 0.611416 [0.580769, 0.642369] |
| memcache | payload TTFB | 0.926942 | large | 0.926827 [0.916574, 0.936515] | 0.927414 [0.917085, 0.937308] |
| memcache | total download | 0.923878 | large | 0.924088 [0.913874, 0.933665] | 0.924632 [0.914377, 0.934042] |
| HTTP | response TTFB | 0.699049 | medium | 0.697331 [0.662254, 0.731291] | 0.696776 [0.661984, 0.730427] |
| HTTP | payload TTFB | 0.847870 | large | 0.848016 [0.813886, 0.879709] | 0.847632 [0.813242, 0.879161] |
| HTTP | total download | 0.843298 | large | 0.843523 [0.810824, 0.873806] | 0.843088 [0.810456, 0.873654] |

Vargha–Delaney `A` is oriented here as
`P(local latency < remote latency) + 0.5 × P(tie)`. Thus 0.5 means no distributional advantage and a
value above 0.5 favors local memory. The full-trace value is exact over all observations in each
placement.

Seventeen million serial requests are not seventeen million independent experiments. To avoid a
meaningless tiny standard error, the analysis also divides each matched fixed-seed trace into 96
contiguous blocks—approximately one minute apiece—computes `A` within every corresponding block, and
bootstraps the mean block effect 20,000 times. The table reports that block-bootstrap 95% interval
and a paired fixed-sequence probability with the same treatment.

An interval excluding 0.5 shows that the two recorded traces are distinguishable on this time scale.
It does not erase the biggest limitation: placements were long sequential runs, not randomly
interleaved trials. A thermal or host-state drift that persisted across an entire placement remains
a possible confounder, however many individual rows were collected.

The reusable analysis is in
[`bench/analyze_numa_latency.py`](https://github.com/adamdeprince/goblin-store/blob/master/bench/analyze_numa_latency.py).
It validates the row counts and fixed-seed key sequence, calculates exact nearest-rank p50, p90, p99,
p99.9, and p99.99 values, and emits CSV, JSON, and the Markdown tables used here.

```bash
python3 bench/analyze_numa_latency.py \
  /home/adam/goblin-numa-results/20260714T104600Z-outer-ns \
  --output-dir /var/tmp/goblin-numa-latency-analysis
```

## What the result can and cannot say

The recorded local traces were generally faster. At p50, local placement reduced response TTFB by
3.531% for memcache and 9.844% for HTTP, payload TTFB by 10.634% and 20.122%, and complete-download
time by 5.199% and 10.126%. The distribution-wide result points the same way: the chance that a
random local observation beat a random remote observation was 0.927 for memcache payload TTFB and
0.848 for HTTP payload TTFB. Total-download `A` values were 0.924 and 0.843. All six blocked
intervals exclude 0.5, and the paired fixed-sequence estimates are nearly identical, so the recorded
separation is not an artifact of the random key mix.

The tail did not improve monotonically. Memcache local response TTFB was worse at p99 and p99.9
before becoming better again at p99.99. HTTP local response TTFB was worse from p90 through p99.99;
HTTP payload TTFB was effectively tied at several tail percentiles, and complete download was 0.224%
worse at p99.99. By contrast, memcache payload TTFB and complete download favored local placement at
every reported percentile. The defensible conclusion is that locality shifted these recorded
distributions and their centers substantially; it did not eliminate the larger host-noise events
that determine individual tail cutoffs.

Nor should the observed gap be read as a calibrated cost for one remote DRAM access. Normal/local
ran first and perverse/remote ran second, each for several hours, because placement was the outer
loop. The fixed ordering leaves whole-run thermal, frequency, and host-state drift as possible
contributors. Randomly interleaving placement would require changing the allocator topology between
requests, which this server deliberately cannot do. The result is strong evidence that the two
controlled placements behaved differently, but not an isolated microsecond estimate of NUMA fabric
latency.

The direct cable removed the switch as one source of variation, but the timestamps still bracket a
kernel TCP path and userspace socket calls. They are not NIC hardware timestamps. IRQ handling,
socket buffering, CPU frequency, scheduler activity, and the receive-call boundary all remain above
or near the effect being sought.

This is why FPGA NICs and direct userspace networking remain on Goblin Store's development path.
Putting the NIC queue, worker, and head RAM on one NUMA node while timestamping request arrival and
first payload DMA in hardware should expose a microsecond-scale locality effect far more cleanly.
Faster links will also make RAM placement visible before 10 GbE caps aggregate throughput.

For the allocator, HugeTLB, promotion, and earlier interconnect-counter results, see
[NUMA-local RAM heads and interconnect bandwidth](numa-interconnect-bandwidth.md).
