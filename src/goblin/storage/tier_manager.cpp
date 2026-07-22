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
#include <sys/statvfs.h>
#include <unistd.h>
#include <utility>

namespace goblin::storage {

namespace {

std::int64_t now_unix_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

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

bool capacity_errno(int error_number) noexcept {
    return error_number == ENOSPC || error_number == EDQUOT;
}

bool device_fault_errno(int error_number) noexcept {
    switch (error_number) {
        case EIO:
        case ENXIO:
        case ENODEV:
        case EROFS:
#ifdef EREMOTEIO
        case EREMOTEIO:
#endif
            return true;
        default:
            return false;
    }
}

bool device_gone_errno(int error_number) noexcept {
    return error_number == ENXIO || error_number == ENODEV;
}

bool object_poison_errno(int error_number) noexcept {
    return error_number == ENOENT || error_number == ESTALE || device_fault_errno(error_number);
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
ObjectFiles::ObjectFiles(
    std::span<const int> src,
    std::span<const std::shared_ptr<CachedFileDescriptor>> cached) noexcept {
    n_ = static_cast<unsigned>(std::min<std::size_t>(src.size(), kMaxPoolDrives));
    for (unsigned i = 0; i < n_; ++i) {
        fds_[i] = src[i];
        if (i < cached.size()) cached_[i] = cached[i];
    }
    for (unsigned i = n_; i < kMaxPoolDrives; ++i) fds_[i] = -1;
}
ObjectFiles::~ObjectFiles() {
    for (unsigned i = 0; i < n_; ++i)
        if (fds_[i] >= 0 && !cached_[i]) ::close(fds_[i]);
}
ObjectFiles::ObjectFiles(ObjectFiles&& o) noexcept
    : fds_(o.fds_), cached_(std::move(o.cached_)), n_(o.n_) {
    for (unsigned i = 0; i < o.n_; ++i) {
        o.fds_[i] = -1;
        o.cached_[i].reset();
    }
    o.n_ = 0;
}
ObjectFiles& ObjectFiles::operator=(ObjectFiles&& o) noexcept {
    if (this != &o) {
        for (unsigned i = 0; i < n_; ++i)
            if (fds_[i] >= 0 && !cached_[i]) ::close(fds_[i]);
        fds_ = o.fds_;
        cached_ = std::move(o.cached_);
        n_ = o.n_;
        for (unsigned i = 0; i < o.n_; ++i) {
            o.fds_[i] = -1;
            o.cached_[i].reset();
        }
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
    : drives_(o.drives_), dirs_(std::move(o.dirs_)), dirfds_(std::move(o.dirfds_)),
      devices_(std::move(o.devices_)),
      direct_io_(o.direct_io_), file_handles_(std::move(o.file_handles_)) {}
Pool& Pool::operator=(Pool&& o) noexcept {
    if (this != &o) {
        for (const int fd : dirfds_)
            if (fd >= 0) ::close(fd);
        drives_ = o.drives_;
        dirs_ = std::move(o.dirs_);
        dirfds_ = std::move(o.dirfds_);
        devices_ = std::move(o.devices_);
        direct_io_ = o.direct_io_;
        file_handles_ = std::move(o.file_handles_);
    }
    return *this;
}

std::vector<Pool::FilesystemCapacity> Pool::filesystem_capacity() const {
    std::vector<FilesystemCapacity> out;
    out.reserve(dirfds_.size());
    for (std::size_t drive = 0; drive < dirfds_.size(); ++drive) {
        const std::uint64_t device = devices_[drive];
        if (std::any_of(out.begin(), out.end(), [device](const auto& fs) {
                return fs.device == device;
            }))
            continue;
        struct statvfs fs {};
        if (::fstatvfs(dirfds_[drive], &fs) != 0) continue;
        const Size block = fs.f_frsize ? static_cast<Size>(fs.f_frsize)
                                       : static_cast<Size>(fs.f_bsize);
        const Size total = static_cast<Size>(fs.f_blocks) * block;
        const Size kernel_free = static_cast<Size>(fs.f_bfree) * block;
        const Size available = static_cast<Size>(fs.f_bavail) * block;
        const std::uint64_t total_inodes = static_cast<std::uint64_t>(fs.f_files);
        const std::uint64_t free_inodes = static_cast<std::uint64_t>(fs.f_ffree);
        const std::uint64_t available_inodes = static_cast<std::uint64_t>(fs.f_favail);
        out.push_back({device, drive < dirs_.size() ? dirs_[drive] : std::string{},
                       total, total - kernel_free, kernel_free, available, block,
                       total_inodes, free_inodes <= total_inodes ? total_inodes - free_inodes : 0,
                       free_inodes,
                       available_inodes});
    }
    return out;
}

Result<Pool> Pool::open(const std::vector<std::string>& dirs, Size stripe_unit, bool direct_io,
                        std::shared_ptr<FileHandleCache> file_handles) {
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
    return Pool(DrivePool(static_cast<unsigned>(dirs.size()), stripe_unit), dirs,
                std::move(dirfds), std::move(devices), direct_io, std::move(file_handles));
}

Result<ObjectFiles> Pool::open_object(const Digest& digest, Size tier_bytes, bool create,
                                      std::uint64_t generation,
                                      std::uint64_t* failed_device,
                                      int* failed_errno) const {
    const unsigned n = drives_.num_drives();
    if (n > kMaxPoolDrives) return err(Errc::invalid_argument, "pool has too many drives");
    std::array<int, kMaxPoolDrives> fds{};
    std::array<bool, kMaxPoolDrives> created{};
    std::array<std::shared_ptr<CachedFileDescriptor>, kMaxPoolDrives> cached{};
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
        int fd = -1;
        int open_errno = 0;
        if (!create && file_handles_) {
            auto opened = file_handles_->open_read(
                FileHandleCache::Key{digest, generation, dirfds_[d]}, name, flags, &open_errno);
            if (opened) {
                cached[d] = std::move(*opened);
                fd = cached[d]->get();
            }
        } else {
            fd = ::openat(dirfds_[d], name, flags, 0644);
            if (fd < 0) open_errno = errno;
        }
        if (fd < 0) {
            if (failed_errno) *failed_errno = open_errno;
            for (unsigned i = 0; i < n; ++i)
                if (fds[i] >= 0 && !cached[i]) ::close(fds[i]);
            for (auto& descriptor : cached) descriptor.reset();
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
    if (create) return ObjectFiles(std::span<const int>(fds.data(), n));
    return ObjectFiles(std::span<const int>(fds.data(), n),
                       std::span<const std::shared_ptr<CachedFileDescriptor>>(cached.data(), n));
}

Status Pool::reserve_object(const Digest& digest, Size tier_bytes,
                            const ObjectFiles& files, std::uint64_t* failed_device,
                            int* failed_errno) const {
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
        if (failed_device) *failed_device = device_of_drive(drive);
        if (failed_errno) *failed_errno = e;
        if (e == ENOSPC || e == EDQUOT) {
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
    (void)failed_errno;
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
        if (file_handles_)
            file_handles_->invalidate(FileHandleCache::Key{digest, generation, dirfds_[d]});
    }
}

// ---------------- TierManager ----------------

Result<TierManager> TierManager::open(const TierSizes& t, const MemoryConfig& mem,
                                      const EvictionConfig& ev, const PoolConfig& ssd,
                                      const PoolConfig& hdd, Index& index, Size io_chunk,
                                      unsigned write_buffers, bool direct_io,
                                      AccessScoreConfig access_score, Size write_io_chunk,
                                      Size max_object_size, unsigned file_handle_cache) {
    if (max_object_size == 0 || max_object_size > kMaxObjectSize)
        return err(Errc::invalid_argument,
                   "max object size must be between 1 byte and the 4 GiB hard limit");
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

    auto file_handles = FileHandleCache::create(file_handle_cache);
    if (!file_handles) return std::unexpected(file_handles.error());
    auto sp = Pool::open(ssd.dirs, ssd.stripe_unit, direct_io, *file_handles);
    if (!sp) return std::unexpected(sp.error());
    std::optional<Pool> hp;
    if (!hdd.dirs.empty()) {
        auto h = Pool::open(hdd.dirs, hdd.stripe_unit, direct_io, *file_handles);
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
                   ev.high_watermark, ev.low_watermark,
                   std::make_unique<NumaHeadScoreTable>(std::move(*head_scores)), max_object_size);
    try {
        const auto add_capacity_domain = [&](std::uint64_t device) {
            if (!tm.capacity_policies_.contains(device))
                tm.capacity_policies_.emplace(device, make_eviction_policy(ev.policy, obj_cap));
        };
        for (unsigned drive = 0; drive < tm.ssd_.drives().num_drives(); ++drive) {
            const auto device = tm.ssd_.device_of_drive(drive);
            add_capacity_domain(device);
            tm.register_device(device, tm.ssd_.path_of_drive(drive), Tier::ssd);
        }
        if (tm.hdd_) {
            for (unsigned drive = 0; drive < tm.hdd_->drives().num_drives(); ++drive) {
                const auto device = tm.hdd_->device_of_drive(drive);
                add_capacity_domain(device);
                tm.register_device(device, tm.hdd_->path_of_drive(drive), Tier::hdd);
            }
        }
    } catch (const std::bad_alloc&) {
        return err(Errc::out_of_memory, "allocate per-filesystem eviction policy");
    }
    tm.write_pool_ = std::make_unique<core::IoBufferPool>(std::move(*wp));
    return tm;
}

void TierManager::register_device(std::uint64_t device, std::string_view path, Tier tier) {
    auto [it, inserted] = device_io_.try_emplace(device);
    if (inserted) {
        it->second = std::make_unique<DeviceIoCounters>();
        it->second->device = device;
    }
    auto& counters = *it->second;
    counters.tier_mask |= tier == Tier::ssd ? 1u : 2u;
    if (!path.empty()) {
        const std::string needle(path);
        const bool already = counters.paths == needle ||
            counters.paths.starts_with(needle + ",") ||
            counters.paths.ends_with("," + needle) ||
            counters.paths.find("," + needle + ",") != std::string::npos;
        if (!already) {
            if (!counters.paths.empty()) counters.paths += ',';
            counters.paths += path;
        }
    }
}

TierManager::DeviceIoCounters* TierManager::device_counters(
    std::uint64_t device) const noexcept {
    const auto found = device_io_.find(device);
    return found == device_io_.end() ? nullptr : found->second.get();
}

std::string_view TierManager::health_state_name(StorageHealthState state) noexcept {
    switch (state) {
        case StorageHealthState::healthy: return "healthy";
        case StorageHealthState::degraded: return "degraded";
        case StorageHealthState::read_only: return "read_only";
        case StorageHealthState::failed: return "failed";
    }
    return "failed";
}

void TierManager::note_device_read(std::uint64_t device, int error_number) noexcept {
    if (error_number <= 0) return;
    auto* counters = device_counters(device);
    if (!counters) return;
    constexpr auto relaxed = std::memory_order_relaxed;
    counters->read_errors.fetch_add(1, relaxed);
    if (capacity_errno(error_number)) counters->capacity_errors.fetch_add(1, relaxed);
    counters->last_errno.store(error_number, relaxed);
    counters->last_error_unix.store(now_unix(), relaxed);
    if (!device_fault_errno(error_number)) return;
    const auto target = device_gone_errno(error_number) ? StorageHealthState::failed
                                                         : StorageHealthState::degraded;
    auto current = counters->state.load(relaxed);
    while (current < target && !counters->state.compare_exchange_weak(
               current, target, relaxed, relaxed)) {}
}

void TierManager::note_device_write(std::uint64_t device, int error_number) noexcept {
    if (error_number <= 0) return;
    auto* counters = device_counters(device);
    if (!counters) return;
    constexpr auto relaxed = std::memory_order_relaxed;
    counters->write_errors.fetch_add(1, relaxed);
    if (capacity_errno(error_number)) counters->capacity_errors.fetch_add(1, relaxed);
    counters->last_errno.store(error_number, relaxed);
    counters->last_error_unix.store(now_unix(), relaxed);
    if (!device_fault_errno(error_number)) return;
    const auto target = device_gone_errno(error_number) ? StorageHealthState::failed
                                                         : StorageHealthState::read_only;
    auto current = counters->state.load(relaxed);
    while (current < target && !counters->state.compare_exchange_weak(
               current, target, relaxed, relaxed)) {}
}

bool TierManager::layout_uses_device(const Digest& digest, const ObjectLayout& layout,
                                     std::uint64_t device) const noexcept {
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

Status TierManager::ensure_store_devices_writable(const Digest& digest,
                                                   const ObjectLayout& layout) const {
    for (const auto& [device, owned] : device_io_) {
        const auto state = owned->state.load(std::memory_order_relaxed);
        if (state >= StorageHealthState::read_only &&
            layout_uses_device(digest, layout, device))
            return err(Errc::read_only,
                       "backing device is " + std::string(health_state_name(state)));
    }
    return {};
}

Status TierManager::reserve_store_space(const Digest& digest, const ObjectLayout& layout,
                                        const ObjectFiles& ssd_files,
                                        const std::optional<ObjectFiles>& hdd_files,
                                        WriteMode write_mode) {
    const Size ssd_extent =
        layout.hdd_bytes > 0 ? layout.ssd_bytes : align_up(layout.ssd_bytes, kDeviceBlock);
    const Size hdd_extent = align_up(layout.hdd_bytes, kDeviceBlock);
    std::uint64_t failed_device = 0;
    int failed_errno = 0;
    const auto attempt = [&]() -> Status {
        failed_device = 0;
        failed_errno = 0;
        if (auto writable = ensure_store_devices_writable(digest, layout); !writable)
            return writable;
        if (ssd_extent > 0) {
            if (auto st =
                    ssd_.reserve_object(digest, ssd_extent, ssd_files, &failed_device,
                                        &failed_errno);
                !st) {
                note_device_write(failed_device, failed_errno);
                return st;
            }
        }
        if (hdd_extent > 0 && hdd_ && hdd_files) {
            if (auto st =
                    hdd_->reserve_object(digest, hdd_extent, *hdd_files, &failed_device,
                                         &failed_errno);
                !st) {
                note_device_write(failed_device, failed_errno);
                return st;
            }
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

bool TierManager::unavailable_locked(const ObjectMeta& meta, std::uint32_t now,
                                     std::int64_t now_ns) const noexcept {
    if (is_expired(meta, now)) return true;
    const std::int64_t cutoff = flush_deadline_ns_->load(std::memory_order_relaxed);
    return cutoff != 0 && now_ns >= cutoff && meta.stored_at_ns <= cutoff;
}

std::optional<ObjectMeta> TierManager::lookup_live(const Digest& digest, std::uint32_t now) const {
    if (now == 0) now = now_unix();
    const std::int64_t now_ns = now_unix_ns();
    std::shared_lock<std::shared_mutex> lk(*mu_);
    const auto meta = index_->lookup(digest);
    if (!meta || unavailable_locked(*meta, now, now_ns)) return std::nullopt;
    return meta;
}

void TierManager::flush_all(std::uint32_t delay_seconds) {
    const std::int64_t now_ns = now_unix_ns();
    const std::int64_t delay_ns =
        static_cast<std::int64_t>(delay_seconds) * INT64_C(1000000000);
    const std::int64_t cutoff =
        delay_ns > std::numeric_limits<std::int64_t>::max() - now_ns
            ? std::numeric_limits<std::int64_t>::max()
            : now_ns + delay_ns;
    std::unique_lock<std::shared_mutex> lk(*mu_);
    flush_deadline_ns_->store(cutoff, std::memory_order_relaxed);
    any_ttl_->store(true, std::memory_order_relaxed); // wake the existing physical reaper
}

// meta `T` touch: overwrite an object's absolute expiry in place (0 = never). false if absent.
bool TierManager::touch_ttl(const Digest& digest, std::uint32_t expiry, std::uint32_t now) {
    // Expiry participates in add/replace presence. Serialize it with conditional publication so a
    // touch cannot revive an expired entry between commit's presence check and its Index swap.
    std::unique_lock<std::shared_mutex> lk(*mu_);
    const auto current = index_->lookup(digest);
    if (!current || unavailable_locked(*current, now, now_unix_ns())) return false;
    const bool found = index_->update_expiry(digest, expiry);
    if (found && expiry != 0) any_ttl_->store(true, std::memory_order_relaxed); // arm the reaper
    return found;
}

Index::RecacheClaim TierManager::claim_recache(const Digest& digest, std::uint32_t now) {
    std::unique_lock<std::shared_mutex> lk(*mu_);
    const auto current = index_->lookup(digest);
    if (!current || unavailable_locked(*current, now, now_unix_ns()))
        return Index::RecacheClaim::missing;
    return index_->claim_recache(digest);
}

Index::MetaMutation TierManager::mark_stale(const Digest& digest,
                                            std::uint64_t cas_expected,
                                            std::optional<std::uint32_t> expiry,
                                            std::uint32_t now) {
    std::unique_lock<std::shared_mutex> lk(*mu_);
    const auto current = index_->lookup(digest);
    if (!current || unavailable_locked(*current, now, now_unix_ns()))
        return Index::MetaMutation::missing;
    const std::uint64_t next = etag_seq_->fetch_add(1, std::memory_order_relaxed) + 1;
    const auto result = index_->mark_stale(digest, cas_expected, next, expiry);
    if (result == Index::MetaMutation::stored && expiry && *expiry != 0)
        any_ttl_->store(true, std::memory_order_relaxed);
    return result;
}

// Drop every object whose TTL has passed. Lazy-skip already hides expired objects from reads; this
// reclaims their RAM head + disk files. O(1) when no TTL has ever been set (the common cache case).
std::size_t TierManager::reap_expired() {
    if (!any_ttl_->load(std::memory_order_relaxed)) return 0;
    const auto keys = index_->keys();
    const std::uint32_t now = now_unix();
    const std::int64_t now_ns = now_unix_ns();
    std::size_t n = 0;
    std::unique_lock<std::shared_mutex> lk(*mu_);
    for (const auto& digest : keys) {
        const auto meta = index_->lookup(digest);
        if (!meta || !unavailable_locked(*meta, now, now_ns)) continue;
        if (auto dropped = drop_object(digest); dropped) {
            object_evictions_->fetch_add(1, std::memory_order_relaxed);
            if (is_expired(*meta, now)) ttl_evictions_->fetch_add(1, std::memory_order_relaxed);
            ++n;
        }
    }
    return n;
}

Result<TierManager::StoreHandle> TierManager::begin_store(const Digest& digest, Size size,
                                                          WriteMode write_mode,
                                                          StoreCondition condition) try {
    if (size > max_object_size_)
        return err(Errc::too_large, "object exceeds --max-object-size");
    const ObjectLayout layout = compute_layout(size, tiers_, three_layer());
    const bool ram_only = layout.ssd_bytes == 0 && layout.hdd_bytes == 0;
    if (!ram_only) {
        if (auto writable = ensure_store_devices_writable(digest, layout); !writable)
            return std::unexpected(writable.error());
    }

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
                    head_evictions_->fetch_add(1, std::memory_order_relaxed);
                    object_evictions_->fetch_add(1, std::memory_order_relaxed);
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
                    head_evictions_->fetch_add(1, std::memory_order_relaxed);
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

        const auto open_with_reclaim = [&](Pool& pool, Size bytes,
                                           Tier tier) -> Result<ObjectFiles> {
            std::uint64_t failed_device = 0;
            int failed_errno = 0;
            const auto attempt = [&]() {
                failed_device = 0;
                failed_errno = 0;
                if (auto writable = ensure_store_devices_writable(digest, layout); !writable)
                    return Result<ObjectFiles>(std::unexpected(writable.error()));
                auto opened = pool.open_object(digest, bytes, /*create=*/true, file_generation,
                                               &failed_device, &failed_errno);
                if (!opened) note_device_write(failed_device, failed_errno);
                (void)tier; // tier is retained at the callsite for diagnostic readability
                return opened;
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
            auto opened = open_with_reclaim(ssd_, layout.ssd_bytes, Tier::ssd);
            if (!opened) {
                lk.lock();
                return std::unexpected(opened.error());
            }
            ssd_files.emplace(std::move(*opened));
        }
        if (layout.hdd_bytes > 0 && hdd_) {
            auto opened = open_with_reclaim(*hdd_, layout.hdd_bytes, Tier::hdd);
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
                       condition, store_protection, pending_small_head);
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
                                      WriteMode write_mode, StoreCondition condition,
                                      bool store_protection,
                                      bool pending_small_head) noexcept
    : tm_(tm), digest_(digest), layout_(layout), ssd_(std::move(ssd)), hdd_(std::move(hdd)),
      head_(head), file_generation_(file_generation), stage_(stage), write_mode_(write_mode),
      condition_(condition), store_protection_(store_protection),
      pending_small_head_(pending_small_head) {}

TierManager::StoreHandle::StoreHandle(StoreHandle&& o) noexcept
    : tm_(o.tm_), digest_(o.digest_), layout_(o.layout_), ssd_(std::move(o.ssd_)),
      hdd_(std::move(o.hdd_)), head_(o.head_), file_generation_(o.file_generation_), stage_(o.stage_),
      stage_fill_(o.stage_fill_), flushed_(o.flushed_), off_(o.off_), committed_(o.committed_),
      write_mode_(o.write_mode_), condition_(o.condition_), store_protection_(o.store_protection_),
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
        condition_ = o.condition_;
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
        if (auto writable = tm_->ensure_store_devices_writable(digest_, layout_); !writable)
            return writable;
        const Size ssd_stop = std::min<Size>(o + n, ssd_extent);
        if (o < ssd_stop) {
            unsigned failed_drive = 0;
            int write_errno = 0;
            const auto started = std::chrono::steady_clock::now();
            if (auto st = striped_pwrite(tm_->ssd_.drives(), digest_.bucket(), ssd_.fds(), o,
                                         ByteView(stage_.data(), ssd_stop - o), &failed_drive,
                                         &write_errno);
                !st) {
                tm_->note_disk_write(Tier::ssd, static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - started).count()), write_errno);
                failed_device = tm_->ssd_.device_of_drive(failed_drive);
                tm_->note_device_write(failed_device, write_errno);
                return st;
            }
            tm_->note_disk_write(Tier::ssd, static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - started).count()));
        }
        if (o + n > layout_.ssd_bytes && hdd_) {
            const Size begin = std::max<Size>(o, layout_.ssd_bytes);
            const Size src = begin - o;
            unsigned failed_drive = 0;
            int write_errno = 0;
            const auto started = std::chrono::steady_clock::now();
            if (auto st = striped_pwrite(tm_->hdd_->drives(), digest_.bucket(), hdd_->fds(),
                                         begin - layout_.ssd_bytes,
                                         ByteView(stage_.data() + src, (o + n) - begin),
                                         &failed_drive, &write_errno);
                !st) {
                tm_->note_disk_write(Tier::hdd, static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - started).count()), write_errno);
                failed_device = tm_->hdd_->device_of_drive(failed_drive);
                tm_->note_device_write(failed_device, write_errno);
                return st;
            }
            tm_->note_disk_write(Tier::hdd, static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - started).count()));
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
                                                       std::shared_ptr<const HttpCacheMetadata> http,
                                                       CommitMetadata metadata) {
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
    const std::uint32_t commit_now = now_unix();
    const std::int64_t commit_now_ns = now_unix_ns();
    const bool present = old && !tm_->unavailable_locked(*old, commit_now, commit_now_ns);
    if (condition_ == StoreCondition::add && present)
        return err(Errc::condition_not_met); // another writer won before this handle linearized
    if (condition_ == StoreCondition::replace && !present)
        return err(Errc::condition_not_met); // removed/expired while this handle was being written
    if (cas_expected != 0 && (!present || old->etag != cas_expected))
        return err(Errc::cas_mismatch); // object changed/absent under us -> don't publish (scratch aborts)

    ObjectMeta meta;
    meta.size = layout_.size;
    meta.flags = flags;
    meta.expiry = expiry; // absolute Unix time; 0 = never (ADR-0007)
    meta.etag = tm_->etag_seq_->fetch_add(1, std::memory_order_relaxed) + 1; // unique per (re)store
    meta.stored_at_ns = commit_now_ns;
    meta.last_access = commit_now;
    meta.fetched = false;
    meta.stale = metadata.stale;
    meta.recache_claimed = metadata.recache_claimed;
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
        tm_->total_items_->fetch_add(1, std::memory_order_relaxed);
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
        if (!m || unavailable_locked(*m, now_unix(), now_unix_ns()))
            return err(Errc::not_found, "object not in index");
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

static void add_drive_segs(TierManager::ReadStream::Plan& p, const Pool& pool,
                           std::uint64_t bucket, std::span<const int> fds, Offset tier_off, Size len,
                           Size out_base, Tier tier) {
    std::array<ReadSegment, TierManager::ReadStream::kMaxSegs> raw{};
    const auto& dp = pool.drives();
    const std::size_t n = dp.plan_reads(bucket, tier_off, len, raw);
    Size running = 0;
    for (std::size_t i = 0; i < n && p.nsegs < p.segs.size(); ++i) {
        const auto& s = raw[i];
        p.segs[p.nsegs++] = {fds[s.drive], s.file_offset, s.length, out_base + running, tier,
                             pool.device_of_drive(s.drive)};
        p.total += static_cast<std::size_t>(s.length);
        running += s.length;
    }
}

Result<TierManager::ReadStream> TierManager::open_read(const Digest& digest) {
    // This legacy API owns its pin inside ReadStream. Keep one exclusive lock across metadata,
    // immutable-generation opens, and pin creation so it cannot combine two incarnations.
    std::unique_lock<std::shared_mutex> lk(*mu_);
    const auto found = index_->lookup(digest);
    if (!found || unavailable_locked(*found, now_unix(), now_unix_ns()))
        return err(Errc::not_found, "object not in index");
    const ObjectMeta meta = *found;
    record_access_locked(digest, meta);
    const ObjectLayout layout = compute_layout(meta.size, tiers_, three_layer());
    ObjectFiles ssd; // RAM-only objects have no SSD extent -> empty files; plan() then yields head-only
    if (layout.ssd_bytes > 0) {
        std::uint64_t failed_device = 0;
        int failed_errno = 0;
        auto f = ssd_.open_object(digest, layout.ssd_bytes, /*create=*/false,
                                  meta.file_generation, &failed_device, &failed_errno);
        if (!f) {
            note_device_read(failed_device, failed_errno);
            if (object_poison_errno(failed_errno))
                (void)quarantine_object_locked(digest, meta.file_generation);
            return std::unexpected(f.error());
        }
        ssd = std::move(*f);
    }
    std::optional<ObjectFiles> hdd;
    if (layout.hdd_bytes > 0 && hdd_) {
        std::uint64_t failed_device = 0;
        int failed_errno = 0;
        auto h = hdd_->open_object(digest, layout.hdd_bytes, /*create=*/false,
                                   meta.file_generation, &failed_device, &failed_errno);
        if (!h) {
            note_device_read(failed_device, failed_errno);
            if (object_poison_errno(failed_errno))
                (void)quarantine_object_locked(digest, meta.file_generation);
            return std::unexpected(h.error());
        }
        hdd.emplace(std::move(*h));
    }
    HeadPin pin;
    if (meta.head.resident()) pin_region(meta.head, pin, /*create=*/true);
    return ReadStream(this, digest, meta.size, meta.file_generation, layout, std::move(ssd),
                      std::move(hdd), meta.head, pin);
}

TierManager::ReadStream::ReadStream(ReadStream&& o) noexcept
    : tm_(o.tm_), digest_(o.digest_), size_(o.size_), generation_(o.generation_),
      layout_(o.layout_), ssd_(std::move(o.ssd_)),
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
        generation_ = o.generation_;
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
        add_drive_segs(p, tm_->ssd_, digest_.bucket(), ssd_.fds(), ssd_begin,
                       ssd_stop - ssd_begin, ssd_begin - off, Tier::ssd);
    if (hdd_ && off + want > layout_.ssd_bytes) {
        const Size begin = std::max<Size>(off, layout_.ssd_bytes);
        add_drive_segs(p, *tm_->hdd_, digest_.bucket(), hdd_->fds(),
                       begin - layout_.ssd_bytes, (off + want) - begin, begin - off, Tier::hdd);
    }
    return p;
}

Result<std::size_t> TierManager::ReadStream::read(core::Reactor& reactor, Offset off,
                                                 MutBytes out) {
    const Plan p = plan(off, out);
    if (p.nsegs == 0) return p.total;
    const auto started = std::chrono::steady_clock::now();
    unsigned tier_mask = 0;
    for (const auto& seg : p.segments()) tier_mask |= seg.tier == Tier::ssd ? 1u : 2u;
    const auto record = [&](int error_number) {
        const auto elapsed = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started).count());
        if (tier_mask & 1u) tm_->note_disk_read(Tier::ssd, elapsed, error_number);
        if (tier_mask & 2u) tm_->note_disk_read(Tier::hdd, elapsed, error_number);
    };

