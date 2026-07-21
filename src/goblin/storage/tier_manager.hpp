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
#include "goblin/core/stats.hpp"
#include "goblin/storage/eviction.hpp"
#include "goblin/crypto/sha256.hpp" // Digest
#include "goblin/storage/drive_pool.hpp"
#include "goblin/storage/index.hpp"
#include "goblin/storage/layout.hpp"
#include "goblin/storage/numa_head_scores.hpp"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace goblin::storage {

// Publication condition carried by a StoreHandle from admission to its linearization point.
// `add` requires the key to be absent (or expired); `replace` requires a live key.
enum class StoreCondition : std::uint8_t { unconditional, add, replace };

// RAII set of an object's per-drive file descriptors (one per drive holding data; -1 elsewhere).
// Fixed capacity avoids a heap allocation per open on the GET/SET hot path (pools are small).
inline constexpr unsigned kMaxPoolDrives = 64;

class ObjectFiles {
public:
    ObjectFiles() noexcept {
        for (auto& f : fds_) f = -1;
    }
    explicit ObjectFiles(std::span<const int> src) noexcept;
    ~ObjectFiles();
    ObjectFiles(ObjectFiles&&) noexcept;
    ObjectFiles& operator=(ObjectFiles&&) noexcept;
    ObjectFiles(const ObjectFiles&) = delete;
    ObjectFiles& operator=(const ObjectFiles&) = delete;
    std::span<const int> fds() const noexcept { return std::span<const int>(fds_.data(), n_); }
    unsigned size() const noexcept { return n_; }

private:
    std::array<int, kMaxPoolDrives> fds_{};
    unsigned n_ = 0;
};

// A storage pool: a DrivePool plus one open O_DIRECTORY fd per drive dir (ADR-0009 openat path).
class Pool {
public:
    struct FilesystemCapacity {
        std::uint64_t device = 0;
        std::string path;
        Size total_bytes = 0;
        Size allocated_bytes = 0;
        Size free_bytes = 0;
        Size available_bytes = 0;
        Size block_bytes = 0;
        std::uint64_t total_inodes = 0;
        std::uint64_t used_inodes = 0;
        std::uint64_t free_inodes = 0;
        std::uint64_t available_inodes = 0;
    };
    static Result<Pool> open(const std::vector<std::string>& dirs, Size stripe_unit,
                             bool direct_io = false);
    ~Pool();
    Pool(Pool&&) noexcept;
    Pool& operator=(Pool&&) noexcept;
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    const DrivePool& drives() const noexcept { return drives_; }
    std::uint64_t device_of_drive(unsigned drive) const noexcept {
        return drive < devices_.size() ? devices_[drive] : 0;
    }
    std::string_view path_of_drive(unsigned drive) const noexcept {
        return drive < dirs_.size() ? std::string_view(dirs_[drive]) : std::string_view{};
    }
    std::span<const std::uint64_t> devices() const noexcept { return devices_; }
    std::vector<FilesystemCapacity> filesystem_capacity() const;
    // Open immutable per-object generation files covering `tier_bytes`. Generation zero is only
    // valid for a zero-byte/RAM-only tier and therefore opens no file.
    Result<ObjectFiles> open_object(const Digest& digest, Size tier_bytes, bool create,
                                    std::uint64_t generation,
                                    std::uint64_t* failed_device = nullptr,
                                    int* failed_errno = nullptr) const;
    // Reserve each shard's exact final extent on its actual filesystem. Unsupported filesystems
    // fall back to checked streaming writes; ENOSPC/EDQUOT are returned as out_of_space.
    Status reserve_object(const Digest& digest, Size tier_bytes, const ObjectFiles& files,
                          std::uint64_t* failed_device = nullptr,
                          int* failed_errno = nullptr) const;
    void unlink_object(const Digest& digest, Size tier_bytes,
                       std::uint64_t generation) const; // remove one immutable incarnation

private:
    Pool(DrivePool dp, std::vector<std::string> dirs, std::vector<int> dirfds,
         std::vector<std::uint64_t> devices,
         bool direct_io)
        : drives_(dp), dirs_(std::move(dirs)), dirfds_(std::move(dirfds)),
          devices_(std::move(devices)),
          direct_io_(direct_io) {}
    DrivePool drives_;
    std::vector<std::string> dirs_;
    std::vector<int> dirfds_;
    std::vector<std::uint64_t> devices_; // st_dev for each drive directory (capacity domain)
    bool direct_io_ = false; // open object files with O_DIRECT (ADR-0011)
};

