// Striped scatter/gather over a DrivePool (ADR-0009). The read path fans one io_uring read per
// chunk across all drives, submitted together, so a single object read uses the whole pool's
// bandwidth. This is the core the tier manager's read pipeline is built on.
#pragma once

#include "goblin/common/error.hpp"
#include "goblin/common/types.hpp"
#include "goblin/core/reactor.hpp"
#include "goblin/storage/drive_pool.hpp"

#include <span>

namespace goblin::storage {

// Scatter [offset, offset+data.size()) across the pool's per-drive files (synchronous pwrite).
// Used to lay objects down (bootstrap / --source loader); the async write path layers on later.
// `fds[i]` is an open, writable fd for drive i's per-object file.
Status striped_pwrite(const DrivePool& pool, std::uint64_t key_hash, std::span<const int> fds,
                      Offset offset, ByteView data, unsigned* failed_drive = nullptr,
                      int* failed_errno = nullptr);

// Gather [offset, offset+out.size()): submits one read per chunk across all drives on the reactor,
// all at once, so the whole pool reads in parallel. Returns total bytes read.
Result<std::size_t> striped_read(core::Reactor& reactor, const DrivePool& pool,
                                 std::uint64_t key_hash, std::span<const int> fds, Offset offset,
                                 MutBytes out);

} // namespace goblin::storage
