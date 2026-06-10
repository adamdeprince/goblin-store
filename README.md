# Goblin Store (`goblincache`)

A pure **C++23, Linux-only** storage-oriented **large-object** key/value cache & HTTP object
server. It keeps the latency-sensitive *head* of each object in a fixed, `mlock`'d RAM budget,
streams the middle from SSD, and (optionally) hoards the cold tail on cheap HDDs — handing
clients the first bytes immediately while the rest is dragged out of the cave behind them.

Positioned against memcached extstore: faster time-to-first-byte, equal/greater throughput,
and — in 3-layer mode — much lower cost per stored GB.

> **Status: design + scaffold.** The architecture is captured in [`docs/adr/`](docs/adr/).
> The storage engine, reactor, and protocol front-ends are being built out. Today the binary
> parses & validates configuration (enforcing the ADR invariants) and prints the resolved setup.

## Design
Start at [`docs/adr/README.md`](docs/adr/README.md) — 12 ADRs covering the thread-per-core
concurrency model, io_uring I/O, O_DIRECT backing-store ownership, positional tiering,
multi-resource eviction, drive-pool striping, and the protocol surface (memcache over TCP +
an HTTP object path; **no UDP, no built-in auth**).

## Dependencies

**Build (all required unless noted):**
- **C++23 compiler** — GCC ≥ 14 (developed on GCC 16) or Clang ≥ 18
- **CMake ≥ 3.28** + **Ninja**
- **OpenSSL** (`libssl-dev`) — TLS for the HTTPS listener. Enabling HTTPS is a *runtime* choice
  (HTTP / HTTPS / both / memcache-only), but the library is always linked. *(The internal
  key-digest SHA-256 is vendored, not from OpenSSL — the storage path never links TLS; ADR-0014.)*
- **liburing** (`liburing-dev`) — the io_uring storage/serving backend (ADR-0002), detected via
  `pkg-config`. If absent the build still configures and the pure-logic layers compile, but the
  reactor is stubbed (`GOBLIN_HAVE_URING=0`) and the server cannot serve.
- *Later (HTTP/2, HTTP/3):* nghttp2, ngtcp2 + nghttp3 — deferred (ADR-0005).

**Runtime:** Linux kernel ≥ 5.19 (io_uring multishot; dev box runs 6.17).

On Debian/Ubuntu:
```sh
sudo apt-get install -y build-essential cmake ninja-build libssl-dev liburing-dev
```

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/goblincache --help
```

## Layout
```
src/goblin/common/    types, error (std::expected), config + validation
src/goblin/core/      RAM block/buddy allocator, io_uring reactor, thread-per-core runtime
src/goblin/storage/   positional layout, drive-pool striping, index, tiering, eviction
src/goblin/protocol/  memcache (TCP) + HTTP front-ends
tests/                dependency-free unit tests
docs/adr/             architecture decision records
```
*(Not all directories are populated yet — this is an in-progress scaffold.)*