class TierManager {
public:
    enum class StorageHealthState : std::uint8_t { healthy, degraded, read_only, failed };

    static Result<TierManager> open(const TierSizes&, const MemoryConfig&, const EvictionConfig&,
                                    const PoolConfig& ssd, const PoolConfig& hdd, Index& index,
                                    Size io_chunk = 256 * KiB, unsigned write_buffers = 8,
                                    bool direct_io = false,
                                    AccessScoreConfig access_score = {},
                                    Size write_io_chunk = 0,
                                    Size max_object_size = kMaxObjectSize);
    bool three_layer() const noexcept { return hdd_.has_value(); }
    Size max_object_size() const noexcept { return max_object_size_; }

    // Streaming write (ADR-0016/0017): open files + reserve the RAM head once, append chunks with
    // write(), publish with commit(). RAM held during a write = the head + the caller's chunk,
    // never the whole object. An uncommitted handle aborts on destruction (frees the head; the
    // object stays invisible since it was never indexed — orphan files are reclaimed on wipe).
    struct CommitMetadata {
        bool stale;
        bool recache_claimed;
    };

    class StoreHandle {
    public:
        StoreHandle(StoreHandle&&) noexcept;
        StoreHandle& operator=(StoreHandle&&) noexcept;
        StoreHandle(const StoreHandle&) = delete;
        StoreHandle& operator=(const StoreHandle&) = delete;
        ~StoreHandle();

        Status write(ByteView chunk);       // append the next bytes (sequential, from offset 0)
        // Persist complete device blocks currently staged once at least `min_complete` bytes are
        // ready, retaining only the unavoidable sub-block tail. Prefer full-stage flushes via
        // write() on high-BDP mirror fills (fast Ethernet / IPoIB); this progressive path is
        // optional for callers that want intermediate durability of staged bytes before commit.
        Status flush_available(Size min_complete = kDeviceBlock);
        // Publish to the index + cache the head. expiry is an absolute Unix time (0 = never,
        // ADR-0007). The StoreCondition captured by begin_store is rechecked under the publication
        // lock here, making memcache add/replace atomic even though the body was written earlier.
        // cas_expected != 0 -> compare-and-swap: publish only if the live object's etag matches, else
        // return Errc::cas_mismatch (checked under the publish lock; the scratch files just abort).
        // On success returns the new CAS/etag (store generation) so callers need not re-lookup.
        Result<std::uint64_t> commit(std::uint32_t flags, std::uint32_t expiry = 0,
                                     std::uint64_t cas_expected = 0,
                                     std::shared_ptr<const HttpCacheMetadata> http = {},
                                     CommitMetadata metadata = CommitMetadata{false, false});

    private:
        friend class TierManager;
        StoreHandle(TierManager* tm, Digest digest, ObjectLayout layout, ObjectFiles ssd,
                    std::optional<ObjectFiles> hdd, std::optional<core::BufferPool::Region> head,
                    std::uint64_t file_generation, MutBytes stage, WriteMode write_mode,
                    StoreCondition condition, bool store_protection,
                    bool pending_small_head) noexcept;
        void abort_uncommitted(); // free the reserved head + unlink scratch files (never published)
        Status flush_block(Size n); // write the staged block (split SSD/HDD) at flushed_; 4 KiB-sized
        TierManager* tm_;
        Digest digest_;
        ObjectLayout layout_;
        ObjectFiles ssd_;
        std::optional<ObjectFiles> hdd_;
        std::optional<core::BufferPool::Region> head_;
        std::uint64_t file_generation_ = 0; // immutable shard incarnation; Index swap publishes it
        MutBytes stage_;      // borrowed aligned staging buffer (from tm_->write_pool_)
        Size stage_fill_ = 0; // bytes accumulated in stage_, not yet flushed to disk
        Size flushed_ = 0;    // bytes already written to disk (a 4 KiB-aligned prefix)
        Offset off_ = 0;
        bool committed_ = false;
        WriteMode write_mode_ = WriteMode::evict;
        StoreCondition condition_ = StoreCondition::unconditional;
        bool store_protection_ = false; // keeps this digest out of both eviction policies
        bool pending_small_head_ = false; // keeps an unpublished arena slot out of compaction
    };

