#include "goblin/http/http_loop.hpp"

#include "goblin/crypto/sha256.hpp"
#include "goblin/http/request.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace goblin::http {
namespace {

// Append a header-only response (status line + Content-Length + Connection [+ Content-Type]). The
// body, if any, is streamed separately by the shared loop.
void append_head(std::string& out, std::string_view status, Size content_length, bool keep_alive) {
    out += "HTTP/1.1 ";
    out += status;
    out += "\r\nContent-Length: ";
    out += std::to_string(content_length);
    out += "\r\n";
    if (content_length > 0) out += "Content-Type: application/octet-stream\r\n";
    out += keep_alive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
}

constexpr std::string_view k400 =
    "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
constexpr std::string_view k405 =
    "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\nAllow: GET, HEAD\r\nConnection: close\r\n\r\n";

} // namespace

void HttpLoop::frame_get_hit(Conn* c, const std::string&, const storage::ObjectMeta& meta) {
    c->get_pos = 0;
    c->get_size = meta.size; // whole object
    append_head(c->out, "200 OK", meta.size, !c->quit_after);
}

void HttpLoop::frame_get_miss(Conn* c) {
    append_head(c->out, "404 Not Found", 0, !c->quit_after);
    c->state = St::idle;
}

void HttpLoop::on_value_sent(Conn* c) { finish_get(c); } // no trailer — keep-alive or close

void HttpLoop::process(Conn* c) {
    while (true) {
        if (c->state != St::idle) return; // streaming a response — driven by sends, not here
        const ParseResult pr = parse_request(c->in);
        if (pr.status == ParseStatus::partial) break; // need more bytes
        if (pr.status == ParseStatus::bad) {
            c->out += k400;
            c->quit_after = true;
            break;
        }
        const Method method = pr.req.method;
        const std::optional<std::string> key = derive_key(pr.req.host, pr.req.target, keyopt_);
        c->quit_after = !pr.req.keep_alive;
        c->in.erase(0, pr.consumed); // request head consumed; GET/HEAD carry no body

        if (method == Method::get) {
            if (!key) { c->out += k400; c->quit_after = true; break; } // vhost mode without a Host
            if (!begin_get(c, *key)) return;        // read pool exhausted -> parked (ADR-0011)
            if (c->state == St::get_header) break;  // hit -> stream the body before parsing further
            // miss: 404 already queued, state idle -> keep parsing the pipeline
        } else if (method == Method::head) {
            if (!key) { c->out += k400; c->quit_after = true; break; }
            const auto meta = index_.lookup(crypto::hash_key(*key)); // headers only, no body/stream
            if (meta) append_head(c->out, "200 OK", meta->size, !c->quit_after);
            else append_head(c->out, "404 Not Found", 0, !c->quit_after);
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
