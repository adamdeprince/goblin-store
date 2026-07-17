#include "goblin/http/mirror.hpp"

#include "goblin/common/types.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <unistd.h>

#if GOBLIN_HAVE_CURL
#include <curl/curl.h>
#endif

namespace goblin::http {
namespace {

#if GOBLIN_HAVE_CURL
std::uint64_t unix_seconds() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string_view trim_line(std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
    return line;
}

std::string_view trim_ows(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return value;
}

std::string lower(std::string_view value) {
    std::string out(value);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char ca = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
        const char cb = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
        if (ca != cb) return false;
    }
    return true;
}

std::optional<Size> response_content_length(const OriginResponseHead& response) {
    std::optional<Size> found;
    for (const auto& header : response.headers) {
        if (!iequals(header.name, "content-length")) continue;
        Size value = 0;
        const std::string_view input = trim_ows(header.value);
        const auto [end, ec] = std::from_chars(input.data(), input.data() + input.size(), value);
        if (ec != std::errc{} || end != input.data() + input.size()) return std::nullopt;
        if (found && *found != value) return std::nullopt;
        found = value;
    }
    if (!found && (response.status == 204 || response.status == 304 ||
                   (response.status >= 100 && response.status < 200)))
        return Size{0};
    return found;
}

struct CurlHeaders {
    curl_slist* value = nullptr;
    ~CurlHeaders() { if (value) ::curl_slist_free_all(value); }
    bool append(std::string_view name, std::string_view value) {
        std::string line;
        line.reserve(name.size() + value.size() + 2);
        line.append(name);
        line += ": ";
        line.append(value);
        curl_slist* next = ::curl_slist_append(this->value, line.c_str());
        if (!next) return false;
        this->value = next;
        return true;
    }
};

struct Transfer {
    MirrorService* service;
    MirrorRequest* request;
    std::shared_ptr<MirrorFetch> fetch;
    std::unique_ptr<OriginResponseHead> parsing;
    std::shared_ptr<const OriginResponseHead> final_head;
    CacheDecision decision;
    std::optional<storage::TierManager::StoreHandle> store;
    std::uint64_t requested_at = 0;
    std::uint64_t response_at = 0;
    Size received = 0;
    bool final_headers = false;
    bool revalidation_304 = false;
    bool write_failed = false;

    Transfer(MirrorService* service_in, MirrorRequest* request_in,
             std::shared_ptr<MirrorFetch> fetch_in)
        : service(service_in), request(request_in), fetch(std::move(fetch_in)) {}

    // libcurl callbacks are C ABI boundaries. Never let allocation or mutex exceptions escape
    // through libcurl; returning zero aborts the transfer and leaves the tentative fill unpublished.
    static std::size_t header(char* data, std::size_t size, std::size_t count,
                              void* opaque) noexcept try {
        const std::size_t bytes = size * count;
        auto& self = *static_cast<Transfer*>(opaque);
        if (self.fetch->cancelled() || self.service->should_stop()) return 0;
        const std::string_view line = trim_line(std::string_view(data, bytes));
        if (line.starts_with("HTTP/")) {
            self.parsing = std::make_unique<OriginResponseHead>();
            const auto first_space = line.find(' ');
            if (first_space == std::string_view::npos) return 0;
            const auto second_space = line.find(' ', first_space + 1);
            const std::string_view code = line.substr(
                first_space + 1,
                (second_space == std::string_view::npos ? line.size() : second_space) - first_space - 1);
            unsigned status = 0;
            const auto [end, ec] = std::from_chars(code.data(), code.data() + code.size(), status);
            if (ec != std::errc{} || end != code.data() + code.size() || status > 999) return 0;
            self.parsing->status = status;
            if (second_space != std::string_view::npos)
                self.parsing->reason = std::string(line.substr(second_space + 1));
            return bytes;
        }
        if (!self.parsing) return bytes;
        if (!line.empty()) {
            const auto colon = line.find(':');
            if (colon == std::string_view::npos) return 0;
            if (self.parsing->headers.size() >= 256) return 0;
            self.parsing->headers.push_back(
                {lower(line.substr(0, colon)), std::string(trim_ows(line.substr(colon + 1)))});
            return bytes;
        }
        if (self.parsing->status < 200) return bytes; // interim response; the next status line resets it

        self.response_at = unix_seconds();
        self.parsing->content_length = response_content_length(*self.parsing);
        self.final_head = std::shared_ptr<const OriginResponseHead>(self.parsing.release());
        self.final_headers = true;
        self.revalidation_304 = self.request->stale && self.final_head->status == 304;
        if (self.revalidation_304) return bytes; // the cached representation, not this 304, is downstream

        self.decision = evaluate_cacheability(*self.final_head, self.request->cache_policy,
                                              self.requested_at, self.response_at);
        if (self.request->head_only) self.decision.cacheable = false;
        self.fetch->publish_headers(self.final_head);

        // Reserve the tentative object while the event loop is transmitting the response headers.
        if (self.decision.cacheable && self.final_head->content_length) {
            auto opened = self.service->tier_manager().begin_store(
                self.request->digest, *self.final_head->content_length, WriteMode::evict);
            if (opened) self.store.emplace(std::move(*opened));
        }
        return bytes;
    } catch (...) {
        return 0;
    }

