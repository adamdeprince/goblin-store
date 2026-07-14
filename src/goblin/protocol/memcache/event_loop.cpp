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

enum MetaRF : std::uint8_t { RF_F = 1, RF_S = 2, RF_T = 4, RF_C = 8, RF_K = 16 };

// The space-prefixed meta return-flag string for a hit: requested f/s/t/c/k values + echoed opaque.
std::string meta_rflags_str(std::uint8_t rf, const storage::ObjectMeta& m, std::string_view key,
                            std::string_view opaque, std::uint32_t now) {
    std::string s;
    if (rf & RF_F) { s += " f"; s += std::to_string(m.flags); }
    if (rf & RF_S) { s += " s"; s += std::to_string(m.size); }
    if (rf & RF_T) {
        const std::int64_t t =
            m.expiry == 0 ? -1 : std::max<std::int64_t>(0, std::int64_t(m.expiry) - now); // -1 = never
        s += " t"; s += std::to_string(t);
    }
    if (rf & RF_C) { s += " c"; s += std::to_string(m.etag); }
    if (rf & RF_K) { s += " k"; s += key; }
    if (!opaque.empty()) { s += " O"; s += opaque; }
    return s;
}
} // namespace

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
    o += "STAT version goblin-store 0.0.1\r\n"; // keep in sync with kVersion
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
    if (c->meta) { // meta `mg v` -> "VA <size> <return-flags>"
        c->out += "VA " + std::to_string(meta.size);
        c->out += meta_rflags_str(c->meta_rflags, meta, key, c->meta_opaque, storage::now_unix());
        c->out += "\r\n";
    } else if (c->get_with_cas) {
        c->out += value_header_cas(key, meta.flags, meta.size, meta.etag); // gets: VALUE k f n <cas>
    } else {
        c->out += value_header(key, meta.flags, meta.size);
    }
}

void EventLoop::frame_get_miss(Conn* c) {
    if (c->meta) { if (!c->meta_quiet) c->out += kMetaMiss; } // meta: EN (suppressed by q)
    else c->out += kEnd;                                      // classic: END
    c->state = St::idle;
}