    Result<StoreHandle> begin_store(const Digest&, Size size,
                                    WriteMode write_mode = WriteMode::evict,
                                    StoreCondition condition = StoreCondition::unconditional);
    Status store(const Digest&, ByteView data, std::uint32_t flags, std::uint32_t expiry = 0,
                 WriteMode write_mode = WriteMode::evict); // begin+write+commit
    // Objects published at or before this cutoff become lazy misses when its optional delay elapses;
    // later stores survive. The normal reaper physically releases invalidated storage.
    void flush_all(std::uint32_t delay_seconds = 0);
    std::optional<ObjectMeta> lookup_live(const Digest&, std::uint32_t now = 0) const;
    std::size_t reap_expired(); // drop objects past their TTL; returns count (O(1) when no TTLs are set)
    // Meta T: overwrite TTL only while the item is live; an expired-but-not-reaped entry stays dead.
    bool touch_ttl(const Digest&, std::uint32_t expiry, std::uint32_t now = now_unix());
    Index::RecacheClaim claim_recache(const Digest&, std::uint32_t now = now_unix());
    Index::MetaMutation mark_stale(const Digest&, std::uint64_t cas_expected,
                                   std::optional<std::uint32_t> expiry = std::nullopt,
                                   std::uint32_t now = now_unix());
    Result<std::size_t> read(core::Reactor&, const Digest&, Offset offset, MutBytes out,
                             bool record_access = true);
    bool remove(const Digest&); // erase from index + free the cached head
    // Resident RAM head bytes for a zero-copy send, or nullopt if not cached (ADR-0017).
    std::optional<ByteView> head_view(const Digest&);
    void touch(const Digest&);          // record one logical read: eviction touch + score increment
    // Eviction-policy visited bits only (no score). Used after open_snapshot which already did the
    // atomic score update under its lock — avoids a double score bump.
    void touch_policies(const Digest&, Size object_size, bool head_resident);
    std::size_t head_resident() const;  // # heads currently cached in RAM (stats/tests)
    std::optional<double> access_score(const Digest&) const; // unified score regardless of owner

    // Multiply every key score by --decay. A dense-worker dispatch failure is fatal because an
    // interrupted multi-node epoch may already have decayed only a subset of authoritative scores.
    void decay_access_scores();
    bool promote_hot_remote_block();    // one local/foreign full-block exchange, or false if sorted
    struct NumaPromotionStats {
        std::uint64_t count = 0;
        std::uint64_t bytes_moved = 0;
        std::uint64_t total_ns = 0;
        std::uint64_t max_ns = 0;
        std::uint64_t scans = 0;
        std::uint64_t no_candidate = 0;
        std::uint64_t maintenance_yields = 0;
        std::uint64_t failures = 0;
        std::uint64_t last_success_unix = 0;
        bool healthy = true;
    };
    NumaPromotionStats numa_promotion_stats() const noexcept;