    static std::size_t body(char* data, std::size_t size, std::size_t count,
                            void* opaque) noexcept try {
        const std::size_t bytes = size * count;
        auto& self = *static_cast<Transfer*>(opaque);
        if (self.fetch->cancelled() || self.service->should_stop()) return 0;
        if (!self.final_headers || self.revalidation_304) return bytes == 0 ? 0 : bytes;
        if (!self.fetch->wait_for_header_ack()) return 0;

        std::shared_ptr<const std::vector<std::byte>> chunk;
        std::uint64_t sequence = 0;
        if (self.fetch->client_attached()) {
            auto copy = std::make_shared<std::vector<std::byte>>(bytes);
            std::memcpy(copy->data(), data, bytes);
            chunk = std::move(copy);
            sequence = self.fetch->publish_chunk(chunk);
        }

        // The send and this write now race. Returning from the callback (and therefore receiving the
        // next origin bytes) is gated on both this synchronous write and the send acknowledgement.
        if (self.store && !self.write_failed) {
            const ByteView view(reinterpret_cast<const std::byte*>(data), bytes);
            auto status = self.store->write(view);
            if (status) status = self.store->flush_available();
            if (!status) {
                self.write_failed = true;
                self.store.reset();
            }
        }
        self.received += bytes;
        if (chunk && !self.fetch->wait_for_chunk_ack(sequence)) return 0;
        return bytes;
    } catch (...) {
        return 0;
    }

    static int progress(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
        try {
            auto& self = *static_cast<Transfer*>(opaque);
            return (self.fetch->cancelled() || self.service->should_stop()) ? 1 : 0;
        } catch (...) {
            return 1;
        }
    }
};
#endif

} // namespace

Result<std::shared_ptr<MirrorFetch>> MirrorFetch::create() {
    int fds[2] = {-1, -1};
    if (::pipe(fds) < 0) return err(Errc::io_error, "create mirror notification pipe");
    for (const int fd : fds) {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        const int descriptor_flags = ::fcntl(fd, F_GETFD, 0);
        if (flags < 0 || descriptor_flags < 0 ||
            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
            ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
            const int saved = errno;
            ::close(fds[0]);
            ::close(fds[1]);
            errno = saved;
            return err(Errc::io_error, "configure mirror notification pipe");
        }
    }
    return std::shared_ptr<MirrorFetch>(new MirrorFetch(fds[0], fds[1]));
}

MirrorFetch::~MirrorFetch() {
    if (notify_read_ >= 0) ::close(notify_read_);
    if (notify_write_ >= 0) ::close(notify_write_);
}

