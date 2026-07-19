#include "goblin/storage/tier_manager.hpp"

#include "goblin/storage/layout.hpp"
#include "goblin/storage/striped_io.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio> // renameat
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <new>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace goblin::storage {

namespace {

[[noreturn]] void fatal_score_state(const char* context, const Error& error) {
    const std::string_view code = to_string(error.code);
    std::fprintf(stderr, "fatal: %s (%.*s)%s%s\n", context, static_cast<int>(code.size()),
                 code.data(), error.detail.empty() ? "" : ": ", error.detail.c_str());
    std::fflush(stderr);
    std::abort();
}

void restore_policy_or_die(EvictionPolicy& policy, const Digest& digest) {
    try {
        policy.insert(digest);
    } catch (const std::bad_alloc&) {
        std::fprintf(stderr, "fatal: cannot restore eviction-policy membership\n");
        std::fflush(stderr);
        std::abort();
    }
}

template <class F>
class ScopeExit {
public:
    explicit ScopeExit(F cleanup) : cleanup_(std::move(cleanup)) {}
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ~ScopeExit() {
        if (active_) cleanup_();
    }
    void dismiss() noexcept { active_ = false; }

private:
    F cleanup_;
    bool active_ = true;
};

} // namespace

// ---------------- ObjectFiles ----------------

ObjectFiles::ObjectFiles(std::span<const int> src) noexcept {
    n_ = static_cast<unsigned>(std::min<std::size_t>(src.size(), kMaxPoolDrives));
    for (unsigned i = 0; i < n_; ++i) fds_[i] = src[i];
    for (unsigned i = n_; i < kMaxPoolDrives; ++i) fds_[i] = -1;
}
ObjectFiles::~ObjectFiles() {
    for (unsigned i = 0; i < n_; ++i)
        if (fds_[i] >= 0) ::close(fds_[i]);
}
ObjectFiles::ObjectFiles(ObjectFiles&& o) noexcept : fds_(o.fds_), n_(o.n_) {
    for (unsigned i = 0; i < o.n_; ++i) o.fds_[i] = -1;
    o.n_ = 0;
}
ObjectFiles& ObjectFiles::operator=(ObjectFiles&& o) noexcept {
    if (this != &o) {
        for (unsigned i = 0; i < n_; ++i)
            if (fds_[i] >= 0) ::close(fds_[i]);
        fds_ = o.fds_;
        n_ = o.n_;
        for (unsigned i = 0; i < o.n_; ++i) o.fds_[i] = -1;
        o.n_ = 0;
    }
    return *this;
}

// Format "<64-hex><suffix>\0" into buf. Returns false if suffix is too long for the buffer.
static bool format_object_name(const Digest& digest, std::string_view suffix, char* buf,
                               std::size_t buf_len) noexcept {
    constexpr std::size_t kHex = Digest::kHexLen;
    if (buf_len < kHex + suffix.size() + 1) return false;
    digest.write_hex(buf);
    if (!suffix.empty()) std::memcpy(buf + kHex, suffix.data(), suffix.size());
    buf[kHex + suffix.size()] = '\0';
    return true;
}

// ---------------- Pool ----------------

Pool::~Pool() {
    for (const int fd : dirfds_)
        if (fd >= 0) ::close(fd);
}
Pool::Pool(Pool&& o) noexcept
    : drives_(o.drives_), dirfds_(std::move(o.dirfds_)), devices_(std::move(o.devices_)),
      direct_io_(o.direct_io_) {}
Pool& Pool::operator=(Pool&& o) noexcept {
    if (this != &o) {
        for (const int fd : dirfds_)
            if (fd >= 0) ::close(fd);
        drives_ = o.drives_;
        dirfds_ = std::move(o.dirfds_);
        devices_ = std::move(o.devices_);
        direct_io_ = o.direct_io_;
    }
    return *this;
}

Result<Pool> Pool::open(const std::vector<std::string>& dirs, Size stripe_unit, bool direct_io) {
    if (dirs.empty()) return err(Errc::invalid_argument, "empty pool");
    std::vector<int> dirfds;
    std::vector<std::uint64_t> devices;
    dirfds.reserve(dirs.size());
    devices.reserve(dirs.size());
    for (const auto& d : dirs) {
        const int fd = ::open(d.c_str(), O_RDONLY | O_DIRECTORY);
        if (fd < 0) {
            for (const int f : dirfds) ::close(f);
            return err(Errc::io_error, "open pool dir: " + d);
        }
        struct stat st {};
        if (::fstat(fd, &st) != 0) {
            ::close(fd);
            for (const int f : dirfds) ::close(f);
            return err(Errc::io_error, "fstat pool dir: " + d);
        }
        dirfds.push_back(fd);
        devices.push_back(static_cast<std::uint64_t>(st.st_dev));
    }
    return Pool(DrivePool(static_cast<unsigned>(dirs.size()), stripe_unit), std::move(dirfds),
                std::move(devices), direct_io);
}

Result<ObjectFiles> Pool::open_object(const Digest& digest, Size tier_bytes, bool create,
                                      std::uint64_t generation,
                                      std::uint64_t* failed_device) const {
    const unsigned n = drives_.num_drives();
    if (n > kMaxPoolDrives) return err(Errc::invalid_argument, "pool has too many drives");
    std::array<int, kMaxPoolDrives> fds{};
    std::array<bool, kMaxPoolDrives> created{};
    for (unsigned i = 0; i < n; ++i) fds[i] = -1;
    if (tier_bytes == 0) return ObjectFiles(std::span<const int>(fds.data(), n));

    if (generation == 0)
        return err(Errc::invalid_argument, "disk object has zero file generation");

    // Stack path name: <64-hex>.g<generation>\0. Immutable names turn publication into an atomic
    // Index metadata swap; O_EXCL makes a generation collision fail rather than reopen old bytes.
    char name[Digest::kHexLen + 32];
    char suffix[32];
    suffix[0] = '.';
    suffix[1] = 'g';
    const auto [suffix_end, suffix_ec] =
        std::to_chars(suffix + 2, suffix + sizeof suffix, generation);
    if (suffix_ec != std::errc{} ||
        !format_object_name(
            digest,
            std::string_view(suffix, static_cast<std::size_t>(suffix_end - suffix)), name,
            sizeof name))
        return err(Errc::invalid_argument, "object name too long");
    const Size stripe = drives_.stripe_unit();
    const Size nchunks = (tier_bytes + stripe - 1) / stripe;
    const unsigned used = static_cast<unsigned>(nchunks < n ? nchunks : n);
    int flags = create ? (O_RDWR | O_CREAT | O_EXCL) : O_RDONLY;
    if (direct_io_) flags |= O_DIRECT; // bypass the page cache: own the backing store (ADR-0011)

    for (unsigned c = 0; c < used; ++c) {
        const unsigned d = drives_.drive_of(digest.bucket(), static_cast<Offset>(c) * stripe);
        if (fds[d] >= 0) continue;
        const int fd = ::openat(dirfds_[d], name, flags, 0644);
        if (fd < 0) {
            const int open_errno = errno;
            for (unsigned i = 0; i < n; ++i)
                if (fds[i] >= 0) ::close(fds[i]);
            if (create) {
                // Never unlink the pathname that failed O_EXCL: it may be a pre-existing generation
                // from a sequence collision. Roll back only files this invocation actually created.
                for (unsigned drive = 0; drive < n; ++drive)
                    if (created[drive]) ::unlinkat(dirfds_[drive], name, 0);
            }
            if (failed_device) *failed_device = device_of_drive(d);
            if (open_errno == ENOSPC || open_errno == EDQUOT)
                return err(Errc::out_of_space, "openat exhausted filesystem metadata capacity");
            return err(Errc::io_error, create ? "openat (create) object file"
                                              : "openat object file (not found?)");
        }
        fds[d] = fd;
        created[d] = create;
    }
    return ObjectFiles(std::span<const int>(fds.data(), n));
}

Status Pool::reserve_object(const Digest& digest, Size tier_bytes,
                            const ObjectFiles& files, std::uint64_t* failed_device) const {
    if (tier_bytes == 0) return {};
#if defined(__linux__)
    const auto fds = files.fds();
    for (unsigned drive = 0; drive < fds.size(); ++drive) {
        if (fds[drive] < 0) continue;
        const Size extent = drives_.file_extent(digest.bucket(), tier_bytes, drive);
        if (extent == 0) continue;
        if (extent > static_cast<Size>(std::numeric_limits<off_t>::max()))
            return err(Errc::too_large, "object shard exceeds off_t");
        int rc;
        do {
            rc = ::fallocate(fds[drive], FALLOC_FL_KEEP_SIZE, 0, static_cast<off_t>(extent));
        } while (rc != 0 && errno == EINTR);
        if (rc == 0) continue;
        const int e = errno;
        if (e == ENOSPC || e == EDQUOT) {
            if (failed_device) *failed_device = device_of_drive(drive);
            return err(Errc::out_of_space, "fallocate could not reserve object shard");
        }
        // Some otherwise valid backing filesystems do not implement fallocate. EINVAL is included
        // because older/network filesystems commonly use it for an unsupported KEEP_SIZE mode.
        // The streaming pwrite path still checks ENOSPC and safely replays its private aligned block.
        if (e == EOPNOTSUPP || e == ENOSYS || e == EINVAL) continue;
        return err(Errc::io_error, "fallocate failed while reserving object shard");
    }
#else
    (void)digest;
    (void)files;
    (void)failed_device;
#endif
    return {};
}

void Pool::unlink_object(const Digest& digest, Size tier_bytes, std::uint64_t generation) const {
    if (tier_bytes == 0) return;
    if (generation == 0) return;
    char name[Digest::kHexLen + 32];
    char suffix[32];
    suffix[0] = '.';
    suffix[1] = 'g';
    const auto [suffix_end, suffix_ec] =
        std::to_chars(suffix + 2, suffix + sizeof suffix, generation);
    if (suffix_ec != std::errc{} ||
        !format_object_name(
            digest,
            std::string_view(suffix, static_cast<std::size_t>(suffix_end - suffix)), name,
            sizeof name))
        return;
    const Size stripe = drives_.stripe_unit();
    const Size nchunks = (tier_bytes + stripe - 1) / stripe;
    const unsigned n = drives_.num_drives();
    const unsigned used = static_cast<unsigned>(nchunks < n ? nchunks : n);
    for (unsigned c = 0; c < used; ++c) {
        const unsigned d = drives_.drive_of(digest.bucket(), static_cast<Offset>(c) * stripe);
        ::unlinkat(dirfds_[d], name, 0); // best-effort; ENOENT is fine
    }
}

// ---------------- TierManager ----------------

Result<TierManager> TierManager::open(const TierSizes& t, const MemoryConfig& mem,
                                      const EvictionConfig& ev, const PoolConfig& ssd,
                                      const PoolConfig& hdd, Index& index, Size io_chunk,
                                      unsigned write_buffers, bool direct_io,
                                      AccessScoreConfig access_score, Size write_io_chunk) {
    auto make_ram = [&]() -> Result<core::BufferPool> {
        if (!mem.split_pools() && mem.numa_regions.empty())
            return core::BufferPool::create(mem.total_bytes, mem.block_bytes, kDeviceBlock,
                                            mem.lock_memory, mem.use_hugepages,
                                            mem.hugetlb_page_bytes);
        std::vector<core::BlockPoolRegion> regions;
        const auto append_regions = [&](std::span<const NumaMemoryRegionConfig> configured,
                                        Size local_bytes,
                                        core::BufferPoolClass allocation_class) {
            if (configured.empty()) {
                regions.push_back({local_bytes, std::nullopt, allocation_class});
                return;
            }
            for (const auto& region : configured)
                regions.push_back({region.bytes, region.node, allocation_class});
        };
        if (!mem.split_pools()) {
            regions.reserve(mem.numa_regions.size());
            for (const auto& region : mem.numa_regions)
                regions.push_back({region.bytes, region.node});
        } else {
            const std::size_t head_regions = mem.numa_regions.empty() ? 1 : mem.numa_regions.size();
            const std::size_t small_regions =
                mem.small_numa_regions.empty() ? 1 : mem.small_numa_regions.size();
            regions.reserve(head_regions + small_regions);
            append_regions(mem.numa_regions, mem.total_bytes,
                           core::BufferPoolClass::fixed_head);
            if (mem.small_numa_regions.empty() && !mem.numa_regions.empty()) {
                for (std::size_t i = 0; i < mem.numa_regions.size(); ++i) {
                    const Size bytes = i == 0 ? *mem.small_total_bytes
                                              : mem.small_sub_bytes.value_or(0);
                    if (bytes > 0)
                        regions.push_back({bytes, mem.numa_regions[i].node,
                                           core::BufferPoolClass::small_object});
                }
            } else {
                append_regions(mem.small_numa_regions, *mem.small_total_bytes,
                               core::BufferPoolClass::small_object);
            }
        }
        return core::BufferPool::create_regions(regions, mem.block_bytes, kDeviceBlock,
                                                mem.lock_memory, mem.use_hugepages,
                                                mem.hugetlb_page_bytes);
    };
    auto ram = make_ram();
    if (!ram) return std::unexpected(ram.error());

    // Fixed-head scores are dense by physical head slot and backed on the same NUMA node as the
    // corresponding head blocks. Region-local scanner threads therefore never pull the O(N) score
    // array through the interconnect just to choose one block.
    std::vector<NumaScoreRegionConfig> score_regions;
    const std::size_t fixed_region_count =
        mem.split_pools() ? (mem.numa_regions.empty() ? 1 : mem.numa_regions.size())
                          : ram->region_count();
    score_regions.reserve(fixed_region_count);
    for (std::size_t region = 0; region < fixed_region_count; ++region) {
        NumaScoreRegionConfig score_region;
        score_region.first_block = ram->region_first_block(region);
        score_region.block_count =
            static_cast<std::size_t>(ram->region_end_block(region) - score_region.first_block);
        score_region.numa_node = ram->region_numa_node(region);
        if (region < mem.numa_regions.size()) score_region.cpus = mem.numa_regions[region].cpus;
        score_regions.push_back(std::move(score_region));
    }
    auto head_scores = NumaHeadScoreTable::create(mem.block_bytes, t.ram_head, score_regions);
    if (!head_scores) return std::unexpected(head_scores.error());

    const std::size_t cap_hint =
        t.ram_head ? static_cast<std::size_t>(mem.arena_bytes() / t.ram_head) : 1;
    auto head_policy = make_eviction_policy(ev.policy, cap_hint);
    const std::size_t small_cap_hint =
        mem.split_pools()
            ? std::max<std::size_t>(
                  1, static_cast<std::size_t>(mem.small_arena_bytes() / mem.small_min_alloc))
            : 1;
    auto small_policy = make_eviction_policy(ev.policy, small_cap_hint);
    const std::size_t obj_cap =
        ev.max_ssd_objects ? static_cast<std::size_t>(ev.max_ssd_objects) : cap_hint;
    auto object_policy = make_eviction_policy(ev.policy, obj_cap);

    auto sp = Pool::open(ssd.dirs, ssd.stripe_unit, direct_io);
    if (!sp) return std::unexpected(sp.error());
    std::optional<Pool> hp;
    if (!hdd.dirs.empty()) {
        auto h = Pool::open(hdd.dirs, hdd.stripe_unit, direct_io);
        if (!h) return std::unexpected(h.error());
        hp.emplace(std::move(*h));
    }
    if (write_io_chunk == 0) write_io_chunk = io_chunk;
    auto wp = core::IoBufferPool::create(write_io_chunk, write_buffers, mem.lock_memory,
                                         mem.use_hugepages, mem.hugetlb_page_bytes);
    if (!wp) return std::unexpected(wp.error());
    TierManager tm(t, std::move(*ram), std::move(head_policy), std::move(small_policy),
                   std::move(object_policy), ev.max_ssd_objects, std::move(*sp), std::move(hp),
                   index, mem.small_min_alloc, mem.split_pools(), access_score,
                   std::make_unique<NumaHeadScoreTable>(std::move(*head_scores)));
    try {
        const auto add_capacity_domain = [&](std::uint64_t device) {
            if (!tm.capacity_policies_.contains(device))
                tm.capacity_policies_.emplace(device, make_eviction_policy(ev.policy, obj_cap));
        };
        for (const auto device : tm.ssd_.devices()) add_capacity_domain(device);
        if (tm.hdd_)
            for (const auto device : tm.hdd_->devices()) add_capacity_domain(device);
    } catch (const std::bad_alloc&) {
        return err(Errc::out_of_memory, "allocate per-filesystem eviction policy");
    }
    tm.write_pool_ = std::make_unique<core::IoBufferPool>(std::move(*wp));
    return tm;
}

Status TierManager::reserve_store_space(const Digest& digest, const ObjectLayout& layout,
                                        const ObjectFiles& ssd_files,
                                        const std::optional<ObjectFiles>& hdd_files,
                                        WriteMode write_mode) {
    const Size ssd_extent =
        layout.hdd_bytes > 0 ? layout.ssd_bytes : align_up(layout.ssd_bytes, kDeviceBlock);
    const Size hdd_extent = align_up(layout.hdd_bytes, kDeviceBlock);
    std::uint64_t failed_device = 0;
    const auto attempt = [&]() -> Status {
        failed_device = 0;
        if (ssd_extent > 0) {
            if (auto st =
                    ssd_.reserve_object(digest, ssd_extent, ssd_files, &failed_device);
                !st)
                return st;
        }
        if (hdd_extent > 0 && hdd_ && hdd_files) {
            if (auto st =
                    hdd_->reserve_object(digest, hdd_extent, *hdd_files, &failed_device);
                !st)
                return st;
        }
        return {};
    };

    auto first = attempt();
    if (first || first.error().code != Errc::out_of_space) return first;
    if (write_mode == WriteMode::block) return first;

    // Only the slow/full path serializes. Retry before evicting: a previous coordinator or an
    // unrelated filesystem user may already have released capacity while this writer waited.
    std::unique_lock<std::mutex> reclaim_lk(*disk_reclaim_mu_);
    for (;;) {
        auto retry = attempt();
        if (retry || retry.error().code != Errc::out_of_space) return retry;
        if (failed_device == 0 || !reclaim_one_disk_object(failed_device)) return retry;
    }
}

Status TierManager::store(const Digest& digest, ByteView data, std::uint32_t flags,
                          std::uint32_t expiry, WriteMode write_mode) {
    auto h = begin_store(digest, data.size(), write_mode);
    if (!h) return std::unexpected(h.error());
    if (auto st = h->write(data); !st) return st;
    if (auto st = h->commit(flags, expiry); !st) return std::unexpected(st.error());
    return {};
}

// meta `T` touch: overwrite an object's absolute expiry in place (0 = never). false if absent.
bool TierManager::touch_ttl(const Digest& digest, std::uint32_t expiry) {
    const bool found = index_->update_expiry(digest, expiry);
    if (found && expiry != 0) any_ttl_->store(true, std::memory_order_relaxed); // arm the reaper
    return found;
}

// Drop every object whose TTL has passed. Lazy-skip already hides expired objects from reads; this
// reclaims their RAM head + disk files. O(1) when no TTL has ever been set (the common cache case).
std::size_t TierManager::reap_expired() {
    if (!any_ttl_->load(std::memory_order_relaxed)) return 0;
    const auto keys = index_->expired_keys(now_unix());
    std::size_t n = 0;
    for (const auto& d : keys)
        if (remove(d)) ++n;
    return n;
}

Result<TierManager::StoreHandle> TierManager::begin_store(const Digest& digest, Size size,
                                                          WriteMode write_mode) try {
    if (size > kMaxObjectSize)
        return err(Errc::too_large, "object exceeds the configured 4 GiB maximum");
    const ObjectLayout layout = compute_layout(size, tiers_, three_layer());
    const bool ram_only = layout.ssd_bytes == 0 && layout.hdd_bytes == 0;

    // Copy-on-write (ADR-0018): write into a fresh immutable disk generation so the old version
    // stays fully live for concurrent readers until one in-memory Index swap publishes the new one.
    // Zero is reserved for RAM-only objects and for uninitialised metadata.
    const std::uint64_t file_generation =
        store_seq_->fetch_add(1, std::memory_order_relaxed) + 1;
    if (!ram_only && file_generation == 0)
        return err(Errc::out_of_space, "disk file generation space exhausted");
    auto scratch_cleanup = ScopeExit([&] {
        if (layout.ssd_bytes > 0)
            ssd_.unlink_object(digest, layout.ssd_bytes, file_generation);
        if (hdd_ && layout.hdd_bytes > 0)
            hdd_->unlink_object(digest, layout.hdd_bytes, file_generation);
    });
    // File creation is deliberately delayed until after store protection is installed. If openat
    // itself hits inode/metadata ENOSPC, capacity reclaim must not select the incarnation that this
    // copy-on-write store is replacing.
    std::optional<ObjectFiles> ssd_files;
    std::optional<ObjectFiles> hdd_files;

    // Reserve the new RAM head, evicting cold heads (ADR-0007/0012) to make room. The old version's
    // head stays put; commit frees it once the new version is published.
    std::unique_lock<std::shared_mutex> lk(*mu_); // mutates allocator + pool + policy (exclusive)

    // Disk-backed stores acquire a bounded, page-aligned staging buffer (ADR-0011). RAM-only objects
    // memcpy straight into the head and never touch the stage pool — skipping the acquire keeps
    // small-object SET concurrency off the write-buffer bound (which is sized for large tails).
    std::optional<MutBytes> stage;
    if (!ram_only) {
        auto s = write_pool_->acquire();
        if (!s) {
            lk.unlock();
            return err(Errc::would_block, "write staging buffers exhausted");
        }
        stage = *s;
    }

    bool store_protection = false;
    std::optional<core::BufferPool::Region> head;
    bool pending_small_head = false;
    auto admission_cleanup = ScopeExit([&] {
        if (!lk.owns_lock()) lk.lock(); // unlocked open/reserve/reclaim may throw under pressure
        if (head) {
            if (pending_small_head) {
                release_pending_small_head_locked(*head);
                pending_small_head = false;
            }
            ram_.deallocate(head->block, head->offset, head->len);
            head.reset();
        }
        if (store_protection) {
            release_store_protection_locked(digest);
            store_protection = false;
        }
        if (stage) write_pool_->release(*stage);
    });
    if (auto protected_store = acquire_store_protection_locked(digest); !protected_store)
        return std::unexpected(protected_store.error());
    store_protection = true;

    const Size head_len = std::min<Size>(size, tiers_.ram_head);
    // Fractional RAM-only objects use the compact arena. An object exactly one ram_head uses the
    // same fixed buddy slot as a larger object's head: these slots pack exactly into --block and a
    // completely occupied block is eligible for NUMA promotion (ADR-0008-rev).
    const Size head_min = (size < tiers_.ram_head) ? small_min_alloc_ : kDeviceBlock;
    auto& admission_policy = resident_policy(size);
    if (head_len > 0) {
        auto region = ram_.allocate(static_cast<std::uint32_t>(head_len), head_min,
                                    allocation_class(size));
        // Small class: reclaim dead arena space by sliding compaction before evicting anything live.
        // Sliding is in-place, so it frees room even at 100% RAM; eviction (below) remains the backstop.
        if (!region && head_min < kDeviceBlock && ram_.small_dead_total() >= head_len) {
            compact_small();
            region = ram_.allocate(static_cast<std::uint32_t>(head_len), head_min,
                                   allocation_class(size));
        }
        while (!region) {
            const auto victim = admission_policy.evict();
            if (!victim) break;
            if (const auto vm = index_->lookup(*victim); vm && vm->head.resident()) {
                if (vm->size <= tiers_.ram_head) {
                    // RAM-only victim: its head is the only copy, so evicting the head evicts the
                    // object (no disk files to unlink; evict() already popped it from the head policy).
                    if (auto discarded = discard_owned_score_locked(*victim, *vm); !discarded) {
                        restore_policy_or_die(admission_policy, *victim);
                        return std::unexpected(discarded.error());
                    }
                    free_head_region(vm->head.block, vm->head.offset, vm->head.len);
                    object_policy_->remove(*victim);
                    index_->erase(*victim);
                } else {
                    // A disk-backed object survives without its head, so move—not copy—its heat from
                    // the fixed NUMA slot back into the Index before releasing the physical slot.
                    auto score = extract_owned_score_locked(*victim, *vm);
                    if (!score) {
                        restore_policy_or_die(admission_policy, *victim);
                        return std::unexpected(score.error());
                    }
                    if (!index_->restore_score(*victim, *score)) {
                        // Restore the old owner before returning; metadata still names the old head.
                        if (auto rollback = install_owned_score_locked(*victim, *vm, *score);
                            !rollback)
                            fatal_score_state("cannot roll back authoritative score eviction",
                                              rollback.error());
                        restore_policy_or_die(admission_policy, *victim);
                        return err(Errc::io_error, "restore evicted head score to Index");
                    }
                    free_head_region(vm->head.block, vm->head.offset, vm->head.len);
                    index_->set_head(*victim, HeadLoc{}); // disk-backed: object stays, served from SSD
                }
            }
            // A bump-arena eviction creates dead space rather than rewinding its frontier. As soon
            // as one or more victims have freed enough bytes, compact before selecting another;
            // otherwise a single admission can unnecessarily drain the entire arena block.
            if (head_min < kDeviceBlock && ram_.small_dead_total() >= head_len)
                compact_small();
            region = ram_.allocate(static_cast<std::uint32_t>(head_len), head_min,
                                   allocation_class(size));
        }
        if (region) {
            head = *region;
            if (head_min < kDeviceBlock) {
                const auto [it, inserted] =
                    pending_small_heads_.insert(region_id(head->block, head->offset));
                (void)it;
                if (!inserted)
                    fatal_score_state(
                        "duplicate unpublished small-head reservation",
                        Error{Errc::io_error, "small-head region is already reserved"});
                pending_small_head = true;
            }
        }
    }

    // A RAM-only object that couldn't get a head (everything live is pinned) has nowhere to live ->
    // backpressure rather than index a head-less, body-less object. (head_len>0 excludes 0-byte values.)
    if (head_len > 0 && layout.ssd_bytes == 0 && !head) {
        return err(Errc::would_block, "no RAM for RAM-only head");
    }

    // Reserve physical blocks on the actual shard files before accepting body bytes. The store
    // protection and unpublished-head marker stay active while the storage lock is dropped; they
    // keep the old incarnation and this reservation out of both eviction and compaction.
    if (!ram_only) {
        lk.unlock();

        const auto open_with_reclaim = [&](Pool& pool, Size bytes) -> Result<ObjectFiles> {
            std::uint64_t failed_device = 0;
            const auto attempt = [&]() {
                failed_device = 0;
                return pool.open_object(digest, bytes, /*create=*/true, file_generation,
                                        &failed_device);
            };
            auto first = attempt();
            if (first || first.error().code != Errc::out_of_space ||
                write_mode == WriteMode::block)
                return first;
            std::unique_lock<std::mutex> reclaim_lk(*disk_reclaim_mu_);
            for (;;) {
                auto retry = attempt();
                if (retry || retry.error().code != Errc::out_of_space) return retry;
                if (failed_device == 0 || !reclaim_one_disk_object(failed_device)) return retry;
            }
        };

        if (layout.ssd_bytes > 0) {
            auto opened = open_with_reclaim(ssd_, layout.ssd_bytes);
            if (!opened) {
                lk.lock();
                return std::unexpected(opened.error());
            }
            ssd_files.emplace(std::move(*opened));
        }
        if (layout.hdd_bytes > 0 && hdd_) {
            auto opened = open_with_reclaim(*hdd_, layout.hdd_bytes);
            if (!opened) {
                lk.lock();
                return std::unexpected(opened.error());
            }
            hdd_files.emplace(std::move(*opened));
        }
        auto reserved = reserve_store_space(digest, layout, *ssd_files, hdd_files, write_mode);
        lk.lock();
        if (!reserved) return std::unexpected(reserved.error());
    }

    StoreHandle handle(this, digest, layout,
                       ssd_files ? std::move(*ssd_files) : ObjectFiles{}, std::move(hdd_files), head,
                       ram_only ? 0 : file_generation, stage.value_or(MutBytes{}), write_mode,
                       store_protection, pending_small_head);
    admission_cleanup.dismiss();
    scratch_cleanup.dismiss();
    return handle;
} catch (const std::bad_alloc&) {
    // Do not allocate while translating an allocation failure.  In particular, a non-SSO detail
    // string could throw a second bad_alloc after the admission guards have already unwound.
    return std::unexpected(Error{Errc::out_of_memory});
}

TierManager::StoreHandle::StoreHandle(TierManager* tm, Digest digest, ObjectLayout layout,
                                      ObjectFiles ssd, std::optional<ObjectFiles> hdd,
                                      std::optional<core::BufferPool::Region> head,
                                      std::uint64_t file_generation, MutBytes stage,
                                      WriteMode write_mode, bool store_protection,
                                      bool pending_small_head) noexcept
    : tm_(tm), digest_(digest), layout_(layout), ssd_(std::move(ssd)), hdd_(std::move(hdd)),
      head_(head), file_generation_(file_generation), stage_(stage), write_mode_(write_mode),
      store_protection_(store_protection), pending_small_head_(pending_small_head) {}

TierManager::StoreHandle::StoreHandle(StoreHandle&& o) noexcept
    : tm_(o.tm_), digest_(o.digest_), layout_(o.layout_), ssd_(std::move(o.ssd_)),
      hdd_(std::move(o.hdd_)), head_(o.head_), file_generation_(o.file_generation_), stage_(o.stage_),
      stage_fill_(o.stage_fill_), flushed_(o.flushed_), off_(o.off_), committed_(o.committed_),
      write_mode_(o.write_mode_), store_protection_(o.store_protection_),
      pending_small_head_(o.pending_small_head_) {
    o.tm_ = nullptr;
    o.committed_ = true; // neutralize the moved-from handle's destructor
    o.store_protection_ = false;
    o.pending_small_head_ = false;
}

TierManager::StoreHandle& TierManager::StoreHandle::operator=(StoreHandle&& o) noexcept {
    if (this != &o) {
        abort_uncommitted();
        tm_ = o.tm_;
        digest_ = o.digest_;
        layout_ = o.layout_;
        ssd_ = std::move(o.ssd_);
        hdd_ = std::move(o.hdd_);
        head_ = o.head_;
        file_generation_ = o.file_generation_;
        stage_ = o.stage_;
        stage_fill_ = o.stage_fill_;
        flushed_ = o.flushed_;
        off_ = o.off_;
        committed_ = o.committed_;
        write_mode_ = o.write_mode_;
        store_protection_ = o.store_protection_;
        pending_small_head_ = o.pending_small_head_;
        o.tm_ = nullptr;
        o.committed_ = true;
        o.store_protection_ = false;
        o.pending_small_head_ = false;
    }
    return *this;
}

TierManager::StoreHandle::~StoreHandle() { abort_uncommitted(); }

// Roll back a handle that was never committed: free the reserved head and delete the scratch files.
void TierManager::StoreHandle::abort_uncommitted() {
    if (committed_ || !tm_) return;
    if (head_ || !stage_.empty() || store_protection_) {
        std::unique_lock<std::shared_mutex> lk(*tm_->mu_); // ram_ + write_pool_ are shared (exclusive)
        if (head_) {
            if (pending_small_head_) {
                tm_->release_pending_small_head_locked(*head_);
                pending_small_head_ = false;
            }
            tm_->ram_.deallocate(head_->block, head_->offset, head_->len);
            head_.reset();
        }
        if (!stage_.empty()) {
            tm_->write_pool_->release(stage_);
            stage_ = {};
        }
        if (store_protection_) {
            tm_->release_store_protection_locked(digest_);
            store_protection_ = false;
        }
    }
    if (layout_.ssd_bytes > 0)
        tm_->ssd_.unlink_object(digest_, layout_.ssd_bytes, file_generation_);
    if (tm_->hdd_ && layout_.hdd_bytes > 0)
        tm_->hdd_->unlink_object(digest_, layout_.hdd_bytes, file_generation_);
    committed_ = true;
    tm_ = nullptr;
}

Status TierManager::StoreHandle::write(ByteView chunk) {
    if (committed_ || !tm_)
        return err(Errc::invalid_argument, "write on a completed store handle");
    const Size len = chunk.size();
    if (off_ > layout_.size || len > layout_.size - off_)
        return err(Errc::invalid_argument, "write past end of object");

    if (head_) { // fill the resident head directly from the chunk (RAM; no alignment needed)
        const Size hend = std::min<Size>(off_ + len, head_->len);
        if (off_ < hend) std::memcpy(head_->data + off_, chunk.data(), hend - off_);
    }
    if (layout_.ssd_bytes == 0 && layout_.hdd_bytes == 0) { // RAM-only: the head holds the whole object
        off_ += len;
        return {};
    }
    // Stage the body into the aligned buffer, flushing full device-block-aligned blocks to disk;
    // the trailing partial block is flushed (zero-padded) at commit. SET is write-once, so the copy
    // through this buffer is a deliberate trade for a zero-copy, page-cache-free read path.
    const std::byte* p = chunk.data();
    Size remaining = len;
    while (remaining > 0) {
        const Size take = std::min<Size>(remaining, stage_.size() - stage_fill_);
        std::memcpy(stage_.data() + stage_fill_, p, take);
        stage_fill_ += take;
        p += take;
        remaining -= take;
        if (stage_fill_ == stage_.size())
            if (auto st = flush_block(stage_.size()); !st) return st;
    }
    off_ += len;
    return {};
}

Status TierManager::StoreHandle::flush_available(Size min_complete) {
    if (committed_ || !tm_)
        return err(Errc::invalid_argument, "flush on a completed store handle");
    if (layout_.ssd_bytes == 0 && layout_.hdd_bytes == 0) return {};
    if (min_complete == 0) min_complete = kDeviceBlock;
    // Round the caller's quantum up to a device block so O_DIRECT never sees a partial write.
    const Size threshold = align_up(min_complete, kDeviceBlock);
    const Size complete = stage_fill_ - (stage_fill_ % kDeviceBlock);
    if (complete < threshold) return {};
    const Size tail = stage_fill_ - complete;
    if (auto status = flush_block(complete); !status) return status;
    if (tail > 0) std::memmove(stage_.data(), stage_.data() + complete, tail);
    stage_fill_ = tail;
    return {};
}

// Write the staged block [0, n) (n device-block-aligned) at disk offset flushed_, split across the
// SSD prefix and HDD tail. Aligned source/offset/length keep O_DIRECT happy. The SSD file is padded
// to a 4 KiB boundary only when it holds the object's tail (no HDD); with an HDD tail the prefix
// ends on the already-aligned ssd_bytes split.
Status TierManager::StoreHandle::flush_block(Size n) {
    const Offset o = flushed_;
    const Size ssd_extent =
        (layout_.hdd_bytes > 0) ? layout_.ssd_bytes : align_up(layout_.ssd_bytes, kDeviceBlock);
    std::uint64_t failed_device = 0;
    const auto attempt = [&]() -> Status {
        failed_device = 0;
        const Size ssd_stop = std::min<Size>(o + n, ssd_extent);
        if (o < ssd_stop) {
            unsigned failed_drive = 0;
            if (auto st = striped_pwrite(tm_->ssd_.drives(), digest_.bucket(), ssd_.fds(), o,
                                         ByteView(stage_.data(), ssd_stop - o), &failed_drive);
                !st) {
                if (st.error().code == Errc::out_of_space)
                    failed_device = tm_->ssd_.device_of_drive(failed_drive);
                return st;
            }
        }
        if (o + n > layout_.ssd_bytes && hdd_) {
            const Size begin = std::max<Size>(o, layout_.ssd_bytes);
            const Size src = begin - o;
            unsigned failed_drive = 0;
            if (auto st = striped_pwrite(tm_->hdd_->drives(), digest_.bucket(), hdd_->fds(),
                                         begin - layout_.ssd_bytes,
                                         ByteView(stage_.data() + src, (o + n) - begin),
                                         &failed_drive);
                !st) {
                if (st.error().code == Errc::out_of_space)
                    failed_device = tm_->hdd_->device_of_drive(failed_drive);
                return st;
            }
        }
        return {};
    };

    auto first = attempt();
    if (!first && first.error().code == Errc::out_of_space) {
        if (write_mode_ == WriteMode::block) return first;
        // Reservation-less filesystems can still fill between blocks. Only one writer evicts at a
        // time, then the complete aligned staging block is replayed. Generation files are private,
        // so replay is safe even if the failed pwrite had already modified an earlier segment.
        std::unique_lock<std::mutex> reclaim_lk(*tm_->disk_reclaim_mu_);
        for (;;) {
            auto retry = attempt();
            if (retry) break;
            if (retry.error().code != Errc::out_of_space) return retry;
            if (failed_device == 0 || !tm_->reclaim_one_disk_object(failed_device)) return retry;
        }
    } else if (!first) {
        return first;
    }
    flushed_ += n;
    stage_fill_ = 0;
    return {};
}

Result<std::uint64_t> TierManager::StoreHandle::commit(std::uint32_t flags, std::uint32_t expiry,
                                                       std::uint64_t cas_expected,
                                                       std::shared_ptr<const HttpCacheMetadata> http) {
    if (committed_ || !tm_)
        return err(Errc::invalid_argument, "commit on a completed store handle");
    if (off_ != layout_.size)
        return err(Errc::invalid_argument, "commit before the full value was written");

    // Flush the trailing partial block, zero-padded up to the device block (O_DIRECT). The file then
    // ends on a 4 KiB boundary; the padding is never served (the index records the real size).
    if (stage_fill_ > 0) {
        const Size padded = align_up(stage_fill_, kDeviceBlock);
        if (padded > stage_fill_) std::memset(stage_.data() + stage_fill_, 0, padded - stage_fill_);
        if (auto st = flush_block(padded); !st) return std::unexpected(st.error());
    }

    std::unique_lock<std::shared_mutex> lk(*tm_->mu_); // publish + swap atomically (exclusive, ADR-0018)
    const auto old = tm_->index_->lookup(digest_);
    if (cas_expected != 0 && (!old || is_expired(*old, now_unix()) || old->etag != cas_expected))
        return err(Errc::cas_mismatch); // object changed/absent under us -> don't publish (scratch aborts)

    ObjectMeta meta;
    meta.size = layout_.size;
    meta.flags = flags;
    meta.expiry = expiry; // absolute Unix time; 0 = never (ADR-0007)
    meta.etag = tm_->etag_seq_->fetch_add(1, std::memory_order_relaxed) + 1; // unique per (re)store
    meta.file_generation = file_generation_;
    if (head_) meta.head = HeadLoc{head_->block, head_->offset, head_->len};

    // First insert: no score to extract/rollback. Replacement moves the old score across.
    double logical_score = 0.0;
    if (old) {
        auto extracted = tm_->extract_owned_score_locked(digest_, *old);
        if (!extracted) return std::unexpected(extracted.error());
        logical_score = *extracted;
    }
    const auto restore_old_score = [&]() -> Status {
        if (!old) return {};
        return tm_->install_owned_score_locked(digest_, *old, logical_score);
    };
    bool new_score_published = false;
    if (tm_->fixed_score_owner(meta)) {
        if (auto published = tm_->head_scores_->publish(meta.head, logical_score); !published) {
            if (auto restored = restore_old_score(); !restored)
                fatal_score_state("cannot restore old score after dense publication failure",
                                  restored.error());
            return std::unexpected(published.error());
        }
        new_score_published = true;
    }
    const auto rollback_scores = [&]() -> Status {
        if (!old && !new_score_published) return {}; // first-insert fast path: nothing to roll back
        std::optional<Error> first_error;
        if (new_score_published) {
            auto removed = tm_->head_scores_->extract(meta.head);
            if (!removed) first_error = removed.error();
        }
        if (auto restored = restore_old_score(); !restored && !first_error)
            first_error = restored.error();
        if (first_error) return std::unexpected(std::move(*first_error));
        return {};
    };

    // Prepare every potentially allocating side structure before the Index swap. Once metadata
    // points at the new head/generation, publication must have no fallible correctness-critical
    // steps left or an exception could make the handle destructor retire live bytes.
    bool pin_inserted = false;
    if (meta.head.resident()) {
        try {
            const auto id = region_id(meta.head.block, meta.head.offset);
            auto [it, inserted] = tm_->pins_.try_emplace(id, meta.head.len);
            pin_inserted = inserted;
            if (!inserted) it->second.len = meta.head.len;
        } catch (const std::bad_alloc&) {
            if (auto rolled_back = rollback_scores(); !rolled_back)
                fatal_score_state("cannot roll back score after pin registration failure",
                                  rolled_back.error());
            return err(Errc::out_of_memory, "allocate head pin registration");
        }
    }

    if (expiry != 0) tm_->any_ttl_->store(true, std::memory_order_relaxed); // arm the reaper
    try {
        tm_->index_->set_with_score(
            digest_, meta,
            tm_->fixed_score_owner(meta) ? std::nullopt : std::optional<double>(logical_score),
            std::move(http));
    } catch (const std::bad_alloc&) {
        if (pin_inserted)
            tm_->pins_.erase(region_id(meta.head.block, meta.head.offset));
        if (auto rolled_back = rollback_scores(); !rolled_back)
            fatal_score_state("cannot roll back score after Index allocation failure",
                              rolled_back.error());
        return err(Errc::out_of_memory, "allocate Index entry for stored object");
    }
    if (pending_small_head_) {
        tm_->release_pending_small_head_locked(*head_);
        pending_small_head_ = false;
    }
    // This digest stays detached from both eviction policies for the StoreHandle's full lifetime.
    // The last concurrent handle release reattaches whichever incarnation is current. Retire only
    // the replaced bytes here; policy bookkeeping must remain absent until that release.
    if (old && old->head.resident())
        tm_->free_head_region(old->head.block, old->head.offset, old->head.len);
    // The Index swap above is the sole publication point. Old readers already hold open file
    // descriptors, so unlinking the retired immutable generation here cannot tear their stream.
    if (old && old->file_generation != 0) {
        const ObjectLayout old_layout = compute_layout(old->size, tm_->tiers_, tm_->three_layer());
        if (old_layout.ssd_bytes > 0)
            tm_->ssd_.unlink_object(digest_, old_layout.ssd_bytes, old->file_generation);
        if (tm_->hdd_ && old_layout.hdd_bytes > 0)
            tm_->hdd_->unlink_object(digest_, old_layout.hdd_bytes, old->file_generation);
    }
    if (!stage_.empty()) {
        tm_->write_pool_->release(stage_); // return the staging buffer (under the lock)
        stage_ = {};
    }
    committed_ = true;
    if (store_protection_) {
        tm_->release_store_protection_locked(digest_);
        store_protection_ = false;
    }
    const std::uint64_t etag = meta.etag;
    tm_ = nullptr;
    return etag;
}

Result<std::size_t> TierManager::read(core::Reactor& reactor, const Digest& digest, Offset offset,
                                      MutBytes out, bool record_access) {
    if (record_access) touch(digest);
    Size size = 0, ram_end = 0;
    ObjectLayout layout{};
    std::optional<ObjectFiles> ssd_files, hdd_files;

    // Snapshot under the shared lock (ADR-0018): copy the RAM head AND open the disk files for ONE
    // version, serialized against a copy-on-write commit (which publishes + swaps under the exclusive
    // lock). The opened fds pin the old inodes (unlinked-but-open), so the later lock-free disk reads
    // stay consistent with the head we copied here — never old-head + new-body.
    {
        std::shared_lock<std::shared_mutex> lk(*mu_); // read-only: shares with other readers
        const auto m = index_->lookup(digest);
        if (!m) return err(Errc::not_found, "object not in index");
        size = m->size;
        if (offset >= size) return std::size_t{0};
        layout = compute_layout(size, tiers_, three_layer());
        const Size want = std::min<Size>(out.size(), size - offset);
        if (m->head.resident()) {
            ram_end = m->head.len;
            if (offset < ram_end) {
                const Size n = std::min<Size>(offset + want, ram_end) - offset;
                std::memcpy(out.data(), ram_.addr(m->head.block, m->head.offset) + offset, n);
            }
        }
        if (std::max<Size>(offset, ram_end) < std::min<Size>(offset + want, layout.ssd_bytes)) {
            auto f = ssd_.open_object(digest, layout.ssd_bytes, /*create=*/false,
                                      m->file_generation);
            if (!f) return std::unexpected(f.error());
            ssd_files = std::move(*f);
        }
        if (three_layer() && layout.hdd_bytes > 0 && offset + want > layout.ssd_bytes) {
            auto f = hdd_->open_object(digest, layout.hdd_bytes, /*create=*/false,
                                       m->file_generation);
            if (!f) return std::unexpected(f.error());
            hdd_files = std::move(*f);
        }
    }

    // Stream the disk portions from the snapshot fds on the caller's reactor — no lock held.
    // O_DIRECT (ADR-0011): the caller's buffer is page-aligned + a device-block multiple, so each
    // portion reads straight into it with a block-aligned length (no bounce); the final block may
    // read a few padding bytes into the buffer's aligned tail, which we never serve.
    const Size want = std::min<Size>(out.size(), size - offset);
    const Size ssd_begin = std::max<Size>(offset, ram_end);
    const Size ssd_stop = std::min<Size>(offset + want, layout.ssd_bytes);
    if (ssd_begin < ssd_stop) {
        const Size len = ssd_stop - ssd_begin;
        const Size room = std::min<Size>(align_up(len, kDeviceBlock), out.size() - (ssd_begin - offset));
        auto n = striped_read(reactor, ssd_.drives(), digest.bucket(), ssd_files->fds(), ssd_begin,
                              out.subspan(ssd_begin - offset, room));
        if (!n) return std::unexpected(n.error());
    }
    if (three_layer() && layout.hdd_bytes > 0 && offset + want > layout.ssd_bytes) {
        const Size begin = std::max<Size>(offset, layout.ssd_bytes);
        const Size len = (offset + want) - begin;
        const Size room = std::min<Size>(align_up(len, kDeviceBlock), out.size() - (begin - offset));
        auto n = striped_read(reactor, hdd_->drives(), digest.bucket(), hdd_files->fds(),
                              begin - layout.ssd_bytes, out.subspan(begin - offset, room));
        if (!n) return std::unexpected(n.error());
    }
    return static_cast<std::size_t>(want);
}

static void add_drive_segs(TierManager::ReadStream::Plan& p, const DrivePool& dp,
                           std::uint64_t bucket, std::span<const int> fds, Offset tier_off, Size len,
                           Size out_base) {
    std::array<ReadSegment, TierManager::ReadStream::kMaxSegs> raw{};
    const std::size_t n = dp.plan_reads(bucket, tier_off, len, raw);
    Size running = 0;
    for (std::size_t i = 0; i < n && p.nsegs < p.segs.size(); ++i) {
        const auto& s = raw[i];
        p.segs[p.nsegs++] = {fds[s.drive], s.file_offset, s.length, out_base + running};
        p.total += static_cast<std::size_t>(s.length);
        running += s.length;
    }
}

Result<TierManager::ReadStream> TierManager::open_read(const Digest& digest) {
    // This legacy API owns its pin inside ReadStream. Keep one exclusive lock across metadata,
    // immutable-generation opens, and pin creation so it cannot combine two incarnations.
    std::unique_lock<std::shared_mutex> lk(*mu_);
    const auto found = index_->lookup(digest);
    if (!found) return err(Errc::not_found, "object not in index");
    const ObjectMeta meta = *found;
    record_access_locked(digest, meta);
    const ObjectLayout layout = compute_layout(meta.size, tiers_, three_layer());
    ObjectFiles ssd; // RAM-only objects have no SSD extent -> empty files; plan() then yields head-only
    if (layout.ssd_bytes > 0) {
        auto f = ssd_.open_object(digest, layout.ssd_bytes, /*create=*/false,
                                  meta.file_generation);
        if (!f) return std::unexpected(f.error());
        ssd = std::move(*f);
    }
    std::optional<ObjectFiles> hdd;
    if (layout.hdd_bytes > 0 && hdd_) {
        auto h = hdd_->open_object(digest, layout.hdd_bytes, /*create=*/false,
                                   meta.file_generation);
        if (!h) return std::unexpected(h.error());
        hdd.emplace(std::move(*h));
    }
    HeadPin pin;
    if (meta.head.resident()) pin_region(meta.head, pin, /*create=*/true);
    return ReadStream(this, digest, meta.size, layout, std::move(ssd), std::move(hdd), meta.head,
                      pin);
}

TierManager::ReadStream::ReadStream(ReadStream&& o) noexcept
    : tm_(o.tm_), digest_(o.digest_), size_(o.size_), layout_(o.layout_), ssd_(std::move(o.ssd_)),
      hdd_(std::move(o.hdd_)), head_(o.head_), owned_pin_(o.owned_pin_) {
    o.tm_ = nullptr;
    o.owned_pin_.valid = false;
}

TierManager::ReadStream& TierManager::ReadStream::operator=(ReadStream&& o) noexcept {
    if (this != &o) {
        if (tm_ && owned_pin_.valid) tm_->unpin_head(owned_pin_);
        tm_ = o.tm_;
        digest_ = o.digest_;
        size_ = o.size_;
        layout_ = o.layout_;
        ssd_ = std::move(o.ssd_);
        hdd_ = std::move(o.hdd_);
        head_ = o.head_;
        owned_pin_ = o.owned_pin_;
        o.tm_ = nullptr;
        o.owned_pin_.valid = false;
    }
    return *this;
}

TierManager::ReadStream::~ReadStream() {
    if (tm_ && owned_pin_.valid) tm_->unpin_head(owned_pin_);
}

TierManager::ReadStream::Plan TierManager::ReadStream::plan(Offset off, MutBytes out) {
    return plan(off, out, out.size());
}

TierManager::ReadStream::Plan TierManager::ReadStream::plan(Offset off, MutBytes out,
                                                            Size max_logical) {
    Plan p;
    if (off >= size_) return p;
    const Size want = std::min<Size>({out.size(), size_ - off, max_logical});

    // Head portion comes from the exact pinned locator captured with these immutable-generation
    // fds. Looking the digest up again here could pair an old body with a replacement's head.
    Size ram_end = 0;
    if (head_.resident()) {
        ram_end = head_.len;
        if (off < ram_end) {
            const Size n = std::min<Size>(off + want, ram_end) - off;
            std::memcpy(out.data(), tm_->ram_.addr(head_.block, head_.offset) + off, n);
            p.total += static_cast<std::size_t>(n);
        }
    }
    // SSD + HDD portions -> per-drive disk segments (read async by the caller).
    const Size ssd_begin = std::max<Size>(off, ram_end);
    const Size ssd_stop = std::min<Size>(off + want, layout_.ssd_bytes);
    if (ssd_begin < ssd_stop)
        add_drive_segs(p, tm_->ssd_.drives(), digest_.bucket(), ssd_.fds(), ssd_begin,
                       ssd_stop - ssd_begin, ssd_begin - off);
    if (hdd_ && off + want > layout_.ssd_bytes) {
        const Size begin = std::max<Size>(off, layout_.ssd_bytes);
        add_drive_segs(p, tm_->hdd_->drives(), digest_.bucket(), hdd_->fds(),
                       begin - layout_.ssd_bytes, (off + want) - begin, begin - off);
    }
    return p;
}

Result<std::size_t> TierManager::ReadStream::read(core::Reactor& reactor, Offset off,
                                                 MutBytes out) {
    const Plan p = plan(off, out);
    if (p.nsegs == 0) return p.total;

    std::array<Size, kMaxSegs> read_lengths{};
    Size expected_io = 0;
    for (std::size_t i = 0; i < p.nsegs; ++i) {
        const auto& seg = p.segs[i];
        // O_DIRECT requires a block-multiple length. Only the object's final segment is normally
        // short; read its padding into the unused aligned tail of the caller's full I/O buffer.
        const Size read_len =
            std::min<Size>(align_up(seg.len, kDeviceBlock), out.size() - seg.out_off);
        if (read_len < seg.len)
            return err(Errc::invalid_argument, "stream buffer lacks O_DIRECT tail room");
        read_lengths[i] = read_len;
        expected_io += read_len;
    }

    std::size_t submitted = 0;
    bool submission_failed = false;
    for (std::size_t i = 0; i < p.nsegs; ++i) {
        const auto& seg = p.segs[i];
        const Size read_len = read_lengths[i];
        if (!reactor.submit_read(seg.fd, seg.file_off, out.subspan(seg.out_off, read_len), 0)) {
            reactor.submit();
            if (!reactor.submit_read(seg.fd, seg.file_off,
                                     out.subspan(seg.out_off, read_len), 0)) {
                submission_failed = true;
                break;
            }
        }
        ++submitted;
    }
    if (submitted > 0) reactor.submit_and_wait(static_cast<unsigned>(submitted));

    std::array<core::Completion, kMaxSegs> completions{};
    Size disk_bytes = 0;
    std::size_t completed = 0;
    bool read_failed = false;
    while (completed < submitted) {
        const unsigned got = reactor.reap(
            std::span<core::Completion>(completions.data(), submitted - completed));
        if (got == 0) {
            reactor.submit_and_wait(1);
            continue;
        }
        for (unsigned i = 0; i < got; ++i) {
            if (completions[i].res < 0)
                read_failed = true;
            else
                disk_bytes += static_cast<Size>(completions[i].res);
        }
        completed += got;
    }
    if (submission_failed) return err(Errc::io_error, "submission queue full");
    if (read_failed) return err(Errc::io_error, "stream read failed");
    if (disk_bytes != expected_io) return err(Errc::io_error, "short stream read");
    return p.total;
}

bool TierManager::remove(const Digest& digest) {
    std::unique_lock<std::shared_mutex> lk(*mu_);
    if (!index_->contains(digest)) return false;
    if (auto dropped = drop_object(digest); !dropped) {
        disable_numa_promotion(dropped.error());
        return false;
    }
    return true;
}

Status TierManager::acquire_store_protection_locked(const Digest& digest) {
    try {
        const auto [it, inserted] = store_protections_.try_emplace(digest, 0);
        if (it->second == std::numeric_limits<unsigned>::max())
            return err(Errc::out_of_memory, "too many concurrent stores for one key");
        if (inserted) {
            // Keep the current incarnation out of every victim selector for the entire lifetime of
            // every in-flight handle for this digest. A new/absent digest simply has nothing to
            // detach yet; its eventual commit also remains detached until the last token releases.
            if (const auto current = index_->lookup(digest); current) {
                object_policy_->remove(digest);
                remove_capacity_policies_locked(digest);
                if (current->head.resident()) resident_policy(current->size).remove(digest);
            }
        }
        ++it->second;
        return {};
    } catch (const std::bad_alloc&) {
        return err(Errc::out_of_memory, "allocate in-flight store protection");
    }
}

void TierManager::release_store_protection_locked(const Digest& digest) {
    const auto it = store_protections_.find(digest);
    if (it == store_protections_.end() || it->second == 0) {
        std::fprintf(stderr, "fatal: unmatched in-flight store protection release\n");
        std::fflush(stderr);
        std::abort();
    }
    if (--it->second != 0) return;
    store_protections_.erase(it);

    try {
        // DELETE/TTL may have removed the original incarnation, and another concurrent handle may
        // have published a newer one. Reattach whatever is current now, never a stale snapshot.
        if (const auto current = index_->lookup(digest); current) {
            // This policy is the whole-disk-object selector. RAM-only entries consume no
            // filesystem capacity and must never disappear merely because a disk is full.
            if (current->file_generation != 0) {
                object_policy_->insert(digest);
                insert_capacity_policies_locked(digest, *current);
            }
            if (current->head.resident()) resident_policy(current->size).insert(digest);
        }
    } catch (const std::bad_alloc&) {
        // Policy insertion is not transactional and may already be partially visible. Continuing
        // would make live bytes permanently unevictable or double-accounted.
        std::fprintf(stderr, "fatal: cannot reattach a completed store to eviction policy\n");
        std::fflush(stderr);
        std::abort();
    }
    enforce_object_bound();
}

void TierManager::release_pending_small_head_locked(const core::BufferPool::Region& head) {
    if (pending_small_heads_.erase(region_id(head.block, head.offset)) != 1) {
        std::fprintf(stderr, "fatal: unmatched unpublished small-head reservation release\n");
        std::fflush(stderr);
        std::abort();
    }
}

Status TierManager::drop_object(const Digest& digest) {
    const auto meta = index_->lookup(digest);
    if (!meta) return err(Errc::not_found, "drop object missing from Index");
    if (auto discarded = discard_owned_score_locked(digest, *meta); !discarded)
        return discarded;
    if (meta->head.resident()) {
        free_head_region(meta->head.block, meta->head.offset, meta->head.len);
        resident_policy(meta->size).remove(digest);
    }
    object_policy_->remove(digest);
    remove_capacity_policies_locked(digest);
    const ObjectLayout layout = compute_layout(meta->size, tiers_, three_layer());
    if (layout.ssd_bytes > 0)
        ssd_.unlink_object(digest, layout.ssd_bytes, meta->file_generation);
    if (layout.hdd_bytes > 0 && hdd_)
        hdd_->unlink_object(digest, layout.hdd_bytes, meta->file_generation);
    index_->erase(digest);
    return {};
}

bool TierManager::object_uses_device(const Digest& digest, const ObjectMeta& meta,
                                     std::uint64_t device) const noexcept {
    if (meta.file_generation == 0 || device == 0) return false;
    const ObjectLayout layout = compute_layout(meta.size, tiers_, three_layer());
    const Size ssd_extent =
        layout.hdd_bytes > 0 ? layout.ssd_bytes : align_up(layout.ssd_bytes, kDeviceBlock);
    for (unsigned drive = 0; drive < ssd_.drives().num_drives(); ++drive)
        if (ssd_.device_of_drive(drive) == device &&
            ssd_.drives().file_extent(digest.bucket(), ssd_extent, drive) != 0)
            return true;
    if (hdd_) {
        const Size hdd_extent = align_up(layout.hdd_bytes, kDeviceBlock);
        for (unsigned drive = 0; drive < hdd_->drives().num_drives(); ++drive)
            if (hdd_->device_of_drive(drive) == device &&
                hdd_->drives().file_extent(digest.bucket(), hdd_extent, drive) != 0)
                return true;
    }
    return false;
}

void TierManager::insert_capacity_policies_locked(const Digest& digest, const ObjectMeta& meta) {
    for (auto& [device, policy] : capacity_policies_)
        if (object_uses_device(digest, meta, device)) policy->insert(digest);
}

void TierManager::remove_capacity_policies_locked(const Digest& digest) {
    for (auto& [device, policy] : capacity_policies_) {
        (void)device;
        policy->remove(digest);
    }
}

void TierManager::touch_capacity_policies_locked(const Digest& digest) {
    for (auto& [device, policy] : capacity_policies_) {
        (void)device;
        policy->touch(digest);
    }
}

bool TierManager::reclaim_one_disk_object(std::uint64_t device) {
    std::unique_lock<std::shared_mutex> lk(*mu_);

    // Expired disk objects have no remaining cache value and win over policy-selected live data.
    if (any_ttl_->load(std::memory_order_relaxed)) {
        const auto expired = index_->expired_keys(now_unix());
        for (const auto& digest : expired) {
            const auto meta = index_->lookup(digest);
            if (!meta || meta->file_generation == 0 || store_protected_locked(digest)) continue;
            if (!object_uses_device(digest, *meta, device)) continue;
            if (auto dropped = drop_object(digest); dropped) return true;
        }
    }

    const auto found_policy = capacity_policies_.find(device);
    if (found_policy == capacity_policies_.end()) return false;
    auto& capacity_policy = *found_policy->second;
    while (const auto victim = capacity_policy.evict()) {
        // Stale policy entries are harmless. Protected stores are removed on acquisition, but keep
        // this guard because reclaim correctness must not depend on policy implementation details.
        const auto meta = index_->lookup(*victim);
        if (!meta) continue;
        if (store_protected_locked(*victim)) {
            restore_policy_or_die(capacity_policy, *victim);
            return false;
        }
        if (!object_uses_device(*victim, *meta, device)) continue;
        if (auto dropped = drop_object(*victim); dropped) return true;
        restore_policy_or_die(capacity_policy, *victim);
        return false;
    }
    return false;
}

void TierManager::enforce_object_bound() {
    while (max_objects_ != 0 && object_policy_->resident() > max_objects_) {
        const auto victim = object_policy_->evict();
        if (!victim) break;
        if (auto dropped = drop_object(*victim); !dropped) {
            restore_policy_or_die(*object_policy_, *victim);
            disable_numa_promotion(dropped.error());
            break;
        }
    }
}

std::optional<ByteView> TierManager::head_view(const Digest& digest) {
    touch(digest);
    const auto meta = index_->lookup(digest);
    if (!meta || !meta->head.resident()) return std::nullopt;
    return ByteView(ram_.addr(meta->head.block, meta->head.offset), meta->head.len);
}

void TierManager::touch(const Digest& digest) {
    std::unique_lock<std::shared_mutex> lk(*mu_); // touch mutates the policy (visited bit)
    const auto meta = index_->lookup(digest);
    if (meta && meta->head.resident()) resident_policy(meta->size).touch(digest);
    object_policy_->touch(digest);
    touch_capacity_policies_locked(digest);
    if (meta) record_access_locked(digest, *meta);
}

void TierManager::touch_policies(const Digest& digest, Size object_size, bool head_resident) {
    // Membership changes take mu_ exclusively. Hits only atomically set S3-FIFO visited bits, so
    // readers share this lock instead of serializing every successful GET.
    std::shared_lock<std::shared_mutex> lk(*mu_);
    object_policy_->touch(digest);
    touch_capacity_policies_locked(digest);
    if (head_resident) resident_policy(object_size).touch(digest);
}

std::optional<double> TierManager::access_score(const Digest& digest) const {
    std::shared_lock<std::shared_mutex> lk(*mu_);
    const auto meta = index_->lookup(digest);
    if (!meta) return std::nullopt;
    if (fixed_score_owner(*meta)) return head_scores_->score(meta->head);
    return index_->score(digest);
}

Result<double> TierManager::extract_owned_score_locked(const Digest& digest,
                                                        const ObjectMeta& meta) {
    if (fixed_score_owner(meta)) {
        if (!index_->score_external(digest))
            return err(Errc::io_error,
                       "full resident head has a second score in the Index");
        return head_scores_->extract(meta.head);
    }
    if (index_->score_external(digest))
        return err(Errc::io_error,
                   "non-fixed object has no score in the Index");
    const auto score = index_->extract_score(digest);
    if (!score) return err(Errc::not_found, "object score is absent from the Index");
    return *score;
}

Status TierManager::install_owned_score_locked(const Digest& digest, const ObjectMeta& meta,
                                                double value) {
    if (fixed_score_owner(meta)) {
        bool moved_index_score = false;
        if (!index_->score_external(digest)) {
            const auto placeholder = index_->extract_score(digest);
            if (!placeholder)
                return err(Errc::io_error,
                           "cannot mark full-head Index score as externally owned");
            if (*placeholder != value) {
                if (!index_->restore_score(digest, *placeholder))
                    return err(Errc::io_error,
                               "cannot restore conflicting full-head Index score");
                return err(Errc::io_error,
                           "full-head score conflicts with its Index placeholder");
            }
            moved_index_score = true;
        }
        if (auto published = head_scores_->publish(meta.head, value); !published) {
            if (moved_index_score && !index_->restore_score(digest, value))
                return err(Errc::io_error,
                           "cannot restore Index score after dense publication failure");
            return published;
        }
        return {};
    }

    if (index_->score_external(digest)) {
        if (!index_->restore_score(digest, value))
            return err(Errc::io_error, "cannot restore object score to the Index");
        return {};
    }
    const auto current = index_->score(digest);
    if (!current || *current != value)
        return err(Errc::io_error, "object already has a conflicting Index score");
    return {};
}

Status TierManager::discard_owned_score_locked(const Digest& digest, const ObjectMeta& meta) {
    (void)digest;
    if (fixed_score_owner(meta)) {
        // The Index entry is erased immediately after this cleanup. Remove any dense owner even if
        // an earlier invariant violation also left a numeric Index value. An already-empty slot is
        // likewise safe: deletion cannot leave stale score state behind for its next allocator user.
        auto removed = head_scores_->extract(meta.head);
        if (!removed && removed.error().code != Errc::not_found)
            return std::unexpected(removed.error());
    }
    return {};
}

void TierManager::record_access_locked(const Digest& digest, const ObjectMeta& meta) {
    if (fixed_score_owner(meta)) {
        if (auto status = head_scores_->increment(meta.head, access_score_.increment); !status)
            fatal_score_state("cannot increment authoritative dense score", status.error());
        return;
    }
    if (!index_->increment_score(digest, access_score_.increment))
        fatal_score_state(
            "cannot increment authoritative Index score",
            Error{Errc::io_error, "Index-owned object score is absent or externally marked"});
}

void TierManager::disable_numa_promotion(const Error& error) {
    if (!numa_promotion_healthy_->exchange(false, std::memory_order_relaxed)) return;
    std::fprintf(stderr, "NUMA promotion disabled: %s\n", error.detail.c_str());
}

void TierManager::decay_access_scores() {
    auto& gate = *score_maintenance_;
    struct PendingRescore {
        explicit PendingRescore(std::atomic<bool>& pending) : pending_(pending) {
            pending_.store(true, std::memory_order_release);
        }
        ~PendingRescore() { pending_.store(false, std::memory_order_release); }
        std::atomic<bool>& pending_;
    } pending(gate.rescore_pending);

    // A current block exchange may finish, but its tight loop sees rescore_pending before starting
    // another. The storage lock keeps ownership transfers and block swaps disjoint from decay;
    // individual score operations themselves are relaxed atomics.
    std::lock_guard maintenance(gate.operation);
    std::unique_lock<std::shared_mutex> lk(*mu_);
    index_->decay_scores(access_score_.decay);
    if (auto status = head_scores_->decay(access_score_.decay); !status) {
        // Each NUMA worker owns a disjoint authoritative slice. Dispatch failure can arrive after
        // some workers completed the epoch, so neither retrying the whole table nor continuing with
        // a mixed epoch is correct. Stop explicitly instead of silently freezing or double-decaying
        // an unknown subset.
        fatal_score_state("authoritative NUMA decay failed; epoch may be partial", status.error());
    }
}

bool TierManager::promote_hot_remote_block() {
    auto& gate = *score_maintenance_;
    if (!numa_promotion_healthy_->load(std::memory_order_relaxed)) return false;
    if (gate.rescore_pending.load(std::memory_order_acquire)) return false;

    // Do not reserve the maintenance gate while waiting for ordinary storage readers/writers. A
    // pending decay announces itself first, so this lock order cannot starve or deadlock it.
    std::unique_lock<std::shared_mutex> lk(*mu_);
    if (!numa_promotion_healthy_->load(std::memory_order_relaxed)) return false;
    if (gate.rescore_pending.load(std::memory_order_acquire)) return false;
    std::unique_lock maintenance(gate.operation, std::try_to_lock);
    if (!maintenance.owns_lock()) return false;
    // Decay may have announced itself between either earlier check and try_lock(). Yield immediately
    // so the promotion thread's no-sleep success loop cannot overtake it.
    if (gate.rescore_pending.load(std::memory_order_acquire)) return false;

    if (ram_.region_count() < 2) return false;

    // The node workers see only their node-bound atomic doubles. This coordinator supplies the
    // comparatively tiny set of pinned block numbers and receives one extrema summary per node.
    std::vector<unsigned> pinned_blocks;
    pinned_blocks.reserve(pins_.size());
    for (const auto& [id, pin] : pins_)
        if (pin.refcount.load(std::memory_order_relaxed) > 0)
            pinned_blocks.push_back(static_cast<unsigned>(id >> 32));
    const auto candidates = head_scores_->promotion_candidates(pinned_blocks);
    if (!candidates) {
        disable_numa_promotion(candidates.error());
        return false;
    }
    const auto& cold_local = candidates->cold_preferred;
    const auto& hot_remote = candidates->hot_foreign;
    if (!cold_local || !hot_remote || hot_remote->score <= cold_local->score) return false;
    const auto started = std::chrono::steady_clock::now();
    const auto cold_block = static_cast<unsigned>(cold_local->index);
    const auto hot_block = static_cast<unsigned>(hot_remote->index);
    if (!ram_.swap_blocks(cold_block, hot_block)) return false;
    index_->swap_head_blocks(cold_block, hot_block);
    if (auto status = head_scores_->swap_blocks(cold_block, hot_block); !status)
        fatal_score_state("cannot swap authoritative score slices after RAM block swap",
                          status.error());
    const auto elapsed = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count());
    constexpr auto relaxed = std::memory_order_relaxed;
    numa_promotion_count_->fetch_add(1, relaxed);
    numa_promotion_bytes_->fetch_add(2 * ram_.block_bytes(), relaxed);
    numa_promotion_total_ns_->fetch_add(elapsed, relaxed);
    std::uint64_t old_max = numa_promotion_max_ns_->load(relaxed);
    while (old_max < elapsed &&
           !numa_promotion_max_ns_->compare_exchange_weak(old_max, elapsed, relaxed)) {}
    return true;
}

