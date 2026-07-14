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
#include "goblin/storage/numa_head_scores.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <atomic>
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
    static Result<Pool> open(const std::vector<std::string>& dirs, Size stripe_unit,
                             bool direct_io = false);
    ~Pool();
    Pool(Pool&&) noexcept;
    Pool& operator=(Pool&&) noexcept;
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    const DrivePool& drives() const noexcept { return drives_; }
    // Open the per-object files for `digest` covering `tier_bytes`. A non-empty `name_suffix`
    // (e.g. ".tmp.N") opens copy-on-write scratch files; "" opens the live files (ADR-0018).
    Result<ObjectFiles> open_object(const Digest& digest, Size tier_bytes, bool create,
                                    std::string_view name_suffix = "") const;
    // Atomically rename the `name_suffix` scratch files over the live names (per-drive rename()).
    Status publish(const Digest& digest, Size tier_bytes, std::string_view name_suffix) const;
    void unlink_object(const Digest& digest, Size tier_bytes,
                       std::string_view name_suffix = "") const; // remove the per-object files

private:
    Pool(DrivePool dp, std::vector<int> dirfds, bool direct_io)
        : drives_(dp), dirfds_(std::move(dirfds)), direct_io_(direct_io) {}
    DrivePool drives_;
    std::vector<int> dirfds_;
    bool direct_io_ = false; // open object files with O_DIRECT (ADR-0011)
};

class TierManager {
public:
    static Result<TierManager> open(const TierSizes&, const MemoryConfig&, const EvictionConfig&,
                                    const PoolConfig& ssd, const PoolConfig& hdd, Index& index,
                                    Size io_chunk = 256 * KiB, unsigned write_buffers = 8,
                                    bool direct_io = false,
                                    AccessScoreConfig access_score = {});
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
        // publish to the index + cache the head. expiry is an absolute Unix time (0 = never, ADR-0007).
        // cas_expected != 0 -> compare-and-swap: publish only if the live object's etag matches, else
        // return Errc::cas_mismatch (checked under the publish lock; the scratch files just abort).
        Status commit(std::uint32_t flags, std::uint32_t expiry = 0, std::uint64_t cas_expected = 0);

    private:
        friend class TierManager;
        StoreHandle(TierManager* tm, Digest digest, ObjectLayout layout, ObjectFiles ssd,
                    std::optional<ObjectFiles> hdd, std::optional<core::BufferPool::Region> head,
                    std::string suffix, MutBytes stage);
        void abort_uncommitted(); // free the reserved head + unlink scratch files (never published)
        Status flush_block(Size n); // write the staged block (split SSD/HDD) at flushed_; 4 KiB-sized
        TierManager* tm_;
        Digest digest_;
        ObjectLayout layout_;
        ObjectFiles ssd_;
        std::optional<ObjectFiles> hdd_;
        std::optional<core::BufferPool::Region> head_;
        std::string suffix_; // CoW scratch-file suffix; renamed onto the live names at commit
        MutBytes stage_;      // borrowed aligned staging buffer (from tm_->write_pool_)
        Size stage_fill_ = 0; // bytes accumulated in stage_, not yet flushed to disk
        Size flushed_ = 0;    // bytes already written to disk (a 4 KiB-aligned prefix)
        Offset off_ = 0;
        bool committed_ = false;
    };

    Result<StoreHandle> begin_store(const Digest&, Size size);
    Status store(const Digest&, ByteView data, std::uint32_t flags, std::uint32_t expiry = 0); // begin+write+commit
    std::size_t reap_expired(); // drop objects past their TTL; returns count (O(1) when no TTLs are set)
    bool touch_ttl(const Digest&, std::uint32_t expiry); // meta T: overwrite the TTL; false if absent
    Result<std::size_t> read(core::Reactor&, const Digest&, Offset offset, MutBytes out,
                             bool record_access = true);
    bool remove(const Digest&); // erase from index + free the cached head
    // Resident RAM head bytes for a zero-copy send, or nullopt if not cached (ADR-0017).
    std::optional<ByteView> head_view(const Digest&);
    void touch(const Digest&);          // record one logical read: eviction touch + score increment
    std::size_t head_resident() const;  // # heads currently cached in RAM (stats/tests)

    void decay_access_scores();         // multiply every key score by --decay
    bool promote_hot_remote_block();    // one local/foreign full-block exchange, or false if sorted
    struct NumaPromotionStats {
        std::uint64_t count = 0;
        std::uint64_t bytes_moved = 0;
        std::uint64_t total_ns = 0;
        std::uint64_t max_ns = 0;
    };
    NumaPromotionStats numa_promotion_stats() const noexcept;

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

    // A consistent read snapshot (ADR-0018): the object's metadata, a pinned RAM head (if resident),
    // and the open disk files (if a tail remains) -- all captured under one lock so a concurrent
    // copy-on-write replace can't split head from body. The open fds pin the old inodes.
    struct Snapshot {
        ObjectMeta meta;
        HeadPin pin;                  // pin.valid == false when the head isn't resident
        std::optional<ReadStream> rs; // present iff size > head_len (a disk tail to stream)
    };
    // record_access=false is used only when an internally backpressured GET re-takes its snapshot;
    // one logical request must not gain score repeatedly while it waits for an I/O buffer.
    std::optional<Snapshot> open_snapshot(const Digest&, bool record_access = true);