    std::array<Size, kMaxSegs> read_lengths{};
    for (std::size_t i = 0; i < p.nsegs; ++i) {
        const auto& seg = p.segs[i];
        // O_DIRECT requires a block-multiple length. Only the object's final segment is normally
        // short; read its padding into the unused aligned tail of the caller's full I/O buffer.
        const Size read_len =
            std::min<Size>(align_up(seg.len, kDeviceBlock), out.size() - seg.out_off);
        if (read_len < seg.len)
            return err(Errc::invalid_argument, "stream buffer lacks O_DIRECT tail room");
        read_lengths[i] = read_len;
    }

    std::size_t submitted = 0;
    bool submission_failed = false;
    for (std::size_t i = 0; i < p.nsegs; ++i) {
        const auto& seg = p.segs[i];
        const Size read_len = read_lengths[i];
        const std::uint64_t tag = i + 1; // completion -> exact segment/device; zero stays unused
        if (!reactor.submit_read(seg.fd, seg.file_off, out.subspan(seg.out_off, read_len), tag)) {
            reactor.submit();
            if (!reactor.submit_read(seg.fd, seg.file_off,
                                     out.subspan(seg.out_off, read_len), tag)) {
                submission_failed = true;
                break;
            }
        }
        ++submitted;
    }
    if (submitted > 0) reactor.submit_and_wait(static_cast<unsigned>(submitted));