TierManager::NumaPromotionStats TierManager::numa_promotion_stats() const noexcept {
    constexpr auto relaxed = std::memory_order_relaxed;
    return {
        numa_promotion_count_->load(relaxed),
        numa_promotion_bytes_->load(relaxed),
        numa_promotion_total_ns_->load(relaxed),
        numa_promotion_max_ns_->load(relaxed),
    };
}

std::size_t TierManager::head_resident() const {
    std::shared_lock<std::shared_mutex> lk(*mu_);
    // In-flight stores temporarily detach their digest from eviction policies. The Index remains
    // authoritative for residency, so stats must count metadata rather than policy membership.
    return index_->resident_heads().size();
}

bool TierManager::pin_region(const HeadLoc& loc, HeadPin& out, bool create) {
    const auto id = region_id(loc.block, loc.offset);
    auto it = pins_.find(id);
    if (it == pins_.end()) {
        if (!create) return false;
        it = pins_.try_emplace(id, loc.len).first;
    } else if (create) {
        // `create` is used only while holding mu_ exclusively. Shared-lock snapshot pins must never
        // write this plain field; concurrent readers modify only the atomic refcount.
        it->second.len = loc.len;
    } else if (it->second.len != loc.len) {
        return false;
    }
    it->second.refcount.fetch_add(1, std::memory_order_relaxed);
    out = HeadPin{loc.block, loc.offset, loc.len, true};
    return true;
}