void MirrorFetch::notify() noexcept {
    const std::byte one{1};
    for (;;) {
        const ssize_t result = ::write(notify_write_, &one, 1);
        if (result == 1 || (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) return;
        if (result < 0 && errno == EINTR) continue;
        return;
    }
}

void MirrorFetch::drain_notification() noexcept {
    std::byte bytes[64];
    for (;;) {
        const ssize_t result = ::read(notify_read_, bytes, sizeof bytes);
        if (result > 0) continue;
        if (result < 0 && errno == EINTR) continue;
        return;
    }
}

MirrorFetch::View MirrorFetch::view() const {
    std::lock_guard lock(mu_);
    return {response_, chunk_, chunk_sequence_, cache_ready_, done_, failed_,
            headers_published_, error_};
}

bool MirrorFetch::client_attached() const {
    std::lock_guard lock(mu_);
    return client_attached_;
}

bool MirrorFetch::cancelled() const {
    std::lock_guard lock(mu_);
    return cancelled_;
}

void MirrorFetch::acknowledge_headers() {
    {
        std::lock_guard lock(mu_);
        headers_acknowledged_ = true;
    }
    cv_.notify_all();
}

void MirrorFetch::acknowledge_chunk(std::uint64_t sequence) {
    {
        std::lock_guard lock(mu_);
        acknowledged_sequence_ = std::max(acknowledged_sequence_, sequence);
        if (chunk_sequence_ == sequence) chunk_.reset();
    }
    cv_.notify_all();
}

void MirrorFetch::detach_client() {
    {
        std::lock_guard lock(mu_);
        client_attached_ = false;
        headers_acknowledged_ = true;
        acknowledged_sequence_ = std::numeric_limits<std::uint64_t>::max();
        chunk_.reset();
    }
    cv_.notify_all();
}

void MirrorFetch::cancel() {
    {
        std::lock_guard lock(mu_);
        cancelled_ = true;
        client_attached_ = false;
        headers_acknowledged_ = true;
        acknowledged_sequence_ = std::numeric_limits<std::uint64_t>::max();
    }
    cv_.notify_all();
    notify();
}

void MirrorFetch::publish_headers(std::shared_ptr<const OriginResponseHead> response) {
    bool wake = false;
    {
        std::lock_guard lock(mu_);
        response_ = std::move(response);
        headers_published_ = true;
        wake = client_attached_;
    }
    if (wake) notify();
}

bool MirrorFetch::wait_for_header_ack() {
    std::unique_lock lock(mu_);
    cv_.wait(lock, [&] { return headers_acknowledged_ || !client_attached_ || cancelled_; });
    return !cancelled_;
}

std::uint64_t MirrorFetch::publish_chunk(std::shared_ptr<const std::vector<std::byte>> chunk) {
    bool wake = false;
    std::uint64_t sequence = 0;
    {
        std::lock_guard lock(mu_);
        sequence = ++chunk_sequence_;
        chunk_ = std::move(chunk);
        wake = client_attached_;
    }
    if (wake) notify();
    return sequence;
}

bool MirrorFetch::wait_for_chunk_ack(std::uint64_t sequence) {
    std::unique_lock lock(mu_);
    cv_.wait(lock, [&] {
        return acknowledged_sequence_ >= sequence || !client_attached_ || cancelled_;
    });
    return !cancelled_;
}

void MirrorFetch::finish(bool cache_ready) {
    bool wake = false;
    {
        std::lock_guard lock(mu_);
        cache_ready_ = cache_ready;
        done_ = true;
        wake = client_attached_;
    }
    cv_.notify_all();
    if (wake) notify();
}

void MirrorFetch::fail(std::string detail) {
    bool wake = false;
    {
        std::lock_guard lock(mu_);
        failed_ = true;
        done_ = true;
        error_ = std::move(detail);
        wake = client_attached_;
    }
    cv_.notify_all();
    if (wake) notify();
}

Result<std::unique_ptr<MirrorService>> MirrorService::create(
    std::string base_url, storage::TierManager& tm, storage::Index& index,
    const std::atomic<bool>* shutdown, unsigned workers) {
#if !GOBLIN_HAVE_CURL
    (void)base_url; (void)tm; (void)index; (void)shutdown; (void)workers;
    return err(Errc::unsupported, "--mirror requested, but this build has no libcurl support");
#else
    if (::curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        return err(Errc::io_error, "initialize libcurl");
    auto service = std::unique_ptr<MirrorService>(
        new MirrorService(std::move(base_url), tm, index, shutdown));
    workers = std::max(1u, workers);
    try {
        service->workers_.reserve(workers);
        for (unsigned i = 0; i < workers; ++i)
            service->workers_.emplace_back([ptr = service.get()] { ptr->worker(); });
    } catch (const std::system_error&) {
        return err(Errc::io_error, "start mirror fetch worker");
    }
    return service;
#endif
}

MirrorService::~MirrorService() {
    std::vector<std::shared_ptr<MirrorFetch>> cancel;
    {
        std::lock_guard lock(mu_);
        stopping_ = true;
        for (auto& task : queue_) cancel.push_back(task.fetch);
        for (auto& entry : flights_)
            for (auto& task : entry.second.followers) cancel.push_back(task.fetch);
        flights_.clear();
        cancel.insert(cancel.end(), active_.begin(), active_.end());
    }
    for (const auto& fetch : cancel) fetch->cancel();
    cv_.notify_all();
    for (auto& worker : workers_) if (worker.joinable()) worker.join();
#if GOBLIN_HAVE_CURL
    ::curl_global_cleanup();
#endif
}

Result<std::shared_ptr<MirrorFetch>> MirrorService::fetch(MirrorRequest request) {
    auto state = MirrorFetch::create();
    if (!state) return std::unexpected(state.error());
    Task task{std::move(request), *state};
    bool cache_ready = false;
    bool wake_worker = false;
    {
        std::lock_guard lock(mu_);
        if (stopping_) return err(Errc::io_error, "mirror service is stopping");
        std::size_t waiting = queue_.size();
        for (const auto& [digest, flight] : flights_) waiting += flight.followers.size();
        if (waiting >= kMaximumQueuedFetches)
            return err(Errc::would_block, "mirror fetch queue is full");

        // Close the race between the event loop's miss snapshot and a fill that committed just
        // before this call reached the service.
        if (cache_ready_for(task.request)) {
            cache_ready = true;
        } else if (auto flight = flights_.find(task.request.digest); flight != flights_.end()) {
            flight->second.followers.push_back(std::move(task));
        } else {
            flights_.try_emplace(task.request.digest);
            queue_.push_back(std::move(task));
            wake_worker = true;
        }
    }
    if (cache_ready) (*state)->finish(/*cache_ready=*/true);
    if (wake_worker) cv_.notify_one();
    return *state;
}

bool MirrorService::cache_ready_for(const MirrorRequest& request) const {
    const auto record = index_.lookup_with_http(request.digest);
    const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    return record && record->http &&
           !storage::is_expired(record->meta, storage::now_unix()) &&
           cache_is_fresh(*record->http, request.cache_policy, now);
}

void MirrorService::enqueue_followup(Task task) {
    bool cache_ready = false;
    bool cancel = false;
    bool wake_worker = false;
    {
        std::lock_guard lock(mu_);
        if (stopping_) {
            cancel = true;
        } else if (cache_ready_for(task.request)) {
            cache_ready = true;
        } else if (auto flight = flights_.find(task.request.digest); flight != flights_.end()) {
            flight->second.followers.push_back(std::move(task));
        } else {
            flights_.try_emplace(task.request.digest);
            queue_.push_back(std::move(task));
            wake_worker = true;
        }
    }
    if (cache_ready) task.fetch->finish(/*cache_ready=*/true);
    if (cancel) task.fetch->cancel();
    if (wake_worker) cv_.notify_one();
}

void MirrorService::complete_flight(Task& leader) {
    std::deque<Task> followers;
    {
        std::lock_guard lock(mu_);
        const auto flight = flights_.find(leader.request.digest);
        if (flight == flights_.end()) return; // service shutdown already detached the flight
        followers = std::move(flight->second.followers);
        flights_.erase(flight);
    }

    const MirrorFetch::View outcome = leader.fetch->view();
    for (auto& follower : followers) {
        if (cache_ready_for(follower.request)) {
            follower.fetch->finish(/*cache_ready=*/true);
        } else if (outcome.failed) {
            // Let every waiting connection apply its own stale-if-error policy without issuing the
            // same failed origin request serially.
            follower.fetch->fail(outcome.error);
        } else {
            // A successful response can be intentionally non-cacheable. Such followers still need
            // their own origin response; requeue one as the next leader and coalesce behind it.
            enqueue_followup(std::move(follower));
        }
    }
}

void MirrorService::worker() {
    for (;;) {
        std::optional<Task> task;
        {
            std::unique_lock lock(mu_);
            cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) return;
            task.emplace(std::move(queue_.front()));
            queue_.pop_front();
            active_.push_back(task->fetch);
        }
        perform(*task);
        complete_flight(*task);
        {
            std::lock_guard lock(mu_);
            const auto it = std::find(active_.begin(), active_.end(), task->fetch);
            if (it != active_.end()) active_.erase(it);
        }
    }
}

void MirrorService::perform(Task& task) {
#if !GOBLIN_HAVE_CURL
    task.fetch->fail("libcurl unavailable");
#else
    auto url = mirror_origin_url(base_url_, task.request.target);
    if (!url) { task.fetch->fail(url.error().detail); return; }
    CURL* easy = ::curl_easy_init();
    if (!easy) { task.fetch->fail("create origin HTTP handle"); return; }
    struct EasyCleanup { void operator()(CURL* value) const { ::curl_easy_cleanup(value); } };
    std::unique_ptr<CURL, EasyCleanup> cleanup(easy);

    CurlHeaders request_headers;
    for (const auto& header : task.request.headers) {
        if (task.request.stale &&
            (iequals(header.name, "if-none-match") || iequals(header.name, "if-modified-since")))
            continue;
        if (!request_headers.append(header.name, header.value)) {
            task.fetch->fail("allocate origin request headers");
            return;
        }
    }
    if (task.request.stale) {
        if (!task.request.stale->etag.empty())
            request_headers.append("if-none-match", task.request.stale->etag);
        else if (!task.request.stale->last_modified.empty())
            request_headers.append("if-modified-since", task.request.stale->last_modified);
    }

    Transfer transfer{this, &task.request, task.fetch};
    transfer.requested_at = unix_seconds();
    ::curl_easy_setopt(easy, CURLOPT_URL, url->c_str());
    ::curl_easy_setopt(easy, CURLOPT_HTTPHEADER, request_headers.value);
    ::curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, &Transfer::header);
    ::curl_easy_setopt(easy, CURLOPT_HEADERDATA, &transfer);
    ::curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &Transfer::body);
    ::curl_easy_setopt(easy, CURLOPT_WRITEDATA, &transfer);
    ::curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, &Transfer::progress);
    ::curl_easy_setopt(easy, CURLOPT_XFERINFODATA, &transfer);
    ::curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
    ::curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    ::curl_easy_setopt(easy, CURLOPT_TCP_NODELAY, 1L);
    ::curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 10'000L);
    ::curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
    ::curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, 30L);
    ::curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
    ::curl_easy_setopt(easy, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500 // CURLOPT_PROTOCOLS_STR was added in curl 7.85.0.
    ::curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    ::curl_easy_setopt(easy, CURLOPT_PROTOCOLS,
                       static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    ::curl_easy_setopt(easy, CURLOPT_USERAGENT, "goblin-store/0.0.2");
    if (task.request.head_only) ::curl_easy_setopt(easy, CURLOPT_NOBODY, 1L);

    const CURLcode result = ::curl_easy_perform(easy);
    if (result != CURLE_OK) {
        transfer.store.reset();
        if (!task.fetch->cancelled()) task.fetch->fail(::curl_easy_strerror(result));
        return;
    }
    if (!transfer.final_headers) {
        task.fetch->fail("origin returned no final HTTP response");
        return;
    }

    if (transfer.revalidation_304) {
        OriginResponseHead merged;
        merged.status = task.request.stale->status;
        merged.reason = task.request.stale->reason;
        merged.content_length = index_.lookup(task.request.digest).transform(
            [](const storage::ObjectMeta& meta) { return meta.size; });
        for (const auto& header : task.request.stale->headers)
            merged.headers.push_back({header.name, header.value});
        // A 304 replaces fields it supplies while retaining the cached response's other end-to-end
        // metadata. This covers validators and freshness fields without touching the stored body.
        for (const auto& update : transfer.final_head->headers) {
            auto it = std::remove_if(merged.headers.begin(), merged.headers.end(),
                                     [&](const OwnedHeader& old) {
                                         return iequals(old.name, update.name);
                                     });
            merged.headers.erase(it, merged.headers.end());
            merged.headers.push_back(update);
        }
        const std::uint64_t response_time = unix_seconds();
        auto refreshed = evaluate_cacheability(merged, task.request.cache_policy,
                                               transfer.requested_at, response_time);
        if (refreshed.cacheable && refreshed.metadata && merged.content_length &&
            index_.update_http_if_etag(task.request.digest, task.request.stale_object_etag,
                                       std::move(refreshed.metadata))) {
            task.fetch->finish(/*cache_ready=*/true);
        } else {
            task.fetch->fail("origin revalidation raced with cache replacement");
        }
        return;
    }

    if (!task.request.head_only && transfer.final_head->content_length &&
        transfer.received != *transfer.final_head->content_length) {
        transfer.store.reset();
        task.fetch->fail("origin body length did not match Content-Length");
        return;
    }
    if (transfer.store && transfer.decision.metadata) {
        auto committed = transfer.store->commit(0, 0, 0, transfer.decision.metadata);
        if (!committed) transfer.store.reset(); // client response remains successful; this fill is discarded
    }
    task.fetch->finish();
#endif
}

} // namespace goblin::http