    struct MemoryUsage {
        core::BufferPoolClass allocation_class = core::BufferPoolClass::shared;
        Size capacity_bytes = 0;
        Size used_bytes = 0;
        Size free_bytes = 0;
        Size fragmented_bytes = 0;
    };
    struct HugeTlbMapping {
        std::size_t region = 0;
        std::optional<unsigned> numa_node;
        core::BufferPoolClass allocation_class = core::BufferPoolClass::shared;
        Size bytes = 0;
        bool hugetlb = false;
    };
    struct BuddyFreeBlocks {
        core::BufferPoolClass allocation_class = core::BufferPoolClass::shared;
        Size block_bytes = 0;
        std::uint64_t count = 0;
    };
    struct FilesystemUsage {
        Tier tier = Tier::ssd;
        std::uint64_t device = 0;
        std::string path;
        Size logical_bytes = 0;
        Size reserved_bytes = 0;
        Size filesystem_allocated_bytes = 0;
        Size filesystem_free_bytes = 0;
        Size filesystem_available_bytes = 0;
        Size filesystem_total_bytes = 0;
        std::uint64_t filesystem_total_inodes = 0;
        std::uint64_t filesystem_used_inodes = 0;
        std::uint64_t filesystem_free_inodes = 0;
        std::uint64_t filesystem_available_inodes = 0;
    };
    struct FilesystemCapacityStats {
        Tier tier = Tier::ssd;
        Pool::FilesystemCapacity capacity;
    };
    struct EvictionStats {
        std::uint64_t heads = 0;
        std::uint64_t objects = 0;
        std::uint64_t capacity = 0;
        std::uint64_t ttl = 0;
    };
    struct DiskIoStats {
        Tier tier = Tier::ssd;
        core::DurationSnapshot reads;
        core::DurationSnapshot writes;
        std::uint64_t read_errors = 0;
        std::uint64_t write_errors = 0;
        std::vector<std::pair<int, std::uint64_t>> read_errno;
        std::vector<std::pair<int, std::uint64_t>> write_errno;
    };
    struct DeviceHealthStats {
        std::uint64_t device = 0;
        std::string paths;
        unsigned tier_mask = 0; // bit 0 SSD, bit 1 HDD
        StorageHealthState state = StorageHealthState::healthy;
        std::uint64_t read_errors = 0;
        std::uint64_t write_errors = 0;
        std::uint64_t capacity_errors = 0;
        int last_errno = 0;
        std::uint64_t last_error_unix = 0;
    };
    struct StorageHealthSnapshot {
        StorageHealthState state = StorageHealthState::healthy;
        bool ready = true;
        std::uint64_t quarantined_objects = 0;
        std::uint64_t quarantine_failures = 0;
        std::uint64_t watermark_scans = 0;
        std::uint64_t watermark_reclaim_runs = 0;
        std::uint64_t watermark_reclaimed_objects = 0;
        std::vector<DeviceHealthStats> devices;
    };
    struct ObservabilitySnapshot {
        Index::Usage objects;
        std::uint64_t total_items = 0;
        std::vector<MemoryUsage> memory;
        std::vector<HugeTlbMapping> mappings;
        std::vector<FilesystemUsage> filesystems;
        std::vector<DiskIoStats> io;
        EvictionStats evictions;
    };
    struct CompatibilitySnapshot {
        Index::Usage objects;
        std::uint64_t total_items = 0;
        Size memory_limit_bytes = 0;
        EvictionStats evictions;
    };
    CompatibilitySnapshot compatibility_snapshot() const noexcept;
    std::vector<BuddyFreeBlocks> buddy_free_blocks_snapshot() const;
    // Cheap live filesystem gauges used by the normal memcache `stats` command. This performs one
    // fstatvfs() per distinct filesystem and never scans the object index.
    std::vector<FilesystemCapacityStats> filesystem_capacity_snapshot() const;
    ObservabilitySnapshot observability_snapshot() const;
    StorageHealthSnapshot storage_health_snapshot() const;
    // Check live block/inode availability and reclaim filesystem-local victims when either use
    // ratio reaches the high watermark. Work is bounded per call; the maintenance thread repeats.
    std::size_t reclaim_to_watermarks(std::size_t max_victims = 1024);
    // The serving I/O layer reports exact completion ownership here. Capacity errors are counted
    // separately and never poison a device; non-capacity I/O errors transition health state.
    void note_device_read(std::uint64_t device, int error_number) noexcept;
    void note_device_write(std::uint64_t device, int error_number) noexcept;
    // Remove only the incarnation that produced the failure. A concurrent replacement wins and is
    // left alone; open readers keep the old unlinked inode alive until their snapshot is destroyed.
    bool quarantine_object(const Digest&, std::uint64_t generation);
    static std::string_view health_state_name(StorageHealthState) noexcept;
    void note_disk_read(Tier, std::uint64_t duration_ns, int error_number = 0) noexcept;
    void note_disk_write(Tier, std::uint64_t duration_ns, int error_number = 0) noexcept;

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
        ReadStream(ReadStream&&) noexcept;
        ReadStream& operator=(ReadStream&&) noexcept;
        ReadStream(const ReadStream&) = delete;
        ReadStream& operator=(const ReadStream&) = delete;
        ~ReadStream();

        struct Seg {
            int fd;
            Offset file_off;
            Size len;
            Size out_off;
            Tier tier;
            std::uint64_t device;
        };
        // Worst case segs for one piece: ~ceil(io_chunk / min_stripe) across SSD+HDD. 128 covers
        // a 256 KiB piece at 4 KiB stripe with room to spare, without a heap alloc per piece.
        static constexpr std::size_t kMaxSegs = 128;
        struct Plan {
            std::size_t total = 0; // bytes this piece delivers (head already copied into out[0..])
            std::array<Seg, kMaxSegs> segs{};
            std::size_t nsegs = 0; // disk reads to issue; 0 => all-head, ready now
            std::span<const Seg> segments() const noexcept {
                return std::span<const Seg>(segs.data(), nsegs);
            }
        };
        Size size() const noexcept { return size_; }
        std::uint64_t generation() const noexcept { return generation_; }
        Plan plan(Offset off, MutBytes out);
        Plan plan(Offset off, MutBytes out, Size max_logical);
        Result<std::size_t> read(core::Reactor&, Offset off, MutBytes out);
        Result<std::size_t> read_sync(Offset off, MutBytes out); // uncommon mutation copy path