std::optional<TierManager::HeadPin> TierManager::pin_head(const Digest& digest) {
    std::unique_lock<std::shared_mutex> lk(*mu_);
    const auto meta = index_->lookup(digest);
    if (!meta) return std::nullopt;
    record_access_locked(digest, *meta);
    object_policy_->touch(digest); // the whole object was accessed
    touch_capacity_policies_locked(digest);
    if (!meta->head.resident()) return std::nullopt;
    resident_policy(meta->size).touch(digest); // head-cache hit
    HeadPin pin;
    pin_region(meta->head, pin, /*create=*/true);
    return pin;
}

void TierManager::unpin_head(const HeadPin& pin) {
    if (!pin.valid) return;
    // Common path: just drop the refcount. Only the last unpin that finds orphaned bytes takes the
    // exclusive lock to free RAM (and erase the pin slot).
    const auto id = region_id(pin.block, pin.offset);
    {
        std::shared_lock<std::shared_mutex> lk(*mu_);
        const auto it = pins_.find(id);
        if (it == pins_.end()) return;
        const unsigned prev = it->second.refcount.fetch_sub(1, std::memory_order_acq_rel);
        if (prev > 1) return; // still held by other readers
        if (prev == 0) {      // underflow guard (should not happen)
            it->second.refcount.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // prev == 1: we were the last holder. If not orphaned, leave the zero-refcount slot for
        // future shared-lock pins (pre-registered at commit). If orphaned, free below.
        if (!it->second.orphaned.load(std::memory_order_acquire)) return;
    }
    std::unique_lock<std::shared_mutex> lk(*mu_);
    const auto it = pins_.find(id);
    if (it == pins_.end()) return;
    // Another pin may have raced in after we dropped to zero; only free when still zero + orphaned.
    if (it->second.refcount.load(std::memory_order_acquire) == 0 &&
        it->second.orphaned.load(std::memory_order_acquire)) {
        ram_.deallocate(pin.block, pin.offset, pin.len);
        pins_.erase(it);
    }
}

ByteView TierManager::pinned_bytes(const HeadPin& pin) const {
    return ByteView(ram_.addr(pin.block, pin.offset), pin.len);
}

// Free a head's RAM, or — if a reader has it pinned — orphan it so the last unpin frees it.
// Called under the exclusive storage lock (by begin_store / drop_object / commit).
void TierManager::free_head_region(unsigned block, std::uint32_t offset, std::uint32_t len) {
    const auto it = pins_.find(region_id(block, offset));
    if (it != pins_.end() && it->second.refcount.load(std::memory_order_acquire) > 0) {
        it->second.orphaned.store(true, std::memory_order_release);
    } else {
        if (it != pins_.end()) pins_.erase(it);
        ram_.deallocate(block, offset, len);
    }
}

// Reclaim dead space in fragmented small (arena) blocks by sliding their live heads down to squeeze
// out the holes and rewinding the frontier -- in-place, so it needs no spare block and works at 100%
// RAM. A block with any pinned head (a reader is streaming it) is skipped this pass. Heads are movable
// because they're reachable only through the index (HeadLoc), which we rewrite here (ADR-0008/0018).
// Called under the exclusive storage lock from the admission path.
void TierManager::compact_small() {
    struct Slot { std::uint32_t offset; std::uint32_t len; Digest digest; };
    std::unordered_map<unsigned, std::vector<Slot>> by_block;
    for (const auto& [d, h] : index_->resident_heads()) {
        if (!ram_.small_arena(h.block)) continue; // only small (byte-granular arena) blocks compact here
        by_block[h.block].push_back(Slot{h.offset, h.len, d});
    }
    for (auto& [block, slots] : by_block) {
        auto* arena = ram_.small_arena(block);
        if (!arena || arena->dead() == 0) continue; // no holes to squeeze out
        const auto in_block = [block](std::uint64_t id) {
            return static_cast<unsigned>(id >> 32) == block;
        };
        if (std::any_of(pending_small_heads_.begin(), pending_small_heads_.end(), in_block))
            continue; // unpublished StoreHandle bytes are not represented by `slots`
        bool pinned = false; // a reader is sending a head here -> leave it, retry later
        for (const auto& [id, pin] : pins_)
            if (in_block(id) && pin.refcount.load(std::memory_order_relaxed) > 0) {
                pinned = true;
                break;
            }
        if (pinned) continue;
        std::sort(slots.begin(), slots.end(),
                  [](const Slot& a, const Slot& b) { return a.offset < b.offset; });
        Size dest = 0;
        for (const auto& s : slots) {
            if (s.offset != dest) { // slide down into the reclaimed hole; rewrite the head locator
                std::memmove(ram_.addr(block, static_cast<std::uint32_t>(dest)),
                             ram_.addr(block, s.offset), s.len);
                index_->set_head(s.digest, HeadLoc{block, static_cast<std::uint32_t>(dest), s.len});
                // Relocate any zero-refcount pin pre-registration to the new offset.
                const auto old_id = region_id(block, s.offset);
                const auto new_id = region_id(block, static_cast<std::uint32_t>(dest));
                if (auto pit = pins_.find(old_id); pit != pins_.end()) {
                    RegionPin moved(std::move(pit->second));
                    pins_.erase(pit);
                    pins_.try_emplace(new_id, std::move(moved));
                }
            }
            dest += arena->slot_size(s.len);
        }
        arena->set_frontier(dest); // [dest, block) is free room again; live count is unchanged
    }
}

std::optional<TierManager::Snapshot> TierManager::open_snapshot(const Digest& digest,
                                                                bool record_access,
                                                                std::uint32_t now,
                                                                bool include_http_metadata) {
    if (now == 0) now = now_unix();

    // Prefer a shared lock: pin refcounts are atomic, score increments are atomic, and S3-FIFO
    // touch only sets a visited bit. Map insert for a missing pin slot still needs exclusive —
    // that is the cold path (pre-registered at commit for newly stored heads).
    auto fill = [&](bool create_pin) -> std::optional<Snapshot> {
        ObjectMeta value;
        std::shared_ptr<const HttpCacheMetadata> http;
        if (include_http_metadata) {
            const auto record = index_->lookup_with_http(digest);
            if (!record) return std::nullopt;
            value = record->meta;
            http = record->http;
        } else {
            const auto m = index_->lookup(digest);
            if (!m) return std::nullopt;
            value = *m;
        }
        const ObjectMeta* m = &value;
        if (is_expired(*m, now)) return std::nullopt; // TTL passed -> lazy miss (reaper reclaims)
        // Score update is atomic (no exclusive needed). Eviction policy touch is deferred to
        // begin_get via touch() after the snapshot returns — keeps openat off the policy path and
        // shortens the lock hold to meta + pin + open only.
        if (record_access) record_access_locked(digest, *m);
        const Size head_len = m->head.resident() ? m->head.len : 0;

        Snapshot snap;
        snap.meta = *m;
        snap.http = std::move(http);
        // Open the disk files first (under the lock), so an open failure needs no pin rollback.
        if (m->size > head_len) {
            const ObjectLayout layout = compute_layout(m->size, tiers_, three_layer());
            auto ssd = ssd_.open_object(digest, layout.ssd_bytes, /*create=*/false,
                                        m->file_generation);
            if (!ssd) return std::nullopt;
            std::optional<ObjectFiles> hdd;
            if (layout.hdd_bytes > 0 && hdd_) {
                auto h = hdd_->open_object(digest, layout.hdd_bytes, /*create=*/false,
                                           m->file_generation);
                if (!h) return std::nullopt;
                hdd.emplace(std::move(*h));
            }
            snap.rs.emplace(ReadStream(this, digest, m->size, layout, std::move(*ssd),
                                       std::move(hdd), m->head, HeadPin{}));
        }
        if (m->head.resident()) {
            if (!pin_region(m->head, snap.pin, create_pin)) return std::nullopt; // need exclusive create
        }
        return snap;
    };

    {
        std::shared_lock<std::shared_mutex> lk(*mu_);
        if (auto snap = fill(/*create_pin=*/false)) return snap;
        // Miss (absent/expired) or pin slot missing. Distinguish: absent stays a miss.
        const auto m = index_->lookup(digest);
        if (!m || is_expired(*m, now)) return std::nullopt;
        if (!m->head.resident()) {
            // No head to pin — fill() should have succeeded; rebuild under shared is fine.
            return fill(/*create_pin=*/false);
        }
        // Head resident but pin slot not yet registered -> exclusive path below.
    }
    std::unique_lock<std::shared_mutex> lk(*mu_);
    return fill(/*create_pin=*/true);
}

} // namespace goblin::storage
