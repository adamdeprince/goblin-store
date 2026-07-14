# ADR-0018: Concurrency — thread-per-core network, shared locked storage

**Status:** Accepted (2026-06-10). Details ADR-0001/0002 and records the v1 trade-offs.

## Context
The v1 server is a blocking, single-connection loop — correct, but it caps throughput at one
in-flight request. Throughput is the headline benchmark vs memcached extstore.

## Decision
- **Thread-per-core.** N worker threads per enabled protocol (`--cores`; zero means the CPU count
  available on the selected NUMA node), each with its own io_uring `Reactor` + I/O-buffer pool. A
  connection is served end-to-end on its accepting core — no cross-core byte movement (ADR-0001).
  - **NUMA affinity before allocation.** `--numa NODE` selects a node explicitly. Otherwise the node
    comes from the Linux NUMA locality of the UP Ethernet interfaces reached by the wildcard IPv4
    listeners. Interfaces on different nodes, an unknown interface locality on a multi-node host,
    or no discoverable listening interface make startup fail with every interface name, address,
    node, and usable `--numa NODE` command. The main thread is bound before the fixed head and
    write-staging arenas are allocated; all worker, TTL-reaper, score-decay, and NUMA-promotion
    threads inherit its affinity. A strict inherited default memory policy also keeps the dynamic
    key index and thread-local allocations on the selected node.
    With explicit `--numa`, `--memory` is strictly bound to that local node and `--sub-memory` may
    add an equal-sized subordinate region on every other online node. Automatic NUMA selection may
    not use `--sub-memory`. Head allocation exhausts the local region before visiting foreign ones;
    decayed access scores then exchange hotter full foreign buddy blocks with colder local blocks
    ([ADR-0019](0019-access-score-numa-promotion.md)). Score decay has priority over promotion: no new
    exchange begins once the minute rescore is waiting, and the traversal runs promotion-free after
    any already-active exchange completes.
  - **Blocking interim: one shared listen socket.** All workers `accept()` from a single listener so
    the kernel hands each new connection to a *free* worker. Per-core `SO_REUSEPORT` listeners are
    wrong here: SO_REUSEPORT pins a connection to a fixed listener by 4-tuple hash, so when two
    connections collide on the hash one worker serves both serially while another idles — and under
    one-connection-per-worker the collided connection *starves* in the backlog. (Measured: cores=2
    showed zero speedup until switched to a shared listener; then 1.7×/4.0×/5.0× at 2/4/8 cores.)
  - Per-core `SO_REUSEPORT` listeners + multishot accept return with the async loop, where a worker
    multiplexes many connections and an uneven hash just means "a few more connections," not starvation.
- **Shared, locked storage.** The index is sharded `shared_mutex` (already). The tier manager's
  head-cache + eviction policies are guarded by a single `TierManager` mutex (coarse, v1), held
  **only** for the control-plane (head memcpy + policy ops) — never during disk I/O or network.
  Drive fds are shared read-only; striped reads/writes go through the per-core reactor and run
  concurrently. **Lock order:** `TierManager` mutex → index shard lock.
- **Staging (v1 → target).** First cut runs the existing blocking `handle_conn` per connection on
  each core thread (concurrency = thread count). The io_uring **multishot-recv async loop**
  (many connections multiplexed per core) is the throughput target and layers on top.

## Trade-offs
- **Zero-copy head — RESOLVED (head pinning).** A GET pins the resident head (refcount, under the
  lock), sends it zero-copy straight from the pool with no lock held, then unpins. An eviction or
  overwrite of a pinned head **orphans** its RAM region (deferred free) until the last reader unpins,
  so a late send can never touch freed memory. Head-resident GETs serve entirely from RAM and never
  borrow an I/O buffer. (Pin/unpin are O(1) under the exclusive lock; a shared/atomic-refcount pin is
  the scaling follow-up.)
- **Atomic publish (deferred).** Stores overwrite the per-object files in place (`O_TRUNC`); a concurrent GET
  of the *same* key could read torn bytes. Index-gated visibility keeps distinct-key concurrency
  (the common case) correct; same-key store+read needs temp-file + atomic rename (ADR-0010) — deferred.
- **Lock granularity.** One TierManager mutex is a contention point at high core counts; sharding
  the head cache + policies (or RCU on the index) is the scaling follow-up.

## Consequences
- ➕ N-way concurrency; disk I/O overlaps across cores; clean path to the async loop.
- ➖ Coarse lock + copy-head + non-atomic same-key writes are v1 simplifications with clear upgrades (pinning, atomic-publish, sharded locks).
