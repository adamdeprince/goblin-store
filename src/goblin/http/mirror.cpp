#include "goblin/http/mirror.hpp"

#include "goblin/common/types.hpp"
#include "goblin/core/reactor.hpp"
#include "picohttpparser.h"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <span>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>
#include <utility>

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

// libcurl receive quantum: one published body chunk at a time (ADR-0021), so this dominates
// callback/mutex/pipe overhead on any high-speed TCP path (Ethernet or IPoIB).
// 1 MiB => ~5k rendezvous/s at 40 Gbit/s vs ~300k/s at libcurl's 16 KiB default.
inline constexpr long kMirrorCurlBufferBytes = static_cast<long>(kMirrorIoChunk);

// Portable TCP buffer sizing at connect time (Ethernet NICs and IPoIB). Keep-alive reuse via the
// worker-local easy handle carries these across subsequent misses to the same peer.
int origin_sockopt(void* /*unused*/, curl_socket_t fd, curlsocktype purpose) noexcept {
    if (purpose != CURLSOCKTYPE_IPCXN) return CURL_SOCKOPT_OK;
    const int bytes = static_cast<int>(kHighBdpSocketBuffer);
    (void)::setsockopt(static_cast<int>(fd), SOL_SOCKET, SO_RCVBUF, &bytes, sizeof bytes);
    (void)::setsockopt(static_cast<int>(fd), SOL_SOCKET, SO_SNDBUF, &bytes, sizeof bytes);
    return CURL_SOCKOPT_OK;
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
    bool headers_acked = false; // local: after the first wait, skip the mutex trip per body chunk

    Transfer(MirrorService* service_in, MirrorRequest* request_in,
             std::shared_ptr<MirrorFetch> fetch_in)
        : service(service_in), request(request_in), fetch(std::move(fetch_in)) {}

    void accept_final_head(std::unique_ptr<OriginResponseHead> head) {
        response_at = unix_seconds();
        head->content_length = response_content_length(*head);
        final_head = std::shared_ptr<const OriginResponseHead>(head.release());
        final_headers = true;
        revalidation_304 = request->stale && final_head->status == 304;
        if (revalidation_304) return;

        // Publish before cacheability / begin_store so the event loop can send the response head
        // while this worker reserves the tentative generation.
        fetch->publish_headers(final_head);
        if (request->head_only) return;

        decision = evaluate_cacheability(*final_head, request->cache_policy,
                                         requested_at, response_at);
        if (decision.cacheable && final_head->content_length) {
            auto opened = service->tier_manager().begin_store(
                request->digest, *final_head->content_length, WriteMode::evict);
            if (opened) store.emplace(std::move(*opened));
        }
    }

    bool accept_body(ByteView bytes) {
        if (!final_headers || revalidation_304) return !bytes.empty();
        if (!headers_acked) {
            if (!fetch->wait_for_header_ack()) return false;
            headers_acked = true;
        }
        if (bytes.empty()) return true;

        std::shared_ptr<const MirrorChunk> chunk;
        std::uint64_t sequence = 0;
        ByteView store_view = bytes;
        if (fetch->client_attached()) {
            auto copy = std::make_shared<MirrorChunk>(bytes.size());
            std::memcpy(copy->mutable_data(), bytes.data(), bytes.size());
            copy->set_size(bytes.size());
            store_view = ByteView(copy->data(), copy->size());
            sequence = fetch->publish_chunk(copy);
            chunk = std::move(copy);
        }

        if (store && !write_failed) {
            if (auto status = store->write(store_view); !status) {
                write_failed = true;
                store.reset();
            }
        }
        received += bytes.size();
        return !chunk || fetch->wait_for_chunk_ack(sequence);
    }

    bool accept_owned_body(std::shared_ptr<MirrorChunk> body) {
        if (!body) return false;
        const ByteView bytes(body->data(), body->size());
        if (!final_headers || revalidation_304) return !bytes.empty();
        if (!headers_acked) {
            if (!fetch->wait_for_header_ack()) return false;
            headers_acked = true;
        }
        if (bytes.empty()) return true;

        std::shared_ptr<const MirrorChunk> published;
        std::uint64_t sequence = 0;
        if (fetch->client_attached()) {
            published = body;
            sequence = fetch->publish_chunk(published);
        }
        if (store && !write_failed) {
            if (auto status = store->write(bytes); !status) {
                write_failed = true;
                store.reset();
            }
        }
        received += bytes.size();
        return !published || fetch->wait_for_chunk_ack(sequence);
    }

    void finish_transport() {
        if (!final_headers) {
            service->fail_origin_fetch(fetch, "origin returned no final HTTP response");
            return;
        }
        if (revalidation_304) {
            OriginResponseHead merged;
            merged.status = request->stale->status;
            merged.reason = request->stale->reason;
            merged.content_length = service->index().lookup(request->digest).transform(
                [](const storage::ObjectMeta& meta) { return meta.size; });
            for (const auto& header : request->stale->headers)
                merged.headers.push_back({header.name, header.value});
            for (const auto& update : final_head->headers) {
                auto it = std::remove_if(merged.headers.begin(), merged.headers.end(),
                                         [&](const OwnedHeader& old) {
                                             return iequals(old.name, update.name);
                                         });
                merged.headers.erase(it, merged.headers.end());
                merged.headers.push_back(update);
            }
            const std::uint64_t response_time = unix_seconds();
            auto refreshed = evaluate_cacheability(merged, request->cache_policy,
                                                   requested_at, response_time);
            if (refreshed.cacheable && refreshed.metadata && merged.content_length &&
                service->index().update_http_if_etag(
                    request->digest, request->stale_object_etag,
                    std::move(refreshed.metadata)))
                service->finish_origin_fetch(fetch, /*cache_ready=*/true);
            else
                service->fail_origin_fetch(fetch,
                                           "origin revalidation raced with cache replacement");
            return;
        }
        if (!request->head_only && final_head->content_length &&
            received != *final_head->content_length) {
            store.reset();
            service->fail_origin_fetch(fetch,
                                       "origin body length did not match Content-Length");
            return;
        }
        if (store && decision.metadata) {
            auto committed = store->commit(0, 0, 0, decision.metadata);
            if (!committed) store.reset();
        }
        service->finish_origin_fetch(fetch);
    }

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

        self.accept_final_head(std::move(self.parsing));
        return bytes;
    } catch (...) {
        return 0;
    }

    static std::size_t body(char* data, std::size_t size, std::size_t count,
                            void* opaque) noexcept try {
        const std::size_t bytes = size * count;
        auto& self = *static_cast<Transfer*>(opaque);
        if (self.fetch->cancelled() || self.service->should_stop()) return 0;
        const auto* body_bytes = reinterpret_cast<const std::byte*>(data);
        return self.accept_body(ByteView(body_bytes, bytes)) ? bytes : 0;
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

#if GOBLIN_HAVE_URING

enum class NativeOutcome { handled, retry };

struct NativeUrl {
    std::string host;
    std::string service;
    std::string authority;
    std::string target;
};

std::optional<NativeUrl> parse_native_url(std::string_view url) {
    constexpr std::string_view scheme = "http://";
    if (!url.starts_with(scheme)) return std::nullopt;
    url.remove_prefix(scheme.size());
    const std::size_t target_at = url.find_first_of("/?");
    const std::string_view authority = url.substr(0, target_at);
    if (authority.empty() || authority.find('@') != std::string_view::npos) return std::nullopt;

    NativeUrl parsed;
    parsed.authority = std::string(authority);
    parsed.target = target_at == std::string_view::npos ? "/" : std::string(url.substr(target_at));
    parsed.service = "80";
    if (authority.front() == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos) return std::nullopt;
        parsed.host = std::string(authority.substr(1, close - 1));
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':' || close + 2 == authority.size()) return std::nullopt;
            parsed.service = std::string(authority.substr(close + 2));
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            if (authority.find(':') != colon || colon == 0 || colon + 1 == authority.size())
                return std::nullopt;
            parsed.host = std::string(authority.substr(0, colon));
            parsed.service = std::string(authority.substr(colon + 1));
        } else {
            parsed.host = std::string(authority);
        }
    }
    if (parsed.host.empty() || parsed.target.empty() || parsed.target.front() != '/')
        return std::nullopt;
    return parsed;
}

