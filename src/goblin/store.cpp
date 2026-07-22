#include "goblin/store.hpp"

#include "goblin/core/reactor.hpp"
#include "goblin/crypto/sha256.hpp"
#include "goblin/storage/index.hpp"
#include "goblin/storage/pool_dir.hpp"
#include "goblin/storage/tier_manager.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <utility>

namespace goblin {

namespace {

ObjectInfo public_info(const storage::ObjectMeta& meta) noexcept {
    return {meta.size, meta.flags, meta.expiry, meta.etag};
}

Status validate_store_options(const StoreOptions& options) {
    ServerConfig config;
    config.tiers = options.tiers;
    config.memory = options.memory;
    config.ssd = options.ssd;
    config.hdd = options.hdd;
    config.eviction = options.eviction;
    config.access_score = options.access_score;
    config.io_chunk_bytes = options.read_chunk_bytes;
    config.write_io_chunk_bytes = options.write_chunk_bytes;
    config.file_handle_cache = options.file_handle_cache;
    config.io_buffers = options.write_buffers;
    config.max_object_size = options.max_object_size;
    config.cache_bypass = options.direct_io ? CacheBypass::o_direct : CacheBypass::buffered;
    return validate(config);
}

} // namespace

struct Store::Impl {
    explicit Impl(StoreOptions configured) : options(std::move(configured)) {}

    StoreOptions options;
    storage::Index index;
    std::optional<storage::TierManager> tiers;
};

struct StoreWriter::Impl {
    Impl(std::shared_ptr<Store::Impl> owner,
         storage::TierManager::StoreHandle pending,
         Size object_size, PutOptions configured)
        : store(std::move(owner)), handle(std::move(pending)), size(object_size),
          options(configured) {}

    std::shared_ptr<Store::Impl> store;
    std::optional<storage::TierManager::StoreHandle> handle;
    Size size = 0;
    PutOptions options;
};

struct StoreReader::Impl {
    Impl(std::shared_ptr<Store::Impl> owner, Size requested_chunk,
         std::optional<core::Reactor> ring)
        : store(std::move(owner)), chunk_bytes(requested_chunk), reactor(std::move(ring)) {}

    ~Impl() {
        std::free(buffer);
        std::free(materialized);
    }