    std::array<core::Completion, kMaxSegs> completions{};
    std::size_t completed = 0;
    bool read_failed = false;
    int read_errno = 0;
    while (completed < submitted) {
        const unsigned got = reactor.reap(
            std::span<core::Completion>(completions.data(), submitted - completed));
        if (got == 0) {
            reactor.submit_and_wait(1);
            continue;
        }
        for (unsigned i = 0; i < got; ++i) {
            const auto tag = completions[i].user_data;
            if (tag == 0 || tag > p.nsegs) {
                read_failed = true;
                if (read_errno == 0) read_errno = EIO;
                continue;
            }
            const std::size_t segment_index = static_cast<std::size_t>(tag - 1);
            const auto& seg = p.segs[segment_index];
            const int completion_errno = completions[i].res < 0
                ? -completions[i].res
                : (static_cast<Size>(completions[i].res) >= seg.len ? 0 : EIO);
            tm_->note_device_read(seg.device, completion_errno);
            if (completion_errno != 0) {
                read_failed = true;
                if (read_errno == 0) {
                    read_errno = completion_errno;
                }
            }
        }
        completed += got;
    }
    if (submission_failed) { record(EBUSY); return err(Errc::io_error, "submission queue full"); }
    if (read_failed) {
        record(read_errno);
        (void)tm_->quarantine_object(digest_, generation_);
        return err(Errc::io_error, "stream read failed; object quarantined");
    }
    record(0);
    return p.total;
}

Result<std::size_t> TierManager::ReadStream::read_sync(Offset off, MutBytes out) {
    const Plan p = plan(off, out);
    for (const auto& seg : p.segments()) {
        const Size read_len =
            std::min<Size>(align_up(seg.len, kDeviceBlock), out.size() - seg.out_off);
        if (read_len < seg.len)
            return err(Errc::invalid_argument, "stream buffer lacks O_DIRECT tail room");
        std::byte* dst = out.data() + seg.out_off;
        Size left = read_len;
        Offset file_off = seg.file_off;
        while (left > 0) {
            const ssize_t n = ::pread(seg.fd, dst, static_cast<std::size_t>(left),
                                      static_cast<off_t>(file_off));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                const int read_errno = n < 0 ? errno : EIO;
                tm_->note_device_read(seg.device, read_errno);
                (void)tm_->quarantine_object(digest_, generation_);
                return err(Errc::io_error,
                           "synchronous mutation read failed; object quarantined");
            }
            dst += n;
            file_off += static_cast<Size>(n);
            left -= static_cast<Size>(n);
        }
    }
    return p.total;
}