bool header_has_token(const OriginResponseHead& head, std::string_view name,
                      std::string_view token) {
    for (const auto& header : head.headers) {
        if (!iequals(header.name, name)) continue;
        std::string_view value = header.value;
        while (!value.empty()) {
            const std::size_t comma = value.find(',');
            if (iequals(trim_ows(value.substr(0, comma)), token)) return true;
            if (comma == std::string_view::npos) break;
            value.remove_prefix(comma + 1);
        }
    }
    return false;
}

bool header_present(const OriginResponseHead& head, std::string_view name) {
    return std::ranges::any_of(head.headers, [&](const OwnedHeader& header) {
        return iequals(header.name, name);
    });
}

bool valid_content_length_headers(const OriginResponseHead& head) {
    std::optional<Size> found;
    for (const auto& header : head.headers) {
        if (!iequals(header.name, "content-length")) continue;
        const std::string_view input = trim_ows(header.value);
        Size value = 0;
        const auto [end, ec] = std::from_chars(input.data(), input.data() + input.size(), value);
        if (input.empty() || ec != std::errc{} || end != input.data() + input.size() ||
            (found && *found != value))
            return false;
        found = value;
    }
    return true;
}

enum class NativeFraming { none, content_length, chunked, invalid };

NativeFraming transfer_framing(const OriginResponseHead& head) {
    std::size_t tokens = 0;
    bool chunked = false;
    for (const auto& header : head.headers) {
        if (!iequals(header.name, "transfer-encoding")) continue;
        std::string_view value = header.value;
        for (;;) {
            const std::size_t comma = value.find(',');
            const std::string_view token = trim_ows(value.substr(0, comma));
            if (token.empty()) return NativeFraming::invalid;
            ++tokens;
            if (!iequals(token, "chunked")) return NativeFraming::invalid;
            chunked = true;
            if (comma == std::string_view::npos) break;
            value.remove_prefix(comma + 1);
        }
    }
    if (tokens > 1) return NativeFraming::invalid;
    if (chunked) return NativeFraming::chunked;
    if (header_present(head, "content-length")) return NativeFraming::content_length;
    return NativeFraming::none;
}

