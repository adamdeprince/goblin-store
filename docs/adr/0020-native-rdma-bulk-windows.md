# ADR-0020: Native RDMA uses an inline control ring and registered bulk windows

**Status:** Accepted (2026-07-15).

## Context

The original Packrat version-2 RDMA ring carries at most 192 application bytes in each inline
RDMA write. Reusing that byte-stream transport unchanged for memcache made it put object bodies
through the control ring too: a 1 MiB value required about 5,462 body writes. That is useful for
small commands, but it is the wrong data plane for an object store.

Goblin Store needs two different paths:

- commands, response headers, trailers, and credits are small, latency-sensitive control data;
- object bodies are large, ordered streams that should use the HCA's bulk transfer machinery.

Registering every object is also the wrong boundary. Objects may live partly in a movable RAM-head
cache and partly on disk, and their lifetimes are unrelated to a connection. Exposing those stores
would make rkeys and object replacement part of the network protocol.

## Decision

Goblin Store's native InfiniBand transport is a strict version-3 protocol. It retains the
receiver-polled 256-byte control-ring slots and adds a bounded, separately registered bulk mapping
to every endpoint. TCP over IPoIB is not involved.

### Handshake and memory geometry

RDMA-CM exchanges a 56-byte, network-byte-order descriptor. Its first 40 bytes retain the Packrat
v2 layout; version 3 uses the former reserved field for the bulk-window power-of-two shift and the
remaining 16 bytes for the bulk mapping:

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 4 | magic `GBRD` |
| 4 | 2 | version, `3` |
| 6 | 2 | control slot stride, `256` |
| 8 | 4 | control slot count |
| 12 | 2 | maximum control record, `192` |
| 14 | 2 | `log2(bytes per bulk window)` |
| 16 | 8 | control-ring address |
| 24 | 4 | control-ring rkey |
| 28 | 4 | control mapping bytes |
| 32 | 8 | diagnostic nonce |
| 40 | 8 | bulk-mapping address |
| 48 | 4 | bulk-mapping rkey |
| 52 | 4 | bulk-mapping bytes |

Each bulk mapping is split into equal halves:

```text
[ local registered TX staging windows ][ peer-write RX windows ]
```

The defaults are four 256 KiB windows in each half, or 2 MiB of registered bulk memory per
endpoint and connection. Window size and count are connection settings and are independent of
`--ram-head`. The negotiated transfer size and in-flight count are the smaller geometry advertised
by the two peers.

Mappings are page-aligned, prefaulted, and registered once for the connection. The client binds
them to the HCA's NUMA node. On the server, the exact `--rdma` address participates in automatic
NUMA selection—including native InfiniBand netdevs—so the serving threads, object-head preference,
and first-touched connection mappings select that HCA's node. If wildcard TCP listeners and the
RDMA listener span nodes, startup reports every interface/address/node and requires an explicit
`--numa` choice. The bulk MR allows remote writes, not remote reads. We do not register individual
objects or expose the head arena and disk-buffer pools through per-object rkeys.

### Ordered control records

Version 3 assigns a type byte inside each control record:

- `INLINE`: up to 191 bytes of command, response framing, or status;
- `BULK_READY`: `{length, sequence}` for a completed write into an RX window;
- `BULK_RELEASE`: a cumulative consumed bulk sequence.

The connection remains one reliable ordered byte stream to the memcache parser. `INLINE` payloads
and the RX-window views named by `BULK_READY` are exposed in control-ring order. Release records are
handled inside the transport and do not appear in the application stream.

A nonempty object body always uses bulk windows, even when it is shorter than 191 bytes. A
zero-length object has no body record. Command lines, value/status headers, and protocol trailers
always use `INLINE`. Goblin Store does not silently reconnect with v2 and put bodies back through
the inline ring.

### Publication, credit, and lifetime rules

For each bulk fragment the sender:

1. waits for both an unused local TX staging window and a released peer RX window;
2. fills the registered staging window;
3. posts one signaled RDMA write into the peer's corresponding RX window;
4. posts `BULK_READY` on the same reliable-connected QP; and
5. advances its bulk sequence.

QP ordering makes observation of `BULK_READY` proof that the preceding data write is visible. The
receiver sends cumulative `BULK_RELEASE` only when the application calls `pop()` and no longer
references that RX view.

These are two separate reuse conditions:

- the local staging slot cannot be modified until the signaled data-write CQE retires it;
- the remote destination cannot be overwritten until the peer releases its sequence.

Control-ring credits remain independent. Pending release records receive priority so a full data
pipeline cannot starve the credits needed to drain it. Malformed types, sequence gaps, impossible
lengths, regressing releases, QP failures, and mid-transfer timeouts poison the connection; the
memcache byte stream cannot be resynchronized safely after them.

### Goblin Store streaming

On `SET`, bulk RX views feed `TierManager::StoreHandle::write()` in order. A view is released only
after that call has consumed or copied it. The object remains invisible until the normal atomic
`commit()` succeeds.

On `GET`, the server sends the `VALUE` header and resident head first. When the first head write is
posted, it also starts the disk-tail read into another registered staging window. Thus SSD/HDD
latency overlaps transmission of the configurable RAM head instead of beginning after the head is
drained. The first implementation copies resident-head bytes into a registered staging window, so
the staging-slot completion rule—not the head pin—keeps the HCA source immutable through the data
write CQE. The head remains pinned until its last bytes have been copied, preventing eviction,
small-arena compaction, or NUMA promotion from changing them during that copy.

Registering stable pool buffers once per HCA protection domain is a later zero-copy optimization.
That path will instead retain the source head pin through its data-write CQE; the wire protocol does
not change.

## Consequences

- A 1 MiB body with the default geometry uses four bulk data writes and four small ready records,
  rather than roughly 5,462 inline body writes.
- Registered and pinned memory is bounded per connection regardless of object size.
- Slow consumers exert backpressure by withholding window releases; they cannot be overwritten.
- Client and server must deploy v3 together. Packrat's v2 peers are intentionally incompatible.
- The first server implementation gives each connection its own QP, CQ, registered mappings,
  io_uring instance, and progress thread. Moving multiple QPs onto one core-local progress loop is
  a future scalability optimization and does not change the wire protocol.
- The protocol assumes a trusted fabric. RC provides reliability and ordering, not authentication
  or encryption, and the advertised per-connection mappings are accessible to the connected peer.