void EventLoop::on_value_sent(Conn* c) {
    c->out += "\r\n";              // CRLF after the value (classic + meta)
    if (!c->meta) c->out += kEnd;  // classic appends END; meta does not
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
            const std::uint32_t exp =
                c->meta ? c->meta_expiry : exptime_to_expiry(c->set_exptime, storage::now_unix());
            std::string_view reply;
            bool stored = false;
            if (!crlf)
                reply = kBadDataChunk;
            else if (c->set_reject)
                reply = c->set_reply; // admission reply (classic NOT_STORED/.. or meta NS/EX/NF)
            else if (c->set_failed || !c->sh)
                reply = c->meta ? kMetaNotStored : kNotStored;
            else if (auto st = c->sh->commit(c->set_flags, exp, c->set_cas); !st)
                reply = (st.error().code == Errc::cas_mismatch) ? (c->meta ? kMetaExists : kExists)
                                                                : (c->meta ? kMetaNotStored : kNotStored);
            else
                stored = true;
            c->sh.reset();
            if (stored) stats_.sets.fetch_add(1, rlx);
            else if (reply != kBadDataChunk) stats_.set_rejected.fetch_add(1, rlx);
            if (stored) {
                if (c->meta) { // HD + return flags (c = new CAS via lookup, k, O); q suppresses success
                    if (!c->meta_quiet) {
                        c->out += "HD";
                        if (c->meta_rflags & RF_C) {
                            const auto nm = index_.lookup(c->set_digest);
                            c->out += " c"; c->out += std::to_string(nm ? nm->etag : 0);
                        }
                        if (c->meta_rflags & RF_K) { c->out += " k"; c->out += c->meta_key; }
                        if (!c->meta_opaque.empty()) { c->out += " O"; c->out += c->meta_opaque; }
                        c->out += "\r\n";
                    }
                } else if (!c->set_noreply) {
                    c->out += kStored;
                }
            } else if (c->meta || !c->set_noreply) { // meta errors always sent; classic honors noreply
                c->out += reply;
            }
            c->state = St::idle;
            continue;
        }
        if (c->state != St::idle) return; // GET streaming — driven by sends/reads, not here

        const auto eol = c->in.find("\r\n");
        if (eol == std::string::npos) break;
        const std::string_view line(c->in.data(), eol);

        // ---- meta protocol (first token is mn/mg/ms/md) ----
        if (line.size() >= 2 && line[0] == 'm' &&
            (line[1] == 'n' || line[1] == 'g' || line[1] == 's' || line[1] == 'd') &&
            (line.size() == 2 || line[2] == ' ')) {
            const auto mc = parse_meta(line);
            if (!mc) {
                c->in.erase(0, eol + 2);
                c->out += "CLIENT_ERROR bad meta command\r\n";
                continue;
            }
            const std::uint32_t now = storage::now_unix();
            if (mc->verb == MetaVerb::mn) {
                c->in.erase(0, eol + 2);
                c->out += kMetaNoop;
                continue;
            }
            if (mc->verb == MetaVerb::ms) { // meta set -> stream the body through set_body
                const std::string mkey(mc->key);
                const std::string mopaque(mc->opaque);
                const std::uint64_t datalen = mc->datalen;
                const std::uint32_t expiry =
                    mc->has_ttl
                        ? (mc->ttl == 0 ? 0u
                                        : (mc->ttl < 0 ? 1u
                                                       : exptime_to_expiry(
                                                             static_cast<std::uint32_t>(mc->ttl), now)))
                        : 0u;
                const std::uint64_t cas = mc->cas;
                const char mode = mc->mode;
                c->in.erase(0, eol + 2);
                const auto digest = crypto::hash_key(mkey);
                const auto exist = index_.lookup(digest);
                const bool present = exist && !storage::is_expired(*exist, now);
                bool reject = false;
                std::string_view rtok = kMetaNotStored;
                std::uint64_t cas_check = 0;
                if (mc->has_cas) {
                    if (!present) { reject = true; rtok = kMetaNotFound; }
                    else if (exist->etag != cas) { reject = true; rtok = kMetaExists; }
                    else cas_check = cas;
                }
                if (!reject && mode == 'E' && present) { reject = true; }            // add: exists
                else if (!reject && mode == 'R' && !present) { reject = true; }       // replace: missing
                else if (!reject && mode != 'S' && mode != 'E' && mode != 'R') reject = true; // bad mode
                c->meta = true;
                c->meta_rflags = (mc->rf_cas ? RF_C : 0) | (mc->rf_key ? RF_K : 0);
                c->meta_quiet = mc->quiet;
                c->meta_opaque = mopaque;
                c->meta_key = mkey;
                c->meta_expiry = expiry;
                c->sh.reset();
                c->set_digest = digest;
                c->set_remaining = datalen;
                c->set_flags = mc->set_flags;
                c->set_cas = cas_check;
                c->set_reply = rtok;
                c->set_noreply = false;
                c->set_reject = reject;
                c->set_failed = false;
                if (!reject) {
                    auto h = tm_.begin_store(digest, datalen);
                    if (h) c->sh.emplace(std::move(*h));
                    else if (h.error().code == Errc::would_block) {
                        stats_.set_backpressure.fetch_add(1, rlx);
                        c->state = St::set_wait;
                        set_waiters_.push_back(c);
                        return;
                    } else { c->set_reject = true; c->set_reply = kMetaNotStored; }
                }
                c->state = St::set_body;
                continue;
            }
            // mg / md (read + delete)
            const std::string mkey(mc->key);
            const std::string mopaque(mc->opaque);
            const std::uint8_t rflags = (mc->rf_flags ? RF_F : 0) | (mc->rf_size ? RF_S : 0) |
                                        (mc->rf_ttl ? RF_T : 0) | (mc->rf_cas ? RF_C : 0) |
                                        (mc->rf_key ? RF_K : 0);
            const bool quiet = mc->quiet;
            const bool has_cas = mc->has_cas;
            const std::uint64_t cas = mc->cas;
            const MetaVerb mv = mc->verb;
            c->in.erase(0, eol + 2);
            const auto digest = crypto::hash_key(mkey);

            if (mv == MetaVerb::md) { // meta delete
                const auto m = index_.lookup(digest);
                const bool present = m && !storage::is_expired(*m, now);
                if (!present) { if (!quiet) c->out += kMetaNotFound; }
                else if (has_cas && m->etag != cas) { if (!quiet) c->out += kMetaExists; }
                else {
                    tm_.remove(digest);
                    if (!quiet) {
                        c->out += "HD";
                        if (rflags & RF_K) { c->out += " k"; c->out += mkey; }
                        if (!mopaque.empty()) { c->out += " O"; c->out += mopaque; }
                        c->out += "\r\n";
                    }
                }
                continue;
            }
            // mg: optional get-and-touch (T), then read
            if (mc->has_ttl) {
                const std::uint32_t newexp = mc->ttl == 0 ? 0u
                                             : (mc->ttl < 0 ? 1u
                                                            : exptime_to_expiry(
                                                                  static_cast<std::uint32_t>(mc->ttl), now));
                tm_.touch_ttl(digest, newexp);
            }
            if (mc->rf_value) { // mg v -> stream the value with VA framing
                c->meta = true;
                c->get_with_cas = false;
                c->meta_rflags = rflags;
                c->meta_quiet = quiet;
                c->meta_opaque = mopaque;
                if (!begin_get(c, mkey)) return;       // parked on read-pool exhaustion
                if (c->state == St::get_header) break;  // hit -> stream before parsing on
                continue;                               // miss -> frame_get_miss queued EN (or q)
            }
            const auto m = index_.lookup(digest); // mg without v -> metadata only (HD / EN)
            const bool present = m && !storage::is_expired(*m, now);
            if (!present) { if (!quiet) c->out += kMetaMiss; }
            else {
                tm_.touch(digest); // metadata-only mg is still a successful key read
                c->out += "HD";
                c->out += meta_rflags_str(rflags, *m, mkey, mopaque, now);
                c->out += "\r\n";
            }
            continue;
        }

        const auto cmd = parse_command(line);
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
        const std::uint64_t cmd_cas = cmd->cas;
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
            c->meta = false;                        // classic get -> VALUE/END framing
            c->get_with_cas = (verb == Verb::gets); // gets -> include the CAS in the VALUE header
            if (!begin_get(c, key)) return;        // read I/O pool exhausted -> parked (ADR-0011)
            if (c->state == St::get_header) break; // hit -> stream the value before parsing further
            // miss: kEnd already queued and state is idle -> keep parsing the pipeline
        } else if (verb == Verb::set || verb == Verb::add || verb == Verb::replace ||
                   verb == Verb::cas) {
            const auto digest = crypto::hash_key(key);
            // Admission (an expired item counts as absent, lazy TTL, ADR-0007):
            const auto exist = index_.lookup(digest);
            const bool present = exist && !storage::is_expired(*exist, storage::now_unix());
            bool reject = false;
            std::string_view reply = kNotStored;
            std::uint64_t cas_check = 0;
            if (verb == Verb::add && present) reject = true;           // add: only if absent
            else if (verb == Verb::replace && !present) reject = true; // replace: only if present
            else if (verb == Verb::cas) {
                if (!present) { reject = true; reply = kNotFound; }                  // cas on missing
                else if (exist->etag != cmd_cas) { reject = true; reply = kExists; } // cas mismatch
                else cas_check = cmd_cas;                                            // cas matched
            }
            c->sh.reset();
            c->meta = false; // classic store -> STORED/NOT_STORED/EXISTS/NOT_FOUND framing
            c->set_digest = digest;
            c->set_remaining = nbytes;
            c->set_flags = flags;
            c->set_exptime = exptime;
            c->set_cas = cas_check;
            c->set_reply = reply;
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