class NativeOriginClient {
public:
    ~NativeOriginClient() { close_socket(); }

    NativeOutcome perform(MirrorService& service, MirrorRequest& request,
                          const std::shared_ptr<MirrorFetch>& fetch,
                          std::string_view full_url) {
        const auto url = parse_native_url(full_url);
        if (!url) {
            service.fail_origin_fetch(fetch, "native origin URL is not sane HTTP/1.1");
            return NativeOutcome::handled;
        }
        auto request_bytes = build_request(*url, request);
        if (!request_bytes) {
            service.fail_origin_fetch(fetch,
                                      "native origin request contains an invalid header");
            return NativeOutcome::handled;
        }
        if (!ring_) {
            auto created = core::Reactor::create(32);
            if (!created) {
                service.fail_origin_fetch(
                    fetch, "create native origin io_uring: " + created.error().detail);
                return NativeOutcome::handled;
            }
            ring_.emplace(std::move(*created));
        }

        // A peer may close an idle HTTP/1.1 connection without us observing it. GET and HEAD are
        // idempotent, so retry once only if no response headers have been published.
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            std::string error;
            if (!ensure_connected(*url, error)) {
                service.fail_origin_fetch(fetch, std::move(error));
                return NativeOutcome::handled;
            }
            const std::uint64_t requested_at = unix_seconds();
            if (!send_all(*request_bytes, error)) {
                close_socket();
                if (attempt == 0) continue;
                service.fail_origin_fetch(fetch, std::move(error));
                return NativeOutcome::handled;
            }
            const NativeOutcome outcome = receive_response(
                service, request, fetch, requested_at, error);
            if (outcome == NativeOutcome::handled) return outcome;
            close_socket();
            if (attempt == 0) continue;
            if (!fetch->cancelled()) service.fail_origin_fetch(fetch, std::move(error));
            return NativeOutcome::handled;
        }
        return NativeOutcome::handled;
    }

