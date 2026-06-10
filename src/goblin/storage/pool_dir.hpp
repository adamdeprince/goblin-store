// Pool-directory safety (ADR-0013): ephemeral, wipe-on-start, but only ever inside a directory
// that was explicitly blessed with a `.goblin-store-marker`. The destructive `wipe_pool_dir`
// refuses any directory lacking the marker, so a mis-typed --ssd-dir can never erase /var.
#pragma once

#include "goblin/common/error.hpp"

#include <string>
#include <string_view>

namespace goblin::storage {

inline constexpr std::string_view kMarkerName = ".goblin-store-marker";

// Bless `dir` as a Goblin pool store (the goblin-store-path-prep tool): `mkdir -p`, then require
// the directory to be empty, then write the marker. Returns Errc::already_exists if it contains
// anything — we never mark a non-empty directory.
Status prepare_pool_dir(const std::string& dir);

// True iff `dir` exists and contains a valid marker file.
Result<bool> has_marker(const std::string& dir);

// Wipe `dir` to a blank slate for startup. REQUIRES the marker — without it we abort
// (Errc::invalid_argument) rather than risk erasing an unblessed directory. The marker itself
// is preserved so the next restart still recognises the directory.
Status wipe_pool_dir(const std::string& dir);

} // namespace goblin::storage
