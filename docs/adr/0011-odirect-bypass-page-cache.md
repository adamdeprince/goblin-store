# ADR-0011: O_DIRECT — own the backing store, bypass the kernel page cache

**Status:** **Proposed** (2026-06-09) — inferred from the "override the backing store" thesis;
flip to Accepted on operator confirmation.

## Context
Project thesis (operator, 2026-06-09): Goblin **overrides the OS backing store** and caches in
RAM the latency-sensitive part of each file *itself*, because it knows what the kernel page
cache can't — the head of each object is the latency-critical part. RAM is a **fixed,
command-line-specified, `mlock`ed budget** ([ADR-0008](0008-ram-allocator.md)) that Goblin
manages. If the kernel page cache *also* manages the backing-store files, the two caches fight.

## Decision (proposed)
Open all SSD/HDD backing files with **`O_DIRECT`** — bypass the kernel page cache entirely. The
kernel collapses to a block transport: `io_uring` submit → DMA into our registered, locked
buffers. Goblin owns **caching** (mlock'd head cache), **read-ahead** (the pipeline, [ADR-0006](0006-positional-tiering-pipeline.md)),
and **eviction** (multi-resource).

## Rationale
- **Deterministic footprint:** head-cache RAM is exactly `--memory + (other NUMA nodes ×
  --sub-memory)`, plus separately bounded streaming pools. The page cache
  would add *unbudgeted* RAM on top and evict unpredictably — breaking the "fixed amount" promise.
- **No double-caching, no competition** with the mlock'd region; the kernel won't cache cold HDD
  tail bytes we stream once.
- **Alignment is already paid for:** O_DIRECT needs buffer/offset/length aligned to the device
  block (512/4 KiB); ADR-0008's power-of-two, block-aligned arena gives this for free, and stripe
  units ([ADR-0009](0009-striping-and-eof.md)) are multiples of the block size.
- **We didn't want kernel read-ahead:** O_DIRECT disables it; our pipeline does object-aware read-ahead.
- **Lowest-copy path:** O_DIRECT + io_uring fixed buffers = DMA device → our buffer, no page-cache bounce.

## Consequences
- ➕ Deterministic memory; no cache contention; the literal expression of "own the backing store."
- ➖ The RAM head cache is the **only** cache — a head miss is a real device read (~50–100 µs NVMe; deterministic). Raises the stakes on head residency (eviction valve 1 is a real read).
- ➖ Sub-block reads must be aligned-and-trimmed (read the enclosing aligned range, return the slice).
- 🔬 **Benchmark vs `RWF_DONTCACHE`** (uncached buffered I/O, kernel ≥ ~6.14; dev box is 6.17): no
  cache pollution without strict alignment — a pragmatic alternative. Default to O_DIRECT.