private:
    static constexpr std::uint64_t kOperation = 1;
    static constexpr std::uint64_t kTimeout = 2;
    static constexpr std::size_t kMaximumHeader = 64 * KiB;
    static constexpr unsigned kConnectTimeoutMs = 10'000;
    static constexpr unsigned kIoTimeoutMs = 30'000;

    std::optional<core::Reactor> ring_;
    int fd_ = -1;
    std::string endpoint_;

    void close_socket() noexcept {
        if (fd_ >= 0) ::close(std::exchange(fd_, -1));
        endpoint_.clear();
    }

    Result<int> completion(bool queued, unsigned timeout_ms, std::string_view operation) {
        if (!queued) return err(Errc::would_block, "native origin io_uring submission queue full");
        core::TimeoutSpec timeout{};
        timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(timeout_ms / 1000);
        timeout.tv_nsec = static_cast<decltype(timeout.tv_nsec)>(timeout_ms % 1000) * 1'000'000;
        if (!ring_->submit_link_timeout(&timeout, kTimeout))
            return err(Errc::would_block, "native origin io_uring timeout queue full");

        bool have_operation = false;
        bool have_timeout = false;
        int operation_result = -EIO;
        int timeout_result = -ECANCELED;
        while (!have_operation || !have_timeout) {
            const int submitted = ring_->submit_and_wait(
                static_cast<unsigned>(!have_operation) + static_cast<unsigned>(!have_timeout));
            if (submitted < 0)
                return err(Errc::io_error,
                           "native origin io_uring wait: " +
                           std::string(std::strerror(-submitted)));
            std::array<core::Completion, 4> completions{};
            const unsigned count = ring_->reap(completions);
            for (unsigned i = 0; i < count; ++i) {
                if (completions[i].user_data == kOperation) {
                    operation_result = completions[i].res;
                    have_operation = true;
                } else if (completions[i].user_data == kTimeout) {
                    timeout_result = completions[i].res;
                    have_timeout = true;
                }
            }
        }
        if (timeout_result == -ETIME)
            return err(Errc::io_error,
                       std::string(operation) + " timed out after " +
                       std::to_string(timeout_ms) + " ms");
        return operation_result;
    }

    bool ensure_connected(const NativeUrl& url, std::string& error) {
        const std::string endpoint = url.host + ':' + url.service;
        if (fd_ >= 0 && endpoint_ == endpoint) return true;
        close_socket();

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        addrinfo* addresses = nullptr;
        const int resolved = ::getaddrinfo(url.host.c_str(), url.service.c_str(), &hints,
                                           &addresses);
        if (resolved != 0) {
            error = "resolve native origin: " + std::string(::gai_strerror(resolved));
            return false;
        }
        struct AddressCleanup {
            void operator()(addrinfo* value) const noexcept { if (value) ::freeaddrinfo(value); }
        };
        std::unique_ptr<addrinfo, AddressCleanup> cleanup(addresses);

        int last_error = ECONNREFUSED;
        for (const addrinfo* address = addresses; address; address = address->ai_next) {
            const int candidate = ::socket(address->ai_family,
                                           address->ai_socktype | SOCK_CLOEXEC,
                                           address->ai_protocol);
            if (candidate < 0) { last_error = errno; continue; }
            const int one = 1;
            const int bytes = static_cast<int>(kHighBdpSocketBuffer);
            (void)::setsockopt(candidate, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            (void)::setsockopt(candidate, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
            (void)::setsockopt(candidate, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof bytes);
            (void)::setsockopt(candidate, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof bytes);
            auto result = completion(ring_->submit_connect(
                candidate, address->ai_addr, address->ai_addrlen, kOperation, /*link=*/true),
                kConnectTimeoutMs, "connect native origin");
            if (result && *result == 0) {
                fd_ = candidate;
                endpoint_ = endpoint;
                return true;
            }
            if (!result) {
                error = result.error().detail;
                ::close(candidate);
                return false;
            }
            last_error = -*result;
            ::close(candidate);
        }
        error = "connect native origin: " + std::string(std::strerror(last_error));
        return false;
    }

    static std::optional<std::string> build_request(const NativeUrl& url,
                                                    const MirrorRequest& request) {
        std::string out;
        out.reserve(1024);
        out += request.head_only ? "HEAD " : "GET ";
        out += url.target;
        out += " HTTP/1.1\r\nHost: ";
        out += url.authority;
        out += "\r\nUser-Agent: goblin-store/0.0.2\r\nConnection: keep-alive\r\n";
        for (const auto& header : request.headers) {
            if (request.stale &&
                (iequals(header.name, "if-none-match") ||
                 iequals(header.name, "if-modified-since")))
                continue;
            if (iequals(header.name, "host") || iequals(header.name, "connection") ||
                iequals(header.name, "content-length") ||
                iequals(header.name, "transfer-encoding"))
                continue;
            if (header.name.find_first_of("\r\n:") != std::string::npos ||
                header.value.find_first_of("\r\n") != std::string::npos)
                return std::nullopt;
            out += header.name;
            out += ": ";
            out += header.value;
            out += "\r\n";
        }
        if (request.stale) {
            if (!request.stale->etag.empty()) {
                out += "if-none-match: ";
                out += request.stale->etag;
                out += "\r\n";
            } else if (!request.stale->last_modified.empty()) {
                out += "if-modified-since: ";
                out += request.stale->last_modified;
                out += "\r\n";
            }
        }
        out += "\r\n";
        if (out.size() > kMaximumHeader) return std::nullopt;
        return out;
    }

    bool send_all(std::string_view bytes, std::string& error) {
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            const auto* data = reinterpret_cast<const std::byte*>(bytes.data() + sent);
            auto result = completion(ring_->submit_send(
                fd_, ByteView(data, bytes.size() - sent), kOperation, 0, /*link=*/true),
                kIoTimeoutMs, "send native origin request");
            if (!result || *result <= 0) {
                error = result ? "send native origin request: " +
                                     std::string(std::strerror(-*result))
                               : result.error().detail;
                return false;
            }
            sent += static_cast<std::size_t>(*result);
        }
        return true;
    }

    Result<int> receive(MutBytes bytes) {
        return completion(ring_->submit_recv(fd_, bytes, kOperation, /*link=*/true),
                          kIoTimeoutMs, "receive native origin response");
    }

    NativeOutcome receive_response(MirrorService& service, MirrorRequest& request,
                                   const std::shared_ptr<MirrorFetch>& fetch,
                                   std::uint64_t requested_at, std::string& error) {
        // Only [0, used) is ever parsed. Avoid zeroing 64 KiB on every cache miss.
        std::array<char, kMaximumHeader> header_bytes;
        std::size_t used = 0;
        std::size_t offset = 0;
        std::unique_ptr<OriginResponseHead> head;
        int minor_version = 1;
        std::size_t body_at = 0;
        unsigned interim_responses = 0;

        auto fail_protocol = [&](std::string detail) {
            close_socket();
            if (!fetch->cancelled()) service.fail_origin_fetch(fetch, std::move(detail));
            return NativeOutcome::handled;
        };

        for (;;) {
            phr_header headers[256];
            std::size_t header_count = std::size(headers);
            int status = 0;
            const char* reason = nullptr;
            std::size_t reason_length = 0;
            const int parsed = phr_parse_response(
                header_bytes.data() + offset, used - offset, &minor_version, &status,
                &reason, &reason_length, headers, &header_count, 0);
            if (parsed == -1) {
                return fail_protocol("origin violated HTTP/1.1 response-header syntax");
            }
            if (parsed > 0) {
                if (minor_version != 1)
                    return fail_protocol("origin did not return HTTP/1.1");
                if (status < 100 || status > 599)
                    return fail_protocol("origin returned an invalid HTTP status code");
                if (status < 200) {
                    if (status == 101)
                        return fail_protocol("origin attempted an unsupported protocol upgrade");
                    if (++interim_responses > 8)
                        return fail_protocol("origin returned too many interim responses");
                    offset += static_cast<std::size_t>(parsed);
                    if (offset == used) { offset = used = 0; }
                    continue;
                }
                head = std::make_unique<OriginResponseHead>();
                head->status = static_cast<std::uint16_t>(status);
                head->reason.assign(reason, reason_length);
                head->headers.reserve(header_count);
                for (std::size_t i = 0; i < header_count; ++i) {
                    head->headers.push_back({
                        lower(std::string_view(headers[i].name, headers[i].name_len)),
                        std::string(trim_ows(std::string_view(headers[i].value,
                                                              headers[i].value_len))) });
                }
                head->content_length = response_content_length(*head);
                body_at = offset + static_cast<std::size_t>(parsed);
                break;
            }
            if (used == header_bytes.size()) {
                return fail_protocol("origin response headers exceed 64 KiB");
            }
            if (offset > 0) {
                std::memmove(header_bytes.data(), header_bytes.data() + offset, used - offset);
                used -= offset;
                offset = 0;
            }
            auto result = receive(MutBytes(
                reinterpret_cast<std::byte*>(header_bytes.data() + used),
                header_bytes.size() - used));
            if (!result || *result <= 0) {
                error = result ? "receive native origin response headers: " +
                                     std::string(std::strerror(-*result))
                               : result.error().detail;
                return NativeOutcome::retry;
            }
            used += static_cast<std::size_t>(*result);
        }

        if (!valid_content_length_headers(*head))
            return fail_protocol("origin returned an invalid or conflicting Content-Length");
        const NativeFraming framing = transfer_framing(*head);
        if (framing == NativeFraming::invalid)
            return fail_protocol("origin returned an unsupported or malformed Transfer-Encoding");
        if (framing == NativeFraming::chunked && header_present(*head, "content-length"))
            return fail_protocol("origin returned both Content-Length and Transfer-Encoding");
        if (header_has_token(*head, "connection", "close"))
            return fail_protocol("origin disabled HTTP/1.1 persistent connections");
        if (header_present(*head, "upgrade") || header_has_token(*head, "connection", "upgrade"))
            return fail_protocol("origin attempted an unsupported protocol upgrade");

        const bool no_body = request.head_only || head->status == 204 || head->status == 304;
        if (!no_body && framing == NativeFraming::none)
            return fail_protocol(
                "origin body has neither Content-Length nor chunked Transfer-Encoding");
        if (no_body && framing == NativeFraming::chunked)
            return fail_protocol("origin used chunked framing for a bodyless response");
        Size remaining = no_body || framing != NativeFraming::content_length
            ? 0 : *head->content_length;

        Transfer transfer{&service, &request, fetch};
        transfer.requested_at = requested_at;
        transfer.accept_final_head(std::move(head));

        auto fail_body = [&](std::string detail) {
            transfer.store.reset();
            close_socket();
            if (!fetch->cancelled()) service.fail_origin_fetch(fetch, std::move(detail));
            return NativeOutcome::handled;
        };

        const std::size_t initially_buffered = used - body_at;
        if (framing == NativeFraming::content_length) {
            if (initially_buffered > remaining)
                return fail_body("origin sent bytes beyond Content-Length");
            if (initially_buffered > 0) {
                auto chunk = std::make_shared<MirrorChunk>(initially_buffered);
                std::memcpy(chunk->mutable_data(), header_bytes.data() + body_at,
                            initially_buffered);
                chunk->set_size(initially_buffered);
                remaining -= initially_buffered;
                if (!transfer.accept_owned_body(std::move(chunk))) {
                    close_socket();
                    return NativeOutcome::handled;
                }
            }
            while (remaining > 0) {
                if (fetch->cancelled() || service.should_stop()) {
                    close_socket();
                    return NativeOutcome::handled;
                }
                const std::size_t capacity = static_cast<std::size_t>(
                    std::min<Size>(remaining, kMirrorIoChunk));
                auto chunk = std::make_shared<MirrorChunk>(capacity);
                auto result = receive(MutBytes(chunk->mutable_data(), capacity));
                if (!result || *result <= 0) {
                    error = result ? "receive native origin response body: " +
                                         std::string(std::strerror(-*result))
                                   : result.error().detail;
                    return fail_body(std::move(error));
                }
                chunk->set_size(static_cast<std::size_t>(*result));
                remaining -= static_cast<Size>(*result);
                if (!transfer.accept_owned_body(std::move(chunk))) {
                    close_socket();
                    return NativeOutcome::handled;
                }
            }
            transfer.finish_transport();
            return NativeOutcome::handled;
        }

        std::vector<std::byte> buffered;
        buffered.reserve(std::max<std::size_t>(4 * KiB, initially_buffered));
        if (initially_buffered > 0) {
            const auto* first = reinterpret_cast<const std::byte*>(header_bytes.data() + body_at);
            buffered.assign(first, first + initially_buffered);
        }
        std::size_t consumed = 0;

        auto compact_buffer = [&] {
            if (consumed == 0) return;
            if (consumed == buffered.size()) buffered.clear();
            else buffered.erase(buffered.begin(), buffered.begin() +
                                                   static_cast<std::ptrdiff_t>(consumed));
            consumed = 0;
        };
        auto receive_buffered = [&]() -> bool {
            compact_buffer();
            std::array<std::byte, 4 * KiB> incoming{};
            auto result = receive(MutBytes(incoming.data(), incoming.size()));
            if (!result || *result <= 0) {
                error = result ? "receive native origin chunk framing: " +
                                     std::string(std::strerror(-*result))
                               : result.error().detail;
                return false;
            }
            buffered.insert(buffered.end(), incoming.begin(),
                            incoming.begin() + static_cast<std::ptrdiff_t>(*result));
            return true;
        };
        auto deliver = [&](Size bytes) -> bool {
            while (bytes > 0) {
                if (consumed < buffered.size()) {
                    const std::size_t take = static_cast<std::size_t>(std::min<Size>(
                        bytes, buffered.size() - consumed));
                    auto chunk = std::make_shared<MirrorChunk>(take);
                    std::memcpy(chunk->mutable_data(), buffered.data() + consumed, take);
                    chunk->set_size(take);
                    consumed += take;
                    bytes -= take;
                    if (!transfer.accept_owned_body(std::move(chunk))) return false;
                    continue;
                }
                compact_buffer();
                const std::size_t capacity = static_cast<std::size_t>(
                    std::min<Size>(bytes, kMirrorIoChunk));
                auto chunk = std::make_shared<MirrorChunk>(capacity);
                auto result = receive(MutBytes(chunk->mutable_data(), capacity));
                if (!result || *result <= 0) {
                    error = result ? "receive native origin response body: " +
                                         std::string(std::strerror(-*result))
                                   : result.error().detail;
                    return false;
                }
                chunk->set_size(static_cast<std::size_t>(*result));
                bytes -= static_cast<Size>(*result);
                if (!transfer.accept_owned_body(std::move(chunk))) return false;
            }
            return true;
        };
        auto read_line = [&](std::string& line, std::size_t maximum) -> bool {
            for (;;) {
                for (std::size_t i = consumed; i < buffered.size(); ++i) {
                    if (buffered[i] != std::byte{'\n'}) continue;
                    if (i == consumed || buffered[i - 1] != std::byte{'\r'}) {
                        error = "origin chunk framing used a bare line feed";
                        return false;
                    }
                    const std::size_t length = i - consumed - 1;
                    if (length > maximum) {
                        error = "origin chunk framing line is too long";
                        return false;
                    }
                    line.assign(reinterpret_cast<const char*>(buffered.data() + consumed), length);
                    consumed = i + 1;
                    return true;
                }
                if (buffered.size() - consumed > maximum + 1) {
                    error = "origin chunk framing line is too long";
                    return false;
                }
                if (!receive_buffered()) return false;
            }
        };

        if (framing == NativeFraming::chunked) {
            std::size_t trailer_bytes = 0;
            for (;;) {
                std::string line;
                if (!read_line(line, 8 * KiB)) return fail_body(std::move(error));
                const std::size_t extension = line.find(';');
                const std::size_t digit_count = extension == std::string::npos
                    ? line.size() : extension;
                const std::string_view digits(line.data(), digit_count);
                if (digits.empty() || digits.size() > 16)
                    return fail_body("origin returned an invalid chunk size");
                Size chunk_size = 0;
                const auto [end, ec] = std::from_chars(
                    digits.data(), digits.data() + digits.size(), chunk_size, 16);
                if (ec != std::errc{} || end != digits.data() + digits.size())
                    return fail_body("origin returned an invalid chunk size");
                if (chunk_size == 0) {
                    for (;;) {
                        if (!read_line(line, 8 * KiB)) return fail_body(std::move(error));
                        trailer_bytes += line.size() + 2;
                        if (trailer_bytes > kMaximumHeader)
                            return fail_body("origin chunk trailers exceed 64 KiB");
                        if (line.empty()) break;
                        const std::size_t colon = line.find(':');
                        if (colon == std::string::npos || colon == 0 ||
                            line.front() == ' ' || line.front() == '\t')
                            return fail_body("origin returned malformed chunk trailers");
                    }
                    break;
                }
                if (!deliver(chunk_size)) {
                    if (fetch->cancelled() || service.should_stop()) {
                        close_socket();
                        return NativeOutcome::handled;
                    }
                    return fail_body(std::move(error));
                }
                while (buffered.size() - consumed < 2)
                    if (!receive_buffered()) return fail_body(std::move(error));
                if (buffered[consumed] != std::byte{'\r'} ||
                    buffered[consumed + 1] != std::byte{'\n'})
                    return fail_body("origin omitted CRLF after chunk data");
                consumed += 2;
            }
        } else if (!buffered.empty()) {
            return fail_body("origin sent a body on a bodyless response");
        }

        if (consumed != buffered.size())
            return fail_body("origin sent bytes beyond the framed HTTP response");
        if (fetch->cancelled() || service.should_stop()) {
            close_socket();
            return NativeOutcome::handled;
        }
        transfer.finish_transport();
        return NativeOutcome::handled;
    }
};

