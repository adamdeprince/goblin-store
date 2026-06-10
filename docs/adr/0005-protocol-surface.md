# ADR-0005: Protocol surface — memcache over TCP only, HTTP object path, no UDP, no auth

**Status:** Accepted (2026-06-09). Supersedes the earlier memcache-UDP / kernel-bypass exploration.

## Context
The memcache **UDP** path is semantically wrong for large objects: its framing targets small
responses (request-id + datagram seq/count), there is no flow control and no retransmit, and a
1 GB value becomes ~700k datagrams the client must reassemble across packet loss. Decision on
**2026-06-09: drop UDP.**

## Decision
- **memcache protocol over TCP ONLY.** Implemented subset (compatible for what we implement):
  `get`, `gets`, `set`, `add`, `replace`, `delete`, `stats`, `version`, `quit`.
  **Not:** `incr`/`decr`, CAS-as-coordination, counter mutation, touch/gat, slab/LRU admin.
  The value block is length-prefixed, so we **stream it out** for fast time-to-first-byte.
- **No ranged GET on the memcache port** — the protocol has no partial-get/resume. Range &
  partial reads belong to HTTP.
- **HTTP object-serving path:** HTTP/1.1 first (TLS via OpenSSL, keep-alive, **Range
  requests**, streaming, cache-control, low-copy send, backpressure-aware). HTTP/2 later.
  **HTTP/3 deferred** — it rides QUIC/UDP, but that is a separate, congestion-controlled,
  integrated-library decision (ngtcp2 + nghttp3), **distinct from the raw memcache-UDP path we
  just dropped**. Revisit post-v1.
- **No built-in authentication anywhere.** TLS is transport only. Auth/policy = trusted
  network, public-edge use, or a reverse proxy (nginx/Envoy/HAProxy/Caddy) in front.

## Consequences
- ➕ Big scope reduction: a single network substrate (TCP) for v1; DPDK/AF_XDP/recvmmsg no longer relevant.
- ➕ Clear contract: streaming + range on HTTP; simple key ops on memcache/TCP.
- ➖ Clients that require memcache-UDP are unsupported — intended.

## Amendment (2026-06-09): SSL is required at build, optional at runtime
**OpenSSL is a required build dependency** (not compile-time optional). What's optional is
**enabling HTTPS when launching the server.** The HTTP front-end can run:
- **HTTP only** (plaintext) — default;
- **HTTPS only** — `--no-http` plus `--tls-cert`/`--tls-key`;
- **both at once** — a plaintext listener and a TLS listener on separate ports, simultaneously.

Config: independent `enable_http` (`--http-port`, default 8080) and `enable_https`
(`--https-port`, default 8443; turned on by supplying `--tls-cert` + `--tls-key`). **Both HTTP and
HTTPS may be off** — a **memcache-only** server is valid (the classic cache case; memcache/TCP
serves reads too). The only rule: at least one listener overall (memcache, HTTP, or HTTPS). TLS
stays transport-only (no auth). The keyless-identity hash
([ADR-0014](0014-keyless-digest-identity.md)) is kept **independent of OpenSSL by choice** (a
vendored SHA-256), so the storage engine doesn't link the TLS library.