Status TierManager::copy_snapshot(StoreHandle& destination, Snapshot& source) {
    if (source.meta.size == 0) return {};
    if (!source.rs) {
        if (!source.pin.valid || source.pin.len < source.meta.size)
            return err(Errc::io_error, "resident mutation source is not pinned");
        return destination.write(pinned_bytes(source.pin).first(source.meta.size));
    }

    constexpr Size kCopyChunk = 256 * KiB;
    void* allocation = nullptr;
    if (::posix_memalign(&allocation, kDeviceBlock, static_cast<std::size_t>(kCopyChunk)) != 0)
        return err(Errc::out_of_memory, "allocate append/prepend copy buffer");
    auto cleanup = ScopeExit([&] { std::free(allocation); });
    MutBytes scratch(static_cast<std::byte*>(allocation), static_cast<std::size_t>(kCopyChunk));
    for (Size offset = 0; offset < source.meta.size;) {
        const Size logical = std::min<Size>(scratch.size(), source.meta.size - offset);
        auto read = source.rs->read_sync(offset, scratch);
        if (!read) return std::unexpected(read.error());
        if (*read != logical) return err(Errc::io_error, "short append/prepend source read");
        if (auto written = destination.write(ByteView(scratch.data(), logical)); !written)
            return written;
        offset += logical;
    }
    return {};
}

