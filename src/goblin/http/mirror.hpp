#pragma once

#include "goblin/common/error.hpp"
#include "goblin/crypto/sha256.hpp"
#include "goblin/http/mirror_cache.hpp"
#include "goblin/storage/index.hpp"
#include "goblin/storage/tier_manager.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace goblin::http {

struct MirrorRequest {
    std::string target; // origin-form, query included
    std::vector<OwnedHeader> headers;
    RequestCachePolicy cache_policy;
    crypto::Digest digest;
    bool head_only = false;
    std::uint64_t stale_object_etag = 0;
    std::shared_ptr<const storage::HttpCacheMetadata> stale;
};

// Cross-thread, single-subscriber rendezvous for one origin request. At most one body chunk is
// published. The producer waits for its downstream acknowledgement before libcurl can receive the
// next chunk; detaching the subscriber releases that gate while cache population continues.
class MirrorFetch {
public:
    struct View {
        std::shared_ptr<const OriginResponseHead> response;
        std::shared_ptr<const std::vector<std::byte>> chunk;
        std::uint64_t chunk_sequence = 0;
        bool cache_ready = false;
        bool done = false;
        bool failed = false;
        bool headers_published = false;
        std::string error;
    };

    ~MirrorFetch();
    MirrorFetch(const MirrorFetch&) = delete;
    MirrorFetch& operator=(const MirrorFetch&) = delete;

    int notification_fd() const noexcept { return notify_read_; }
    void drain_notification() noexcept;
    View view() const;
    void acknowledge_headers();
    void acknowledge_chunk(std::uint64_t sequence);
    void detach_client();

    // Producer side, used by the bounded origin worker. Public only to keep the libcurl callbacks
    // ordinary free functions; callers outside MirrorService have no reason to invoke these.
    bool client_attached() const;
    bool cancelled() const;
    void publish_headers(std::shared_ptr<const OriginResponseHead>);
    bool wait_for_header_ack();
    std::uint64_t publish_chunk(std::shared_ptr<const std::vector<std::byte>>);
    bool wait_for_chunk_ack(std::uint64_t sequence);

private:
    friend class MirrorService;
    static Result<std::shared_ptr<MirrorFetch>> create();
    MirrorFetch(int notify_read, int notify_write) noexcept
        : notify_read_(notify_read), notify_write_(notify_write) {}

    void cancel();
    void finish(bool cache_ready = false);
    void fail(std::string detail);
    void notify() noexcept;

    int notify_read_ = -1;
    int notify_write_ = -1;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::shared_ptr<const OriginResponseHead> response_;
    std::shared_ptr<const std::vector<std::byte>> chunk_;
    std::uint64_t chunk_sequence_ = 0;
    std::uint64_t acknowledged_sequence_ = 0;
    bool headers_published_ = false;
    bool headers_acknowledged_ = false;
    bool client_attached_ = true;
    bool cancelled_ = false;
    bool cache_ready_ = false;
    bool done_ = false;
    bool failed_ = false;
    std::string error_;
};

class MirrorService {
public:
    static Result<std::unique_ptr<MirrorService>> create(
        std::string base_url, storage::TierManager& tm, storage::Index& index,
        const std::atomic<bool>* shutdown, unsigned workers);
    ~MirrorService();
    MirrorService(const MirrorService&) = delete;
    MirrorService& operator=(const MirrorService&) = delete;

    Result<std::shared_ptr<MirrorFetch>> fetch(MirrorRequest request);
    storage::TierManager& tier_manager() noexcept { return tm_; } // origin worker's tentative fill
    bool should_stop() const noexcept {
        return shutdown_ && shutdown_->load(std::memory_order_relaxed);
    }
    static constexpr bool available() noexcept {
#if GOBLIN_HAVE_CURL
        return true;
#else
        return false;
#endif
    }

private:
    struct Task {
        MirrorRequest request;
        std::shared_ptr<MirrorFetch> fetch;
    };
    struct Flight {
        std::deque<Task> followers;
    };
    MirrorService(std::string base_url, storage::TierManager& tm, storage::Index& index,
                  const std::atomic<bool>* shutdown)
        : base_url_(std::move(base_url)), tm_(tm), index_(index), shutdown_(shutdown) {}
    void worker();
    void perform(Task&);
    bool cache_ready_for(const MirrorRequest&) const;
    void enqueue_followup(Task);
    void complete_flight(Task&);

    std::string base_url_;
    storage::TierManager& tm_;
    storage::Index& index_;
    const std::atomic<bool>* shutdown_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Task> queue_;
    std::unordered_map<crypto::Digest, Flight, storage::DigestHash> flights_;
    std::vector<std::shared_ptr<MirrorFetch>> active_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
    static constexpr std::size_t kMaximumQueuedFetches = 1024;
};

} // namespace goblin::http
