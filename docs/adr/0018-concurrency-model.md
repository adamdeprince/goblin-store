# ADR-0018: Concurrency — thread-per-core network, shared locked storage

**Status:** Accepted (2026-06-10). Details ADR-0001/0002 and records the v1 trade-offs.

## Context
The server needs many in-flight requests without handing connection state or payload buffers between
workers. The original blocking, one-connection-per-worker loop remains as a memcache fallback;
io_uring event loops are now the default and the only HTTP/HTTPS implementation.

## Decision
- **Thread-per-core.** N worker threads per enabled protocol (`--cores`; zero means the CPU count
  available on the selected NUMA node), each with its own io_uring `Reactor` + I/O-buffer pool. A
  connection is served end-to-end on its accepting core — no cross-core byte movement (ADR-0001).
  - **NUMA affinity before allocation.** `--numa NODE` selects a node explicitly. Otherwise the node
    comes from the Linux NUMA locality of the UP Ethernet interfaces reached by the wildcard IPv4
    listeners. Interfaces on different nodes, an unknown interface locality on a multi-node host,
    or no discoverable listening interface make startup fail with every interface name, address,
    node, and usable `--numa NODE` command. The main thread is bound before the fixed head and
    write-staging arenas are allocated; serving, TTL-reaper, and NUMA-promotion coordinator threads
    inherit its affinity. Dense-score workers explicitly bind to the NUMA node whose score pages
    they scan and decay. A strict inherited default memory policy also keeps the dynamic key index
    and ordinary thread-local allocations on the selected node.
    With explicit `--numa`, `--memory` is strictly bound to that local node and `--sub-memory` may
    add an equal-sized subordinate region on every other online node. Automatic NUMA selection may
    not use `--sub-memory`. Head allocation exhausts the local region before visiting foreign ones;
    decayed access scores then exchange hotter full foreign buddy blocks with colder local blocks
    ([ADR-0019](0019-access-score-numa-promotion.md)). Score decay has priority over promotion: no new
    exchange begins once the minute rescore is waiting, and the traversal runs promotion-free after
    any already-active exchange completes.
  - **Blocking memcache fallback: one shared listen socket.** All workers `accept()` from a single
    listener so the kernel hands each new connection to a *free* worker. Per-core `SO_REUSEPORT`
    listeners are
    wrong here: SO_REUSEPORT pins a connection to a fixed listener by 4-tuple hash, so when two
    connections collide on the hash one worker serves both serially while another idles — and under
    one-connection-per-worker the collided connection *starves* in the backlog. (Measured: cores=2
    showed zero speedup until switched to a shared listener; then 1.7×/4.0×/5.0× at 2/4/8 cores.)
  - **Async default:** each worker owns a `SO_REUSEPORT` listener and one io_uring loop. A one-shot
    async accept is rearmed after each completion, and the worker multiplexes many connections, so
    an uneven reuseport hash means more connections on one loop rather than backlog starvation.
- **Shared, locked storage.** The index is sharded `shared_mutex` (already). The tier manager's
  head-cache + eviction policies are guarded by a single `TierManager` mutex (coarse, v1), held
  **only** for the control-plane (head memcpy + policy ops) — never during disk I/O or network.
  A read snapshot owns descriptors for one immutable generation; its striped reads go through the
  connection worker's reactor. Each `StoreHandle` owns private generation descriptors and performs
  synchronous striped `pwrite` outside the storage lock, so different handles can still write
  concurrently. Long-lived pool-directory fds are shared. **Lock order:** `TierManager` mutex →
  index shard lock.
- **In-flight replacement protection.** Each `StoreHandle` holds a refcounted digest token from
  admission through commit or abort. The first token detaches the current incarnation from its
  head, global object-count, and filesystem-capacity victim selectors; the last release reattaches
  whichever incarnation is current. Concurrent replacements therefore cannot evict the version
  another handle is replacing. Unpublished
  fractional-head reservations are also tracked by physical slot so arena compaction skips their
  blocks. A successfully committed handle is terminal: later writes or commits are rejected.
- **Bounded connection state.** The async loop owns every connection object until all tagged
  completions referencing it have drained. SETs park on a shared write-staging pool without reading
  more body bytes; GETs park on their worker's read-buffer pool. Disk-tail read-ahead uses at most
  two buffers: the initial tail read starts with the first response send and may complete behind the
  resident head, after which the read of piece N+1 overlaps the send of piece N. Ready tail lanes
  cannot send until the header/head state machine reaches the disk-stream phase.

## Trade-offs
- **Pinned-head safety — RESOLVED.** A GET pins the resident head under the storage snapshot. The
  ≤16 KiB fast path copies a complete RAM-only body into its framed output and unpins before the
  single send; larger resident slices are sent zero-copy from the pinned pool region. An eviction
  or overwrite of a still-pinned head **orphans** its RAM region (deferred free) until the last
  reader unpins, so a late send can never touch freed memory. Head-resident GETs never borrow a disk
  I/O buffer. The common pin/unpin path holds the shared storage lock and changes an atomic refcount;
  only creation of a missing pin record and final release of orphaned bytes need the exclusive lock.
- **Atomic publish — RESOLVED.** A store writes immutable
  `<64-hex-digest>.g<file_generation>` files with `O_EXCL` and a private RAM head. Once complete, it
  installs metadata, head locator, and sole score owner with one Index swap while holding the
  storage write lock; no shard rename is part of publication. A reader captures one head/generation
  snapshot under the read lock. Already-open descriptors pin the retired inodes after their names
  are unlinked. It therefore observes one complete incarnation, never an old head joined to a new
  tail.
- **Lock granularity.** One TierManager mutex is a contention point at high core counts; sharding
  the head cache + policies (or RCU on the index) is the scaling follow-up.

## Consequences
- ➕ N-way concurrency; each async worker multiplexes connections and disk reads on its own ring.
- ➖ The coarse control-plane lock remains the principal v1 simplification; sharded policies or RCU
  remain follow-ups if profiling shows contention.