private:
    struct ScoreMaintenanceGate {
        std::mutex operation;
        std::atomic<bool> rescore_pending{false};
    };

    TierManager(TierSizes t, core::BufferPool ram, std::unique_ptr<EvictionPolicy> head_policy,
                std::unique_ptr<EvictionPolicy> object_policy, std::uint64_t max_objects, Pool ssd,
                std::optional<Pool> hdd, Index& ix, Size small_min_alloc,
                AccessScoreConfig access_score,
                std::unique_ptr<NumaHeadScoreTable> head_scores)
        : tiers_(t), access_score_(access_score), small_min_alloc_(small_min_alloc),
          ram_(std::move(ram)), policy_(std::move(head_policy)),
          object_policy_(std::move(object_policy)), max_objects_(max_objects), ssd_(std::move(ssd)),
          hdd_(std::move(hdd)), index_(&ix),
          head_scores_(std::move(head_scores)),
          store_seq_(std::make_unique<std::atomic<std::uint64_t>>(0)),
          etag_seq_(std::make_unique<std::atomic<std::uint64_t>>(0)),
          any_ttl_(std::make_unique<std::atomic<bool>>(false)),
          mu_(std::make_unique<std::shared_mutex>()),
          score_maintenance_(std::make_unique<ScoreMaintenanceGate>()),
          numa_scores_healthy_(std::make_unique<std::atomic<bool>>(true)),
          numa_promotion_count_(std::make_unique<std::atomic<std::uint64_t>>(0)),
          numa_promotion_bytes_(std::make_unique<std::atomic<std::uint64_t>>(0)),
          numa_promotion_total_ns_(std::make_unique<std::atomic<std::uint64_t>>(0)),
          numa_promotion_max_ns_(std::make_unique<std::atomic<std::uint64_t>>(0)) {}
    void drop_object(const Digest&); // free head + unlink files + erase from index & policies
    void enforce_object_bound();     // evict whole objects while over the count limit
    void free_head_region(unsigned block, std::uint32_t offset, std::uint32_t len); // free or orphan
    void compact_small(); // slide live heads down within fragmented small arenas; reclaim dead space (ADR-0008)
    // Caller holds mu_ exclusively. Keep the canonical key score and its NUMA-local fixed-head
    // slot in step; headless and fractional-arena objects have no dense slot.
    void record_access_locked(const Digest&, const ObjectMeta&);
    void disable_numa_scores(const Error&); // log once and prevent unsafe score-based swaps
    static std::uint64_t region_id(unsigned block, std::uint32_t offset) {
        return (static_cast<std::uint64_t>(block) << 32) | offset;
    }

    TierSizes tiers_;
    AccessScoreConfig access_score_;
    Size small_min_alloc_;                           // buddy min-order for RAM-only heads (ADR-0008-rev)
    core::BufferPool ram_;                           // RAM-head cache (ADR-0003/0008)
    std::unique_ptr<core::IoBufferPool> write_pool_; // bounded aligned CoW write staging (ADR-0011)
    std::unique_ptr<EvictionPolicy> policy_;        // resident-head eviction (ADR-0007)
    std::unique_ptr<EvictionPolicy> object_policy_; // whole-object / SSD count bound (ADR-0012)
    std::uint64_t max_objects_;                     // 0 => unbounded
    Pool ssd_;
    std::optional<Pool> hdd_;
    Index* index_;
    std::unique_ptr<NumaHeadScoreTable> head_scores_;
    std::unique_ptr<std::atomic<std::uint64_t>> store_seq_; // unique CoW scratch-file suffixes
    std::unique_ptr<std::atomic<std::uint64_t>> etag_seq_;  // monotonic store generation -> ETag
    std::unique_ptr<std::atomic<bool>> any_ttl_; // a TTL has been set -> reaper scans (else O(1) skip)
    std::unique_ptr<std::shared_mutex> mu_; // rwlock: shared for reads, exclusive for writes (ADR-0018)
    // Decay announces itself before taking operation, so the tight promotion loop cannot overtake it.
    std::unique_ptr<ScoreMaintenanceGate> score_maintenance_;
    std::unique_ptr<std::atomic<bool>> numa_scores_healthy_;
    std::unique_ptr<std::atomic<std::uint64_t>> numa_promotion_count_;
    std::unique_ptr<std::atomic<std::uint64_t>> numa_promotion_bytes_;
    std::unique_ptr<std::atomic<std::uint64_t>> numa_promotion_total_ns_;
    std::unique_ptr<std::atomic<std::uint64_t>> numa_promotion_max_ns_;
    struct RegionPin {
        std::uint32_t len = 0;
        unsigned refcount = 0;
        bool orphaned = false;
    };
    std::unordered_map<std::uint64_t, RegionPin> pins_; // pinned head regions (region_id -> state)
};

} // namespace goblin::storage
