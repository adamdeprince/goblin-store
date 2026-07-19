#include "goblin/http/http_loop.hpp"

#include "goblin/crypto/sha256.hpp"
#include "goblin/http/request.hpp"

#include <charconv>
#include <cerrno>
#include <chrono>
#include <optional>
#include <poll.h>
#include <set>
#include <string>
#include <string_view>

namespace goblin::http {
namespace {

// Append an unsigned decimal integer without a temporary std::string.
void append_u64(std::string& out, std::uint64_t v) {
    char buf[24];
    const auto [p, ec] = std::to_chars(buf, buf + sizeof buf, v);
    out.append(buf, static_cast<std::size_t>(p - buf));
}

// Strong ETag into a caller buffer: `"<size-hex>-<etag-hex>"`. Returns the written length.
std::size_t format_etag(char* buf, std::size_t cap, const storage::ObjectMeta& m) {
    if (cap < 4) return 0;
    char* p = buf;
    char* end = buf + cap;
    *p++ = '"';
    {
        const auto [q, ec] = std::to_chars(p, end - 2, m.size, 16);
        if (ec != std::errc{}) return 0;
        p = q;
    }
    *p++ = '-';
    {
        const auto [q, ec] = std::to_chars(p, end - 1, m.etag, 16);
        if (ec != std::errc{}) return 0;
        p = q;
    }
    *p++ = '"';
    return static_cast<std::size_t>(p - buf);
}

// RFC 7232 If-None-Match (weak comparison): "*" matches any current representation; otherwise the
// quoted tag must appear among the listed tags. Searching for the fully-quoted tag (e.g. "7d0-2a")
// matches both `"7d0-2a"` and `W/"7d0-2a"` and can't prefix-collide with a longer tag.
bool inm_matches(std::string_view header, std::string_view quoted_etag) {
    if (header.find('*') != std::string_view::npos) return true;
    return header.find(quoted_etag) != std::string_view::npos;
}

// Append a header-only response (status line + Content-Length + Connection [+ Content-Type] [+ ETag]).
// The body, if any, is streamed separately by the shared loop. Content-Type is emitted only when
// there is a body to type; ETag whenever one is supplied.
void append_head(std::string& out, std::string_view status, Size content_length, bool keep_alive,
                 std::string_view content_type = "application/octet-stream",
                 std::string_view etag = {}) {
    out.reserve(out.size() + 160 + content_type.size() + etag.size());
    out += "HTTP/1.1 ";
    out += status;
    out += "\r\nContent-Length: ";
    append_u64(out, content_length);
    out += "\r\n";
    if (content_length > 0) {
        out += "Content-Type: ";
        out += content_type;
        out += "\r\nAccept-Ranges: bytes\r\n"; // advertise range support so clients resume/parallelize
    }
    if (!etag.empty()) {
        out += "ETag: ";
        out += etag;
        out += "\r\n";
    }
    out += keep_alive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
}

constexpr std::string_view k400 =
    "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
constexpr std::string_view k405 =
    "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\nAllow: GET, HEAD\r\nConnection: close\r\n\r\n";
constexpr std::string_view k421 =
    "HTTP/1.1 421 Misdirected Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
constexpr std::string_view k502 =
    "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
constexpr std::string_view k503 =
    "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
constexpr std::string_view k504 =
    "HTTP/1.1 504 Gateway Timeout\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

std::uint64_t unix_seconds() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

bool iequals_ascii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char ca = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
        const char cb = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
        if (ca != cb) return false;
    }
    return true;
}

bool response_hop_header(std::string_view name) {
    return iequals_ascii(name, "connection") || iequals_ascii(name, "proxy-connection") ||
           iequals_ascii(name, "keep-alive") || iequals_ascii(name, "proxy-authenticate") ||
           iequals_ascii(name, "proxy-authorization") || iequals_ascii(name, "te") ||
           iequals_ascii(name, "trailer") || iequals_ascii(name, "transfer-encoding") ||
           iequals_ascii(name, "upgrade");
}