    private:
        friend class TierManager;
        ReadStream(TierManager* tm, Digest d, Size size, std::uint64_t generation,
                   ObjectLayout layout, ObjectFiles ssd,
                   std::optional<ObjectFiles> hdd, HeadLoc head, HeadPin owned_pin)
            : tm_(tm), digest_(d), size_(size), generation_(generation), layout_(layout),
              ssd_(std::move(ssd)),
              hdd_(std::move(hdd)), head_(head), owned_pin_(owned_pin) {}
        TierManager* tm_;
        Digest digest_;
        Size size_;
        std::uint64_t generation_ = 0;
        ObjectLayout layout_;
        ObjectFiles ssd_;
        std::optional<ObjectFiles> hdd_;
        HeadLoc head_;       // exact incarnation captured with the generation fds
        HeadPin owned_pin_;  // legacy open_read owns this; Snapshot owns its pin externally
    };
    Result<ReadStream> open_read(const Digest&);

    // A consistent read snapshot (ADR-0018): the object's metadata, a pinned RAM head (if resident),
    // and the open disk files (if a tail remains) -- all captured under one lock so a concurrent
    // copy-on-write replace can't split head from body. The open fds pin the old inodes.
    struct Snapshot {
        ObjectMeta meta;
        std::shared_ptr<const HttpCacheMetadata> http; // requested only by the mirror HTTP path
        HeadPin pin;                  // pin.valid == false when the head isn't resident
        std::optional<ReadStream> rs; // present iff size > head_len (a disk tail to stream)
    };
    // record_access=false is used only when an internally backpressured GET re-takes its snapshot;
    // one logical request must not gain score repeatedly while it waits for an I/O buffer.
    // `now` is absolute Unix seconds for the lazy-TTL check; pass 0 to sample system_clock once.
    std::optional<Snapshot> open_snapshot(const Digest&, bool record_access = true,
                                          std::uint32_t now = 0,
                                          bool include_http_metadata = false);
    // Copy one immutable snapshot into a fresh StoreHandle with bounded aligned scratch space.
    // Used by append/prepend without buffering either the old object or the incoming fragment.
    Status copy_snapshot(StoreHandle&, Snapshot&);

    struct ArithmeticResult {
        std::uint64_t value = 0;
        ObjectMeta meta;
        bool created = false;
    };
    // Atomic unsigned decimal arithmetic. `initial` enables meta N/J creation on miss; classic
    // incr/decr passes nullopt. Decrement floors at zero and increment wraps uint64_t.
    Result<ArithmeticResult> arithmetic(const Digest&, std::uint64_t delta, bool decrement,
                                        std::optional<std::uint64_t> initial = std::nullopt,
                                        std::optional<std::uint32_t> new_expiry = std::nullopt,
                                        std::uint64_t cas_expected = 0,
                                        WriteMode write_mode = WriteMode::evict);

