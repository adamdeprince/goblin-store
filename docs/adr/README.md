# Architecture Decision Records

Lightweight ADRs (MADR-ish). Status values: **Proposed** / **Accepted** / **Superseded**.
Everything marked *Accepted* below is the working default chosen **2026-06-09** and is revisable —
these are decisions of record, not laws.

| # | Decision | Status |
|---|----------|--------|
| [0001](0001-thread-per-core-concurrency.md) | Thread-per-core, network-sharded concurrency | Accepted |
| [0002](0002-iouring-rings-shared-drives.md) | Per-worker io_uring rings, immutable snapshot fds, striped parallel reads | Accepted |
| [0003](0003-cache-semantics-head-as-cache.md) | Best-effort cache semantics; RAM head = write-through cache of an SSD-resident prefix | Accepted |
| [0004](0004-cpp23-toolchain.md) | C++23 toolchain & build | Accepted |
| [0005](0005-protocol-surface.md) | Protocol surface: memcache over TCP and native RDMA, HTTP object path, **no UDP**, no auth | Accepted |
| [0006](0006-positional-tiering-pipeline.md) | Positional tiering & the request-triggered read-ahead pipeline | Accepted |
| [0007](0007-eviction-policy.md) | Eviction: size-aware, FIFO-based (S3-FIFO/SIEVE + GDSF), **not LRU/LFU** | Accepted |
| [0008](0008-ram-allocator.md) | RAM allocator: power-of-two block array + intra-block buddy | Accepted |
| [0009](0009-striping-and-eof.md) | Drive pools: immutable generation files and rotated round-robin striping | Accepted |
| [0010](0010-write-admission-modes.md) | Per-write admission: EVICT vs BLOCK (no capacity reclaim) on filesystem exhaustion | Accepted |
| [0011](0011-odirect-bypass-page-cache.md) | O_DIRECT: own the backing store, bypass the kernel page cache | **Proposed** |
| [0012](0012-multi-resource-eviction.md) | Multi-resource eviction: RAM heads, global object count, and per-filesystem capacity | Accepted |
| [0013](0013-ephemeral-wipe-on-start.md) | Ephemeral v1: no persistence, wipe-on-start (dedicated subdir + marker guard) | Accepted |
| [0014](0014-keyless-digest-identity.md) | Keyless identity: client keys → 256-bit SHA-256 digest as the internal ID | Accepted |
| [0015](0015-http-key-derivation.md) | HTTP key derivation: path vs virtual-host mode; `--source` directory preloader | Accepted |
| [0016](0016-bounded-locked-memory.md) | Bounded, locked memory: local `--memory`, optional per-node `--sub-memory`, fixed streaming pools | Accepted (partial) |
| [0017](0017-buffer-taxonomy.md) | Buffer taxonomy: fixed heads, inline small responses, and streaming I/O pools | Accepted |
| [0018](0018-concurrency-model.md) | Concurrency: thread-per-core network, shared locked storage | Accepted |
| [0019](0019-access-score-numa-promotion.md) | Decayed per-key access scores and hot full-block NUMA promotion | Accepted |
| [0020](0020-native-rdma-bulk-windows.md) | Native RDMA: inline control ring plus registered, credit-controlled bulk windows | Accepted |

See [the project README](../../README.md) for the readable overview that ties these together.