bool response_has_body(unsigned status) {
    return status >= 200 && status != 204 && status != 304;
}

} // namespace

void HttpLoop::append_mirror_head(Conn* c, const OriginResponseHead& response) {
    c->out.reserve(c->out.size() + 256);
    c->out += "HTTP/1.1 ";
    append_u64(c->out, response.status);
    c->out.push_back(' ');
    c->out += response.reason.empty() ? "Unknown" : response.reason;
    c->out += "\r\n";

    std::set<std::string> nominated;
    for (const auto& header : response.headers) {
        if (!iequals_ascii(header.name, "connection")) continue;
        std::string_view value = header.value;
        while (!value.empty()) {
            const auto comma = value.find(',');
            std::string token(value.substr(0, comma));
            while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) token.erase(0, 1);
            while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) token.pop_back();
            for (char& ch : token)
                if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
            if (!token.empty()) nominated.insert(std::move(token));
            if (comma == std::string_view::npos) break;
            value.remove_prefix(comma + 1);
        }
    }
    for (const auto& header : response.headers) {
        if (response_hop_header(header.name) || nominated.contains(header.name) ||
            iequals_ascii(header.name, "content-length"))
            continue;
        c->out += header.name;
        c->out += ": ";
        c->out += header.value;
        c->out += "\r\n";
    }
    if (response.content_length) {
        c->out += "Content-Length: ";
        append_u64(c->out, *response.content_length);
        c->out += "\r\n";
    } else if (response_has_body(response.status)) {
        // libcurl has removed the origin transfer coding. Without a decoded length the only
        // buffer-free HTTP/1.1 framing is connection close.
        c->quit_after = true;
        mirrors_.at(c).close_delimited = true;
    }
    c->out += "Via: 1.1 goblin-store\r\n";
    c->out += c->quit_after ? "Connection: close\r\n\r\n"
                            : "Connection: keep-alive\r\n\r\n";
}

void HttpLoop::append_cached_head(Conn* c, const storage::ObjectMeta& object,
                                  const storage::HttpCacheMetadata& cached) {
    MirrorContext& context = mirrors_.at(c);
    const bool head_only = context.request.head_only;
    const bool conditional = !c->inm.empty() && !cached.etag.empty() &&
                             inm_matches(c->inm, cached.etag);
    const std::uint64_t resident = unix_seconds() > cached.response_time
        ? unix_seconds() - cached.response_time : 0;
    const std::uint64_t age = cached.corrected_initial_age + resident;

    Size body_offset = 0;
    Size body_length = object.size;
    unsigned status = cached.status;
    std::string_view reason = cached.reason;
    bool ranged = false;
    if (conditional) {
        status = 304;
        reason = "Not Modified";
        body_length = 0;
    } else if (c->req_range && cached.status == 200) {
        const auto resolved = resolve_range(*c->req_range, object.size);
        if (!resolved) {
            c->get_pos = c->get_size = 0;
            c->out += "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */";
            append_u64(c->out, object.size);
            c->out += "\r\nContent-Length: 0\r\n";
            c->out += c->quit_after ? "Connection: close\r\n\r\n"
                                    : "Connection: keep-alive\r\n\r\n";
            return;
        }
        body_offset = resolved->first;
        body_length = resolved->second;
        status = 206;
        reason = "Partial Content";
        ranged = true;
    }

    c->out += "HTTP/1.1 ";
    append_u64(c->out, status);
    c->out.push_back(' ');
    c->out += reason.empty() ? std::string_view("Unknown") : reason;
    c->out += "\r\n";
    for (const auto& header : cached.headers) {
        if (iequals_ascii(header.name, "content-length") || iequals_ascii(header.name, "age") ||
            iequals_ascii(header.name, "content-range") || response_hop_header(header.name))
            continue;
        c->out += header.name;
        c->out += ": ";
        c->out += header.value;
        c->out += "\r\n";
    }
    c->out += "Age: ";
    append_u64(c->out, age);
    c->out += "\r\n";
    if (ranged) {
        c->out += "Content-Range: bytes ";
        append_u64(c->out, body_offset);
        c->out.push_back('-');
        append_u64(c->out, body_offset + body_length - 1);
        c->out.push_back('/');
        append_u64(c->out, object.size);
        c->out += "\r\n";
    }
    c->out += "Content-Length: ";
    append_u64(c->out, conditional ? 0 : body_length);
    c->out += "\r\nVia: 1.1 goblin-store\r\n";
    c->out += c->quit_after ? "Connection: close\r\n\r\n"
                            : "Connection: keep-alive\r\n\r\n";
    if (head_only || conditional) {
        c->get_pos = c->get_size = 0;
    } else {
        c->get_pos = body_offset;
        c->get_size = body_offset + body_length;
    }
}

