#include "goblin/storage/tier_manager.hpp"

#include "goblin/storage/layout.hpp"
#include "goblin/storage/striped_io.hpp"

#include <algorithm>
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

Result<Pool> Pool::open(const std::vector<std::string>& dirs, Size stripe_unit) {
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
    return Pool(DrivePool(static_cast<unsigned>(dirs.size()), stripe_unit), std::move(dirfds));
}

Result<ObjectFiles> Pool::open_object(const Digest& digest, Size tier_bytes, bool create) const {
    const unsigned n = drives_.num_drives();
    std::vector<int> fds(n, -1);
    if (tier_bytes == 0) return ObjectFiles(std::move(fds));

    const std::string hex = digest.hex();
    const Size stripe = drives_.stripe_unit();
    const Size nchunks = (tier_bytes + stripe - 1) / stripe;
    const unsigned used = static_cast<unsigned>(nchunks < n ? nchunks : n);
    const int flags = create ? (O_RDWR | O_CREAT) : O_RDONLY;

    for (unsigned c = 0; c < used; ++c) {
        const unsigned d = drives_.drive_of(digest.bucket(), static_cast<Offset>(c) * stripe);
        if (fds[d] >= 0) continue;
        const int fd = ::openat(dirfds_[d], hex.c_str(), flags, 0644);
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

void Pool::unlink_object(const Digest& digest, Size tier_bytes) const {
    if (tier_bytes == 0) return;
    const std::string hex = digest.hex();
    const Size stripe = drives_.stripe_unit();
    const Size nchunks = (tier_bytes + stripe - 1) / stripe;
    const unsigned n = drives_.num_drives();
    const unsigned used = static_cast<unsigned>(nchunks < n ? nchunks : n);
    for (unsigned c = 0; c < used; ++c) {
        const unsigned d = drives_.drive_of(digest.bucket(), static_cast<Offset>(c) * stripe);
        ::unlinkat(dirfds_[d], hex.c_str(), 0); // best-effort; ENOENT is fine
    }
}

// ---------------- TierManager ----------------

Result<TierManager> TierManager::open(const TierSizes& t, const MemoryConfig& mem,
                                      const EvictionConfig& ev, const PoolConfig& ssd,
                                      const PoolConfig& hdd, Index& index) {
    auto ram = core::BufferPool::create(mem.total_bytes, mem.block_bytes, kDeviceBlock,
                                        mem.lock_memory);
    if (!ram) return std::unexpected(ram.error());

    const std::size_t cap_hint =
        t.ram_head ? static_cast<std::size_t>(mem.total_bytes / t.ram_head) : 1;
    auto head_policy = make_eviction_policy(ev.policy, cap_hint);
    const std::size_t obj_cap =
        ev.max_ssd_objects ? static_cast<std::size_t>(ev.max_ssd_objects) : cap_hint;
    auto object_policy = make_eviction_policy(ev.policy, obj_cap);

    auto sp = Pool::open(ssd.dirs, ssd.stripe_unit);
    if (!sp) return std::unexpected(sp.error());
    std::optional<Pool> hp;
    if (!hdd.dirs.empty()) {
        auto h = Pool::open(hdd.dirs, hdd.stripe_unit);
        if (!h) return std::unexpected(h.error());
        hp.emplace(std::move(*h));
    }
    return TierManager(t, std::move(*ram), std::move(head_policy), std::move(object_policy),
                       ev.max_ssd_objects, std::move(*sp), std::move(hp), index);
}

Status TierManager::store(const Digest& digest, ByteView data, std::uint32_t flags) {
    auto h = begin_store(digest, data.size());
    if (!h) return std::unexpected(h.error());
    if (auto st = h->write(data); !st) return st;
    return h->commit(flags);
}

Result<TierManager::StoreHandle> TierManager::begin_store(const Digest& digest, Size size) {
    const ObjectLayout layout = compute_layout(size, tiers_, three_layer());

    std::unique_lock<std::shared_mutex> lk(*mu_); // mutates allocator + policy (exclusive, ADR-0018)

    // Free an overwritten object's old cached head before reserving a new one.
    if (const auto old = index_->lookup(digest); old && old->head.resident()) {
        ram_.deallocate(old->head.block, old->head.offset, old->head.len);
        policy_->remove(digest);
    }

    auto ssd_files = ssd_.open_object(digest, layout.ssd_bytes, /*create=*/true);
    if (!ssd_files) return std::unexpected(ssd_files.error());
    std::optional<ObjectFiles> hdd_files;
    if (layout.hdd_bytes > 0) {
        auto h = hdd_->open_object(digest, layout.hdd_bytes, /*create=*/true);
        if (!h) return std::unexpected(h.error());
        hdd_files.emplace(std::move(*h));
    }

    // Reserve the RAM head, evicting cold heads (ADR-0007/0012) to make room.
    std::optional<core::BufferPool::Region> head;
    const Size head_len = std::min<Size>(size, tiers_.ram_head);
    if (head_len > 0) {
        auto region = ram_.allocate(static_cast<std::uint32_t>(head_len));
        while (!region) {
            const auto victim = policy_->evict();
            if (!victim) break;
            if (const auto vm = index_->lookup(*victim); vm && vm->head.resident()) {
                ram_.deallocate(vm->head.block, vm->head.offset, vm->head.len);
                index_->set_head(*victim, HeadLoc{});
            }
            region = ram_.allocate(static_cast<std::uint32_t>(head_len));
        }
        if (region) head = *region;
    }

    return StoreHandle(this, digest, layout, std::move(*ssd_files), std::move(hdd_files), head);
}

TierManager::StoreHandle::StoreHandle(TierManager* tm, Digest digest, ObjectLayout layout,
                                      ObjectFiles ssd, std::optional<ObjectFiles> hdd,
                                      std::optional<core::BufferPool::Region> head)
    : tm_(tm), digest_(digest), layout_(layout), ssd_(std::move(ssd)), hdd_(std::move(hdd)),
      head_(head) {}

TierManager::StoreHandle::StoreHandle(StoreHandle&& o) noexcept
    : tm_(o.tm_), digest_(o.digest_), layout_(o.layout_), ssd_(std::move(o.ssd_)),
      hdd_(std::move(o.hdd_)), head_(o.head_), off_(o.off_), committed_(o.committed_) {
    o.tm_ = nullptr;
    o.committed_ = true; // neutralize the moved-from handle's destructor
}

TierManager::StoreHandle& TierManager::StoreHandle::operator=(StoreHandle&& o) noexcept {
    if (this != &o) {
        if (!committed_ && tm_ && head_)
            tm_->ram_.deallocate(head_->block, head_->offset, head_->len);
        tm_ = o.tm_;
        digest_ = o.digest_;
        layout_ = o.layout_;
        ssd_ = std::move(o.ssd_);
        hdd_ = std::move(o.hdd_);
        head_ = o.head_;
        off_ = o.off_;
        committed_ = o.committed_;
        o.tm_ = nullptr;
        o.committed_ = true;
    }
    return *this;
}

TierManager::StoreHandle::~StoreHandle() {
    if (!committed_ && tm_ && head_)
        tm_->ram_.deallocate(head_->block, head_->offset, head_->len);
}

Status TierManager::StoreHandle::write(ByteView chunk) {
    const Size len = chunk.size();
    if (off_ + len > layout_.size) return err(Errc::invalid_argument, "write past end of object");

    if (head_) { // fill the resident head as the bytes stream past
        const Size hend = std::min<Size>(off_ + len, head_->len);
        if (off_ < hend) std::memcpy(head_->data + off_, chunk.data(), hend - off_);
    }
    // SSD prefix portion: [off_, min(off_+len, ssd_bytes))
    const Size ssd_stop = std::min<Size>(off_ + len, layout_.ssd_bytes);
    if (off_ < ssd_stop) {
        if (auto st = striped_pwrite(tm_->ssd_.drives(), digest_.bucket(), ssd_.fds(), off_,
                                     ByteView(chunk.data(), ssd_stop - off_));
            !st)
            return st;
    }
    // HDD tail portion: [max(off_, ssd_bytes), off_+len)
    if (off_ + len > layout_.ssd_bytes && tm_->hdd_) {
        const Size begin = std::max<Size>(off_, layout_.ssd_bytes);
        const Size src = begin - off_;
        if (auto st = striped_pwrite(tm_->hdd_->drives(), digest_.bucket(), hdd_->fds(),
                                     begin - layout_.ssd_bytes,
                                     ByteView(chunk.data() + src, (off_ + len) - begin));
            !st)
            return st;
    }
    off_ += len;
    return {};
}

Status TierManager::StoreHandle::commit(std::uint32_t flags) {
    if (off_ != layout_.size)
        return err(Errc::invalid_argument, "commit before the full value was written");

    std::unique_lock<std::shared_mutex> lk(*tm_->mu_); // publish + enforce (exclusive, ADR-0018)
    ObjectMeta meta;
    meta.size = layout_.size;
    meta.flags = flags;
    if (head_) meta.head = HeadLoc{head_->block, head_->offset, head_->len};
    tm_->index_->set(digest_, meta);
    if (head_) tm_->policy_->insert(digest_);
    tm_->object_policy_->insert(digest_); // track for the SSD object-count bound (ADR-0012)
    committed_ = true;
    tm_->enforce_object_bound();          // evict whole objects if over the limit
    return {};
}

Result<std::size_t> TierManager::read(core::Reactor& reactor, const Digest& digest, Offset offset,
                                      MutBytes out) {
    const auto meta = index_->lookup(digest);
    if (!meta) return err(Errc::not_found, "object not in index");
    if (offset >= meta->size) return std::size_t{0};

    const ObjectLayout layout = compute_layout(meta->size, tiers_, three_layer());
    const Size want = std::min<Size>(out.size(), meta->size - offset);
    std::size_t total = 0;

    // RAM head portion: copied out under the lock so a concurrent eviction can't free it mid-read
    // (zero-copy would need pinning — ADR-0018). Residency is re-checked under the lock.
    Size ram_end = 0;
    {
        std::shared_lock<std::shared_mutex> lk(*mu_); // read-only: shares with other readers
        if (const auto m = index_->lookup(digest); m && m->head.resident()) {
            ram_end = m->head.len;
            if (offset < ram_end) {
                const Size n = std::min<Size>(offset + want, ram_end) - offset;
                std::memcpy(out.data(), ram_.addr(m->head.block, m->head.offset) + offset, n);
                total += static_cast<std::size_t>(n);
            }
        }
    }

    // SSD + HDD portions stream from disk on the caller's reactor — no lock held (ADR-0018).
    const Size ssd_begin = std::max<Size>(offset, ram_end);
    const Size ssd_stop = std::min<Size>(offset + want, layout.ssd_bytes);
    if (ssd_begin < ssd_stop) {
        const Size len = ssd_stop - ssd_begin;
        auto files = ssd_.open_object(digest, layout.ssd_bytes, /*create=*/false);
        if (!files) return std::unexpected(files.error());
        auto n = striped_read(reactor, ssd_.drives(), digest.bucket(), files->fds(), ssd_begin,
                              out.subspan(ssd_begin - offset, len));
        if (!n) return std::unexpected(n.error());
        total += *n;
    }
    if (three_layer() && offset + want > layout.ssd_bytes) {
        const Size begin = std::max<Size>(offset, layout.ssd_bytes);
        const Size len = (offset + want) - begin;
        auto files = hdd_->open_object(digest, layout.hdd_bytes, /*create=*/false);
        if (!files) return std::unexpected(files.error());
        auto n = striped_read(reactor, hdd_->drives(), digest.bucket(), files->fds(),
                              begin - layout.ssd_bytes, out.subspan(begin - offset, len));
        if (!n) return std::unexpected(n.error());
        total += *n;
    }
    return total;
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
        ram_.deallocate(meta->head.block, meta->head.offset, meta->head.len);
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

} // namespace goblin::storage
