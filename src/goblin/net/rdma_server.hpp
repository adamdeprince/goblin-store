// Native reliable-connected RDMA memcache endpoint (Goblin bulk ABI v3).
#pragma once

#include "goblin/common/config.hpp"
#include "goblin/common/error.hpp"

#include <atomic>
#include <functional>

namespace goblin::core { class StatsRegistry; }
namespace goblin::storage { class Index; class TierManager; }

namespace goblin::net {

bool rdma_server_available() noexcept;

// Blocks until shutdown. Each accepted CM id is migrated to its own event channel and served on a
// dedicated progress thread; the registered control/bulk regions and QP are connection-owned.
Status serve_rdma(const RdmaConfig& config, WriteMode write_mode, storage::TierManager& tm,
                  storage::Index& index, core::StatsRegistry& stats,
                  const std::atomic<bool>& shutdown,
                  std::function<void(Status)> startup = {});

} // namespace goblin::net