void HttpLoop::frame_get_hit(Conn* c, std::string_view key, const storage::ObjectMeta& meta) {
    if (mirror_) {
        if (!c->http_meta) { frame_get_miss(c); return; }
        append_cached_head(c, meta, *c->http_meta);
        return;
    }
    const bool keep_alive = !c->quit_after;
    char etag_buf[48];
    const std::size_t etag_len = format_etag(etag_buf, sizeof etag_buf, meta);
    const std::string_view etag(etag_buf, etag_len);
    // Conditional GET: If-None-Match hit -> 304 Not Modified, no body (get_pos == get_size streams
    // nothing, like the 416 path), still advertising the validator so the client refreshes its TTL.
    if (!c->inm.empty() && inm_matches(c->inm, etag)) {
        c->get_pos = c->get_size = 0;
        c->out.reserve(c->out.size() + 64 + etag_len);
        c->out += "HTTP/1.1 304 Not Modified\r\nETag: ";
        c->out += etag;
        c->out += "\r\n";
        c->out += keep_alive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
        return;
    }
    const std::string_view ct = content_type_for(key); // from the served object's extension
    if (c->req_range) { // a Range header was sent -> resolve against the actual size
        const auto r = resolve_range(*c->req_range, meta.size);
        if (!r) { // unsatisfiable -> 416, no body (get_pos == get_size; the shared loop streams nothing)
            c->get_pos = c->get_size = 0;
            c->out.reserve(c->out.size() + 96);
            c->out += "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */";
            append_u64(c->out, meta.size);
            c->out += "\r\nContent-Length: 0\r\n";
            c->out += keep_alive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
            return;
        }
        const auto [off, len] = *r;
        c->get_pos = off;
        c->get_size = off + len;
        c->out.reserve(c->out.size() + 160 + ct.size() + etag_len);
        c->out += "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes ";
        append_u64(c->out, off);
        c->out += '-';
        append_u64(c->out, off + len - 1);
        c->out += '/';
        append_u64(c->out, meta.size);
        c->out += "\r\nContent-Length: ";
        append_u64(c->out, len);
        c->out += "\r\nContent-Type: ";
        c->out += ct;
        c->out += "\r\nETag: ";
        c->out += etag;
        c->out += "\r\n";
        c->out += keep_alive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
        return;
    }
    c->get_pos = 0;
    c->get_size = meta.size; // whole object
    append_head(c->out, "200 OK", meta.size, keep_alive, ct, etag);
}

void HttpLoop::frame_get_miss(Conn* c) {
    if (mirror_) {
        start_mirror_fetch(c);
        return;
    }
    append_head(c->out, "404 Not Found", 0, !c->quit_after);
    c->state = St::idle;
}

void HttpLoop::append_value_trailer(Conn*) {} // HTTP has no after-body trailer

void HttpLoop::on_value_sent(Conn* c) {
    if (mirror_) mirrors_.erase(c);
    finish_get(c);
} // no trailer — keep-alive or close

