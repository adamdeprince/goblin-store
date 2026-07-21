// Thread-per-core memcache/TCP server (ADR-0001/0018): one worker per core, each with its own
// io_uring reactor and I/O-buffer pool. A coordinator accepts each TCP connection and hands its fd
// to one least-loaded worker; that worker owns it end-to-end. The index + tier manager are shared
// and thread-safe. Serves get/gets/set/add/replace/delete/version/quit. serve() blocks.
#pragma once

#include "goblin/common/config.hpp"
#include "goblin/common/error.hpp"
#include "goblin/core/reactor.hpp"
#include "goblin/core/buffer_pool.hpp"
#include "goblin/core/stats.hpp"
#include "goblin/storage/index.hpp"
#include "goblin/storage/tier_manager.hpp"

#include <atomic>
#include <cstdint>

namespace goblin::memcache {

class Authenticator;

// Blocks until `shutdown` is set (e.g. by a SIGTERM/SIGINT handler), then drains in-flight transfers
// and returns. Pass a never-set flag to run forever.
Status serve(const ServerConfig& cfg, storage::TierManager& tm, storage::Index& index,
             std::atomic<bool>& shutdown);

// One already-accepted connection through the synchronous backend. This is also the protocol-test
// seam used to run identical wire transcripts through the blocking and default async handlers.
void serve_blocking_connection(int fd, storage::TierManager& tm, core::Reactor& reactor,
                               core::IoBufferPool& iobufs,
                               WriteMode write_mode = WriteMode::evict,
                               const Authenticator* authenticator = nullptr);

} // namespace goblin::memcache