#endif // GOBLIN_HAVE_URING
#endif

} // namespace

Result<std::shared_ptr<MirrorFetch>> MirrorFetch::create() {
    int fds[2] = {-1, -1};
#if defined(__linux__)
    if (::pipe2(fds, O_NONBLOCK | O_CLOEXEC) < 0)
        return err(Errc::io_error, "create mirror notification pipe");
#else
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
#endif
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
    return client_attached_.load(std::memory_order_acquire);
}

bool MirrorFetch::cancelled() const {
    return cancelled_.load(std::memory_order_acquire);
}

void MirrorFetch::acknowledge_headers() {
    {
        // Store under mu_ so wait_for_header_ack cannot miss the notify between its predicate
        // check and the sleep (notify may run after the unlock that ends this critical section).
        std::lock_guard lock(mu_);
        headers_acknowledged_.store(true, std::memory_order_release);
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
        client_attached_.store(false, std::memory_order_release);
        headers_acknowledged_.store(true, std::memory_order_release);
        acknowledged_sequence_ = std::numeric_limits<std::uint64_t>::max();
        chunk_.reset();
    }
    cv_.notify_all();
}

void MirrorFetch::cancel() {
    {
        std::lock_guard lock(mu_);
        cancelled_.store(true, std::memory_order_release);
        client_attached_.store(false, std::memory_order_release);
        headers_acknowledged_.store(true, std::memory_order_release);
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
        wake = client_attached_.load(std::memory_order_relaxed);
    }
    if (wake) notify();
}