bool HttpLoop::accept_get_snapshot(Conn* c, std::string_view,
                                   const storage::ObjectMeta& object) {
    if (!mirror_) return true;
    const auto found = mirrors_.find(c);
    if (found == mirrors_.end()) return false;
    MirrorContext& context = found->second;
    if (context.allow_stale_once) {
        context.allow_stale_once = false;
        return c->http_meta != nullptr;
    }
    if (!c->http_meta) return false;
    if (cache_is_fresh(*c->http_meta, context.request.cache_policy, unix_seconds())) return true;
    context.request.stale = c->http_meta;
    context.request.stale_object_etag = object.etag;
    return false;
}

void HttpLoop::start_mirror_fetch(Conn* c) {
    auto it = mirrors_.find(c);
    if (it == mirrors_.end()) {
        c->out += k502;
        c->quit_after = true;
        c->state = St::idle;
        return;
    }
    MirrorContext& context = it->second;
    if (context.request.cache_policy.only_if_cached) {
        c->out += k504;
        c->quit_after = true;
        c->state = St::idle;
        return;
    }
    auto fetch = mirror_->fetch(context.request);
    if (!fetch) {
        c->out += fetch.error().code == Errc::would_block ? k503 : k502;
        c->quit_after = true;
        c->state = St::idle;
        return;
    }
    context.fetch = std::move(*fetch);
    c->state = St::mirror_wait;
    arm_mirror_notification(c);
}

void HttpLoop::arm_mirror_notification(Conn* c) {
    auto it = mirrors_.find(c);
    if (it == mirrors_.end() || !it->second.fetch || c->closing) return;
    if (io_.submit_poll(it->second.fetch->notification_fd(), POLLIN, tag(c, kExternal))) {
        ++c->inflight;
    } else {
        abandon_mirror(c);
        close_conn(c);
    }
}

void HttpLoop::start_mirror_chunk_send(Conn* c) {
    auto it = mirrors_.find(c);
    if (it == mirrors_.end() || !it->second.sending || c->closing) return;
    MirrorContext& context = it->second;
    const auto* begin = context.sending->data() + context.sending_offset;
    const ByteView remaining(begin, context.sending->size() - context.sending_offset);
    if (io_.submit_send(c->fd, remaining, tag(c, kSend), 0)) {
        ++c->inflight;
    } else {
        abandon_mirror(c);
        close_conn(c);
    }
}

void HttpLoop::advance_mirror(Conn* c) {
    auto it = mirrors_.find(c);
    if (it == mirrors_.end() || !it->second.fetch || c->closing) return;
    MirrorContext& context = it->second;
    const MirrorFetch::View view = context.fetch->view();

    if (view.failed) {
        const bool can_stale = context.request.stale &&
            cache_allows_stale_on_error(*context.request.stale, unix_seconds());
        context.fetch->detach_client();
        context.fetch.reset();
        if (!context.header_sent && can_stale) {
            context.allow_stale_once = true;
            c->state = St::idle;
            if (!begin_get(c, c->get_key, c->get_digest, /*record_access=*/false)) return;
            if (c->state == St::get_header || !c->out.empty()) start_send(c);
            return;
        }
        if (!context.header_sent) {
            c->out += k502;
            c->quit_after = true;
            c->state = St::idle;
            start_send(c);
        } else {
            close_conn(c); // response already began; truncation is the only honest error signal
        }
        return;
    }

    if (view.done && view.cache_ready) {
        context.fetch->detach_client();
        context.fetch.reset();
        context.allow_stale_once = true; // a successful validation serves even with `no-cache`
        c->state = St::idle;
        if (!begin_get(c, c->get_key, c->get_digest, /*record_access=*/false)) return;
        if (c->state == St::get_header || !c->out.empty()) start_send(c);
        return;
    }

    if (!context.header_sent && view.headers_published && view.response) {
        append_mirror_head(c, *view.response);
        context.header_sent = true;
        c->state = St::mirror_header;
        start_send(c);
        return;
    }

    if (context.header_sent && view.chunk && !context.sending) {
        context.sending = view.chunk;
        context.sending_sequence = view.chunk_sequence;
        context.sending_offset = 0;
        c->state = St::mirror_body;
        start_mirror_chunk_send(c);
        return;
    }

    if (view.done && context.header_sent && !context.sending) {
        finish_mirror(c);
        return;
    }
    arm_mirror_notification(c);
}

