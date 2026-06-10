# ADR-0001: Thread-per-core, network-sharded concurrency

**Status:** Accepted (2026-06-09) — revisable.

## Context
Goblin Store streams large objects through an io_uring pipeline. We want lock-free hot
paths and **zero cross-core data movement** while bytes flow socket ↔ buffer ↔ disk.

Two shared-nothing shapes were considered:
- **Network-sharded** — cores own connections; storage state is shared (read-mostly).
- **Key-sharded** — cores own a slice of the keyspace *and* its storage; misrouted requests
  are forwarded to the owning core (Seastar style).

## Decision
**Thread-per-core reactor, network-sharded.** Each core runs an independent io_uring event
loop and owns a disjoint set of client connections (distributed via `SO_REUSEPORT`). A
request is served **end-to-end on the core that owns its connection**: that core issues all
RAM/SSD/HDD reads on its own ring and writes its own socket. No request hand-off, no
cross-core byte shuffling on the streaming path.

We deliberately reject key-sharded storage: for a streaming workload it would force
gigabytes to cross cores from the storage-owning core back to the connection's core. Storage
state (object index, drive fds) is therefore **shared and read-mostly** — see [ADR-0002](0002-iouring-rings-shared-drives.md).

## Consequences
- ➕ Lock-free serving path; per-core buffer pools; near-linear scaling for independent connections; NUMA/affinity friendly.
- ➖ The shared index needs concurrency control — but objects are *few and large*, so contention is low (sharded `shared_mutex` now; RCU/epoch later if needed).
- ➖ A few enormous connections can imbalance cores. v1 mitigates with per-connection backpressure, **not** migration.
