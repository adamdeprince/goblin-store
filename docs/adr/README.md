# Architecture Decision Records

Lightweight ADRs (MADR-ish). Status values: **Proposed** / **Accepted** / **Superseded**.
Everything marked *Accepted* below is the working default chosen **2026-06-09** and is revisable —
these are decisions of record, not laws.

| # | Decision | Status |
|---|----------|--------|
| [0001](0001-thread-per-core-concurrency.md) | Thread-per-core, network-sharded concurrency | Accepted |
| [0002](0002-iouring-rings-shared-drives.md) | Per-core io_uring rings, shared drive fds, striped parallel reads | Accepted |
| [0003](0003-cache-semantics-head-as-cache.md) | Best-effort cache semantics; RAM head = write-through cache of an SSD-resident prefix | Accepted |
| [0004](0004-cpp23-toolchain.md) | C++23 toolchain & build | Accepted |
| [0005](0005-protocol-surface.md) | Protocol surface: memcache/TCP only, HTTP object path, **no UDP**, no auth | Accepted |
| [0006](0006-positional-tiering-pipeline.md) | Positional tiering & the request-triggered read-ahead pipeline | Accepted |
| [0007](0007-eviction-policy.md) | Eviction: size-aware, FIFO-based (S3-FIFO/SIEVE + GDSF), **not LRU/LFU** | Accepted |
| [0008](0008-ram-allocator.md) | RAM allocator: power-of-two block array + intra-block buddy | Accepted |
| [0009](0009-striping-and-eof.md) | Drive pools: per-object files, round-robin striping, short-read EOF | Accepted |
| [0010](0010-write-admission-modes.md) | Write/upload admission: EVICT (cache) vs BLOCK (origin) on disk exhaustion | Accepted |
| [0011](0011-odirect-bypass-page-cache.md) | O_DIRECT: own the backing store, bypass the kernel page cache | **Proposed** |
| [0012](0012-multi-resource-eviction.md) | Multi-resource eviction: per-resource valves + demotion ladder; SSD count-bound (v1) | Accepted |
| [0013](0013-ephemeral-wipe-on-start.md) | Ephemeral v1: no persistence, wipe-on-start (dedicated subdir + marker guard) | Accepted |
| [0014](0014-keyless-digest-identity.md) | Keyless identity: client keys → 256-bit SHA-256 digest as the internal ID | Accepted |
| [0015](0015-http-key-derivation.md) | HTTP key derivation: path vs virtual-host mode; `--source` directory preloader | Accepted |
| [0016](0016-bounded-locked-memory.md) | Bounded, locked memory: local `--memory`, optional per-node `--sub-memory`, fixed streaming pools | Accepted (partial) |
| [0017](0017-buffer-taxonomy.md) | Buffer taxonomy: fixed head pool vs small streaming I/O pool; zero-copy head send | Accepted |

See [`../ARCHITECTURE.md`](../ARCHITECTURE.md) for the readable overview that ties these together.