Result<TierManager::ArithmeticResult> TierManager::arithmetic(
    const Digest& digest, std::uint64_t delta, bool decrement,
    std::optional<std::uint64_t> initial, std::optional<std::uint32_t> new_expiry,
    std::uint64_t cas_expected, WriteMode write_mode) {
    for (;;) {
        auto source = open_snapshot(digest, /*record_access=*/false);
        if (!source) {
            if (!initial) return err(Errc::not_found);
            const std::string value = std::to_string(*initial);
            auto handle = begin_store(digest, value.size(), write_mode, StoreCondition::add);
            if (!handle) return std::unexpected(handle.error());
            if (auto written = handle->write(ByteView(
                    reinterpret_cast<const std::byte*>(value.data()), value.size())); !written)
                return std::unexpected(written.error());
            const std::uint32_t expiry = new_expiry.value_or(0);
            auto committed = handle->commit(0, expiry);
            if (!committed) {
                if (committed.error().code == Errc::condition_not_met) continue;
                return std::unexpected(committed.error());
            }
            ObjectMeta meta;
            meta.size = value.size();
            meta.expiry = expiry;
            meta.etag = *committed;
            return ArithmeticResult{*initial, meta, true};
        }

        auto unpin = ScopeExit([&] {
            if (source->pin.valid) unpin_head(source->pin);
        });
        if (cas_expected != 0 && source->meta.etag != cas_expected)
            return err(Errc::cas_mismatch);
        if (source->meta.size == 0 || source->meta.size > 20 || !source->pin.valid ||
            source->pin.len < source->meta.size)
            return err(Errc::invalid_argument, "cannot increment or decrement non-numeric value");
        const ByteView bytes = pinned_bytes(source->pin).first(source->meta.size);
        const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        std::uint64_t current = 0;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), current);
        if (ec != std::errc{} || end != text.data() + text.size())
            return err(Errc::invalid_argument, "cannot increment or decrement non-numeric value");
        const std::uint64_t next = decrement ? (delta > current ? 0 : current - delta)
                                             : current + delta;
        const std::string value = std::to_string(next);
        auto handle = begin_store(digest, value.size(), write_mode, StoreCondition::replace);
        if (!handle) return std::unexpected(handle.error());
        if (auto written = handle->write(ByteView(
                reinterpret_cast<const std::byte*>(value.data()), value.size())); !written)
            return std::unexpected(written.error());
        const std::uint32_t expiry = new_expiry.value_or(source->meta.expiry);
        auto committed = handle->commit(source->meta.flags, expiry, source->meta.etag);
        if (!committed) {
            if (cas_expected == 0 && (committed.error().code == Errc::cas_mismatch ||
                                      committed.error().code == Errc::condition_not_met))
                continue;
            return std::unexpected(committed.error());
        }
        ObjectMeta meta = source->meta;
        meta.size = value.size();
        meta.expiry = expiry;
        meta.etag = *committed;
        return ArithmeticResult{next, meta, false};
    }
}

