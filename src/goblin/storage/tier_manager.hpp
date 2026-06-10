// Tier orchestration (ADR-0006): store an object across the RAM head / SSD prefix / HDD tail,
// and read it back, each disk tier fanned across its pool via the reactor (ADR-0009).
//
// v1 covers the disk store/read pipeline (SSD prefix + HDD tail). The RAM-head cache (serve
// [0,ram_head) from the BlockPool for instant TTFB), cross-tier concurrent prefetch, and
// atomic-publish writes are layered on top later.
#pragma once

#include "goblin/common/config.hpp" // TierSizes, PoolConfig
#include "goblin/common/error.hpp"
#include "goblin/common/types.hpp"
#include "goblin/core/buffer_pool.hpp"
#include "goblin/core/reactor.hpp"
#include "goblin/storage/eviction.hpp"
#include "goblin/crypto/sha256.hpp" // Digest
#include "goblin/storage/drive_pool.hpp"
#include "goblin/storage/index.hpp"
#include "goblin/storage/layout.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <span>
#include <vector>

namespace goblin::storage {

// RAII set of an object's per-drive file descriptors (one per drive holding data; -1 elsewhere).
class ObjectFiles {
public:
    ObjectFiles() = default;
    explicit ObjectFiles(std::vector<int> fds) : fds_(std::move(fds)) {}
    ~ObjectFiles();
    ObjectFiles(ObjectFiles&&) noexcept;
    ObjectFiles& operator=(ObjectFiles&&) noexcept;
    ObjectFiles(const ObjectFiles&) = delete;
    ObjectFiles& operator=(const ObjectFiles&) = delete;
    std::span<const int> fds() const noexcept { return fds_; }

private:
    std::vector<int> fds_;
};

// A storage pool: a DrivePool plus one open O_DIRECTORY fd per drive dir (ADR-0009 openat path).
class Pool {
public:
    static Result<Pool> open(const std::vector<std::string>& dirs, Size stripe_unit);
    ~Pool();
    Pool(Pool&&) noexcept;
    Pool& operator=(Pool&&) noexcept;
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    const DrivePool& drives() const noexcept { return drives_; }
    // Open the per-object files for `digest` covering `tier_bytes` (create => O_RDWR|O_CREAT).
    Result<ObjectFiles> open_object(const Digest& digest, Size tier_bytes, bool create) const;
    void unlink_object(const Digest& digest, Size tier_bytes) const; // remove the per-object files

private:
    Pool(DrivePool dp, std::vector<int> dirfds) : drives_(dp), dirfds_(std::move(dirfds)) {}
    DrivePool drives_;
    std::vector<int> dirfds_;
};

class TierManager {
public:
    static Result<TierManager> open(const TierSizes&, const MemoryConfig&, const EvictionConfig&,
                                    const PoolConfig& ssd, const PoolConfig& hdd, Index& index);
    bool three_layer() const noexcept { return hdd_.has_value(); }

    // Streaming write (ADR-0016/0017): open files + reserve the RAM head once, append chunks with
    // write(), publish with commit(). RAM held during a write = the head + the caller's chunk,
    // never the whole object. An uncommitted handle aborts on destruction (frees the head; the
    // object stays invisible since it was never indexed — orphan files are reclaimed on wipe).
    class StoreHandle {
    public:
        StoreHandle(StoreHandle&&) noexcept;
        StoreHandle& operator=(StoreHandle&&) noexcept;
        StoreHandle(const StoreHandle&) = delete;
        StoreHandle& operator=(const StoreHandle&) = delete;
        ~StoreHandle();

        Status write(ByteView chunk);       // append the next bytes (sequential, from offset 0)
        Status commit(std::uint32_t flags); // publish to the index + cache the head

    private:
        friend class TierManager;
        StoreHandle(TierManager* tm, Digest digest, ObjectLayout layout, ObjectFiles ssd,
                    std::optional<ObjectFiles> hdd, std::optional<core::BufferPool::Region> head);
        TierManager* tm_;
        Digest digest_;
        ObjectLayout layout_;
        ObjectFiles ssd_;
        std::optional<ObjectFiles> hdd_;
        std::optional<core::BufferPool::Region> head_;
        Offset off_ = 0;
        bool committed_ = false;
    };

