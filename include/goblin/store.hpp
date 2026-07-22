// Embedded Goblin Store storage API.
//
// This is the protocol-free boundary for C++ applications that want Goblin's RAM-head / disk-tail
// engine without TCP, HTTP, memcache framing, or a child server process.  Stores are shared and
// thread-safe.  Readers are deliberately per-thread: each owns its aligned scratch buffer and, on
// Linux builds where it is available, its own io_uring.
#pragma once

#include "goblin/common/config.hpp"
#include "goblin/common/error.hpp"
#include "goblin/common/types.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace goblin {

struct StoreOptions {
    TierSizes tiers;
    MemoryConfig memory;
    PoolConfig ssd;
    PoolConfig hdd;
    EvictionConfig eviction;
    AccessScoreConfig access_score;

    Size read_chunk_bytes = 256 * KiB;
    Size write_chunk_bytes = 256 * KiB;
    unsigned file_handle_cache = 128;
    unsigned write_buffers = 8;
    Size max_object_size = kMaxObjectSize;
    bool direct_io = true;
};

struct PutOptions {
    std::uint32_t flags = 0;
    // Absolute Unix time. Zero means no expiration.
    std::uint32_t expiry = 0;
    WriteMode write_mode = WriteMode::evict;
};

struct ObjectInfo {
    Size size = 0;
    std::uint32_t flags = 0;
    std::uint32_t expiry = 0;
    std::uint64_t etag = 0;
};

struct LoadedObject {
    ObjectInfo info;
    // Borrowed from the StoreReader's retained, grow-only materialization buffer. The bytes remain
    // valid until the next load() on that reader, or until the reader is moved or destroyed.
    ByteView bytes;
};

class Store;

class StoreWriter {
public:
    StoreWriter(StoreWriter&&) noexcept;
    StoreWriter& operator=(StoreWriter&&) noexcept;
    ~StoreWriter();
    StoreWriter(const StoreWriter&) = delete;
    StoreWriter& operator=(const StoreWriter&) = delete;

    Status write(ByteView bytes);
    Result<ObjectInfo> commit();

private:
    struct Impl;
    explicit StoreWriter(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
    friend class Store;
};

class StoreReader {
public:
    // A chunk remains valid only for the duration of the callback. Returning an error stops the
    // read and passes that error to the caller. For a resident object, the first callback contains
    // the RAM head and runs before the reader waits for any disk-tail bytes.
    using ChunkSink = std::function<Status(ByteView)>;

    StoreReader(StoreReader&&) noexcept;
    StoreReader& operator=(StoreReader&&) noexcept;
    ~StoreReader();
    StoreReader(const StoreReader&) = delete;
    StoreReader& operator=(const StoreReader&) = delete;

    Result<ObjectInfo> stream(std::string_view key, const ChunkSink& sink,
                              bool record_access = true);
    // Materialize into a per-reader aligned buffer. Capacity starts at zero, grows only when the
    // encountered object does not fit, and is retained between calls. Disk I/O targets the final
    // buffer directly; `progress` observes completed pieces in-place and may be used for TTFB.
    Result<LoadedObject> load(std::string_view key, const ChunkSink& progress = {},
                              bool record_access = true);
    Size materialization_capacity() const noexcept;
    Result<std::vector<std::byte>> get(std::string_view key, bool record_access = true);
    bool uses_io_uring() const noexcept;

private:
    struct Impl;
    explicit StoreReader(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
    friend class Store;
};

class Store {
public:
    // The directory must be empty. This writes Goblin's safety marker; open() will only wipe a
    // marked directory, matching the server's protection against destructive path mistakes.
    static Status prepare_directory(const std::string& path);

    // Opens a new empty store and removes old object files from every marked pool directory.
    static Result<Store> open(StoreOptions options);

    Result<StoreWriter> begin_put(std::string_view key, Size size,
                                  PutOptions options = {});
    Status put(std::string_view key, ByteView value, PutOptions options = {});
    Result<ObjectInfo> info(std::string_view key) const;
    bool erase(std::string_view key);
    void flush_all(std::uint32_t delay_seconds = 0);

    // A reader is single-threaded and reusable. Create one per application worker.
    Result<StoreReader> make_reader(Size chunk_bytes = 0,
                                    unsigned io_uring_entries = 256) const;

private:
    struct Impl;
    explicit Store(std::shared_ptr<Impl>) noexcept;
    std::shared_ptr<Impl> impl_;
    friend class StoreWriter;
    friend class StoreReader;
};

} // namespace goblin
