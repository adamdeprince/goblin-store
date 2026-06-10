# ADR-0014: Keyless internal identity — client keys hashed to a digest

**Status:** Accepted (2026-06-09). The hash algorithm (SHA-256 vs blake3) is revisable — both are
trivially swappable behind one function.

## Context
Clients present arbitrary, variable-length keys (memcache keys ≤ 250 B; HTTP paths). We want
fast key → metadata lookups and we do **not** need key enumeration ("list your keys").

## Decision
- **Keyless identity.** The internal object ID is a **256-bit cryptographic digest** of the
  client key. We do **not** store the original key — the digest *is* the identity. (Dropping
  key-enumeration is what makes this safe to do.)
- **Algorithm: SHA-256, hardware-accelerated, from a vendored implementation** (Bitcoin Core's is
  the quality bar — MIT, lean, with runtime **SHA-NI → AVX2 → SSE4 → scalar** dispatch chosen once
  via CPUID). **Speed matters:** 100 ns ≈ 125 bytes of wire time at 10 GbE and we hash once per
  request, so a SHA-NI path (~4–6× a scalar impl; ~125 ns vs ~500–800 ns for a 250 B key) is a
  *requirement*, not a nicety. Bitcoin's lean path also avoids OpenSSL's **EVP per-call overhead**
  (context alloc + provider fetch), which on tiny inputs can cost more than the hash itself.
  Everything sits behind a one-function `hash_key()` seam, so OpenSSL's SHA-256 (a required dep,
  also SHA-NI) is a 1-line A/B — pick by benchmark on ≤ 250 B inputs. **Vendor the real upstream
  files** (don't retype crypto), gated on NIST test vectors. We hash **once** at protocol parse and
  carry the 32-byte digest through the pipeline (per-core state, no contention). **blake3** stays a
  swappable alternative (its edge is large inputs we don't hash).
- **Why a crypto hash:** because we're keyless, a collision would serve the **wrong object**.
  256-bit ⇒ ~2^128 collision resistance ⇒ impossible in practice. (If we stored + compared keys,
  a fast non-crypto hash like xxh3 would suffice — but keyless is the better fit here.)
- **Index shape ([ADR-0001](0001-thread-per-core-concurrency.md)):** open map
  `digest → ObjectMeta`; bucket = low 64 bits of the digest (already uniform — no secondary hash);
  collisions compared on the full 32 bytes (4× `uint64`). The drive-striping seed
  (`drive_of` key_hash, [ADR-0009](0009-striping-and-eof.md)) and per-object file names derive
  from a slice of the digest.
- **We hash keys, never values.** A ≤ 250 B key hashes in ~100 ns — invisible next to network RTT
  and disk I/O for the large-object workload. Index overhead is 32 B/object — negligible vs
  MB/GB objects.

## Representation (confirmed 2026-06-09)
- **Internal identity is the 32-byte BINARY digest, never hex.** 32 B is half a 64-byte cache
  line, so an index slot (digest + compact metadata) fits in **one cache line** → one miss per
  lookup, compare is 4× `uint64`. Hex (64 B) would fill the line with just the key.
- **Index entry:** `digest → ObjectMeta { size; RAM-head residency = (block index, buddy offset,
  head_len) or "not resident"; eviction bookkeeping }`. The RAM-head locator is the
  `hash → block (+ maybe buddy offset)` mapping into the [ADR-0008](0008-ram-allocator.md) pool.
  Disk location is **not** stored — it's recomputed (hex filename + stripe layout,
  [ADR-0009](0009-striping-and-eof.md)).
- **The filesystem is the on-disk index.** Per-object files are named by the **hex** of the
  digest, so a disk lookup is just `openat(drive_dirfd, hex, …)` — no separate on-disk metadata
  index to maintain or rebuild. Consistent with ephemeral wipe-on-start
  ([ADR-0013](0013-ephemeral-wipe-on-start.md)): the RAM index is authoritative, rebuilt empty +
  repopulated. See [ADR-0009](0009-striping-and-eof.md) for the open path.

## Consequences
- ➕ Fixed-size IDs everywhere; fast fixed-width lookups; on-disk names don't leak keys; no variable-length key storage; no new dependency.
- ➖ Cannot enumerate or recover the original keys (intended).
- ➖ An unkeyed digest lets an attacker who knows the scheme target a single bucket (algorithmic-complexity / bucket-flooding *slowdown* — not a wrong answer). Fine for trusted-network / behind-proxy ([ADR-0005](0005-protocol-surface.md)); seed the bucket function if hardening is ever required.