    Result<StoreHandle> begin_store(const Digest&, Size size);
    Status store(const Digest&, ByteView data, std::uint32_t flags); // begin + write + commit
    Result<std::size_t> read(core::Reactor&, const Digest&, Offset offset, MutBytes out);
    bool remove(const Digest&); // erase from index + free the cached head
    // Resident RAM head bytes for a zero-copy send, or nullopt if not cached (ADR-0017).
    std::optional<ByteView> head_view(const Digest&) const;
    void touch(const Digest&);          // record a cache hit (eviction bookkeeping, ADR-0007)
    std::size_t head_resident() const;  // # heads currently cached in RAM (stats/tests)

    // Head pinning (ADR-0017/0018): keep a resident head's RAM alive while a reader sends it
    // zero-copy. Evicting/overwriting a pinned head defers the RAM free until the last unpin.
    struct HeadPin {
        unsigned block = 0;
        std::uint32_t offset = 0;
        std::uint32_t len = 0;
        bool valid = false;
    };
    std::optional<HeadPin> pin_head(const Digest&); // pin + record the hit; nullopt if not resident
    void unpin_head(const HeadPin&);
    ByteView pinned_bytes(const HeadPin&) const; // zero-copy view of the pinned head region

    // Async read support (ADR-0002): open an object's files once, then plan() each piece — the head
    // is copied into the caller's buffer under the storage lock, and the disk portion is returned as
    // per-drive segments the caller reads on its own io_uring ring. Holds the fds for the read's life.
    class ReadStream {
    public:
        struct Seg {
            int fd;
            Offset file_off;
            Size len;
            Size out_off;
        };
        struct Plan {
            std::size_t total = 0; // bytes this piece delivers (head already copied into out[0..])
            std::vector<Seg> segs; // disk reads to issue (each one drive); empty => all-head, ready now
        };
        Size size() const noexcept { return size_; }
        Plan plan(Offset off, MutBytes out);

    private:
        friend class TierManager;
        ReadStream(TierManager* tm, Digest d, Size size, ObjectLayout layout, ObjectFiles ssd,
                   std::optional<ObjectFiles> hdd)
            : tm_(tm), digest_(d), size_(size), layout_(layout), ssd_(std::move(ssd)),
              hdd_(std::move(hdd)) {}
        TierManager* tm_;
        Digest digest_;
        Size size_;
        ObjectLayout layout_;
        ObjectFiles ssd_;
        std::optional<ObjectFiles> hdd_;
    };
    Result<ReadStream> open_read(const Digest&);

private:
    TierManager(TierSizes t, core::BufferPool ram, std::unique_ptr<EvictionPolicy> head_policy,
                std::unique_ptr<EvictionPolicy> object_policy, std::uint64_t max_objects, Pool ssd,
                std::optional<Pool> hdd, Index& ix)
        : tiers_(t), ram_(std::move(ram)), policy_(std::move(head_policy)),
          object_policy_(std::move(object_policy)), max_objects_(max_objects), ssd_(std::move(ssd)),
          hdd_(std::move(hdd)), index_(&ix), mu_(std::make_unique<std::shared_mutex>()) {}
    void drop_object(const Digest&); // free head + unlink files + erase from index & policies
    void enforce_object_bound();     // evict whole objects while over the count limit
    void free_head_region(unsigned block, std::uint32_t offset, std::uint32_t len); // free or orphan
    static std::uint64_t region_id(unsigned block, std::uint32_t offset) {
        return (static_cast<std::uint64_t>(block) << 32) | offset;
    }

    TierSizes tiers_;
    core::BufferPool ram_;                          // RAM-head cache (ADR-0003/0008)
    std::unique_ptr<EvictionPolicy> policy_;        // resident-head eviction (ADR-0007)
    std::unique_ptr<EvictionPolicy> object_policy_; // whole-object / SSD count bound (ADR-0012)
    std::uint64_t max_objects_;                     // 0 => unbounded
    Pool ssd_;
    std::optional<Pool> hdd_;
    Index* index_;
    std::unique_ptr<std::shared_mutex> mu_; // rwlock: shared for reads, exclusive for writes (ADR-0018)
    struct RegionPin {
        std::uint32_t len = 0;
        unsigned refcount = 0;
        bool orphaned = false;
    };
    std::unordered_map<std::uint64_t, RegionPin> pins_; // pinned head regions (region_id -> state)
};

} // namespace goblin::storage
