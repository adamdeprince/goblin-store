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
- **Algorithm: SHA-256** behind the one-function `hash_key()` seam, gated on NIST and padding-boundary
  vectors. Runtime policy is selected once: x86 SHA-NI when CPUID reports it; on pre-SHA-NI x86,
  the unrolled scalar transform through 183 input bytes and OpenSSL for longer inputs when linked;
  scalar on LoongArch; otherwise OpenSSL when linked or the portable scalar fallback. The 183-byte
  cutoff is the last input that fits in three SHA-256 compression blocks including padding. On the
  Xeon E5-4657L v2 test rig, scalar wins decisively through that boundary; the fourth block begins
  at 184 bytes, where OpenSSL catches it. **Speed matters:** 100 ns ≈ 125 bytes of wire time at
  10 GbE, so benchmark backends on actual ≤ 250 B keys rather than assuming their large-buffer
  throughput applies. We hash **once** at protocol parse and carry the 32-byte digest through the
  pipeline (per-core state, no contention). **blake3** stays a swappable alternative (its edge is
  large inputs we don't hash).
- **Why a crypto hash:** because we're keyless, a collision would serve the **wrong object**.
  256-bit ⇒ ~2^128 collision resistance ⇒ impossible in practice. (If we stored + compared keys,
  a fast non-crypto hash like xxh3 would suffice — but keyless is the better fit here.)
- **Index shape ([ADR-0001](0001-thread-per-core-concurrency.md)):** sharded `unordered_map`
  `digest → ObjectMeta`; the map hash is a 64-bit slice of the digest (already uniform — no
  secondary hash), while collisions compare the full 32 bytes. The drive-striping seed
  (`drive_of` key_hash, [ADR-0009](0009-striping-and-eof.md)) and the digest portion of immutable
  generation file names derive from a slice of the digest.
- **We hash keys, never values.** Cost depends on the selected backend. On the pre-SHA-NI R820,
  the measured short scalar path was roughly 420 ns and the prior OpenSSL one-shot path roughly
  920 ns. That is small beside disk I/O and ordinary network RTT, but material for RAM-resident
  small objects. The identity payload is 32 B/object before `unordered_map`, metadata, and allocator
  overhead — negligible vs MB/GB objects.

## Representation (confirmed 2026-06-09)
- **Internal identity is the 32-byte BINARY digest, never hex.** Equality is a fixed-width 32-byte
  comparison, and the map hash uses the digest's low 64 bits. Hex would double the identity payload
  to 64 B; it is generated only for filesystem names. The current node-based `unordered_map` entry
  also contains `ObjectMeta`, an atomic score, and container overhead, so it is not a one-cache-line
  slot.
- **Index entry:** `digest → ObjectMeta { size; file_generation; RAM-head residency = (block index,
  buddy offset, head_len) or "not resident"; eviction bookkeeping }`. The RAM-head locator is the
  `hash → block (+ maybe buddy offset)` mapping into the [ADR-0008](0008-ram-allocator.md) pool.
  The live generation number is stored because old and concurrent incarnations can coexist; the
  drive set and per-shard offsets are recomputed from the digest, logical size, and stripe layout
  ([ADR-0009](0009-striping-and-eof.md)).
- **There is no persistent on-disk metadata index.** Each shard name is
  `<64-hex-digest>.g<file_generation>`, so a lookup from the authoritative RAM Index is one
  `openat(drive_dirfd, generation_name, …)` per used drive. Wipe-on-start
  ([ADR-0013](0013-ephemeral-wipe-on-start.md)) removes every generation and rebuilds the RAM Index
  empty. See [ADR-0009](0009-striping-and-eof.md) for the open and publication path.

## Consequences
- ➕ Fixed-size IDs everywhere; fast fixed-width lookups; on-disk names don't leak keys; no variable-length key storage; no new dependency.
- ➖ Cannot enumerate or recover the original keys (intended).
- ➖ An unkeyed digest lets an attacker who knows the scheme target a single bucket (algorithmic-complexity / bucket-flooding *slowdown* — not a wrong answer). Fine for trusted-network / behind-proxy ([ADR-0005](0005-protocol-surface.md)); seed the bucket function if hardening is ever required.
