#include "goblin/storage/file_handle_cache.hpp"

#include "goblin/common/types.hpp"

#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <mutex>
#include <new>
#include <optional>
#include <unordered_map>
#include <unistd.h>
#include <utility>
#include <vector>

namespace goblin::storage {

CachedFileDescriptor::~CachedFileDescriptor() {
    if (fd_ >= 0) ::close(fd_);
}

namespace {

struct KeyHash {
    std::size_t operator()(const FileHandleCache::Key& key) const noexcept {
        std::uint64_t value = key.digest.bucket();
        value ^= key.generation + UINT64_C(0x9e3779b97f4a7c15) + (value << 6) + (value >> 2);
        value ^= static_cast<std::uint32_t>(key.directory_fd) +
            UINT64_C(0x9e3779b97f4a7c15) + (value << 6) + (value >> 2);
        return static_cast<std::size_t>(value);
    }
};

} // namespace

struct FileHandleCache::Impl {
    struct Slot {
        Key key;
        std::shared_ptr<CachedFileDescriptor> descriptor;
        bool referenced = false;
    };

    struct alignas(64) Shard {
        mutable std::mutex mutex;
        std::vector<Slot> slots;
        std::unordered_map<Key, std::size_t, KeyHash> index;
        std::size_t hand = 0;
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t evictions = 0;
        std::uint64_t bypasses = 0;
        std::uint64_t invalidations = 0;
    };

    explicit Impl(std::size_t requested) : capacity(requested) {
        // Retain useful CLOCK replacement sets in small caches while giving large caches enough
        // independent locks for all storage workers. Slots grow dynamically, so uneven key hashes
        // cannot strand capacity in another shard.
        std::size_t count = 1;
        while (count < 64 && count * 16 < requested) count *= 2;
        shard_mask = count - 1;
        shards.reserve(count);
        const std::size_t expected = (requested + count - 1) / count;
        for (std::size_t i = 0; i < count; ++i) {
            auto shard = std::make_unique<Shard>();
            shard->slots.reserve(expected);
            shard->index.reserve(expected);
            shards.push_back(std::move(shard));
        }
    }

    Shard& shard_for(const Key& key) noexcept {
        return *shards[KeyHash{}(key) & shard_mask];
    }

    bool reserve_slot() noexcept {
        std::size_t used = cached.load(std::memory_order_relaxed);
        while (used < capacity) {
            if (cached.compare_exchange_weak(used, used + 1, std::memory_order_acq_rel,
                                             std::memory_order_relaxed))
                return true;
        }
        return false;
    }

