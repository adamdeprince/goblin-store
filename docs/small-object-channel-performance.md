# Small-object channel and short-key SHA-256 performance

**Tested:** 2026-07-15

Goblin's small-object path now frames a RAM-resident HTTP or memcache response with a body of at most
16 KiB for one send instead of waiting for separate header, body, and trailer completions. Hot-path
digest formatting no longer allocates a temporary string. On x86 with SHA-NI, the hardware path
remains the first choice; on older x86, Goblin uses its scalar SHA-256 through 183-byte keys and
OpenSSL for longer inputs. The
cutoff is the padding boundary where the scalar implementation grows from three compression blocks
to four on the Xeon E5-4657L v2.

We measured the combined channel changes and SHA policy on two four-socket Dell R820s connected by
a direct 10 Gb link. Dopey served 400,000 one-KiB RAM-resident objects while Rain issued strictly
serial pseudorandom reads. Both processes were pinned to CPU 4 on the NIC-local NUMA node. Each of
four ABBA launches ran 100,000 warmups followed by one million measured requests per protocol,
giving eight million measured requests. Every run used the same fixed key sequence and all raw-trace
checksums verified.

Pooled exact percentiles compare the original pre-optimization server with the fixed server:

| Protocol and metric | p50, original -> fixed | p99, original -> fixed | p99.9, original -> fixed |
|---|---:|---:|---:|
| memcache response TTFB | 76.977 -> 58.225 us (+24.4%) | 96.117 -> 85.130 us (+11.4%) | 108.500 -> 96.891 us (+10.7%) |
| memcache completion | 116.867 -> 59.704 us (+48.9%) | 142.387 -> 88.743 us (+37.7%) | 150.534 -> 100.294 us (+33.4%) |
| HTTP response TTFB | 78.707 -> 61.181 us (+22.3%) | 98.770 -> 88.590 us (+10.3%) | 113.220 -> 98.717 us (+12.8%) |
| HTTP completion | 105.534 -> 62.969 us (+40.3%) | 117.800 -> 92.757 us (+21.3%) | 129.181 -> 103.384 us (+20.0%) |

An otherwise identical old-versus-fixed build isolated the short-key SHA policy. Median
completion improved by 28.1% for memcache and 28.4% for HTTP, and both adjacent ABBA pairs agreed.
The CPU cost itself fell only from roughly 920 ns through OpenSSL 3's one-shot path to about
420 ns in the scalar path. The larger end-to-end step is most plausibly a platform amplification:
both bnx2x NICs used 24 us RX interrupt coalescing, almost exactly the observed median TTFB shift.
The network result is real for this configuration, but it is not 23 us of saved SHA computation.

HTTP was stable across the two adjacent comparisons. Memcache's fixed-build median TTFB shifted
between 57.821 and 74.777 us, exposing that coalescing phase and other whole-run effects remain
material. The confidence intervals are therefore conditional on these launches, not general
build-wide significance claims.

The preserved run IDs are `20260715T133128Z-channel-micro-hash-only` for the isolated SHA policy and
`20260715T135011Z-channel-micro-fixed-vs-original` for the full comparison. The fixed build passed
all 182 unit cases under Release, ASan/UBSan, and unit TSan.
