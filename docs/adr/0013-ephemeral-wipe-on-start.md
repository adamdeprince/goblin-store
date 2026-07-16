# ADR-0013: Ephemeral by design — wipe-on-start, no persistence (v1)

**Status:** Accepted (2026-06-09).

## Context
[ADR-0003](0003-cache-semantics-head-as-cache.md) already makes Goblin a best-effort cache (no
fsync). Operator decision (2026-06-09): **v1 is fully ephemeral** — no persistence across
restarts or crashes. Every start is a blank slate; old object files and partials are erased.
Durable/recoverable operation is deferred to a future "professional" version.

## Decision
- **No persistence.** The object index lives only in RAM; it is never written to disk nor
  rebuilt from disk. After a restart the cache is empty.
- **Marker-gated wipe.** A pool directory is wiped to a blank slate at startup **only if it
  contains a `.goblin-store-marker` file**. If the marker is absent, Goblin **aborts and refuses
  to start** — so a fat-fingered `--ssd-dir /var` can never erase anything. The wipe removes all
  immutable `<digest>.g<generation>` files, including unpublished generations left by a crash, but
  **preserves the marker** (otherwise the next restart would abort).
- **A separate one-time prep tool blesses directories:** `goblin-store-path-prep /a/b/c ...`
  - `mkdir -p` the path (or confirm it already exists), then
  - **refuse if the directory contains anything** (it will only mark an empty / freshly-created
    dir), and finally write the `.goblin-store-marker`.
  - Because you can only bless an *empty* directory, the marker can never legitimately appear in
    `/var`, `$HOME`, etc.; the destructive power (wipe-on-start) is gated on a marker the prep
    tool refuses to place anywhere non-empty.
  - *(Re-running prep on an already-blessed dir fails — it now contains the marker, so it's
    non-empty. An idempotent "already prepared → ok" can be added later if wanted.)*
- **Synchronous blank slate.** Startup removes every entry except the marker with `remove_all`
  before the pools are opened. This is simple and deterministic, but a directory containing many
  generation files can make startup wait for their removal. A rename-to-trash/background-unlink
  path remains a possible optimization.

## Consequences
- ➕ No crash-recovery, no index persistence/rebuild, no durability bookkeeping — a large v1
  simplification. Unpublished or retired generation files left by a crash
  ([ADR-0010](0010-write-admission-modes.md)) are cleaned by the wipe.
- ➖ Cold start is always empty (no warm cache after a restart) — acceptable for a cache; the "professional" version adds persistence/recovery.
- ➖ Operators must run `goblin-store-path-prep` on scratch dirs first; the marker gate plus the prep tool's empty-only rule prevent collateral deletion.
