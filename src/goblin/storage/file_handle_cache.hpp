// Bounded immutable-object descriptor cache.
//
// Object generations never change after publication, so read-only descriptors are safe to retain.
// Hash-selected CLOCK shards let unrelated reads proceed independently. Hits set a reference bit;
// replacement gives recently used and currently leased descriptors a second chance, while one
// atomic reservation preserves the exact total capacity. Write/create descriptors are never cached.
#pragma once

#include "goblin/common/error.hpp"
#include "goblin/crypto/sha256.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace goblin::storage {

class CachedFileDescriptor {
public:
    explicit CachedFileDescriptor(int fd) noexcept : fd_(fd) {}
    ~CachedFileDescriptor();
    CachedFileDescriptor(const CachedFileDescriptor&) = delete;
    CachedFileDescriptor& operator=(const CachedFileDescriptor&) = delete;
    int get() const noexcept { return fd_; }

private:
    int fd_ = -1;
};

class FileHandleCache {
public:
    struct Key {
        crypto::Digest digest;
        std::uint64_t generation = 0;
        int directory_fd = -1;

        bool operator==(const Key&) const noexcept = default;
    };

    struct Stats {
        std::size_t capacity = 0;
        std::size_t shards = 0;
        std::size_t cached = 0;
        std::size_t in_use = 0;
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t evictions = 0;
        std::uint64_t bypasses = 0;
        std::uint64_t invalidations = 0;
    };

    static Result<std::shared_ptr<FileHandleCache>> create(std::size_t capacity);
    ~FileHandleCache();
    FileHandleCache(const FileHandleCache&) = delete;
    FileHandleCache& operator=(const FileHandleCache&) = delete;

    Result<std::shared_ptr<CachedFileDescriptor>> open_read(
        const Key& key, const char* name, int flags, int* failed_errno = nullptr);
    void invalidate(const Key& key) noexcept;
    Stats stats() const noexcept;

private:
    struct Impl;
    explicit FileHandleCache(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace goblin::storage