bool MirrorFetch::wait_for_header_ack() {
    // Fast path: headers already acked (common after the first body chunk).
    if (headers_acknowledged_.load(std::memory_order_acquire))
        return !cancelled_.load(std::memory_order_acquire);
    std::unique_lock lock(mu_);
    cv_.wait(lock, [&] {
        return headers_acknowledged_.load(std::memory_order_relaxed) ||
               !client_attached_.load(std::memory_order_relaxed) ||
               cancelled_.load(std::memory_order_relaxed);
    });
    return !cancelled_.load(std::memory_order_relaxed);
}

std::uint64_t MirrorFetch::publish_chunk(std::shared_ptr<const MirrorChunk> chunk) {
    bool wake = false;
    std::uint64_t sequence = 0;
    {
        std::lock_guard lock(mu_);
        sequence = ++chunk_sequence_;
        chunk_ = std::move(chunk);
        wake = client_attached_.load(std::memory_order_relaxed);
    }
    if (wake) notify();
    return sequence;
}

bool MirrorFetch::wait_for_chunk_ack(std::uint64_t sequence) {
    std::unique_lock lock(mu_);
    cv_.wait(lock, [&] {
        return acknowledged_sequence_ >= sequence ||
               !client_attached_.load(std::memory_order_relaxed) ||
               cancelled_.load(std::memory_order_relaxed);
    });
    return !cancelled_.load(std::memory_order_relaxed);
}

