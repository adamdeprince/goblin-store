#include "goblin/http/http_loop.hpp"

#include "goblin/crypto/sha256.hpp"
#include "goblin/http/request.hpp"

#include <charconv>
#include <optional>
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

} // namespace

void HttpLoop::frame_get_hit(Conn* c, std::string_view key, const storage::ObjectMeta& meta) {
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
    append_head(c->out, "404 Not Found", 0, !c->quit_after);
    c->state = St::idle;
}

void HttpLoop::append_value_trailer(Conn*) {} // HTTP has no after-body trailer

void HttpLoop::on_value_sent(Conn* c) { finish_get(c); } // no trailer — keep-alive or close

void HttpLoop::process(Conn* c) {
    while (true) {
        if (c->state != St::idle) return; // streaming a response — driven by sends, not here
        const ParseResult pr = parse_request(in_view(c), c->phr_prev_len);
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
        const std::optional<std::string> key = derive_key(pr.req.host, pr.req.target, keyopt_);
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

        if (method == Method::get) {
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
    else if (c->quit_after)
        close_conn(c);
    else
        start_recv(c);
}

} // namespace goblin::http
