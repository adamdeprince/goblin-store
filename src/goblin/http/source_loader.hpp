// --source preloading (ADR-0013/0014/0015): walk each source directory tree at startup and store
// every regular file under the key derived from its path relative to the root, using the SAME rule
// as the HTTP front-end (derive_key_from_relpath). So a file at <root>/a/b.html becomes servable at
// GET /a/b.html (path mode) — and, sharing storage, via memcache under the same key. Files stream in
// bounded chunks through the normal write path, so a multi-GiB object never lands wholly in RAM.
#pragma once

#include "goblin/http/key_derivation.hpp"
#include "goblin/storage/tier_manager.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace goblin::http {

// Preload every regular file under each `dirs` root into `tm`. Per-file failures are logged and
// skipped; returns the number of files successfully stored.
std::size_t preload_sources(const std::vector<std::string>& dirs, const KeyOptions& opt,
                            storage::TierManager& tm);

} // namespace goblin::http