    std::shared_ptr<Store::Impl> store;
    Size chunk_bytes = 0;
    void* buffer = nullptr;
    void* materialized = nullptr;
    Size materialized_capacity = 0;
    std::optional<core::Reactor> reactor;
};

StoreWriter::StoreWriter(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
StoreWriter::StoreWriter(StoreWriter&&) noexcept = default;
StoreWriter& StoreWriter::operator=(StoreWriter&&) noexcept = default;
StoreWriter::~StoreWriter() = default;

Status StoreWriter::write(ByteView bytes) {
    if (!impl_ || !impl_->handle)
        return err(Errc::invalid_argument, "writer is no longer active");
    return impl_->handle->write(bytes);
}

Result<ObjectInfo> StoreWriter::commit() {
    if (!impl_ || !impl_->handle)
        return err(Errc::invalid_argument, "writer is no longer active");
    auto etag = impl_->handle->commit(impl_->options.flags, impl_->options.expiry);
    if (!etag) return std::unexpected(etag.error());
    impl_->handle.reset();
    return ObjectInfo{impl_->size, impl_->options.flags, impl_->options.expiry, *etag};
}

StoreReader::StoreReader(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
StoreReader::StoreReader(StoreReader&&) noexcept = default;
StoreReader& StoreReader::operator=(StoreReader&&) noexcept = default;
StoreReader::~StoreReader() = default;

Result<ObjectInfo> StoreReader::stream(std::string_view key, const ChunkSink& sink,
                                       bool record_access) {
    if (!impl_ || !sink)
        return err(Errc::invalid_argument, "reader and chunk callback must be valid");
    auto& tiers = *impl_->store->tiers;
    const auto digest = crypto::hash_key(key);
    auto snapshot = tiers.open_snapshot(digest, record_access);
    if (!snapshot) return err(Errc::not_found, "object not found");
    if (record_access)
        tiers.touch_policies(digest, snapshot->meta.size, snapshot->pin.valid);

    struct PinGuard {
        storage::TierManager* tiers;
        storage::TierManager::HeadPin pin;
        ~PinGuard() {
            if (pin.valid) tiers->unpin_head(pin);
        }
    } pin_guard{&tiers, snapshot->pin};

    const ObjectInfo result = public_info(snapshot->meta);
    Size offset = 0;
    if (snapshot->pin.valid) {
        const ByteView head = tiers.pinned_bytes(snapshot->pin).first(
            static_cast<std::size_t>(std::min<Size>(snapshot->pin.len, snapshot->meta.size)));
        if (!head.empty()) {
            if (auto accepted = sink(head); !accepted) return std::unexpected(accepted.error());
            offset = head.size();
        }
    }

    if (offset < snapshot->meta.size) {
        if (!snapshot->rs)
            return err(Errc::io_error, "object tail has no readable storage snapshot");
        MutBytes scratch(static_cast<std::byte*>(impl_->buffer),
                         static_cast<std::size_t>(impl_->chunk_bytes));
        while (offset < snapshot->meta.size) {
            Result<std::size_t> read = impl_->reactor
                ? snapshot->rs->read(*impl_->reactor, offset, scratch)
                : snapshot->rs->read_sync(offset, scratch);
            if (!read) return std::unexpected(read.error());
            if (*read == 0 || *read > snapshot->meta.size - offset)
                return err(Errc::io_error, "invalid object-tail read length");
            if (auto accepted = sink(ByteView(scratch.data(), *read)); !accepted)
                return std::unexpected(accepted.error());
            offset += *read;
        }
    }
    return result;
}

Result<std::vector<std::byte>> StoreReader::get(std::string_view key, bool record_access) {
    auto loaded = load(key, {}, record_access);
    if (!loaded) return std::unexpected(loaded.error());
    try {
        return std::vector<std::byte>(loaded->bytes.begin(), loaded->bytes.end());
    } catch (const std::bad_alloc&) {
        return err(Errc::out_of_memory, "allocate returned object vector");
    }
}

Result<LoadedObject> StoreReader::load(std::string_view key, const ChunkSink& progress,
                                       bool record_access) {
    if (!impl_) return err(Errc::invalid_argument, "reader must be valid");
    auto& tiers = *impl_->store->tiers;
    const auto digest = crypto::hash_key(key);
    auto snapshot = tiers.open_snapshot(digest, record_access);
    if (!snapshot) return err(Errc::not_found, "object not found");
    if (record_access)
        tiers.touch_policies(digest, snapshot->meta.size, snapshot->pin.valid);

    struct PinGuard {
        storage::TierManager* tiers;
        storage::TierManager::HeadPin pin;
        ~PinGuard() {
            if (pin.valid) tiers->unpin_head(pin);
        }
    } pin_guard{&tiers, snapshot->pin};

    if (snapshot->meta.size >
        static_cast<Size>(std::numeric_limits<std::size_t>::max()) - (kDeviceBlock - 1))
        return err(Errc::too_large, "object does not fit in the process address space");
    const Size needed = align_up(snapshot->meta.size, kDeviceBlock);
    if (needed > impl_->materialized_capacity) {
        void* replacement = nullptr;
        if (::posix_memalign(&replacement, static_cast<std::size_t>(kDeviceBlock),
                             static_cast<std::size_t>(needed)) != 0)
            return err(Errc::out_of_memory, "grow retained materialization buffer");
        std::free(impl_->materialized);
        impl_->materialized = replacement;
        impl_->materialized_capacity = needed;
    }

    auto* destination = static_cast<std::byte*>(impl_->materialized);
    Size offset = 0;
    if (snapshot->pin.valid) {
        const ByteView head = tiers.pinned_bytes(snapshot->pin).first(
            static_cast<std::size_t>(std::min<Size>(snapshot->pin.len, snapshot->meta.size)));
        if (!head.empty()) {
            std::memcpy(destination, head.data(), head.size());
            const ByteView ready(destination, head.size());
            if (progress) {
                if (auto accepted = progress(ready); !accepted)
                    return std::unexpected(accepted.error());
            }
            offset = head.size();
        }
    }

    if (offset < snapshot->meta.size) {
        if (!snapshot->rs)
            return err(Errc::io_error, "object tail has no readable storage snapshot");
        while (offset < snapshot->meta.size) {
            const Size logical =
                std::min<Size>(impl_->chunk_bytes, snapshot->meta.size - offset);
            const Size direct_bytes = align_up(logical, kDeviceBlock);
            if (direct_bytes > impl_->materialized_capacity - offset)
                return err(Errc::io_error, "retained materialization buffer lacks tail padding");
            MutBytes target(destination + offset, static_cast<std::size_t>(direct_bytes));
            Result<std::size_t> read = impl_->reactor
                ? snapshot->rs->read(*impl_->reactor, offset, target)
                : snapshot->rs->read_sync(offset, target);
            if (!read) return std::unexpected(read.error());
            if (*read != logical)
                return err(Errc::io_error, "invalid direct materialization read length");
            const ByteView ready(destination + offset, *read);
            if (progress) {
                if (auto accepted = progress(ready); !accepted)
                    return std::unexpected(accepted.error());
            }
            offset += *read;
        }
    }

    const ObjectInfo info = public_info(snapshot->meta);
    return LoadedObject{
        info, ByteView(destination, static_cast<std::size_t>(snapshot->meta.size))};
}

Size StoreReader::materialization_capacity() const noexcept {
    return impl_ ? impl_->materialized_capacity : 0;
}

bool StoreReader::uses_io_uring() const noexcept {
    return impl_ && impl_->reactor.has_value();
}

Store::Store(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Status Store::prepare_directory(const std::string& path) {
    return storage::prepare_pool_dir(path);
}

Result<Store> Store::open(StoreOptions options) {
    if (auto valid = validate_store_options(options); !valid)
        return std::unexpected(valid.error());
    for (const auto& path : options.ssd.dirs) {
        if (auto wiped = storage::wipe_pool_dir(path); !wiped)
            return std::unexpected(wiped.error());
    }
    for (const auto& path : options.hdd.dirs) {
        if (auto wiped = storage::wipe_pool_dir(path); !wiped)
            return std::unexpected(wiped.error());
    }

    auto impl = std::make_shared<Impl>(std::move(options));
    auto tiers = storage::TierManager::open(
        impl->options.tiers, impl->options.memory, impl->options.eviction,
        impl->options.ssd, impl->options.hdd, impl->index,
        impl->options.read_chunk_bytes, impl->options.write_buffers,
        impl->options.direct_io, impl->options.access_score,
        impl->options.write_chunk_bytes, impl->options.max_object_size,
        impl->options.file_handle_cache);
    if (!tiers) return std::unexpected(tiers.error());
    impl->tiers.emplace(std::move(*tiers));
    return Store(std::move(impl));
}

Result<StoreWriter> Store::begin_put(std::string_view key, Size size, PutOptions options) {
    if (!impl_ || !impl_->tiers) return err(Errc::invalid_argument, "store is not open");
    auto handle = impl_->tiers->begin_store(crypto::hash_key(key), size, options.write_mode);
    if (!handle) return std::unexpected(handle.error());
    return StoreWriter(std::make_unique<StoreWriter::Impl>(
        impl_, std::move(*handle), size, options));
}

Status Store::put(std::string_view key, ByteView value, PutOptions options) {
    auto writer = begin_put(key, value.size(), options);
    if (!writer) return std::unexpected(writer.error());
    if (auto written = writer->write(value); !written) return written;
    auto committed = writer->commit();
    if (!committed) return std::unexpected(committed.error());
    return {};
}

Result<ObjectInfo> Store::info(std::string_view key) const {
    if (!impl_ || !impl_->tiers) return err(Errc::invalid_argument, "store is not open");
    const auto found = impl_->tiers->lookup_live(crypto::hash_key(key));
    if (!found) return err(Errc::not_found, "object not found");
    return public_info(*found);
}

bool Store::erase(std::string_view key) {
    return impl_ && impl_->tiers && impl_->tiers->remove(crypto::hash_key(key));
}

void Store::flush_all(std::uint32_t delay_seconds) {
    if (impl_ && impl_->tiers) impl_->tiers->flush_all(delay_seconds);
}

Result<StoreReader> Store::make_reader(Size chunk_bytes, unsigned io_uring_entries) const {
    if (!impl_ || !impl_->tiers) return err(Errc::invalid_argument, "store is not open");
    if (chunk_bytes == 0) chunk_bytes = impl_->options.read_chunk_bytes;
    if (chunk_bytes == 0 ||
        chunk_bytes > std::numeric_limits<std::size_t>::max() - (kDeviceBlock - 1))
        return err(Errc::invalid_argument, "reader chunk size is invalid");
    chunk_bytes = align_up(chunk_bytes, kDeviceBlock);

    std::optional<core::Reactor> reactor;
    if (io_uring_entries != 0 && core::Reactor::available()) {
        auto created = core::Reactor::create(io_uring_entries);
        if (created) reactor.emplace(std::move(*created));
        // An installed liburing with a kernel/policy that rejects queue creation is a supported
        // deployment: the embedded reader falls back to aligned pread rather than failing open.
    }

    auto reader = std::make_unique<StoreReader::Impl>(impl_, chunk_bytes, std::move(reactor));
    if (::posix_memalign(&reader->buffer, static_cast<std::size_t>(kDeviceBlock),
                         static_cast<std::size_t>(chunk_bytes)) != 0)
        return err(Errc::out_of_memory, "allocate aligned embedded-reader buffer");
    return StoreReader(std::move(reader));
}

} // namespace goblin
