# Native RDMA over InfiniBand: 256 KiB latency and throughput

Goblin Store delivered **38.232 Gbit/s of object payload over a 40 Gbit/s native InfiniBand
link** while serving exact 256 KiB values from its resident-head pool. That is 95.58% of the
nominal link rate before counting any control records or fabric overhead. With one request
outstanding, median response-header TTFB was **7.659 us**, the complete 256 KiB payload was visible
at **98.837 us**, and the complete memcache response finished at **99.740 us**.

This is a measurement of the version-3 registered-window data path, not TCP over IPoIB. The test
recorded one million per-request latency samples in addition to the throughput completions, and
moved just over 1 TiB of measured object payload across its latency, scaling, and confirmation
phases.

## Test rig

The direct-link test ran on the same matching Dell PowerEdge R820s used for Goblin Store's NUMA
work:

- `dopey` was the server and `rain` was the client;
- each host has four sockets, 48 physical Ivy Bridge cores, 96 logical CPUs, and 120 MiB aggregate
  L3;
- each Mellanox Connect-IB MT4113 port was active at 40 Gbit/s (4x FDR10) with a 4096-byte verbs
  MTU; and
- a direct cable joined `dopey:ibp65s0` at `10.88.88.1` to `rain:ibp65s0` at `10.88.88.2`.

Both HCAs are attached to NUMA node 1. The exact `--rdma 10.88.88.1` address selected node 1
automatically; no explicit `--numa` override was required. The server's allowed CPU set was all 24
node-1 logical CPUs. Serial latency ran on client CPU 1, while throughput used the 12 first hardware
threads on node 1 (`1,5,9,...,45`); the two-QP confirmation runs used CPUs 1 and 5. Management
traffic stayed on separate interfaces.

The server ran as:

```sh
goblin-store \
  --memory 8G --block 2M --ram-head 256K \
  --ssd-dir /var/tmp/goblin-rdma-bench-256k-pool \
  --no-memcache --no-http \
  --rdma 10.88.88.1 --rdma-port 11211 \
  --rdma-window 256K --rdma-windows 4 \
  --no-mlock --no-numa-promotion --cores 12
```

NUMA placement was active, but this was not a cross-node promotion test. The 4 GiB corpus fit on
the HCA-local node, and promotion was disabled so it could not add background work. Linux
`numa_maps` attributed 4,307,726,336 resident bytes to node 1 and only about 6.2 MB to all other
nodes combined. No HugeTLB pages were reserved on these hosts, so Goblin Store exercised its normal
graceful fallback to ordinary pages. The test ran without memory pressure, and all corpus pages
remained resident.

## Dataset and request shape

The corpus contained 16,384 objects of exactly 262,144 bytes: **4 GiB total**. An object exactly
one configured head long is RAM-only, so the backing directory contained only its 23-byte safety
marker after loading. The working set was more than 34 times the machines' aggregate L3 and could
not be served as a cache-resident toy set.

Twelve QPs loaded the corpus over the native RDMA protocol before timing. Measured reads then used
a uniform SplitMix64 selector with seed `0x676f626c696e`. Each latency repetition reset that seed,
so the five repetitions traversed the same pseudo-random 200,000-key sequence. A miss, wrong key,
wrong declared length, or invalid framing was a fatal benchmark error. The benchmark deliberately
did not copy or checksum every payload byte in the timed path.

The latency phase used one connection, one outstanding GET, and no request overlap. Each of five
repetitions performed 5,000 warmups followed by 200,000 measured requests, producing 1,000,000 raw
samples and 244.141 GiB of measured payload. Sample arrays were allocated and faulted before timing,
and CSV formatting happened only after the measured connection closed.

The throughput sweep used one closed-loop sequential request stream per QP. Every point warmed for
five seconds and measured for 20 seconds. Three further two-QP runs used five-second warmups and
30-second measurement windows to confirm the first saturation point.

## What the timestamps mean

All elapsed times use a client-side monotonic clock and are recorded in nanoseconds, so no
cross-host clock synchronization is involved:

- **response-header TTFB** starts immediately before the successful GET submission attempt and
  ends when the first `VALUE` header record is application-visible;
- **payload ready** ends when the first body view is application-visible; and
- **response complete** ends after the complete value and `\r\nEND\r\n` trailer have been consumed
  by the benchmark parser.

For this exact geometry, “payload ready” has an important RDMA-specific meaning. The 256 KiB value
uses one 256 KiB registered bulk window. Reliable-connected QP ordering publishes `BULK_READY` only
after the preceding RDMA write is visible, so the client receives the complete 256 KiB view at once.
The protocol does not expose the instant when the literal first body byte reaches memory. The
98.837 us median below is therefore full-payload readiness, not a TCP-style first-body-byte receive.

The benchmark consumes the registered view without copying or checksumming all 256 KiB. “Response
complete” is transport/protocol completion rather than completion of an application-owned copy. On
the server, this implementation does copy the resident head into registered TX staging memory, so
that cost is included. Stable pool-buffer registration is a future zero-copy optimization.

## Latency results

The following nearest-rank percentiles pool the five measured repetitions:

