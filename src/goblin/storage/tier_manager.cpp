#include "goblin/storage/tier_manager.hpp"

#include "goblin/storage/layout.hpp"
#include "goblin/storage/striped_io.hpp"

#include <algorithm>
#include <cstdio> // renameat
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <utility>

namespace goblin::storage {

// ---------------- ObjectFiles ----------------

ObjectFiles::~ObjectFiles() {
    for (const int fd : fds_)
        if (fd >= 0) ::close(fd);
}
ObjectFiles::ObjectFiles(ObjectFiles&& o) noexcept : fds_(std::move(o.fds_)) {}
ObjectFiles& ObjectFiles::operator=(ObjectFiles&& o) noexcept {
    if (this != &o) {
        for (const int fd : fds_)
            if (fd >= 0) ::close(fd);
        fds_ = std::move(o.fds_);
    }
    return *this;
}

// ---------------- Pool ----------------

Pool::~Pool() {
    for (const int fd : dirfds_)
        if (fd >= 0) ::close(fd);
}
Pool::Pool(Pool&& o) noexcept : drives_(o.drives_), dirfds_(std::move(o.dirfds_)) {}
Pool& Pool::operator=(Pool&& o) noexcept {
    if (this != &o) {
        for (const int fd : dirfds_)
            if (fd >= 0) ::close(fd);
        drives_ = o.drives_;
        dirfds_ = std::move(o.dirfds_);
    }
    return *this;
}

Result<Pool> Pool::open(const std::vector<std::string>& dirs, Size stripe_unit, bool direct_io) {
    if (dirs.empty()) return err(Errc::invalid_argument, "empty pool");
    std::vector<int> dirfds;
    dirfds.reserve(dirs.size());
    for (const auto& d : dirs) {
        const int fd = ::open(d.c_str(), O_RDONLY | O_DIRECTORY);
        if (fd < 0) {
            for (const int f : dirfds) ::close(f);
            return err(Errc::io_error, "open pool dir: " + d);
        }
        dirfds.push_back(fd);
    }
    return Pool(DrivePool(static_cast<unsigned>(dirs.size()), stripe_unit), std::move(dirfds),
                direct_io);
}

Result<ObjectFiles> Pool::open_object(const Digest& digest, Size tier_bytes, bool create,
                                      std::string_view name_suffix) const {
    const unsigned n = drives_.num_drives();
    std::vector<int> fds(n, -1);
    if (tier_bytes == 0) return ObjectFiles(std::move(fds));

    const std::string name = digest.hex() + std::string(name_suffix);
    const Size stripe = drives_.stripe_unit();
    const Size nchunks = (tier_bytes + stripe - 1) / stripe;
    const unsigned used = static_cast<unsigned>(nchunks < n ? nchunks : n);
    int flags = create ? (O_RDWR | O_CREAT) : O_RDONLY;
    if (direct_io_) flags |= O_DIRECT; // bypass the page cache: own the backing store (ADR-0011)

    for (unsigned c = 0; c < used; ++c) {
        const unsigned d = drives_.drive_of(digest.bucket(), static_cast<Offset>(c) * stripe);
        if (fds[d] >= 0) continue;
        const int fd = ::openat(dirfds_[d], name.c_str(), flags, 0644);
        if (fd < 0) {
            for (const int f : fds)
                if (f >= 0) ::close(f);
            return err(Errc::io_error, create ? "openat (create) object file"
                                              : "openat object file (not found?)");
        }
        fds[d] = fd;
    }
    return ObjectFiles(std::move(fds));
}

Status Pool::publish(const Digest& digest, Size tier_bytes, std::string_view name_suffix) const {
    if (tier_bytes == 0) return {};
    const std::string live = digest.hex();
    const std::string tmp = live + std::string(name_suffix);
    const Size stripe = drives_.stripe_unit();
    const Size nchunks = (tier_bytes + stripe - 1) / stripe;
    const unsigned n = drives_.num_drives();
    const unsigned used = static_cast<unsigned>(nchunks < n ? nchunks : n);
    for (unsigned c = 0; c < used; ++c) {
        const unsigned d = drives_.drive_of(digest.bucket(), static_cast<Offset>(c) * stripe);
        if (::renameat(dirfds_[d], tmp.c_str(), dirfds_[d], live.c_str()) != 0)
            return err(Errc::io_error, "renameat (publish) object file");
    }
    return {};
}

void Pool::unlink_object(const Digest& digest, Size tier_bytes, std::string_view name_suffix) const {
    if (tier_bytes == 0) return;
    const std::string name = digest.hex() + std::string(name_suffix);
    const Size stripe = drives_.stripe_unit();
    const Size nchunks = (tier_bytes + stripe - 1) / stripe;
    const unsigned n = drives_.num_drives();
    const unsigned used = static_cast<unsigned>(nchunks < n ? nchunks : n);
    for (unsigned c = 0; c < used; ++c) {
        const unsigned d = drives_.drive_of(digest.bucket(), static_cast<Offset>(c) * stripe);
        ::unlinkat(dirfds_[d], name.c_str(), 0); // best-effort; ENOENT is fine
    }
}

// ---------------- TierManager ----------------

Result<TierManager> TierManager::open(const TierSizes& t, const MemoryConfig& mem,
                                      const EvictionConfig& ev, const PoolConfig& ssd,
                                      const PoolConfig& hdd, Index& index, Size io_chunk,
                                      unsigned write_buffers, bool direct_io) {
    auto ram = core::BufferPool::create(mem.total_bytes, mem.block_bytes, kDeviceBlock,
                                        mem.lock_memory);
    if (!ram) return std::unexpected(ram.error());

    const std::size_t cap_hint =
        t.ram_head ? static_cast<std::size_t>(mem.total_bytes / t.ram_head) : 1;
    auto head_policy = make_eviction_policy(ev.policy, cap_hint);
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
    auto wp = core::IoBufferPool::create(io_chunk, write_buffers, mem.lock_memory);
    if (!wp) return std::unexpected(wp.error());
    TierManager tm(t, std::move(*ram), std::move(head_policy), std::move(object_policy),
                   ev.max_ssd_objects, std::move(*sp), std::move(hp), index);
    tm.write_pool_ = std::make_unique<core::IoBufferPool>(std::move(*wp));
    return tm;
}

Status TierManager::store(const Digest& digest, ByteView data, std::uint32_t flags,
                          std::uint32_t expiry) {
    auto h = begin_store(digest, data.size());
    if (!h) return std::unexpected(h.error());
    if (auto st = h->write(data); !st) return st;
    return h->commit(flags, expiry);
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

Result<TierManager::StoreHandle> TierManager::begin_store(const Digest& digest, Size size) {
    const ObjectLayout layout = compute_layout(size, tiers_, three_layer());

    // Copy-on-write (ADR-0018): write into fresh scratch files so the old version stays fully live
    // (files + head) for concurrent readers until commit publishes the new one. Opening the scratch
    // files touches no shared state, so it needs no lock.
    const std::string suffix = ".tmp." + std::to_string(store_seq_->fetch_add(1));
    auto ssd_files = ssd_.open_object(digest, layout.ssd_bytes, /*create=*/true, suffix);
    if (!ssd_files) return std::unexpected(ssd_files.error());
    std::optional<ObjectFiles> hdd_files;
    if (layout.hdd_bytes > 0) {
        auto h = hdd_->open_object(digest, layout.hdd_bytes, /*create=*/true, suffix);
        if (!h) return std::unexpected(h.error());
        hdd_files.emplace(std::move(*h));
    }

    // Reserve the new RAM head, evicting cold heads (ADR-0007/0012) to make room. The old version's
    // head stays put; commit frees it once the new version is published.
    std::unique_lock<std::shared_mutex> lk(*mu_); // mutates allocator + pool + policy (exclusive)

    // Acquire a bounded, page-aligned staging buffer for the disk writes (ADR-0011): one per
    // in-flight store, so write-staging RAM stays bounded. Exhaustion returns would_block (not a
    // hard failure): the caller backpressures the connection and retries once a buffer frees, so the
    // bound becomes a concurrency limit (tune via io_buffers/io_chunk), never an unbudgeted RAM grow.
    // (SET is write-once; the extra copy through this buffer is a deliberate trade.)
    auto stage = write_pool_->acquire();
    if (!stage) {
        lk.unlock();
        ssd_.unlink_object(digest, layout.ssd_bytes, suffix); // discard the scratch files we opened
        if (hdd_) hdd_->unlink_object(digest, layout.hdd_bytes, suffix);
        return err(Errc::would_block, "write staging buffers exhausted");
    }

    std::optional<core::BufferPool::Region> head;
    const Size head_len = std::min<Size>(size, tiers_.ram_head);
    if (head_len > 0) {
        auto region = ram_.allocate(static_cast<std::uint32_t>(head_len));
        while (!region) {
            const auto victim = policy_->evict();
            if (!victim) break;
            if (const auto vm = index_->lookup(*victim); vm && vm->head.resident()) {
                free_head_region(vm->head.block, vm->head.offset, vm->head.len);
                index_->set_head(*victim, HeadLoc{});
            }
            region = ram_.allocate(static_cast<std::uint32_t>(head_len));
        }
        if (region) head = *region;
    }

    return StoreHandle(this, digest, layout, std::move(*ssd_files), std::move(hdd_files), head,
                       suffix, *stage);
}

TierManager::StoreHandle::StoreHandle(TierManager* tm, Digest digest, ObjectLayout layout,
                                      ObjectFiles ssd, std::optional<ObjectFiles> hdd,
                                      std::optional<core::BufferPool::Region> head,
                                      std::string suffix, MutBytes stage)
    : tm_(tm), digest_(digest), layout_(layout), ssd_(std::move(ssd)), hdd_(std::move(hdd)),
      head_(head), suffix_(std::move(suffix)), stage_(stage) {}

TierManager::StoreHandle::StoreHandle(StoreHandle&& o) noexcept
    : tm_(o.tm_), digest_(o.digest_), layout_(o.layout_), ssd_(std::move(o.ssd_)),
      hdd_(std::move(o.hdd_)), head_(o.head_), suffix_(std::move(o.suffix_)), stage_(o.stage_),
      stage_fill_(o.stage_fill_), flushed_(o.flushed_), off_(o.off_), committed_(o.committed_) {
    o.tm_ = nullptr;
    o.committed_ = true; // neutralize the moved-from handle's destructor
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
        suffix_ = std::move(o.suffix_);
        stage_ = o.stage_;
        stage_fill_ = o.stage_fill_;
        flushed_ = o.flushed_;
        off_ = o.off_;
        committed_ = o.committed_;
        o.tm_ = nullptr;
        o.committed_ = true;
    }
    return *this;
}

TierManager::StoreHandle::~StoreHandle() { abort_uncommitted(); }

// Roll back a handle that was never committed: free the reserved head and delete the scratch files.
void TierManager::StoreHandle::abort_uncommitted() {
    if (committed_ || !tm_) return;
    if (head_ || !stage_.empty()) {
        std::unique_lock<std::shared_mutex> lk(*tm_->mu_); // ram_ + write_pool_ are shared (exclusive)
        if (head_) {
            tm_->ram_.deallocate(head_->block, head_->offset, head_->len);
            head_.reset();
        }
        if (!stage_.empty()) {
            tm_->write_pool_->release(stage_);
            stage_ = {};
        }
    }
    tm_->ssd_.unlink_object(digest_, layout_.ssd_bytes, suffix_); // scratch files; filesystem only
    if (tm_->hdd_) tm_->hdd_->unlink_object(digest_, layout_.hdd_bytes, suffix_);
}

Status TierManager::StoreHandle::write(ByteView chunk) {
    const Size len = chunk.size();
    if (off_ + len > layout_.size) return err(Errc::invalid_argument, "write past end of object");

    if (head_) { // fill the resident head directly from the chunk (RAM; no alignment needed)
        const Size hend = std::min<Size>(off_ + len, head_->len);
        if (off_ < hend) std::memcpy(head_->data + off_, chunk.data(), hend - off_);
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

// Write the staged block [0, n) (n device-block-aligned) at disk offset flushed_, split across the
// SSD prefix and HDD tail. Aligned source/offset/length keep O_DIRECT happy. The SSD file is padded
// to a 4 KiB boundary only when it holds the object's tail (no HDD); with an HDD tail the prefix
// ends on the already-aligned ssd_bytes split.
Status TierManager::StoreHandle::flush_block(Size n) {
    const Offset o = flushed_;
    const Size ssd_extent =
        (layout_.hdd_bytes > 0) ? layout_.ssd_bytes : align_up(layout_.ssd_bytes, kDeviceBlock);
    const Size ssd_stop = std::min<Size>(o + n, ssd_extent);
    if (o < ssd_stop) {
        if (auto st = striped_pwrite(tm_->ssd_.drives(), digest_.bucket(), ssd_.fds(), o,
                                     ByteView(stage_.data(), ssd_stop - o));
            !st)
            return st;
    }
    if (o + n > layout_.ssd_bytes && hdd_) {
        const Size begin = std::max<Size>(o, layout_.ssd_bytes);
        const Size src = begin - o;
        if (auto st = striped_pwrite(tm_->hdd_->drives(), digest_.bucket(), hdd_->fds(),
                                     begin - layout_.ssd_bytes,
                                     ByteView(stage_.data() + src, (o + n) - begin));
            !st)
            return st;
    }
    flushed_ += n;
    stage_fill_ = 0;
    return {};
}

Status TierManager::StoreHandle::commit(std::uint32_t flags, std::uint32_t expiry,
                                        std::uint64_t cas_expected) {
    if (off_ != layout_.size)
        return err(Errc::invalid_argument, "commit before the full value was written");

    // Flush the trailing partial block, zero-padded up to the device block (O_DIRECT). The file then
    // ends on a 4 KiB boundary; the padding is never served (the index records the real size).
    if (stage_fill_ > 0) {
        const Size padded = align_up(stage_fill_, kDeviceBlock);
        if (padded > stage_fill_) std::memset(stage_.data() + stage_fill_, 0, padded - stage_fill_);
        if (auto st = flush_block(padded); !st) return st;
    }

    std::unique_lock<std::shared_mutex> lk(*tm_->mu_); // publish + swap atomically (exclusive, ADR-0018)
    const auto old = tm_->index_->lookup(digest_);
    if (cas_expected != 0 && (!old || is_expired(*old, now_unix()) || old->etag != cas_expected))
        return err(Errc::cas_mismatch); // object changed/absent under us -> don't publish (scratch aborts)
    // Publish: rename the scratch files over the live names. Readers that already opened the old
    // files keep them (unlinked-but-open inodes) and finish a consistent old version.
    if (auto st = tm_->ssd_.publish(digest_, layout_.ssd_bytes, suffix_); !st) return st;
    if (tm_->hdd_) {
        if (auto st = tm_->hdd_->publish(digest_, layout_.hdd_bytes, suffix_); !st) return st;
    }
    if (old && old->head.resident()) { // retire the replaced version's head now that the new is live
        tm_->free_head_region(old->head.block, old->head.offset, old->head.len);
        tm_->policy_->remove(digest_);
    }
    ObjectMeta meta;
    meta.size = layout_.size;
    meta.flags = flags;
    meta.expiry = expiry; // absolute Unix time; 0 = never (ADR-0007)
    meta.etag = tm_->etag_seq_->fetch_add(1, std::memory_order_relaxed) + 1; // unique per (re)store
    if (head_) meta.head = HeadLoc{head_->block, head_->offset, head_->len};
    if (expiry != 0) tm_->any_ttl_->store(true, std::memory_order_relaxed); // arm the reaper
    tm_->index_->set(digest_, meta);
    if (head_) tm_->policy_->insert(digest_);
    if (!old) tm_->object_policy_->insert(digest_); // only new objects count toward the SSD bound
    tm_->write_pool_->release(stage_);              // return the staging buffer (under the lock)
    stage_ = {};
    committed_ = true;
    tm_->enforce_object_bound();                    // evict whole objects if over the limit
    return {};
}

Result<std::size_t> TierManager::read(core::Reactor& reactor, const Digest& digest, Offset offset,
                                      MutBytes out) {
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
            auto f = ssd_.open_object(digest, layout.ssd_bytes, /*create=*/false);
            if (!f) return std::unexpected(f.error());
            ssd_files = std::move(*f);
        }
        if (three_layer() && offset + want > layout.ssd_bytes) {
            auto f = hdd_->open_object(digest, layout.hdd_bytes, /*create=*/false);
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
    if (three_layer() && offset + want > layout.ssd_bytes) {
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
    Size running = 0;
    for (const auto& s : dp.plan_reads(bucket, tier_off, len)) {
        p.segs.push_back({fds[s.drive], s.file_offset, s.length, out_base + running});
        p.total += static_cast<std::size_t>(s.length);
        running += s.length;
    }
}

Result<TierManager::ReadStream> TierManager::open_read(const Digest& digest) {
    const auto meta = index_->lookup(digest);
    if (!meta) return err(Errc::not_found, "object not in index");
    const ObjectLayout layout = compute_layout(meta->size, tiers_, three_layer());
    auto ssd = ssd_.open_object(digest, layout.ssd_bytes, /*create=*/false);
    if (!ssd) return std::unexpected(ssd.error());
    std::optional<ObjectFiles> hdd;
    if (layout.hdd_bytes > 0 && hdd_) {
        auto h = hdd_->open_object(digest, layout.hdd_bytes, /*create=*/false);
        if (!h) return std::unexpected(h.error());
        hdd.emplace(std::move(*h));
    }
    return ReadStream(this, digest, meta->size, layout, std::move(*ssd), std::move(hdd));
}

TierManager::ReadStream::Plan TierManager::ReadStream::plan(Offset off, MutBytes out) {
    Plan p;
    if (off >= size_) return p;
    const Size want = std::min<Size>(out.size(), size_ - off);

    // Head portion: copied out under the storage read-lock (ADR-0018).
    Size ram_end = 0;
    {
        std::shared_lock<std::shared_mutex> lk(*tm_->mu_);
        if (const auto m = tm_->index_->lookup(digest_); m && m->head.resident()) {
            ram_end = m->head.len;
            if (off < ram_end) {
                const Size n = std::min<Size>(off + want, ram_end) - off;
                std::memcpy(out.data(), tm_->ram_.addr(m->head.block, m->head.offset) + off, n);
                p.total += static_cast<std::size_t>(n);
            }
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

bool TierManager::remove(const Digest& digest) {
    std::unique_lock<std::shared_mutex> lk(*mu_);
    if (!index_->contains(digest)) return false;
    drop_object(digest);
    return true;
}

void TierManager::drop_object(const Digest& digest) {
    const auto meta = index_->lookup(digest);
    if (!meta) return;
    if (meta->head.resident()) {
        free_head_region(meta->head.block, meta->head.offset, meta->head.len);
        policy_->remove(digest);
    }
    object_policy_->remove(digest);
    const ObjectLayout layout = compute_layout(meta->size, tiers_, three_layer());
    ssd_.unlink_object(digest, layout.ssd_bytes);
    if (layout.hdd_bytes > 0 && hdd_) hdd_->unlink_object(digest, layout.hdd_bytes);
    index_->erase(digest);
}

void TierManager::enforce_object_bound() {
    while (max_objects_ != 0 && index_->size() > max_objects_) {
        const auto victim = object_policy_->evict();
        if (!victim) break;
        drop_object(*victim);
    }
}

std::optional<ByteView> TierManager::head_view(const Digest& digest) const {
    const auto meta = index_->lookup(digest);
    if (!meta || !meta->head.resident()) return std::nullopt;
    return ByteView(ram_.addr(meta->head.block, meta->head.offset), meta->head.len);
}

void TierManager::touch(const Digest& digest) {
    std::unique_lock<std::shared_mutex> lk(*mu_); // touch mutates the policy (visited bit)
    policy_->touch(digest);
    object_policy_->touch(digest);
}

std::size_t TierManager::head_resident() const {
    std::shared_lock<std::shared_mutex> lk(*mu_);
    return policy_->resident();
}

std::optional<TierManager::HeadPin> TierManager::pin_head(const Digest& digest) {
    std::unique_lock<std::shared_mutex> lk(*mu_);
    const auto meta = index_->lookup(digest);
    if (!meta) return std::nullopt;
    object_policy_->touch(digest); // the whole object was accessed
    if (!meta->head.resident()) return std::nullopt;
    policy_->touch(digest); // head-cache hit
    auto& p = pins_[region_id(meta->head.block, meta->head.offset)];
    p.len = meta->head.len;
    ++p.refcount;
    return HeadPin{meta->head.block, meta->head.offset, meta->head.len, true};
}

void TierManager::unpin_head(const HeadPin& pin) {
    if (!pin.valid) return;
    std::unique_lock<std::shared_mutex> lk(*mu_);
    const auto it = pins_.find(region_id(pin.block, pin.offset));
    if (it == pins_.end()) return;
    if (--it->second.refcount == 0) {
        if (it->second.orphaned) ram_.deallocate(pin.block, pin.offset, pin.len);
        pins_.erase(it);
    }
}

ByteView TierManager::pinned_bytes(const HeadPin& pin) const {
    return ByteView(ram_.addr(pin.block, pin.offset), pin.len);
}

// Free a head's RAM, or — if a reader has it pinned — orphan it so the last unpin frees it.
// Called under the storage lock (by begin_store / drop_object).
void TierManager::free_head_region(unsigned block, std::uint32_t offset, std::uint32_t len) {
    const auto it = pins_.find(region_id(block, offset));
    if (it != pins_.end() && it->second.refcount > 0)
        it->second.orphaned = true;
    else
        ram_.deallocate(block, offset, len);
}

std::optional<TierManager::Snapshot> TierManager::open_snapshot(const Digest& digest) {
    std::unique_lock<std::shared_mutex> lk(*mu_); // capture meta + head-pin + fds atomically
    const auto m = index_->lookup(digest);
    if (!m) return std::nullopt;
    if (is_expired(*m, now_unix())) return std::nullopt; // TTL passed -> lazy miss (reaper reclaims)
    object_policy_->touch(digest); // the whole object was accessed
    const Size head_len = m->head.resident() ? m->head.len : 0;

    Snapshot snap;
    snap.meta = *m;
    // Open the disk files first (under the lock), so an open failure needs no pin rollback.
    if (m->size > head_len) {
        const ObjectLayout layout = compute_layout(m->size, tiers_, three_layer());
        auto ssd = ssd_.open_object(digest, layout.ssd_bytes, /*create=*/false);
        if (!ssd) return std::nullopt;
        std::optional<ObjectFiles> hdd;
        if (layout.hdd_bytes > 0 && hdd_) {
            auto h = hdd_->open_object(digest, layout.hdd_bytes, /*create=*/false);
            if (!h) return std::nullopt;
            hdd.emplace(std::move(*h));
        }
        snap.rs.emplace(ReadStream(this, digest, m->size, layout, std::move(*ssd), std::move(hdd)));
    }
    // Pin the resident head (orphan-safe across a concurrent replace).
    if (m->head.resident()) {
        policy_->touch(digest);
        auto& p = pins_[region_id(m->head.block, m->head.offset)];
        p.len = m->head.len;
        ++p.refcount;
        snap.pin = HeadPin{m->head.block, m->head.offset, m->head.len, true};
    }
    return snap;
}

} // namespace goblin::storage