bool HttpLoop::on_custom_send(Conn* c, int result) {
    if (c->state != St::mirror_header && c->state != St::mirror_body) return false;
    auto it = mirrors_.find(c);
    if (it == mirrors_.end()) { close_conn(c); return true; }
    MirrorContext& context = it->second;
    if (result < 0) {
        if (result == -EAGAIN || result == -EWOULDBLOCK) {
            if (c->state == St::mirror_header) start_send(c);
            else start_mirror_chunk_send(c);
        } else {
            abandon_mirror(c);
            close_conn(c);
        }
        return true;
    }
    if (result == 0) {
        abandon_mirror(c);
        close_conn(c);
        return true;
    }
    if (c->state == St::mirror_header) {
        c->out_sent += static_cast<std::size_t>(result);
        if (c->out_sent < c->out.size()) {
            start_send(c);
            return true;
        }
        c->out.clear();
        c->out_sent = 0;
        context.fetch->acknowledge_headers();
        c->state = St::mirror_wait;
        advance_mirror(c);
        return true;
    }

    context.sending_offset += static_cast<std::size_t>(result);
    if (context.sending_offset < context.sending->size()) {
        start_mirror_chunk_send(c);
        return true;
    }
    context.body_sent += context.sending->size();
    context.fetch->acknowledge_chunk(context.sending_sequence);
    context.sending.reset();
    context.sending_offset = 0;
    c->state = St::mirror_wait;
    advance_mirror(c);
    return true;
}

void HttpLoop::on_external(Conn* c, int result) {
    auto it = mirrors_.find(c);
    if (it == mirrors_.end() || !it->second.fetch) { close_conn(c); return; }
    if (result < 0) {
        abandon_mirror(c);
        close_conn(c);
        return;
    }
    it->second.fetch->drain_notification();
    advance_mirror(c);
}

void HttpLoop::finish_mirror(Conn* c) {
    auto it = mirrors_.find(c);
    if (it != mirrors_.end() && it->second.fetch) it->second.fetch->detach_client();
    mirrors_.erase(c);
    c->state = St::idle;
    if (c->quit_after)
        close_conn(c);
    else
        process(c);
}

void HttpLoop::abandon_mirror(Conn* c) {
    if (auto it = mirrors_.find(c); it != mirrors_.end()) {
        if (it->second.fetch) it->second.fetch->detach_client();
        mirrors_.erase(it);
    }
}

void HttpLoop::on_destroy(Conn* c) { abandon_mirror(c); }

