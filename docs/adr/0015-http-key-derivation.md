# ADR-0015: HTTP key derivation — path vs virtual-host mode, and the `--source` preloader

**Status:** Accepted (2026-06-09). Canonicalization defaults revisable; the query-string default
(strip) is the one open judgment call.

## Context
HTTP retrieval must turn a request into a Goblin key, and a startup `--source <dir>` preloader
must derive **the same** keys so preloaded content is actually found. The two are duals of one
rule.

## Decision
**Two server-wide modes (flag; default = path):**

| | HTTP request | → key | `--source` file | → key |
|---|---|---|---|---|
| **Path** (default) | `GET /foo/bar.tar.gz` | `/foo/bar.tar.gz` | `src/bar/baz.txt` | `/bar/baz.txt` |
| **Virtual-host** (`--http-vhost`) | `Host: www.example.com` + `GET /foo/bar.tar.gz` | `www.example.com/foo/bar.tar.gz` | `src/www.example.com/bar/baz.txt` | `www.example.com/bar/baz.txt` |

Path mode keeps the URI's leading `/`; **vhost mode has no leading `/`** — the first path
component *is* the host.

- **One shared `derive_key(mode, host, uri)` / `derive_key_from_relpath(mode, relpath)`** used by
  both the HTTP front-end and the loader — same normalization, no drift.
- **`--source <dir>` (repeatable):** read-only input directories, **not** pool dirs (never wiped).
  Startup order: wipe pools ([ADR-0013](0013-ephemeral-wipe-on-start.md)) → ingest each file under
  its derived key through the normal write path (tiering/striping/atomic-publish,
  [ADR-0006](0006-positional-tiering-pipeline.md)/[ADR-0010](0010-write-admission-modes.md)).
  It passes the configured HTTP write mode to each admission (default BLOCK), so content that does
  not fit reports an error instead of reclaiming disk objects for that preload.
- **Canonicalization (recommended defaults):**
  - URI path: percent-decode, collapse duplicate `/`, resolve `.`/`..` segments.
  - **Query string stripped by default** (the directory model has no queries, so this is what
    makes `/app.js?v=2` find the preloaded `/app.js`); flag to key-on-query for cache-busting.
  - Host (vhost): lowercase, strip port and trailing dot (do **not** strip `www.`). Require a
    `Host` header in vhost mode — `400` if missing, since the key depends on it.
- **memcache keys are literal** — the client supplies the exact string (then hashed,
  [ADR-0014](0014-keyless-digest-identity.md)); modes/derivation apply only to HTTP + `--source`.
  The operator picks a mode and uploaders must produce matching keys ("the user has to know").
- **Security:** keys are hashed to opaque digests and files are digest-named
  ([ADR-0014](0014-keyless-digest-identity.md)/[ADR-0009](0009-striping-and-eof.md)), so an
  attacker-controlled `Host`/URI — `..` and all — **cannot escape to the filesystem**; a hostile
  path just hashes to a different digest → miss. Traversal/symlink care applies only to the
  `--source` loader (trusted operator input).

## Consequences
- ➕ Preload-from-directory and HTTP serving line up exactly; one rule; traversal-safe by construction.
- ➖ Mode is server-wide and must match how items were uploaded (intended; documented).
- ➖ Query-string default (strip) is a judgment call — revisit if per-query entries are wanted.
