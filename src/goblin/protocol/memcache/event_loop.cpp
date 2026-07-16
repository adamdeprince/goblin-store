#include "goblin/protocol/memcache/event_loop.hpp"

#include "goblin/crypto/sha256.hpp"
#include "goblin/protocol/memcache/protocol.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <format>
#include <string>
#include <string_view>
#include <unistd.h>

namespace goblin::memcache {

namespace {
constexpr auto rlx = std::memory_order_relaxed;

enum MetaRF : std::uint8_t { RF_F = 1, RF_S = 2, RF_T = 4, RF_C = 8, RF_K = 16 };

void append_u64(std::string& out, std::uint64_t v) {
    char buf[24];
    const auto [p, ec] = std::to_chars(buf, buf + sizeof buf, v);
    out.append(buf, static_cast<std::size_t>(p - buf));
}
void append_i64(std::string& out, std::int64_t v) {
    char buf[24];
    const auto [p, ec] = std::to_chars(buf, buf + sizeof buf, v);
    out.append(buf, static_cast<std::size_t>(p - buf));
}

// Append space-prefixed meta return flags for a hit: requested f/s/t/c/k + echoed opaque.
void append_meta_rflags(std::string& s, std::uint8_t rf, const storage::ObjectMeta& m,
                        std::string_view key, std::string_view opaque, std::uint32_t now) {
    if (rf & RF_F) { s += " f"; append_u64(s, m.flags); }
    if (rf & RF_S) { s += " s"; append_u64(s, m.size); }
    if (rf & RF_T) {
        const std::int64_t t =
            m.expiry == 0 ? -1 : std::max<std::int64_t>(0, std::int64_t(m.expiry) - now); // -1 = never
        s += " t";
        append_i64(s, t);
    }
    if (rf & RF_C) { s += " c"; append_u64(s, m.etag); }
    if (rf & RF_K) { s += " k"; s += key; }
    if (!opaque.empty()) { s += " O"; s += opaque; }
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
    o += "STAT version goblin-store 0.0.2\r\n"; // keep in sync with kVersion
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
    const auto promotion = tm_.numa_promotion_stats();
    line("numa_promotions", promotion.count);
    line("numa_promotion_bytes", promotion.bytes_moved);
    line("numa_promotion_total_ns", promotion.total_ns);
    line("numa_promotion_max_ns", promotion.max_ns);
    o += kEnd; // "END\r\n"
    return o;
}

// ---- the four protocol seams ----

void EventLoop::frame_get_hit(Conn* c, std::string_view key, const storage::ObjectMeta& meta) {
    c->get_pos = 0;
    c->get_size = meta.size; // whole object
    if (c->meta) { // meta `mg v` -> "VA <size> <return-flags>"
        c->out += "VA ";
        append_u64(c->out, meta.size);
        append_meta_rflags(c->out, c->meta_rflags, meta, key, c->meta_opaque, storage::now_unix());
        c->out += "\r\n";
    } else if (c->get_with_cas) {
        append_value_header_cas(c->out, key, meta.flags, meta.size, meta.etag);
    } else {
        append_value_header(c->out, key, meta.flags, meta.size);
    }
}

void EventLoop::frame_get_miss(Conn* c) {
    if (c->meta) { if (!c->meta_quiet) c->out += kMetaMiss; } // meta: EN (suppressed by q)
    else c->out += kEnd;                                      // classic: END
    c->state = St::idle;
}

void EventLoop::append_value_trailer(Conn* c) {
    c->out += "\r\n";              // CRLF after the value (classic + meta)
    if (!c->meta) c->out += kEnd;  // classic appends END; meta does not
}

void EventLoop::on_value_sent(Conn* c) {
    append_value_trailer(c);
    c->state = St::get_trailer;
    start_send(c);
}

// ---- command parse / dispatch + SET ingest ----

void EventLoop::process(Conn* c) {
    constexpr std::size_t kMaxLine = 8 * 1024; // bound the command line (ADR-0016)
    // Sample once per process() entry — expiry is second-granularity; avoids clock syscalls per cmd.
    const std::uint32_t now = storage::now_unix();
    while (true) {
        if (c->state == St::set_body) {
            // Stream the data block to disk as it arrives (sync pwrite per chunk; async between).
            while (c->set_remaining > 0) {
                const std::string_view inv = in_view(c);
                if (inv.empty()) break;
                const std::size_t take =
                    std::min<std::size_t>(inv.size(), static_cast<std::size_t>(c->set_remaining));
                if (c->sh && !c->set_failed) {
                    const ByteView chunk(reinterpret_cast<const std::byte*>(inv.data()), take);
                    if (auto st = c->sh->write(chunk); !st) {
                        c->set_failed = true;
                        // The rest of this request still has to be drained for framing, but its
                        // private generation, reserved blocks, head, and staging buffer do not.
                        c->sh.reset();
                    } else {
                        stats_.bytes_stored.fetch_add(take, rlx);
                    }
                }
                consume_in(c, take);
                c->set_remaining -= take;
            }
            if (c->set_remaining > 0) break; // need more body
            const std::string_view inv = in_view(c);
            if (inv.size() < 2) break; // need the trailing CRLF
            const bool crlf = inv[0] == '\r' && inv[1] == '\n';
            consume_in(c, 2);
            const std::uint32_t exp =
                c->meta ? c->meta_expiry : exptime_to_expiry(c->set_exptime, now);
            std::string_view reply;
            bool stored = false;
            std::uint64_t new_etag = 0;
            if (!crlf)
                reply = kBadDataChunk;
            else if (c->set_reject)
                reply = c->set_reply; // admission reply (classic NOT_STORED/.. or meta NS/EX/NF)
            else if (c->set_failed || !c->sh)
                reply = c->meta ? kMetaNotStored : kNotStored;
            else if (auto st = c->sh->commit(c->set_flags, exp, c->set_cas); !st)
                reply = (st.error().code == Errc::cas_mismatch) ? (c->meta ? kMetaExists : kExists)
                                                                : (c->meta ? kMetaNotStored : kNotStored);
            else {
                stored = true;
                new_etag = *st;
            }
            c->sh.reset();
            if (stored) stats_.sets.fetch_add(1, rlx);
            else if (reply != kBadDataChunk) stats_.set_rejected.fetch_add(1, rlx);
            if (stored) {
                if (c->meta) { // HD + return flags (c = new CAS from commit, k, O); q suppresses success
                    if (!c->meta_quiet) {
                        c->out += "HD";
                        if (c->meta_rflags & RF_C) {
                            c->out += " c";
                            append_u64(c->out, new_etag);
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

        const std::string_view inv = in_view(c);
        // memchr for CR then verify LF — cheaper than find("\r\n") on long buffers.
        std::size_t eol = std::string_view::npos;
        for (std::size_t i = 0; i + 1 < inv.size(); ++i) {
            if (inv[i] == '\r' && inv[i + 1] == '\n') {
                eol = i;
                break;
            }
        }
        if (eol == std::string_view::npos) break;
        const std::string_view line(inv.data(), eol);

        // ---- meta protocol (first token is mn/mg/ms/md) ----
        if (line.size() >= 2 && line[0] == 'm' &&
            (line[1] == 'n' || line[1] == 'g' || line[1] == 's' || line[1] == 'd') &&
            (line.size() == 2 || line[2] == ' ')) {
            const auto mc = parse_meta(line);
            if (!mc) {
                consume_in(c, eol + 2);
                c->out += "CLIENT_ERROR bad meta command\r\n";
                continue;
            }
            if (mc->verb == MetaVerb::mn) {
                consume_in(c, eol + 2);
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
                const bool need_exist = mc->has_cas || mode == 'E' || mode == 'R';
                consume_in(c, eol + 2);
                const auto digest = crypto::hash_key(mkey);
                bool present = false;
                std::optional<storage::ObjectMeta> exist;
                if (need_exist) {
                    exist = index_.lookup(digest);
                    present = exist && !storage::is_expired(*exist, now);
                }
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
                    auto h = tm_.begin_store(digest, datalen, write_mode_);
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
            consume_in(c, eol + 2);
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
                if (!begin_get(c, mkey, /*record_access=*/true, now)) return;
                if (c->state != St::idle) break; // hit streaming (header or inlined body send)
                continue;                         // miss -> EN (or quiet)
            }
            const auto m = index_.lookup(digest); // mg without v -> metadata only (HD / EN)
            const bool present = m && !storage::is_expired(*m, now);
            if (!present) { if (!quiet) c->out += kMetaMiss; }
            else {
                tm_.touch(digest); // metadata-only mg is still a successful key read
                c->out += "HD";
                append_meta_rflags(c->out, rflags, *m, mkey, mopaque, now);
                c->out += "\r\n";
            }
            continue;
        }

        const auto cmd = parse_command(line);
        if (!cmd) {
            consume_in(c, eol + 2);
            c->out += kError;
            continue;
        }
        // Snapshot scalars. Key is a view into `in` — use it before consume_in (which may clear `in`).
        const Verb verb = cmd->verb;
        const bool noreply = cmd->noreply;
        const std::uint32_t flags = cmd->flags;
        const std::uint32_t exptime = cmd->exptime;
        const std::uint64_t nbytes = cmd->bytes;
        const std::uint64_t cmd_cas = cmd->cas;
        const std::string_view key_view = cmd->key;

        if (verb == Verb::version) {
            consume_in(c, eol + 2);
            c->out += kVersion;
        } else if (verb == Verb::quit) {
            consume_in(c, eol + 2);
            c->quit_after = true;
            break;
        } else if (verb == Verb::del) {
            const auto digest = crypto::hash_key(key_view);
            consume_in(c, eol + 2);
            const bool erased = tm_.remove(digest);
            if (!noreply) c->out += (erased ? kDeleted : kNotFound);
        } else if (verb == Verb::get || verb == Verb::gets) {
            c->meta = false;                        // classic get -> VALUE/END framing
            c->get_with_cas = (verb == Verb::gets); // gets -> include the CAS in the VALUE header
            // Frame the VALUE header while key_view is still valid; begin_get copies on park.
            if (!begin_get(c, key_view, /*record_access=*/true, now)) {
                consume_in(c, eol + 2);
                return; // parked on read I/O pool (ADR-0011)
            }
            consume_in(c, eol + 2);
            if (c->state != St::idle) break; // hit: header stream or inlined full response in `out`
            // miss: kEnd already queued and state is idle -> keep parsing the pipeline
        } else if (verb == Verb::set || verb == Verb::add || verb == Verb::replace ||
                   verb == Verb::cas) {
            const auto digest = crypto::hash_key(key_view);
            consume_in(c, eol + 2);
            // Admission (an expired item counts as absent, lazy TTL, ADR-0007). Unconditional `set`
            // does not need a presence check — skip the index lookup on the common path.
            bool reject = false;
            std::string_view reply = kNotStored;
            std::uint64_t cas_check = 0;
            if (verb != Verb::set) {
                const auto exist = index_.lookup(digest);
                const bool present = exist && !storage::is_expired(*exist, now);
                if (verb == Verb::add && present) reject = true;           // add: only if absent
                else if (verb == Verb::replace && !present) reject = true; // replace: only if present
                else if (verb == Verb::cas) {
                    if (!present) { reject = true; reply = kNotFound; }
                    else if (exist->etag != cmd_cas) { reject = true; reply = kExists; }
                    else cas_check = cmd_cas;
                }
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
                auto h = tm_.begin_store(digest, nbytes, write_mode_);
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
            consume_in(c, eol + 2);
            c->out += format_stats(); // aggregated STAT lines (memcache channel only)
        } else {
            consume_in(c, eol + 2);
            c->out += kError;
        }
    }
    if (c->state == St::idle && in_view(c).size() > kMaxLine) {
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
