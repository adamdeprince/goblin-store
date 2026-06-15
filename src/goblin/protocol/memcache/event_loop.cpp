#include "goblin/protocol/memcache/event_loop.hpp"

#include "goblin/crypto/sha256.hpp"
#include "goblin/protocol/memcache/protocol.hpp"

#include <algorithm>
#include <atomic>
#include <format>
#include <string>
#include <string_view>
#include <unistd.h>

namespace goblin::memcache {

namespace {
constexpr auto rlx = std::memory_order_relaxed;
}

// Build the `stats` reply: STAT <name> <value> lines + END. Aggregates every worker's slot via the
// shared registry (reg_), so one memcache `stats` reports the whole process (memcache + HTTP + HTTPS
// GETs included). Falls back to this worker's own counters if no registry is wired (e.g. unit tests).
std::string EventLoop::format_stats() const {
    const core::StatsSnapshot s = reg_ ? reg_->aggregate() : stats_.snapshot();
    const std::uint64_t uptime = reg_ ? reg_->uptime_secs() : 0;
    std::string o;
    auto line = [&o](std::string_view name, std::uint64_t v) {
        o += std::format("STAT {} {}\r\n", name, v);
    };
    o += std::format("STAT pid {}\r\n", ::getpid());
    line("uptime", uptime);
    o += "STAT version goblincache 0.0.1\r\n"; // keep in sync with kVersion
    line("curr_connections", s.curr_conns);
    line("total_connections", s.conns);
    line("cmd_get", s.get_hits + s.get_misses);
    line("cmd_set", s.sets + s.set_rejected);
    line("get_hits", s.get_hits);
    line("get_misses", s.get_misses);
    line("sets_stored", s.sets);
    line("sets_rejected", s.set_rejected);
    line("bytes_served", s.bytes_served);
    line("bytes_stored", s.bytes_stored);
    line("get_backpressure", s.get_backpressure);
    line("set_backpressure", s.set_backpressure);
    line("slow_drops", s.slow_drops);
    o += kEnd; // "END\r\n"
    return o;
}

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
                    else stats_.bytes_stored.fetch_add(take, rlx);
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
            else if (auto st = c->sh->commit(c->set_flags,
                                             exptime_to_expiry(c->set_exptime, storage::now_unix()));
                     !st)
                reply = kNotStored;
            else
                reply = kStored;
            c->sh.reset();
            if (reply == kStored)
                stats_.sets.fetch_add(1, rlx);
            else if (reply == kNotStored)
                stats_.set_rejected.fetch_add(1, rlx);
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
        const std::uint32_t exptime = cmd->exptime;
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
            // add/replace admission treats an expired item as absent (lazy expiry, ADR-0007).
            const auto exist = index_.lookup(digest);
            const bool present = exist && !storage::is_expired(*exist, storage::now_unix());
            const bool reject =
                (verb == Verb::add && present) || (verb == Verb::replace && !present);
            c->sh.reset();
            c->set_digest = digest;
            c->set_remaining = nbytes;
            c->set_flags = flags;
            c->set_exptime = exptime;
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
                    stats_.set_backpressure.fetch_add(1, rlx);
                    c->state = St::set_wait;
                    set_waiters_.push_back(c);
                    return;
                } else {
                    c->set_reject = true; // real open failure -> NOT_STORED
                }
            }
            c->state = St::set_body;
            continue; // consume the body in the set_body branch
        } else if (verb == Verb::stats) {
            c->out += format_stats(); // aggregated STAT lines (memcache channel only)
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