    const std::size_t capacity;
    std::atomic<std::size_t> cached{0};
    std::vector<std::unique_ptr<Shard>> shards;
    std::size_t shard_mask = 0;
};

FileHandleCache::FileHandleCache(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
FileHandleCache::~FileHandleCache() = default;

Result<std::shared_ptr<FileHandleCache>> FileHandleCache::create(std::size_t capacity) try {
    if (capacity == 0 || !is_power_of_two(capacity))
        return err(Errc::invalid_argument,
                   "file-handle cache capacity must be a nonzero power of two");
    auto impl = std::make_unique<Impl>(capacity);
    return std::shared_ptr<FileHandleCache>(new FileHandleCache(std::move(impl)));
} catch (const std::bad_alloc&) {
    return err(Errc::out_of_memory, "allocate file-handle cache");
}

Result<std::shared_ptr<CachedFileDescriptor>> FileHandleCache::open_read(
    const Key& key, const char* name, int flags, int* failed_errno) {
    if (failed_errno) *failed_errno = 0;
    auto& shard = impl_->shard_for(key);
    {
        std::lock_guard<std::mutex> lock(shard.mutex);
        const auto found = shard.index.find(key);
        if (found != shard.index.end()) {
            auto& slot = shard.slots[found->second];
            slot.referenced = true;
            ++shard.hits;
            return slot.descriptor;
        }
        ++shard.misses;
    }

    const int fd = ::openat(key.directory_fd, name, flags);
    if (fd < 0) {
        if (failed_errno) *failed_errno = errno;
        return err(Errc::io_error, "openat object file");
    }
    std::shared_ptr<CachedFileDescriptor> candidate;
    try {
        candidate = std::make_shared<CachedFileDescriptor>(fd);
    } catch (const std::bad_alloc&) {
        ::close(fd);
        return err(Errc::out_of_memory, "allocate cached file descriptor");
    }

    bool reserved = false;
    try {
        std::lock_guard<std::mutex> lock(shard.mutex);
        // A concurrent miss may have installed the same immutable generation while openat ran.
        const auto raced = shard.index.find(key);
        if (raced != shard.index.end()) {
            auto& slot = shard.slots[raced->second];
            slot.referenced = true;
            ++shard.hits;
            return slot.descriptor;
        }

        std::optional<std::size_t> selected;
        reserved = impl_->reserve_slot();
        if (reserved) {
            // Invalidations leave reusable holes. Otherwise this shard may grow past its average
            // share, while the atomic reservation keeps the process-wide capacity exact.
            for (std::size_t i = 0; i < shard.slots.size(); ++i) {
                if (!shard.slots[i].descriptor) {
                    selected = i;
                    break;
                }
            }
            if (!selected) {
                shard.slots.emplace_back();
                selected = shard.slots.size() - 1;
            }
        } else {
            const std::size_t scan_limit = shard.slots.size() * 2;
            for (std::size_t scanned = 0; scanned < scan_limit; ++scanned) {
                const std::size_t index = shard.hand++ % shard.slots.size();
                auto& slot = shard.slots[index];
                if (!slot.descriptor) continue;
                // A leased descriptor is never closed or displaced. It becomes eligible after
                // every ObjectFiles snapshot using it has gone away.
                if (slot.descriptor.use_count() > 1) continue;
                if (slot.referenced) {
                    slot.referenced = false;
                    continue;
                }
                selected = index;
                break;
            }
            if (!selected) {
                ++shard.bypasses;
                return candidate; // this shard's entries are active; close after this lease
            }
        }

        auto& slot = shard.slots[*selected];
        shard.index.emplace(key, *selected); // allocation may fail before either slot is changed
        if (!reserved) {
            shard.index.erase(slot.key);
            slot.descriptor.reset();
            ++shard.evictions;
        }
        slot.key = key;
        slot.descriptor = candidate;
        slot.referenced = true;
        return candidate;
    } catch (const std::bad_alloc&) {
        if (reserved) impl_->cached.fetch_sub(1, std::memory_order_release);
        std::lock_guard<std::mutex> lock(shard.mutex);
        ++shard.bypasses;
        return candidate;
    }
}

void FileHandleCache::invalidate(const Key& key) noexcept {
    auto& shard = impl_->shard_for(key);
    std::lock_guard<std::mutex> lock(shard.mutex);
    const auto found = shard.index.find(key);
    if (found == shard.index.end()) return;
    auto& slot = shard.slots[found->second];
    shard.index.erase(found);
    slot.descriptor.reset(); // an active ObjectFiles lease keeps the unlinked inode alive
    slot.referenced = false;
    impl_->cached.fetch_sub(1, std::memory_order_release);
    ++shard.invalidations;
}

FileHandleCache::Stats FileHandleCache::stats() const noexcept {
    Stats result;
    result.capacity = impl_->capacity;
    result.shards = impl_->shards.size();
    for (const auto& pointer : impl_->shards) {
        const auto& shard = *pointer;
        std::lock_guard<std::mutex> lock(shard.mutex);
        result.hits += shard.hits;
        result.misses += shard.misses;
        result.evictions += shard.evictions;
        result.bypasses += shard.bypasses;
        result.invalidations += shard.invalidations;
        for (const auto& slot : shard.slots) {
            if (!slot.descriptor) continue;
            ++result.cached;
            if (slot.descriptor.use_count() > 1) ++result.in_use;
        }
    }
    return result;
}

} // namespace goblin::storage
