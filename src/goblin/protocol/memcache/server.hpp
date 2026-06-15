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

#include <atomic>
#include <cstdint>

namespace goblin::memcache {

// Blocks until `shutdown` is set (e.g. by a SIGTERM/SIGINT handler), then drains in-flight transfers
// and returns. Pass a never-set flag to run forever.
Status serve(const ServerConfig& cfg, storage::TierManager& tm, storage::Index& index,
             const std::atomic<bool>& shutdown);

} // namespace goblin::memcache