private:
    struct TierIoCounters {
        core::DurationMetric reads;
        core::DurationMetric writes;
        std::atomic<std::uint64_t> read_errors{0};
        std::atomic<std::uint64_t> write_errors{0};
        std::array<std::atomic<std::uint64_t>, 256> read_errno{};
        std::array<std::atomic<std::uint64_t>, 256> write_errno{};
    };
    struct IoCounters { std::array<TierIoCounters, 2> tiers; };
    struct DeviceIoCounters {
        std::uint64_t device = 0;
        std::string paths;
        unsigned tier_mask = 0;
        std::atomic<StorageHealthState> state{StorageHealthState::healthy};
        std::atomic<std::uint64_t> read_errors{0};
        std::atomic<std::uint64_t> write_errors{0};
        std::atomic<std::uint64_t> capacity_errors{0};
        std::atomic<int> last_errno{0};
        std::atomic<std::uint64_t> last_error_unix{0};
    };
    struct ScoreMaintenanceGate {
        std::mutex operation;
        std::atomic<bool> rescore_pending{false};
    };

    TierManager(TierSizes t, core::BufferPool ram, std::unique_ptr<EvictionPolicy> head_policy,
                std::unique_ptr<EvictionPolicy> small_policy,
                std::unique_ptr<EvictionPolicy> object_policy, std::uint64_t max_objects, Pool ssd,
                std::optional<Pool> hdd, Index& ix, Size small_min_alloc, bool split_memory_pools,
                AccessScoreConfig access_score, double high_watermark, double low_watermark,
                std::unique_ptr<NumaHeadScoreTable> head_scores, Size max_object_size)
        : tiers_(t), access_score_(access_score), small_min_alloc_(small_min_alloc),
          split_memory_pools_(split_memory_pools), max_object_size_(max_object_size),
          ram_(std::move(ram)),
          policy_(std::move(head_policy)), small_policy_(std::move(small_policy)),
          object_policy_(std::move(object_policy)), max_objects_(max_objects),
          high_watermark_(high_watermark), low_watermark_(low_watermark), ssd_(std::move(ssd)),
          hdd_(std::move(hdd)), index_(&ix),
          head_scores_(std::move(head_scores)),
          store_seq_(std::make_unique<std::atomic<std::uint64_t>>(0)),
          etag_seq_(std::make_unique<std::atomic<std::uint64_t>>(0)),
          any_ttl_(std::make_unique<std::atomic<bool>>(false)),
          flush_deadline_ns_(std::make_unique<std::atomic<std::int64_t>>(0)),
          mu_(std::make_unique<std::shared_mutex>()),
          disk_reclaim_mu_(std::make_unique<std::mutex>()),
          score_maintenance_(std::make_unique<ScoreMaintenanceGate>()),
          numa_promotion_healthy_(std::make_unique<std::atomic<bool>>(true)),
          numa_promotion_count_(std::make_unique<std::atomic<std::uint64_t>>(0)),
          numa_promotion_bytes_(std::make_unique<std::atomic<std::uint64_t>>(0)),
          numa_promotion_total_ns_(std::make_unique<std::atomic<std::uint64_t>>(0)),
          numa_promotion_max_ns_(std::make_unique<std::atomic<std::uint64_t>>(0)),
          numa_promotion_scans_(std::make_unique<std::atomic<std::uint64_t>>(0)),
          numa_promotion_no_candidate_(std::make_unique<std::atomic<std::uint64_t>>(0)),
          numa_promotion_yields_(std::make_unique<std::atomic<std::uint64_t>>(0)),
          numa_promotion_failures_(std::make_unique<std::atomic<std::uint64_t>>(0)),
          numa_promotion_last_success_(std::make_unique<std::atomic<std::uint64_t>>(0)),
          total_items_(std::make_unique<std::atomic<std::uint64_t>>(0)),
          head_evictions_(std::make_unique<std::atomic<std::uint64_t>>(0)),
          object_evictions_(std::make_unique<std::atomic<std::uint64_t>>(0)),
          capacity_evictions_(std::make_unique<std::atomic<std::uint64_t>>(0)),
          ttl_evictions_(std::make_unique<std::atomic<std::uint64_t>>(0)),
          quarantined_objects_(std::make_unique<std::atomic<std::uint64_t>>(0)),
          quarantine_failures_(std::make_unique<std::atomic<std::uint64_t>>(0)),
          watermark_scans_(std::make_unique<std::atomic<std::uint64_t>>(0)),
          watermark_reclaim_runs_(std::make_unique<std::atomic<std::uint64_t>>(0)),
          watermark_reclaimed_objects_(std::make_unique<std::atomic<std::uint64_t>>(0)) {}
    Status drop_object(const Digest&); // free head + unlink files + erase from index & policies
    bool quarantine_object_locked(const Digest&, std::uint64_t generation);
    Status reserve_store_space(const Digest&, const ObjectLayout&, const ObjectFiles&,
                               const std::optional<ObjectFiles>&, WriteMode);
    // Caller holds disk_reclaim_mu_, not mu_. Drops at most one object occupying `device` (st_dev).
    bool reclaim_one_disk_object(std::uint64_t device);
    bool object_uses_device(const Digest&, const ObjectMeta&, std::uint64_t device) const noexcept;
    bool layout_uses_device(const Digest&, const ObjectLayout&, std::uint64_t device) const noexcept;
    Status ensure_store_devices_writable(const Digest&, const ObjectLayout&) const;
    DeviceIoCounters* device_counters(std::uint64_t device) const noexcept;
    void register_device(std::uint64_t device, std::string_view path, Tier);
    void insert_capacity_policies_locked(const Digest&, const ObjectMeta&);
    void remove_capacity_policies_locked(const Digest&);
    void touch_capacity_policies_locked(const Digest&);
    void enforce_object_bound();     // evict whole objects while over the count limit
    Status acquire_store_protection_locked(const Digest&);
    void release_store_protection_locked(const Digest&);
    void release_pending_small_head_locked(const core::BufferPool::Region&);
    bool store_protected_locked(const Digest& digest) const {
        return store_protections_.contains(digest);
    }
    void free_head_region(unsigned block, std::uint32_t offset, std::uint32_t len); // free or orphan
    void compact_small(); // slide live heads down within fragmented small arenas; reclaim dead space (ADR-0008)
    bool small_object(Size size) const noexcept { return size < tiers_.ram_head; }
    EvictionPolicy& resident_policy(Size size) noexcept {
        return split_memory_pools_ && small_object(size) ? *small_policy_ : *policy_;
    }
    core::BufferPoolClass allocation_class(Size size) const noexcept {
        if (!split_memory_pools_) return core::BufferPoolClass::shared;
        return small_object(size) ? core::BufferPoolClass::small_object
                                  : core::BufferPoolClass::fixed_head;
    }
    bool fixed_score_owner(const ObjectMeta& meta) const noexcept {
        return meta.head.resident() && meta.head.len == tiers_.ram_head;
    }
    // Caller holds mu_ exclusively. A score has exactly one owner: a full resident head's atomic
    // NUMA slot, or the Index entry for every other state. Transitions temporarily extract it from
    // the old owner and install it in the new owner while the lock makes that gap unobservable.
    Result<double> extract_owned_score_locked(const Digest&, const ObjectMeta&);
    Status install_owned_score_locked(const Digest&, const ObjectMeta&, double);
    Status discard_owned_score_locked(const Digest&, const ObjectMeta&);
    void record_access_locked(const Digest&, const ObjectMeta&);
    bool unavailable_locked(const ObjectMeta&, std::uint32_t now,
                            std::int64_t now_ns) const noexcept;
    // Promotion is an optional optimizer. Disabling it must not disable authoritative score
    // ownership, increments, or decay.
    void disable_numa_promotion(const Error&);
    static std::uint64_t region_id(unsigned block, std::uint32_t offset) {
        return (static_cast<std::uint64_t>(block) << 32) | offset;
    }

    TierSizes tiers_;
    AccessScoreConfig access_score_;
    Size small_min_alloc_;                           // buddy min-order for RAM-only heads (ADR-0008-rev)
    bool split_memory_pools_ = false;                // fixed and fractional heads cannot borrow blocks
    Size max_object_size_ = kMaxObjectSize;          // common admission cap for all frontends
    core::BufferPool ram_;                           // RAM-head cache (ADR-0003/0008)
    std::unique_ptr<core::IoBufferPool> write_pool_; // bounded aligned CoW write staging (ADR-0011)
    std::unique_ptr<EvictionPolicy> policy_;         // fixed-head, or legacy shared, eviction (ADR-0007)
    std::unique_ptr<EvictionPolicy> small_policy_;   // fractional-head eviction in split-pool mode
    std::unique_ptr<EvictionPolicy> object_policy_; // whole-object / SSD count bound (ADR-0012)
    // One victim selector per actual filesystem. A full HDD shard cannot evict SSD-only objects or
    // data on another mount in a futile attempt to free the failed capacity domain.
    std::unordered_map<std::uint64_t, std::unique_ptr<EvictionPolicy>> capacity_policies_;
    std::uint64_t max_objects_;                     // 0 => unbounded
    double high_watermark_ = 0.90;
    double low_watermark_ = 0.80;
    Pool ssd_;
    std::optional<Pool> hdd_;
    Index* index_;
    std::unique_ptr<NumaHeadScoreTable> head_scores_;
    std::unique_ptr<std::atomic<std::uint64_t>> store_seq_; // unique immutable disk generations
    std::unique_ptr<std::atomic<std::uint64_t>> etag_seq_;  // monotonic store generation -> ETag
    std::unique_ptr<std::atomic<bool>> any_ttl_; // a TTL has been set -> reaper scans (else O(1) skip)
    std::unique_ptr<std::atomic<std::int64_t>> flush_deadline_ns_; // 0 => no flush_all cutoff
    std::unique_ptr<std::shared_mutex> mu_; // rwlock: shared for reads, exclusive for writes (ADR-0018)
    // Serializes capacity retries and eviction so concurrent writers cannot stampede the victim
    // selector. The failed fallocate/pwrite is always retried after taking this mutex because an
    // earlier writer or an unrelated filesystem user may already have changed available space.
    std::unique_ptr<std::mutex> disk_reclaim_mu_;
    // Decay announces itself before taking operation, so the tight promotion loop cannot overtake it.
    std::unique_ptr<ScoreMaintenanceGate> score_maintenance_;
    std::unique_ptr<std::atomic<bool>> numa_promotion_healthy_;
    std::unique_ptr<std::atomic<std::uint64_t>> numa_promotion_count_;
    std::unique_ptr<std::atomic<std::uint64_t>> numa_promotion_bytes_;
    std::unique_ptr<std::atomic<std::uint64_t>> numa_promotion_total_ns_;
    std::unique_ptr<std::atomic<std::uint64_t>> numa_promotion_max_ns_;
    std::unique_ptr<std::atomic<std::uint64_t>> numa_promotion_scans_;
    std::unique_ptr<std::atomic<std::uint64_t>> numa_promotion_no_candidate_;
    std::unique_ptr<std::atomic<std::uint64_t>> numa_promotion_yields_;
    std::unique_ptr<std::atomic<std::uint64_t>> numa_promotion_failures_;
    std::unique_ptr<std::atomic<std::uint64_t>> numa_promotion_last_success_;
    std::unique_ptr<std::atomic<std::uint64_t>> total_items_;
    std::unique_ptr<std::atomic<std::uint64_t>> head_evictions_;
    std::unique_ptr<std::atomic<std::uint64_t>> object_evictions_;
    std::unique_ptr<std::atomic<std::uint64_t>> capacity_evictions_;
    std::unique_ptr<std::atomic<std::uint64_t>> ttl_evictions_;
    std::unique_ptr<std::atomic<std::uint64_t>> quarantined_objects_;
    std::unique_ptr<std::atomic<std::uint64_t>> quarantine_failures_;
    std::unique_ptr<std::atomic<std::uint64_t>> watermark_scans_;
    std::unique_ptr<std::atomic<std::uint64_t>> watermark_reclaim_runs_;
    std::unique_ptr<std::atomic<std::uint64_t>> watermark_reclaimed_objects_;
    std::unique_ptr<IoCounters> io_counters_ = std::make_unique<IoCounters>();
    // Constructed once before publication, then only the pointed-to atomics change. This makes
    // completion-path lookups lock-free with respect to unrelated object/index operations.
    std::unordered_map<std::uint64_t, std::unique_ptr<DeviceIoCounters>> device_io_;
    // Every in-flight StoreHandle holds one token. On the first token, the current incarnation is
    // detached from both eviction policies; the last release reattaches whichever incarnation is
    // current then. This preserves copy-on-write/CAS state across concurrent admissions.
    std::unordered_map<Digest, unsigned, DigestHash> store_protections_;
    // Fractional heads are not visible in the Index until commit. Compaction must skip any arena
    // block containing one of these reservations or it could rewind over an in-flight writer.
    std::unordered_set<std::uint64_t> pending_small_heads_;
    // Pin table: refcount is atomic so open_snapshot can pin under a shared lock (ADR-0018 follow-up).
    // Map insert/erase still requires the exclusive storage lock.
    struct RegionPin {
        std::uint32_t len = 0;
        std::atomic<unsigned> refcount{0};
        std::atomic<bool> orphaned{false};

        RegionPin() = default;
        explicit RegionPin(std::uint32_t l) : len(l) {}
        RegionPin(RegionPin&& o) noexcept
            : len(o.len), refcount(o.refcount.load(std::memory_order_relaxed)),
              orphaned(o.orphaned.load(std::memory_order_relaxed)) {}
        RegionPin& operator=(RegionPin&& o) noexcept {
            if (this != &o) {
                len = o.len;
                refcount.store(o.refcount.load(std::memory_order_relaxed), std::memory_order_relaxed);
                orphaned.store(o.orphaned.load(std::memory_order_relaxed), std::memory_order_relaxed);
            }
            return *this;
        }
        RegionPin(const RegionPin&) = delete;
        RegionPin& operator=(const RegionPin&) = delete;
    };
    std::unordered_map<std::uint64_t, RegionPin> pins_; // pinned head regions (region_id -> state)

    // Bump pin under the current lock. `create` inserts a zeroed entry when missing (exclusive only).
    bool pin_region(const HeadLoc& loc, HeadPin& out, bool create);
};

} // namespace goblin::storage