bool TierManager::remove(const Digest& digest) {
    std::unique_lock<std::shared_mutex> lk(*mu_);
    const auto meta = index_->lookup(digest);
    if (!meta) return false;
    const bool live = !unavailable_locked(*meta, now_unix(), now_unix_ns());
    if (auto dropped = drop_object(digest); !dropped) {
        disable_numa_promotion(dropped.error());
        return false;
    }
    return live;
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

bool TierManager::quarantine_object_locked(const Digest& digest,
                                           std::uint64_t generation) {
    const auto current = index_->lookup(digest);
    if (!current || current->file_generation != generation)
        return false; // already removed, or a concurrent replacement superseded the poison
    if (auto dropped = drop_object(digest); !dropped) {
        quarantine_failures_->fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    quarantined_objects_->fetch_add(1, std::memory_order_relaxed);
    object_evictions_->fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool TierManager::quarantine_object(const Digest& digest, std::uint64_t generation) {
    std::unique_lock<std::shared_mutex> lk(*mu_);
    return quarantine_object_locked(digest, generation);
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
            if (auto dropped = drop_object(digest); dropped) {
                object_evictions_->fetch_add(1, std::memory_order_relaxed);
                ttl_evictions_->fetch_add(1, std::memory_order_relaxed);
                return true;
            }
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
        if (auto dropped = drop_object(*victim); dropped) {
            object_evictions_->fetch_add(1, std::memory_order_relaxed);
            capacity_evictions_->fetch_add(1, std::memory_order_relaxed);
            return true;
        }
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
        object_evictions_->fetch_add(1, std::memory_order_relaxed);
        capacity_evictions_->fetch_add(1, std::memory_order_relaxed);
    }
}

std::optional<ByteView> TierManager::head_view(const Digest& digest) {
    touch(digest);
    const auto meta = lookup_live(digest);
    if (!meta || !meta->head.resident()) return std::nullopt;
    return ByteView(ram_.addr(meta->head.block, meta->head.offset), meta->head.len);
}

void TierManager::touch(const Digest& digest) {
    std::unique_lock<std::shared_mutex> lk(*mu_); // touch mutates the policy (visited bit)
    const auto meta = index_->lookup(digest);
    if (meta && unavailable_locked(*meta, now_unix(), now_unix_ns())) return;
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
    index_->mark_fetched(digest, now_unix());
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
    numa_promotion_failures_->fetch_add(1, std::memory_order_relaxed);
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
    numa_promotion_scans_->fetch_add(1, std::memory_order_relaxed);
    if (gate.rescore_pending.load(std::memory_order_acquire)) {
        numa_promotion_yields_->fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Do not reserve the maintenance gate while waiting for ordinary storage readers/writers. A
    // pending decay announces itself first, so this lock order cannot starve or deadlock it.
    std::unique_lock<std::shared_mutex> lk(*mu_);
    if (!numa_promotion_healthy_->load(std::memory_order_relaxed)) return false;
    if (gate.rescore_pending.load(std::memory_order_acquire)) {
        numa_promotion_yields_->fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    std::unique_lock maintenance(gate.operation, std::try_to_lock);
    if (!maintenance.owns_lock()) {
        numa_promotion_yields_->fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // Decay may have announced itself between either earlier check and try_lock(). Yield immediately
    // so the promotion thread's no-sleep success loop cannot overtake it.
    if (gate.rescore_pending.load(std::memory_order_acquire)) {
        numa_promotion_yields_->fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    if (ram_.region_count() < 2) {
        numa_promotion_no_candidate_->fetch_add(1, std::memory_order_relaxed);
        return false;
    }

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
    if (!cold_local || !hot_remote || hot_remote->score <= cold_local->score) {
        numa_promotion_no_candidate_->fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto started = std::chrono::steady_clock::now();
    const auto cold_block = static_cast<unsigned>(cold_local->index);
    const auto hot_block = static_cast<unsigned>(hot_remote->index);
    if (!ram_.swap_blocks(cold_block, hot_block)) {
        numa_promotion_failures_->fetch_add(1, std::memory_order_relaxed);
        return false;
    }
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
    numa_promotion_last_success_->store(now_unix(), relaxed);
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
        numa_promotion_scans_->load(relaxed),
        numa_promotion_no_candidate_->load(relaxed),
        numa_promotion_yields_->load(relaxed),
        numa_promotion_failures_->load(relaxed),
        numa_promotion_last_success_->load(relaxed),
        numa_promotion_healthy_->load(relaxed),
    };
}

TierManager::ObservabilitySnapshot TierManager::observability_snapshot() const {
    std::shared_lock<std::shared_mutex> lk(*mu_);
    ObservabilitySnapshot snapshot;
    snapshot.objects = index_->usage();
    snapshot.total_items = total_items_->load(std::memory_order_relaxed);
    snapshot.evictions = {
        head_evictions_->load(std::memory_order_relaxed),
        object_evictions_->load(std::memory_order_relaxed),
        capacity_evictions_->load(std::memory_order_relaxed),
        ttl_evictions_->load(std::memory_order_relaxed),
    };
    for (const auto& usage : ram_.usage())
        snapshot.memory.push_back({usage.allocation_class, usage.capacity_bytes, usage.used_bytes,
                                   usage.free_bytes, usage.fragmented_bytes});
    for (const auto& mapping : ram_.mappings())
        snapshot.mappings.push_back({mapping.region, mapping.numa_node,
                                     mapping.allocation_class, mapping.bytes, mapping.hugetlb});

    const auto records = index_->records();
    const auto add_pool = [&](Tier tier, const Pool& pool, bool ssd) {
        const auto capacity = pool.filesystem_capacity();
        const std::size_t base = snapshot.filesystems.size();
        for (const auto& fs : capacity)
            snapshot.filesystems.push_back({tier, fs.device, fs.path, 0, 0, fs.allocated_bytes,
                                            fs.free_bytes, fs.available_bytes, fs.total_bytes,
                                            fs.total_inodes, fs.used_inodes, fs.free_inodes,
                                            fs.available_inodes});
        const auto fs_index = [&](std::uint64_t device) -> std::optional<std::size_t> {
            for (std::size_t i = 0; i < capacity.size(); ++i)
                if (capacity[i].device == device) return base + i;
            return std::nullopt;
        };
        for (const auto& [digest, meta] : records) {
            if (meta.file_generation == 0) continue;
            const ObjectLayout layout = compute_layout(meta.size, tiers_, three_layer());
            const Size logical = ssd ? layout.ssd_bytes : layout.hdd_bytes;
            const Size reserved = ssd
                ? (layout.hdd_bytes > 0 ? layout.ssd_bytes
                                        : align_up(layout.ssd_bytes, kDeviceBlock))
                : align_up(layout.hdd_bytes, kDeviceBlock);
            if (logical == 0) continue;
            for (unsigned drive = 0; drive < pool.drives().num_drives(); ++drive) {
                const auto target = fs_index(pool.device_of_drive(drive));
                if (!target) continue;
                snapshot.filesystems[*target].logical_bytes +=
                    pool.drives().file_extent(digest.bucket(), logical, drive);
                const Size extent = pool.drives().file_extent(digest.bucket(), reserved, drive);
                if (extent != 0) {
                    const Size block = capacity[*target - base].block_bytes;
                    snapshot.filesystems[*target].reserved_bytes +=
                        block ? align_up(extent, block) : extent;
                }
            }
        }
    };
    add_pool(Tier::ssd, ssd_, true);
    if (hdd_) add_pool(Tier::hdd, *hdd_, false);
    for (std::size_t i = 0; i < io_counters_->tiers.size(); ++i) {
        const auto& counters = io_counters_->tiers[i];
        DiskIoStats io;
        io.tier = i == 0 ? Tier::ssd : Tier::hdd;
        io.reads = counters.reads.snapshot();
        io.writes = counters.writes.snapshot();
        io.read_errors = counters.read_errors.load(std::memory_order_relaxed);
        io.write_errors = counters.write_errors.load(std::memory_order_relaxed);
        for (std::size_t error = 1; error < counters.read_errno.size(); ++error) {
            const auto reads = counters.read_errno[error].load(std::memory_order_relaxed);
            const auto writes = counters.write_errno[error].load(std::memory_order_relaxed);
            if (reads) io.read_errno.emplace_back(static_cast<int>(error), reads);
            if (writes) io.write_errno.emplace_back(static_cast<int>(error), writes);
        }
        snapshot.io.push_back(std::move(io));
    }
    return snapshot;
}

TierManager::CompatibilitySnapshot TierManager::compatibility_snapshot() const noexcept {
    constexpr auto rlx = std::memory_order_relaxed;
    return {
        index_->usage(),
        total_items_->load(rlx),
        ram_.capacity_bytes(),
        {head_evictions_->load(rlx), object_evictions_->load(rlx),
         capacity_evictions_->load(rlx), ttl_evictions_->load(rlx)},
    };
}

std::vector<TierManager::BuddyFreeBlocks> TierManager::buddy_free_blocks_snapshot() const {
    std::shared_lock<std::shared_mutex> lk(*mu_);
    std::vector<BuddyFreeBlocks> out;
    for (const auto& value : ram_.buddy_free_blocks())
        out.push_back({value.allocation_class, value.block_bytes, value.count});
    return out;
}

std::vector<TierManager::FilesystemCapacityStats>
TierManager::filesystem_capacity_snapshot() const {
    std::vector<FilesystemCapacityStats> out;
    const auto append = [&out](Tier tier, const Pool& pool) {
        auto capacity = pool.filesystem_capacity();
        out.reserve(out.size() + capacity.size());
        for (auto& fs : capacity) out.push_back({tier, std::move(fs)});
    };
    append(Tier::ssd, ssd_);
    if (hdd_) append(Tier::hdd, *hdd_);
    return out;
}

TierManager::StorageHealthSnapshot TierManager::storage_health_snapshot() const {
    constexpr auto relaxed = std::memory_order_relaxed;
    StorageHealthSnapshot snapshot;
    snapshot.quarantined_objects = quarantined_objects_->load(relaxed);
    snapshot.quarantine_failures = quarantine_failures_->load(relaxed);
    snapshot.watermark_scans = watermark_scans_->load(relaxed);
    snapshot.watermark_reclaim_runs = watermark_reclaim_runs_->load(relaxed);
    snapshot.watermark_reclaimed_objects = watermark_reclaimed_objects_->load(relaxed);
    snapshot.devices.reserve(device_io_.size());
    for (const auto& [device, owned] : device_io_) {
        const auto& counters = *owned;
        snapshot.devices.push_back({
            device,
            counters.paths,
            counters.tier_mask,
            counters.state.load(relaxed),
            counters.read_errors.load(relaxed),
            counters.write_errors.load(relaxed),
            counters.capacity_errors.load(relaxed),
            counters.last_errno.load(relaxed),
            counters.last_error_unix.load(relaxed),
        });
    }
    std::sort(snapshot.devices.begin(), snapshot.devices.end(),
              [](const auto& a, const auto& b) { return a.device < b.device; });

    const auto promote = [](StorageHealthState& current, StorageHealthState candidate) {
        if (current < candidate) current = candidate;
    };
    const auto used_ratio = [](std::uint64_t total, std::uint64_t available) {
        if (total == 0) return 0.0;
        const auto bounded = std::min(total, available);
        return 1.0 - static_cast<double>(bounded) / static_cast<double>(total);
    };
    for (const auto& device : snapshot.devices) promote(snapshot.state, device.state);
    for (const auto& fs : filesystem_capacity_snapshot()) {
        const auto& c = fs.capacity;
        const double bytes_used = used_ratio(c.total_bytes, c.available_bytes);
        const double inodes_used = used_ratio(c.total_inodes, c.available_inodes);
        StorageHealthState capacity_state = StorageHealthState::healthy;
        if ((c.total_bytes != 0 && c.available_bytes == 0) ||
            (c.total_inodes != 0 && c.available_inodes == 0))
            capacity_state = StorageHealthState::read_only;
        else if (bytes_used >= high_watermark_ || inodes_used >= high_watermark_)
            capacity_state = StorageHealthState::degraded;
        promote(snapshot.state, capacity_state);
        const auto found = std::lower_bound(
            snapshot.devices.begin(), snapshot.devices.end(), c.device,
            [](const DeviceHealthStats& value, std::uint64_t device) {
                return value.device < device;
            });
        if (found != snapshot.devices.end() && found->device == c.device)
            promote(found->state, capacity_state);
    }
    snapshot.ready = snapshot.state < StorageHealthState::read_only;
    return snapshot;
}

std::size_t TierManager::reclaim_to_watermarks(std::size_t max_victims) {
    watermark_scans_->fetch_add(1, std::memory_order_relaxed);
    if (max_victims == 0) return 0;
    const auto used_ratio = [](std::uint64_t total, std::uint64_t available) {
        if (total == 0) return 0.0;
        const auto bounded = std::min(total, available);
        return 1.0 - static_cast<double>(bounded) / static_cast<double>(total);
    };
    const auto above = [&](const Pool::FilesystemCapacity& capacity, double watermark) {
        return used_ratio(capacity.total_bytes, capacity.available_bytes) >= watermark ||
               used_ratio(capacity.total_inodes, capacity.available_inodes) >= watermark;
    };
    const auto current_capacity = [&](std::uint64_t device)
        -> std::optional<Pool::FilesystemCapacity> {
        for (auto capacity : ssd_.filesystem_capacity())
            if (capacity.device == device) return capacity;
        if (hdd_)
            for (auto capacity : hdd_->filesystem_capacity())
                if (capacity.device == device) return capacity;
        return std::nullopt;
    };

    std::vector<std::uint64_t> pressured;
    for (const auto& fs : filesystem_capacity_snapshot()) {
        if (!above(fs.capacity, high_watermark_)) continue;
        if (std::find(pressured.begin(), pressured.end(), fs.capacity.device) == pressured.end())
            pressured.push_back(fs.capacity.device);
    }
    if (pressured.empty()) return 0;

    std::size_t reclaimed = 0;
    std::unique_lock<std::mutex> reclaim_lk(*disk_reclaim_mu_);
    for (const auto device : pressured) {
        if (reclaimed >= max_victims) break;
        watermark_reclaim_runs_->fetch_add(1, std::memory_order_relaxed);
        while (reclaimed < max_victims) {
            const auto capacity = current_capacity(device);
            if (!capacity || !above(*capacity, low_watermark_)) break;
            if (!reclaim_one_disk_object(device)) break;
            ++reclaimed;
            watermark_reclaimed_objects_->fetch_add(1, std::memory_order_relaxed);
        }
    }
    return reclaimed;
}

void TierManager::note_disk_read(Tier tier, std::uint64_t duration_ns,
                                 int error_number) noexcept {
    if (tier == Tier::ram) return;
    auto& counters = io_counters_->tiers[tier == Tier::ssd ? 0 : 1];
    counters.reads.observe(duration_ns);
    if (error_number > 0) {
        counters.read_errors.fetch_add(1, std::memory_order_relaxed);
        const std::size_t bucket = std::min<std::size_t>(error_number,
                                                        counters.read_errno.size() - 1);
        counters.read_errno[bucket].fetch_add(1, std::memory_order_relaxed);
    }
}

void TierManager::note_disk_write(Tier tier, std::uint64_t duration_ns,
                                  int error_number) noexcept {
    if (tier == Tier::ram) return;
    auto& counters = io_counters_->tiers[tier == Tier::ssd ? 0 : 1];
    counters.writes.observe(duration_ns);
    if (error_number > 0) {
        counters.write_errors.fetch_add(1, std::memory_order_relaxed);
        const std::size_t bucket = std::min<std::size_t>(error_number,
                                                        counters.write_errno.size() - 1);
        counters.write_errno[bucket].fetch_add(1, std::memory_order_relaxed);
    }
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
    if (!meta || unavailable_locked(*meta, now_unix(), now_unix_ns())) return std::nullopt;
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
    std::uint64_t poisoned_generation = 0;

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
        if (unavailable_locked(*m, now, now_unix_ns())) return std::nullopt;
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
            std::uint64_t failed_device = 0;
            int failed_errno = 0;
            auto ssd = ssd_.open_object(digest, layout.ssd_bytes, /*create=*/false,
                                        m->file_generation, &failed_device, &failed_errno);
            if (!ssd) {
                note_device_read(failed_device, failed_errno);
                if (object_poison_errno(failed_errno)) poisoned_generation = m->file_generation;
                return std::nullopt;
            }
            std::optional<ObjectFiles> hdd;
            if (layout.hdd_bytes > 0 && hdd_) {
                failed_device = 0;
                failed_errno = 0;
                auto h = hdd_->open_object(digest, layout.hdd_bytes, /*create=*/false,
                                           m->file_generation, &failed_device, &failed_errno);
                if (!h) {
                    note_device_read(failed_device, failed_errno);
                    if (object_poison_errno(failed_errno))
                        poisoned_generation = m->file_generation;
                    return std::nullopt;
                }
                hdd.emplace(std::move(*h));
            }
            snap.rs.emplace(ReadStream(this, digest, m->size, m->file_generation, layout,
                                       std::move(*ssd), std::move(hdd), m->head, HeadPin{}));
        }
        if (m->head.resident()) {
            if (!pin_region(m->head, snap.pin, create_pin)) return std::nullopt; // need exclusive create
        }
        return snap;
    };

    {
        std::shared_lock<std::shared_mutex> lk(*mu_);
        if (auto snap = fill(/*create_pin=*/false)) return snap;
        if (poisoned_generation == 0) {
            // Miss (absent/expired) or pin slot missing. Distinguish: absent stays a miss.
            const auto m = index_->lookup(digest);
            if (!m || unavailable_locked(*m, now, now_unix_ns())) return std::nullopt;
            if (!m->head.resident())
                return std::nullopt; // transient open/resource error; do not poison the object
            // Head resident but pin slot not yet registered -> exclusive path below.
        }
    }
    std::unique_lock<std::shared_mutex> lk(*mu_);
    if (poisoned_generation != 0) {
        (void)quarantine_object_locked(digest, poisoned_generation);
        return std::nullopt;
    }
    auto snapshot = fill(/*create_pin=*/true);
    if (!snapshot && poisoned_generation != 0)
        (void)quarantine_object_locked(digest, poisoned_generation);
    return snapshot;
}

} // namespace goblin::storage
