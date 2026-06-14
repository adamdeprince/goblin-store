#include "goblin/protocol/memcache/event_loop.hpp"

#include "goblin/crypto/sha256.hpp"
#include "goblin/protocol/memcache/protocol.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace goblin::memcache {

// ---- the four protocol seams ----

void EventLoop::frame_get_hit(Conn* c, const std::string& key, const storage::ObjectMeta& meta) {
    c->get_pos = 0;
    c->get_size = meta.size; // whole object
    c->out += value_header(key, meta.flags, meta.size);
}

void EventLoop::frame_get_miss(Conn* c) {
    c->out += kEnd; // miss (or files vanished)
    c->state = St::idle;
}

void EventLoop::on_value_sent(Conn* c) {
    c->out += "\r\n";
    c->out += kEnd;
    c->state = St::get_trailer;
    start_send(c);
}

// ---- command parse / dispatch + SET ingest ----

void EventLoop::process(Conn* c) {
    constexpr std::size_t kMaxLine = 8 * 1024; // bound the command line (ADR-0016)
    while (true) {
        if (c->state == St::set_body) {
            // Stream the data block to disk as it arrives (sync pwrite per chunk; async between).
            while (c->set_remaining > 0 && !c->in.empty()) {
                const std::size_t take =
                    std::min<std::size_t>(c->in.size(), static_cast<std::size_t>(c->set_remaining));
                if (c->sh && !c->set_failed) {
                    const ByteView chunk(reinterpret_cast<const std::byte*>(c->in.data()), take);
                    if (auto st = c->sh->write(chunk); !st) c->set_failed = true;
                }
                c->in.erase(0, take);
                c->set_remaining -= take;
            }
            if (c->set_remaining > 0) break; // need more body
            if (c->in.size() < 2) break;     // need the trailing CRLF
            const bool crlf = c->in[0] == '\r' && c->in[1] == '\n';
            c->in.erase(0, 2);
            std::string_view reply;
            if (!crlf)
                reply = kBadDataChunk;
            else if (c->set_reject || c->set_failed || !c->sh)
                reply = kNotStored;
            else if (auto st = c->sh->commit(c->set_flags); !st)
                reply = kNotStored;
            else
                reply = kStored;
            c->sh.reset();
            if (!c->set_noreply) c->out += reply;
            c->state = St::idle;
            continue;
        }
        if (c->state != St::idle) return; // GET streaming — driven by sends/reads, not here

        const auto eol = c->in.find("\r\n");
        if (eol == std::string::npos) break;
        const auto cmd = parse_command(std::string_view(c->in.data(), eol));
        if (!cmd) {
            c->in.erase(0, eol + 2);
            c->out += kError;
            continue;
        }
        // Copy fields out before erasing — cmd's views point into `in`.
        const Verb verb = cmd->verb;
        const bool noreply = cmd->noreply;
        const std::uint32_t flags = cmd->flags;
        const std::uint64_t nbytes = cmd->bytes;
        const std::string key(cmd->key);
        c->in.erase(0, eol + 2);

        if (verb == Verb::version) {
            c->out += kVersion;
        } else if (verb == Verb::quit) {
            c->quit_after = true;
            break;
        } else if (verb == Verb::del) {
            const bool erased = tm_.remove(crypto::hash_key(key));
            if (!noreply) c->out += (erased ? kDeleted : kNotFound);
        } else if (verb == Verb::get || verb == Verb::gets) {
            if (!begin_get(c, key)) return;        // read I/O pool exhausted -> parked (ADR-0011)
            if (c->state == St::get_header) break; // hit -> stream the value before parsing further
            // miss: kEnd already queued and state is idle -> keep parsing the pipeline
        } else if (verb == Verb::set || verb == Verb::add || verb == Verb::replace) {
            const auto digest = crypto::hash_key(key);
            const bool reject = (verb == Verb::add && index_.contains(digest)) ||
                                (verb == Verb::replace && !index_.contains(digest));
            c->sh.reset();
            c->set_digest = digest;
            c->set_remaining = nbytes;
            c->set_flags = flags;
            c->set_noreply = noreply;
            c->set_reject = reject;
            c->set_failed = false;
            if (!reject) {
                auto h = tm_.begin_store(digest, nbytes);
                if (h) {
                    c->sh.emplace(std::move(*h));
                } else if (h.error().code == Errc::would_block) {
                    // Write-staging pool exhausted (ADR-0011/0016): park without posting a recv, so
                    // the kernel's socket buffer fills and TCP backpressures this client. The body is
                    // NOT drained into RAM. drain_set_waiters() retries begin_store once a buffer frees.
                    c->state = St::set_wait;
                    set_waiters_.push_back(c);
                    return;
                } else {
                    c->set_reject = true; // real open failure -> NOT_STORED
                }
            }
            c->state = St::set_body;
            continue; // consume the body in the set_body branch
        } else {
            c->out += kError;
        }
    }
    if (c->state == St::idle && c->in.size() > kMaxLine) {
        close_conn(c);
        return;
    }
    if (!c->out.empty())
        start_send(c);
    else if (c->quit_after)
        close_conn(c);
    else
        start_recv(c);
}

} // namespace goblin::memcache
