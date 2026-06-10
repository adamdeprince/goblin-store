# ADR-0010: Write/upload admission — EVICT vs BLOCK on disk exhaustion

**Status:** Accepted (2026-06-09).

## Context
[ADR-0003](0003-cache-semantics-head-as-cache.md) makes the default a best-effort cache that
evicts to admit. But when Goblin is an **HTTP object origin**, eviction-driven disappearance of
stored objects is wrong — clients expect what they uploaded to stay put. We need a write-time
policy for what happens when a tier pool is full.

## Decision
Two operator-selectable **write-admission modes** (per server, with a sensible per-protocol
default, overridable per request):

- **EVICT — cache mode (default on the memcache path).** On exhaustion, evict low-value objects
  (GDSF, [ADR-0007](0007-eviction-policy.md)) to make room. Best-effort; existing objects may
  disappear.
- **BLOCK — store/origin mode (default on the HTTP path).** Never evict to admit. Existing
  objects are **pinned**. On exhaustion the write is refused with a clear status —
  HTTP **`507 Insufficient Storage`**; memcache **`SERVER_ERROR out of memory storing object`**.
  Streaming uploads get **bounded backpressure** (pause, wait for space freed by deletes/TTL)
  and then fail on a timeout — they never block a connection forever.

### Cross-cutting rules (both modes)
- **Atomic publish.** Stream uploads into **temp shards**; **register in the index only on full
  receipt**. A partial or failed upload (including mid-stream exhaustion in BLOCK mode) is
  aborted and its temp shards `unlink`ed — an object is **never half-visible and never corrupt**.
  (Pairs with per-object-file striping, [ADR-0009](0009-striping-and-eof.md).)
- **Reserve when length is known.** memcache `<bytes>` and HTTP `Content-Length` let us reserve
  SSD-prefix + HDD-tail space up front and reject immediately; chunked / unknown-length uploads
  stream-and-check and may fail late.
- **Check both pools (3-layer):** an object needs SSD-prefix space **and** HDD-tail space.
- BLOCK prevents **eviction-driven** loss only. **Crash durability (fsync) is a separate axis** —
  still the future "durable store mode" of [ADR-0003](0003-cache-semantics-head-as-cache.md), not
  implied here.

## Consequences
- ➕ One binary serves both roles: ephemeral cache (EVICT) and pinned origin (BLOCK); no half-written objects; standard, honest out-of-space signaling.
- ➖ BLOCK needs per-pool space accounting/reservation and a backpressure-timeout knob.
