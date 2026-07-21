#pragma once

#include "goblin/core/stats.hpp"

#include <string>

namespace goblin::storage { class TierManager; }

namespace goblin::memcache {

std::string format_stats_response(storage::TierManager&, const core::StatsSnapshot&,
                                  std::uint64_t uptime, bool detailed = false);
std::string format_settings_response(const core::StatsSettings&);

} // namespace goblin::memcache
