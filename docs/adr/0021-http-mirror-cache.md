# ADR-0021: HTTP mirror mode is a bounded streaming shared cache

**Status:** Accepted (2026-07-16).

## Context

Goblin Store's ordinary HTTP endpoint serves objects that were inserted through memcache or loaded
at startup. An edge deployment also needs a reverse-cache mode: a missing URI should be retrieved
from an HTTP(S) origin, returned immediately, and retained in the same RAM-head/SSD/HDD store for
later requests.

Buffering a complete miss before answering would throw away the product's first-byte advantage and
would require memory proportional to object size. Letting either the downstream network or backing
store run arbitrarily ahead would merely move that unbounded buffer elsewhere. A disconnected
downstream must not waste an otherwise useful cache fill, while a storage failure must not turn a
valid origin response into a failed client request.

## Decision

`--mirror URL` changes the HTTP/HTTPS endpoint into a shared reverse cache. It is mutually exclusive
with `--http-vhost` / `--virtual-host`: mirror identity is selected by the complete origin-form URI,
including its query, rather than by Goblin Store's ordinary path/vhost key modes. The configured
scheme, authority, and base path are preserved. For example:

```text
--mirror https://origin.example/z + GET /a/b.html?v=2
    -> https://origin.example/z/a/b.html?v=2
```

Only `GET` and `HEAD` participate. System libcurl performs DNS, TLS verification, HTTP transfer
decoding, and origin I/O; it is an optional build dependency, and selecting `--mirror` on a build
without it is a startup error. Redirects are returned as origin responses and are not followed.

### Shared-cache identity and policy

The internal key contains the URI (query included) and a canonical fingerprint of every
representation-selecting end-to-end request field. Field names and field groups are normalized,
while the order of repeated lines with the same name is retained. Hop-by-hop fields, framing,
conditionals, range controls, and request cache directives are not part of the representation key.
This deliberately over-keys before an origin's `Vary` list is known: it can produce redundant
entries, but cannot return an entry selected by different representation headers. `Vary: *` and a
`Vary` nomination of a forwarded-but-unkeyed request-control field make the response non-cacheable.

The cache honors shared-cache controls including request `no-store`, `no-cache`, `only-if-cached`,
`max-age`, `min-fresh`, and `max-stale`; response `public`, `private`, `no-store`, `no-cache`,
`max-age`, `s-maxage`, `must-revalidate`, `proxy-revalidate`, and `stale-if-error`; and `Date`, `Age`,
`Expires`, `ETag`, and `Last-Modified`. Authorized responses require explicit shared-cache
permission. Stale validators are revalidated at the origin; a successful 304 refreshes metadata
without rewriting the object generation. Hop-by-hop response fields are stripped, `Via` and an
updated `Age` are emitted, and origin status, reason, end-to-end fields, and body are otherwise
preserved. A cached full 200 response retains the ordinary endpoint's conditional and byte-range
serving path.

### Miss rendezvous

A bounded origin-worker pool owns misses independently of client connections. Same-key misses are
coalesced behind one active fill. Once that fill publishes a fresh entry, followers reopen it through
the normal hit path; if the response is intentionally non-cacheable, followers perform their own
origin requests instead.

The producer exposes at most one libcurl body chunk at a time. For each chunk it:

1. publishes the immutable chunk to the serving event loop;
2. writes the same bytes through `TierManager::StoreHandle` into the RAM head and positional backing
   tiers; and
3. waits until both the store write and downstream send acknowledge that chunk before accepting the
   next origin chunk.

Thus neither side can accumulate an unbounded lead, and origin backpressure is automatic. The
tentative generation remains invisible until the complete declared body is written and atomically
committed with its HTTP metadata. Cache hits then use the existing pinned-head first send and start
their disk-tail read when that first response send is queued.

If storage admission or a later write fails, the tentative generation is discarded and the producer
continues feeding the downstream response. If the client disconnects, its rendezvous gate is
released and the origin worker continues writing and publishing the cache entry. A slow connected
client remains subject to the ordinary `--io-timeout` resource-holder timeout.

### Known-length boundary

The current `StoreHandle` fixes positional layout and reserves filesystem capacity before accepting
body bytes, so a cache fill needs the decoded final `Content-Length`. Responses without one—such as
decoded chunked or close-delimited bodies—are still streamed to the client with correct close
framing, but are not cached. They are never buffered in RAM merely to discover their size. A future
growable unpublished generation can remove this boundary without changing the network rendezvous or
atomic publication rules.

## Consequences

- Miss time-to-first-byte is not delayed until the object has been downloaded or stored.
- Memory used for origin/downstream handoff is bounded by one libcurl chunk per active key.
- Disk errors degrade cache population, not delivery of an otherwise valid origin response.
- Client abandonment can still produce a useful cache entry.
- Same-key request bursts do not stampede a cacheable origin object.
- Representation safety is favored over cache density until the index supports a primary URI key
  with multiple exact `Vary` variants.
- Unknown-length responses remain pass-through misses in this version.
