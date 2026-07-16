# ADR-0004: C++23 toolchain & build

**Status:** Accepted (2026-06-09).

## Context
Observed dev box (2026-06-09): **GCC 16.1** (system `g++`), `g++-15` present, **CMake 3.31**,
**Ninja 1.13**, **kernel 6.17**; clang absent. `std::expected`, `std::print`, and
`std::format` all confirmed working under `-std=c++23`. **liburing not installed.**

## Decision
- **Language:** C++23. **Compiler floor:** GCC 14+ (or Clang 18+); we develop on GCC 16.
- **Build:** CMake ≥ 3.28, Ninja generator. Dev builds use `-Wall -Wextra -Wpedantic -Werror`
  and ASan/UBSan in `Debug`.
- **Headers only for now — C++ modules OFF.** Module/toolchain maturity isn't worth the build
  risk yet.
- **Dependencies:** **OpenSSL — required** (`libssl-dev`; TLS for the runtime-optional HTTPS
  listener, [ADR-0005](0005-protocol-surface.md), and the long-input SHA-256 fallback where selected;
  short-key scalar and hardware SHA implementations are vendored,
  [ADR-0014](0014-keyless-digest-identity.md)). **liburing — required for the io_uring
  backend** (`liburing-dev`; detected via `pkg-config`, gated by `GOBLIN_HAVE_URING`; the
  pure-logic layers build without it, but the server can't serve). HTTP/2,3 later: nghttp2 /
  ngtcp2 + nghttp3.
- **C++23 features we use now:** `std::expected` (hot-path error returns, no exceptions),
  `std::span`, `std::print`/`std::format`, `if consteval` + `constexpr` (compile-time
  command table). **Defer** `std::generator`, `std::flat_map`, `std::mdspan` until measured.

## Consequences
- ➕ Modern, exception-light error handling; fast iteration with Ninja.
- ➖ The GCC-14 floor excludes some older LTS distros — acceptable; document the install.