void HttpLoop::process(Conn* c) {
    while (true) {
        if (c->state != St::idle) return; // streaming a response — driven by sends, not here
        const ParseResult pr = parse_request(in_view(c), c->phr_prev_len, mirror_ != nullptr);
        if (pr.status == ParseStatus::partial) {
            c->phr_prev_len = in_view(c).size(); // resume picohttpparser scan on the next recv
            break;
        }
        c->phr_prev_len = 0;
        if (pr.status == ParseStatus::bad) {
            c->out += k400;
            c->quit_after = true;
            break;
        }
        const Method method = pr.req.method;
        std::optional<std::string> key;
        std::optional<MirrorContext> mirror_context;
        if (mirror_ && (method == Method::get || method == Method::head)) {
            if (!pr.req.target.empty() && pr.req.target.front() == '/' &&
                !pr.req.target.starts_with("//") &&
                pr.req.target.find_first_of("\r\n#") == std::string_view::npos) {
                key = mirror_cache_key(pr.req.target, pr.req.headers);
                mirror_context.emplace();
                mirror_context->request.target = std::string(pr.req.target);
                mirror_context->request.headers = mirror_forward_headers(pr.req.headers);
                mirror_context->request.cache_policy = request_cache_policy(pr.req.headers);
                mirror_context->request.digest = crypto::hash_key(*key);
                mirror_context->request.head_only = method == Method::head;
            }
        } else {
            key = derive_key(pr.req.host, pr.req.target, keyopt_);
        }
        c->quit_after = !pr.req.keep_alive;
        // Strict per-tenant isolation (HTTPS only): the request Host must match the handshake SNI, so
        // a connection opened for one tenant can't fetch another's content (plaintext has no sni).
        // Evaluate it BEFORE consuming `in` — pr.req.host is a view into it.
        const bool host_ok = c->sni.empty() || normalize_host(pr.req.host) == c->sni;
        // Copy If-None-Match out before consuming `in` (it views into the buffer); used by the GET hit
        // (frame_get_hit) and the HEAD below for conditional 304s. Reassigned every request.
        c->inm.assign(pr.req.if_none_match.data(), pr.req.if_none_match.size());
        consume_in(c, pr.consumed); // request head consumed; GET/HEAD carry no body
        if (!host_ok) {
            c->out += k421;
            c->quit_after = true;
            break;
        }

        if (mirror_context) mirrors_.insert_or_assign(c, std::move(*mirror_context));

        if (mirror_ && (method == Method::get || method == Method::head)) {
            if (!key) { c->out += k400; c->quit_after = true; break; }
            c->req_range = pr.req.range;
            const auto digest = mirrors_.at(c).request.digest;
            if (!begin_get(c, *key, digest, /*record_access=*/true, storage::now_unix())) return;
            if (c->state != St::idle) break;
            // Inline fresh hit, only-if-cached miss, or immediate fetch-admission error.
            mirrors_.erase(c);
            if (c->quit_after) break;
        } else if (method == Method::get) {
            if (!key) { c->out += k400; c->quit_after = true; break; } // vhost mode without a Host
            c->req_range = pr.req.range;            // resolved in frame_get_hit against the object size
            if (!begin_get(c, *key)) return;        // read pool exhausted -> parked (ADR-0011)
            if (c->state != St::idle) break;         // hit -> stream (or inlined body already in out)
            // miss: 404 already queued, state idle -> keep parsing the pipeline
        } else if (method == Method::head) {
            if (!key) { c->out += k400; c->quit_after = true; break; }
            const auto digest = crypto::hash_key(*key);
            const auto meta = index_.lookup(digest); // headers only, no body/stream
            if (!meta || storage::is_expired(*meta, storage::now_unix())) { // lazy TTL skip
                append_head(c->out, "404 Not Found", 0, !c->quit_after);
            } else {
                char etag_buf[48];
                const std::size_t etag_len = format_etag(etag_buf, sizeof etag_buf, *meta);
                const std::string_view etag(etag_buf, etag_len);
                if (!c->inm.empty() && inm_matches(c->inm, etag)) { // conditional HEAD -> 304
                    c->out.reserve(c->out.size() + 64 + etag_len);
                    c->out += "HTTP/1.1 304 Not Modified\r\nETag: ";
                    c->out += etag;
                    c->out += "\r\n";
                    c->out += !c->quit_after ? "Connection: keep-alive\r\n\r\n"
                                             : "Connection: close\r\n\r\n";
                } else {
                    append_head(c->out, "200 OK", meta->size, !c->quit_after, content_type_for(*key),
                                etag);
                }
            }
        } else {
            c->out += k405; // PUT/POST/DELETE/... not served yet
            c->quit_after = true;
            break;
        }
    }
    if (!c->out.empty())
        start_send(c);
    else if (c->state != St::idle)
        return; // an asynchronous miss (for example --mirror) owns the next socket action
    else if (c->quit_after)
        close_conn(c);
    else
        start_recv(c);
}

} // namespace goblin::http