void MirrorFetch::finish(bool cache_ready) {
    bool wake = false;
    {
        std::lock_guard lock(mu_);
        cache_ready_ = cache_ready;
        done_ = true;
        wake = client_attached_.load(std::memory_order_relaxed);
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
        wake = client_attached_.load(std::memory_order_relaxed);
    }
    cv_.notify_all();
    if (wake) notify();
}

Result<std::unique_ptr<MirrorService>> MirrorService::create(
    std::string base_url, storage::TierManager& tm, storage::Index& index,
    const std::atomic<bool>* shutdown, unsigned workers, MirrorClient client) {
#if !GOBLIN_HAVE_CURL
    (void)base_url; (void)tm; (void)index; (void)shutdown; (void)workers; (void)client;
    return err(Errc::unsupported, "--mirror requested, but this build has no libcurl support");
#else
#if !GOBLIN_HAVE_URING
    if (client == MirrorClient::uring)
        return err(Errc::unsupported,
                   "--mirror-client uring requested, but this build has no io_uring support");
#endif
    if (::curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        return err(Errc::io_error, "initialize libcurl");
    auto service = std::unique_ptr<MirrorService>(
        new MirrorService(std::move(base_url), tm, index, shutdown, client));
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

void MirrorService::fail_origin_fetch(const std::shared_ptr<MirrorFetch>& fetch,
                                      std::string detail) {
    static std::mutex log_mu;
    {
        std::lock_guard lock(log_mu);
        std::cerr << "goblin-store: mirror origin failure: " << detail << '\n';
    }
    fetch->fail(std::move(detail));
}

void MirrorService::perform(Task& task) {
#if !GOBLIN_HAVE_CURL
    task.fetch->fail("libcurl unavailable");
#else
    auto url = mirror_origin_url(base_url_, task.request.target);
    if (!url) { task.fetch->fail(url.error().detail); return; }

#if GOBLIN_HAVE_URING
    if (client_ == MirrorClient::uring) {
        thread_local NativeOriginClient native;
        (void)native.perform(*this, task.request, task.fetch, *url);
        return;
    }
#endif

    // Reuse one easy handle per origin worker so DNS, TLS session tickets, and HTTP keep-alive
    // sockets survive across misses to the same origin (the common reverse-cache case).
    thread_local struct EasyHolder {
        CURL* handle = ::curl_easy_init();
        ~EasyHolder() { if (handle) ::curl_easy_cleanup(handle); }
    } easy_holder;
    CURL* const easy = easy_holder.handle;
    if (!easy) { task.fetch->fail("create origin HTTP handle"); return; }
    ::curl_easy_reset(easy);

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
    ::curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE, 1L);
    ::curl_easy_setopt(easy, CURLOPT_BUFFERSIZE, kMirrorCurlBufferBytes);
    ::curl_easy_setopt(easy, CURLOPT_SOCKOPTFUNCTION, &origin_sockopt);
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
    transfer.finish_transport();
#endif
}

} // namespace goblin::http