| metric | mean | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| response-header TTFB | 7.844 us | 7.659 us | 8.806 us | 9.003 us | 22.388 us | 49.850 us | 186.286 us |
| payload ready | 98.748 us | 98.837 us | 100.036 us | 105.961 us | 115.925 us | 163.887 us | 280.110 us |
| response complete | 99.678 us | 99.740 us | 100.960 us | 106.911 us | 116.856 us | 165.124 us | 2,983.147 us |
| header to payload ready | 90.903 us | 91.141 us | 91.506 us | 97.349 us | 107.019 us | 138.553 us | 213.813 us |

The five per-repetition mean completion times ranged from 99.598 to 99.873 us. One isolated maximum
completed in 2.983 ms, but its payload was already ready at 93.969 us; that outlier occurred between
payload readiness and the final protocol trailer rather than during the bulk data transfer. The
p99.99 completion result remained 165.124 us.

## Throughput results

Throughput counts only completed 256 KiB value bytes inside each measurement window. Memcache
framing and RDMA control records are excluded from the payload rate.

| QPs | requests/s | payload GiB/s | payload Gbit/s | nominal 40 Gbit/s link |
|---:|---:|---:|---:|---:|
| 1 | 9,971.1 | 2.434 | 20.911 | 52.28% |
| 2 | 18,099.1 | 4.419 | 37.957 | 94.89% |
| 4 | 18,197.3 | 4.443 | 38.163 | 95.41% |
| 8 | 18,214.4 | 4.447 | 38.198 | 95.50% |
| 12 | 18,230.2 | 4.451 | **38.232** | **95.58%** |

One closed-loop QP leaves roughly half the link unused because each request waits for its complete
reply. Two QPs overlap that turnaround and reach within 0.72% of the observed 12-QP maximum. Adding
ten more QPs improves payload rate by less than one percent, so two is the smallest practical
saturation point for this object size and hardware.

Three independent 30-second two-QP confirmation runs produced 37.648, 37.898, and 38.028 Gbit/s.
Their mean was **37.858 Gbit/s** with a **0.193 Gbit/s sample standard deviation**. The `+/-` value
is run-to-run standard deviation, not a confidence interval.

Client HCA counters agreed with the completed object traffic plus warmups and wire overhead. For
example, each 205,000-request latency repetition requested 53,739,520,000 value bytes and received
about 54.11 billion fabric-accounted bytes. Both ports ended with zero symbol errors, link-down
events, receive errors, transmit discards, integrity errors, overruns, or VL15 drops.

## Scope and limitations

This result establishes the performance of the native bulk-window transport for RAM-resident
256 KiB objects on 40 Gbit/s InfiniBand. It does not establish:

- TCP-versus-RDMA improvement; no matched TCP run was part of this experiment;
- SSD or HDD streaming performance; every object fit exactly in its RAM head;
- NUMA promotion benefit; placement was local and `--no-numa-promotion` was intentional;
- literal first-body-byte arrival, which version 3 does not expose for a bulk window; or
- application-copy latency, because the benchmark releases the registered body view in place; or
- CPU efficiency or scaling to larger client populations, because CPU utilization was not
  collected.

The current one-progress-thread-per-QP implementation is sufficient to saturate this link with two
connections. Faster fabrics, registered stable head buffers, shared core-local progress loops, FPGA
NICs, and direct userspace networking remain the next useful measurements and optimizations.

## Reproducing the run

Build the separately installable C++ client and its benchmark with native RDMA enabled:

```sh
cmake -S python/cpp -B build-client \
  -DCMAKE_BUILD_TYPE=Release \
  -DGOBLIN_STORE_CLIENT_ENABLE_RDMA=ON \
  -DGOBLIN_STORE_CLIENT_BUILD_BENCHMARKS=ON
cmake --build build-client -j
```

After preparing an empty `/var/tmp` pool and starting the server, load the exact corpus:

```sh
taskset -c 1,5,9,13,17,21,25,29,33,37,41,45 \
  build-client/goblin_store_rdma_benchmark load \
  --address 10.88.88.1 --objects 16384 --object-bytes 262144 --threads 12
```

Then run and analyze the suite. The CPU list and HCA device are explicit environment settings so a
different machine cannot silently inherit the R820 topology:

```sh
BIN=build-client/goblin_store_rdma_benchmark \
ADDRESS=10.88.88.1 IBDEV=ibp65s0 \
LATENCY_CPU=1 THROUGHPUT_CPUS=1,5,9,13,17,21,25,29,33,37,41,45 \
CONFIRM_CPUS=1,5 \
  bench/run_rdma_256k_ib.sh /var/tmp/goblin-rdma-results

python3 bench/analyze_rdma_256k.py /var/tmp/goblin-rdma-results
```

At the time of writing, the original 34 MiB artifact set remains at
`rain:/var/tmp/goblin-rdma-bench-256k-results-0715`. It contains all one million raw nanosecond
rows, per-run summaries, throughput CSVs, HCA counter deltas, client and server topology metadata,
the exact runner and analyzer, and `SHA256SUMS`.

For the wire protocol and credit/lifetime rules, see
[ADR-0020: Native RDMA uses an inline control ring and registered bulk windows](adr/0020-native-rdma-bulk-windows.md).
