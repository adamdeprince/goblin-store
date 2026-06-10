// Thread-per-core memcache/TCP server (ADR-0001/0018): one worker per core, each with its own
// io_uring reactor, I/O-buffer pool, and SO_REUSEPORT listener; the index + tier manager are shared
// and thread-safe. Serves get/gets/set/add/replace/delete/version/quit. The io_uring multishot
// async loop (many connections per core) layers on later behind this wiring. serve() blocks.
#pragma once

#include "goblin/common/config.hpp"
#include "goblin/common/error.hpp"
#include "goblin/core/reactor.hpp"
#include "goblin/storage/index.hpp"
#include "goblin/storage/tier_manager.hpp"

#include <cstdint>

namespace goblin::memcache {

Status serve(const ServerConfig& cfg, storage::TierManager& tm, storage::Index& index);

} // namespace goblin::memcache
