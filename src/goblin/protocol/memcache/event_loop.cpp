#include "goblin/protocol/memcache/event_loop.hpp"
#include "goblin/protocol/memcache/auth.hpp"
#include "goblin/protocol/memcache/stats_format.hpp"

#include "goblin/crypto/sha256.hpp"
#include "goblin/protocol/memcache/protocol.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <unistd.h>

namespace goblin::memcache {

namespace {
constexpr auto rlx = std::memory_order_relaxed;

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

// Append requested meta response tokens in request order. W/Z are emitted at the N/R position;
// X is item state, so it precedes the refresh claim just as memcached's examples do.
void append_meta_rflags(std::string& s, std::string_view order, const storage::ObjectMeta& m,
                        std::string_view key, std::string_view opaque, std::uint32_t now,
                        bool binary_key = false, bool recache = false, bool winner = false,
                        bool stale = false) {
    bool emitted_claim = false;
    bool emitted_stale = false;
    for (const char flag : order) {
        switch (flag) {
            case 'f': s += " f"; append_u64(s, m.flags); break;
            case 's': s += " s"; append_u64(s, m.size); break;
            case 't': {
                const std::int64_t ttl = m.expiry == 0
                    ? -1 : std::max<std::int64_t>(0, std::int64_t(m.expiry) - now);
                s += " t"; append_i64(s, ttl); break;
            }
            case 'c': s += " c"; append_u64(s, m.etag); break;
            case 'k':
                s += " k"; s += key;
                if (binary_key) s += " b";
                break;
            case 'h': s += m.fetched ? " h1" : " h0"; break;
            case 'l': {
                s += " l";
                append_u64(s, now >= m.last_access ? now - m.last_access : 0);
                break;
            }
            case 'O': if (!opaque.empty()) { s += " O"; s += opaque; } break;
            case 'N':
            case 'R':
                if (recache && !emitted_claim) {
                    if (stale && !emitted_stale) { s += " X"; emitted_stale = true; }
                    s += winner ? " W" : " Z";
                    emitted_claim = true;
                }
                break;
            default: break;
        }
    }
    if (stale && !emitted_stale) s += " X";
    if (recache && !emitted_claim) s += winner ? " W" : " Z";
    // O predates ordered-return tracking in some internal callers; preserve its echo.
    if (!opaque.empty() && order.find('O') == std::string_view::npos) { s += " O"; s += opaque; }
}
} // namespace

// Build the `stats` reply: STAT <name> <value> lines + END. Aggregates every worker's slot via the
// shared registry (reg_). Compatible command/hit counters include memcache TCP and native RDMA,
// while HTTP and HTTPS stay in their own operational domains. Falls back to this worker in tests.
std::string EventLoop::format_stats() const {
    const core::StatsSnapshot s =
        reg_ ? reg_->aggregate(core::StatsSelection::memcache) : stats_.snapshot();
    const std::uint64_t uptime = reg_ ? reg_->uptime_secs() : 0;
    return format_stats_response(tm_, s, uptime);
}

std::string EventLoop::format_settings() const {
    return format_settings_response(reg_ ? reg_->settings() : core::StatsSettings{});
}

// ---- the four protocol seams ----

void EventLoop::frame_get_hit(Conn* c, std::string_view key, const storage::ObjectMeta& meta) {
    c->get_pos = 0;
    c->get_size = meta.size; // whole object
    if (c->meta) {
        c->meta_suppress_value = c->meta_conditional_cas != 0 &&
                                 c->meta_conditional_cas == meta.etag;
        if (c->meta_suppress_value) c->out += "HD";
        else { c->out += "VA "; append_u64(c->out, meta.size); }
        append_meta_rflags(c->out, c->meta_return_order, meta, key, c->meta_opaque,
                           storage::now_unix(), c->meta_binary_key, c->meta_recache,
                           c->meta_winner, c->meta_stale);
        c->out += "\r\n";
    } else if (c->get_with_cas) {
        append_value_header_cas(c->out, key, meta.flags, meta.size, meta.etag);
    } else {
        append_value_header(c->out, key, meta.flags, meta.size);
    }
}

void EventLoop::frame_get_miss(Conn* c) {
    if (c->meta) { if (!c->meta_quiet) c->out += kMetaMiss; } // meta: EN (suppressed by q)
    else if (!c->get_batch_active) c->out += kEnd;             // batch misses are omitted
    c->state = St::idle;
}

void EventLoop::append_value_trailer(Conn* c) {
    c->out += "\r\n"; // CRLF after the value (classic + meta)
    // A classic multi-get has one END for the complete command, not one after each VALUE.
    if (!c->meta && !c->get_batch_active) c->out += kEnd;
}

void EventLoop::on_value_sent(Conn* c) {
    append_value_trailer(c);
    c->state = St::get_trailer;
    start_send(c);
}

bool EventLoop::continue_get_batch(Conn* c, std::uint32_t now) {
    while (c->get_batch_next < c->get_batch_keys.size()) {
        const std::string& key = c->get_batch_keys[c->get_batch_next++];
        if (c->get_batch_expiry)
            tm_.touch_ttl(crypto::hash_key(key), *c->get_batch_expiry, now);
        if (!begin_get(c, key, /*record_access=*/true, now)) return false;
        if (c->state != St::idle) return true; // this hit must finish before the next key
        // A miss emits nothing; a small inline hit is already complete in `out`. Both can advance.
    }
    c->out += kEnd;
    c->get_batch_keys.clear();
    c->get_batch_next = 0;
    c->get_batch_active = false;
    c->get_batch_expiry.reset();
    return true;
}

// ---- command parse / dispatch + SET ingest ----

void EventLoop::process(Conn* c) {
    constexpr std::size_t kMaxLine = 8 * 1024; // bound the command line (ADR-0016)
    constexpr std::size_t kMaxAuthToken = 16 * 1024; // memcached ASCII auth limit
    // Sample once per process() entry — expiry is second-granularity; avoids clock syscalls per cmd.
    const std::uint32_t now = storage::now_unix();
    while (true) {
        if (authenticator_ && !c->authenticated) {
            if (c->state == St::auth_body) {
                const std::string_view body = in_view(c);
                if (body.size() < c->auth_remaining + 2) break;
                const std::string_view token = body.substr(0, c->auth_remaining);
                const bool terminated = body[c->auth_remaining] == '\r' &&
                                        body[c->auth_remaining + 1] == '\n';
                const bool has_separator = token.find(' ') != std::string_view::npos;
                const bool accepted = terminated && has_separator &&
                                      authenticator_->authenticate(token);
                consume_in(c, c->auth_remaining + 2); // token view is dead after this point
                c->auth_remaining = 0;
                c->state = St::idle;
                stats_.auth_cmds.fetch_add(1, rlx);
                if (!terminated) {
                    stats_.auth_errors.fetch_add(1, rlx);
                    c->out += kClientErrorBadTermination;
                } else if (!has_separator) {
                    stats_.auth_errors.fetch_add(1, rlx);
                    c->out += kClientErrorAuthFormat;
                } else if (!accepted) {
                    stats_.auth_errors.fetch_add(1, rlx);
                    c->out += kClientErrorAuthFailure;
                } else {
                    c->authenticated = true;
                    c->out += kStored;
                }
                continue;
            }
            const std::string_view pending = in_view(c);
            const std::size_t auth_eol = pending.find("\r\n");
            if (auth_eol == std::string_view::npos) {
                if (pending.size() > kMaxLine) {
                    c->out += kClientErrorUnauthenticated;
                    consume_in(c, pending.size());
                }
                break;
            }
            const auto auth_command = parse_command(pending.substr(0, auth_eol));
            if (!auth_command || auth_command->verb != Verb::set) {
                consume_in(c, auth_eol + 2);
                stats_.auth_errors.fetch_add(1, rlx);
                c->out += kClientErrorUnauthenticated;
                continue;
            }
            consume_in(c, auth_eol + 2);
            if (auth_command->bytes > kMaxAuthToken) {
                stats_.auth_cmds.fetch_add(1, rlx);
                stats_.auth_errors.fetch_add(1, rlx);
                c->out += kClientErrorAuthTooLong;
                continue;
            }
            c->auth_remaining = static_cast<std::size_t>(auth_command->bytes);
            c->state = St::auth_body;
            continue;
        }
        if (c->state == St::idle && c->get_batch_active) {
            if (!continue_get_batch(c, now)) return; // parked on the current key
            if (c->state != St::idle) break;         // streaming hit; resume after its trailer
        }
        if (c->state == St::set_body) {
            const auto copy_mutation_source = [&] {
                if (!c->set_source || c->set_source_copied || !c->sh || c->set_failed) return;
                if (auto copied = tm_.copy_snapshot(*c->sh, *c->set_source); !copied) {
                    c->set_failed = true;
                    c->set_error = copied.error().code;
                    c->set_reply = storage_failure_reply(copied.error().code);
                    c->sh.reset();
                }
                c->set_source_copied = true;
                release_set_source(c);
            };
            if (c->set_mode == 'A') copy_mutation_source(); // old bytes, then request fragment
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
                        c->set_error = st.error().code;
                        c->set_reply = storage_failure_reply(st.error().code);
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
            if (c->set_mode == 'P') copy_mutation_source(); // request fragment, then old bytes
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
                reply = c->set_error == Errc::ok ? c->set_reply
                                                  : storage_failure_reply(c->set_error);
            else if (c->set_failed || !c->sh)
                reply = c->set_reply;
            else if (auto st = c->sh->commit(c->set_flags, exp, c->set_cas, {}, c->set_metadata); !st)
                reply = (c->set_mode == 'A' || c->set_mode == 'P') &&
                                st.error().code == Errc::cas_mismatch
                            ? (c->meta ? kMetaNotStored : kNotStored)
                            : commit_failure_reply(st.error().code, c->meta);
            else {
                stored = true;
                new_etag = *st;
            }
            c->sh.reset();
            release_set_source(c);
            if (stored) stats_.sets.fetch_add(1, rlx);
            else if (reply != kBadDataChunk) stats_.set_rejected.fetch_add(1, rlx);
            const bool condition = reply == kNotStored || reply == kExists || reply == kNotFound ||
                                   reply == kMetaNotStored || reply == kMetaExists ||
                                   reply == kMetaNotFound;
            const auto command_result = stored
                ? core::CommandResult::success
                : c->set_command == core::CommandKind::cas && reply == kNotFound
                    ? core::CommandResult::miss
                    : condition ? core::CommandResult::condition : core::CommandResult::error;
            stats_.note_command(c->set_command, command_result);
            if (stored) {
                if (c->meta) { // HD + return flags (c = new CAS from commit, k, O); q suppresses success
                    if (!c->meta_quiet) {
                        c->out += "HD";
                        storage::ObjectMeta response_meta;
                        response_meta.size = c->set_store_size;
                        response_meta.flags = c->set_flags;
                        response_meta.expiry = exp;
                        response_meta.etag = new_etag;
                        append_meta_rflags(c->out, c->meta_return_order, response_meta,
                                           c->meta_key, c->meta_opaque, now,
                                           c->meta_binary_key);
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

        // ---- meta protocol ----
        if (line.size() >= 2 && line[0] == 'm' &&
            (line[1] == 'n' || line[1] == 'g' || line[1] == 's' || line[1] == 'd' ||
             line[1] == 'a' || line[1] == 'e') &&
            (line.size() == 2 || line[2] == ' ')) {
            const auto mc = parse_meta(line);
            if (!mc) {
                consume_in(c, eol + 2);
                stats_.note_command(core::CommandKind::meta, core::CommandResult::error);
                c->out += "CLIENT_ERROR bad meta command\r\n";
                continue;
            }
            if (mc->verb == MetaVerb::mn) {
                consume_in(c, eol + 2);
                stats_.note_command(core::CommandKind::meta, core::CommandResult::success);
                c->out += kMetaNoop;
                continue;
            }
            const std::string mkey(mc->key);             // response key (possibly base64 text)
            const std::string lookup_key(mc->lookup_key()); // decoded key used only for hashing
            const auto digest = crypto::hash_key(lookup_key);

            if (mc->verb == MetaVerb::ms) { // meta set -> stream the body through set_body
                const std::string mopaque(mc->opaque);
                const std::uint64_t datalen = mc->datalen;
                std::uint32_t expiry = mc->has_ttl ? exptime_to_expiry(mc->ttl, now) : 0u;
                const std::uint64_t cas = mc->cas;
                const char mode = mc->mode;
                consume_in(c, eol + 2);
                release_set_source(c);
                std::optional<storage::TierManager::Snapshot> source;
                if (mode == 'A' || mode == 'P')
                    source = tm_.open_snapshot(digest, /*record_access=*/false, now);
                const auto exist = source ? std::optional(source->meta) : tm_.lookup_live(digest, now);
                const bool present = exist.has_value();
                bool reject = false;
                Errc admission_error = Errc::ok;
                std::string_view rtok = kMetaNotStored;
                std::uint64_t cas_check = 0;
                storage::StoreCondition condition = storage::StoreCondition::unconditional;
                if (mc->has_cas) {
                    if (!present) { reject = true; rtok = kMetaNotFound; }
                    else if (exist->etag != cas) { reject = true; rtok = kMetaExists; }
                    else cas_check = cas;
                }
                if (mode == 'E') condition = storage::StoreCondition::add;
                else if (mode == 'R' || mode == 'A' || mode == 'P')
                    condition = storage::StoreCondition::replace;
                if (!reject && mode == 'E' && present) { reject = true; }             // add: exists
                else if (!reject && mode == 'R' && !present) { reject = true; }        // replace: missing
                else if (!reject && (mode == 'A' || mode == 'P') && !present) reject = true;
                else if (!reject && mode != 'S' && mode != 'E' && mode != 'R' &&
                         mode != 'A' && mode != 'P') reject = true;
                Size store_size = datalen;
                std::uint32_t store_flags = mc->set_flags;
                if (!reject && (mode == 'A' || mode == 'P')) {
                    if (exist->size > std::numeric_limits<Size>::max() - datalen) {
                        reject = true;
                        admission_error = Errc::too_large;
                    }
                    else store_size = exist->size + datalen;
                    store_flags = exist->flags;
                    expiry = exist->expiry;
                    cas_check = exist->etag; // prevent two append/prepend operations losing bytes
                }
                if (store_size > tm_.max_object_size()) {
                    if (source && source->pin.valid) tm_.unpin_head(source->pin);
                    stats_.set_rejected.fetch_add(1, rlx);
                    stats_.note_command(core::CommandKind::meta, core::CommandResult::error);
                    c->out += kServerErrorTooLarge;
                    c->quit_after = true; // do not drain an attacker-controlled oversized body
                    break;
                }
                c->meta = true;
                c->meta_quiet = mc->quiet;
                c->meta_opaque = mopaque;
                c->meta_key = mkey;
                c->meta_return_order = mc->return_order;
                c->meta_binary_key = mc->key_binary;
                c->meta_expiry = expiry;
                c->sh.reset();
                c->set_digest = digest;
                c->set_remaining = datalen;
                c->set_store_size = store_size;
                c->set_mode = mode == 'A' || mode == 'P' ? mode : 'S';
                c->set_source = std::move(source);
                c->set_source_copied = false;
                c->set_flags = store_flags;
                c->set_cas = cas_check;
                c->set_condition = condition;
                c->set_command = core::CommandKind::meta;
                c->set_metadata = {.stale = mc->invalidate, .recache_claimed = false};
                c->set_reply = rtok;
                c->set_noreply = false;
                c->set_reject = reject;
                c->set_failed = false;
                c->set_error = admission_error;
                if (!reject) {
                    auto h = tm_.begin_store(digest, store_size, write_mode_, condition);
                    if (h) c->sh.emplace(std::move(*h));
                    else if (h.error().code == Errc::would_block) {
                        stats_.set_backpressure.fetch_add(1, rlx);
                        (void)park_set_waiter(c);
                        return;
                    } else {
                        c->set_reject = true;
                        c->set_error = h.error().code;
                        c->set_reply = storage_failure_reply(h.error().code);
                    }
                }
                c->state = St::set_body;
                continue;
            }

            if (mc->verb == MetaVerb::me) {
                consume_in(c, eol + 2);
                const auto m = tm_.lookup_live(digest, now);
                stats_.note_command(core::CommandKind::meta,
                                    m ? core::CommandResult::success : core::CommandResult::miss);
                if (!m) { c->out += kMetaMiss; continue; }
                c->out += "ME "; c->out += mkey;
                if (mc->key_binary) c->out += " b";
                c->out += " exp="; append_u64(c->out, m->expiry);
                c->out += " la="; append_u64(c->out, m->last_access);
                c->out += " cas="; append_u64(c->out, m->etag);
                c->out += m->fetched ? " fetch=yes" : " fetch=no";
                c->out += " cls=0 size="; append_u64(c->out, m->size);
                c->out += "\r\n";
                continue;
            }

            if (mc->verb == MetaVerb::ma) {
                consume_in(c, eol + 2);
                const bool decrement = mc->mode == 'D';
                if (mc->mode != 'I' && mc->mode != 'D') {
                    c->out += "CLIENT_ERROR invalid arithmetic mode\r\n";
                    continue;
                }
                const std::optional<std::uint64_t> initial =
                    mc->has_vivify ? std::optional(mc->has_initial ? mc->initial : 0) : std::nullopt;
                const std::optional<std::uint32_t> expiry = mc->has_ttl
                    ? std::optional(exptime_to_expiry(mc->ttl, now))
                    : mc->has_vivify
                        ? std::optional(exptime_to_expiry(mc->vivify_ttl, now))
                        : std::nullopt;
                auto result = tm_.arithmetic(digest, mc->delta, decrement, initial, expiry,
                                             mc->has_cas ? mc->cas : 0, write_mode_);
                stats_.note_command(core::CommandKind::meta,
                    result ? core::CommandResult::success
                    : result.error().code == Errc::not_found ? core::CommandResult::miss
                    : result.error().code == Errc::cas_mismatch ? core::CommandResult::condition
                                                               : core::CommandResult::error);
                if (!result) {
                    if (result.error().code == Errc::not_found) c->out += kMetaNotFound;
                    else if (result.error().code == Errc::cas_mismatch) c->out += kMetaExists;
                    else if (result.error().code == Errc::invalid_argument)
                        c->out += "CLIENT_ERROR invalid numeric value\r\n";
                    else c->out += storage_failure_reply(result.error().code);
                } else if (!mc->quiet) {
                    c->out += "VA "; append_u64(c->out, result->value);
                    append_meta_rflags(c->out, mc->return_order, result->meta, mkey, mc->opaque,
                                       now, mc->key_binary);
                    c->out += "\r\n";
                }
                continue;
            }

            // mg / md (read + delete/invalidate)
            const std::string mopaque(mc->opaque);
            const bool quiet = mc->quiet;
            const bool has_cas = mc->has_cas;
            const std::uint64_t cas = mc->cas;
            const MetaVerb mv = mc->verb;
            consume_in(c, eol + 2);

            if (mv == MetaVerb::md) { // meta delete
                const auto m = tm_.lookup_live(digest, now);
                const bool present = m.has_value();
                if (!present) { if (!quiet) c->out += kMetaNotFound; }
                else if (has_cas && m->etag != cas) { if (!quiet) c->out += kMetaExists; }
                else {
                    bool stored = false;
                    Errc mutation_error = Errc::ok;
                    if (mc->invalidate) {
                        const std::optional<std::uint32_t> expiry = mc->has_ttl
                            ? std::optional(exptime_to_expiry(mc->ttl, now)) : std::nullopt;
                        if (mc->remove_value) {
                            auto h = tm_.begin_store(digest, 0, write_mode_,
                                                     storage::StoreCondition::replace);
                            if (h) {
                                storage::TierManager::CommitMetadata state{true, false};
                                auto committed = h->commit(m->flags, expiry.value_or(m->expiry),
                                                           m->etag, {}, state);
                                stored = committed.has_value();
                                if (!committed) mutation_error = committed.error().code;
                            } else mutation_error = h.error().code;
                        } else {
                            const auto outcome =
                                tm_.mark_stale(digest, has_cas ? cas : 0, expiry, now);
                            stored = outcome == storage::Index::MetaMutation::stored;
                            if (outcome == storage::Index::MetaMutation::cas_mismatch)
                                mutation_error = Errc::cas_mismatch;
                        }
                    } else {
                        stored = tm_.remove(digest);
                    }
                    stats_.note_command(core::CommandKind::meta,
                        stored ? core::CommandResult::success
                        : mutation_error == Errc::cas_mismatch ? core::CommandResult::condition
                        : mutation_error == Errc::ok ? core::CommandResult::miss
                                                     : core::CommandResult::error);
                    if (!stored) {
                        if (!quiet) {
                            if (mutation_error == Errc::cas_mismatch) c->out += kMetaExists;
                            else if (mutation_error != Errc::ok)
                                c->out += storage_failure_reply(mutation_error);
                            else c->out += kMetaNotFound;
                        }
                        continue;
                    }
                    if (!quiet) {
                        c->out += "HD";
                        append_meta_rflags(c->out, mc->return_order, *m, mkey, mopaque, now,
                                           mc->key_binary);
                        c->out += "\r\n";
                    }
                }
                continue;
            }

            // mg: optional vivify, get-and-touch, stale/early-recache election, then read.
            auto m = tm_.lookup_live(digest, now);
            bool recache = false;
            bool winner = false;
            Errc vivify_error = Errc::ok;
            if (!m && mc->has_vivify) {
                auto h = tm_.begin_store(digest, 0, write_mode_, storage::StoreCondition::add);
                if (h) {
                    storage::TierManager::CommitMetadata state{false, true};
                    auto committed = h->commit(0, exptime_to_expiry(mc->vivify_ttl, now), 0, {}, state);
                    if (committed) { recache = true; winner = true; }
                    else if (committed.error().code != Errc::condition_not_met)
                        vivify_error = committed.error().code;
                } else vivify_error = h.error().code;
                m = tm_.lookup_live(digest, now);
                if (m && !winner && m->size == 0 && m->recache_claimed) recache = true;
            }
            if (!m && vivify_error != Errc::ok) {
                c->out += storage_failure_reply(vivify_error);
                continue;
            }
            if (m && mc->has_ttl) {
                const std::uint32_t newexp = exptime_to_expiry(mc->ttl, now);
                if (tm_.touch_ttl(digest, newexp, now)) m = tm_.lookup_live(digest, now);
            }
            if (m) {
                const bool below_recache = mc->has_recache && m->expiry != 0 &&
                    m->expiry > now && m->expiry - now < mc->recache_ttl;
                if (m->stale || below_recache) {
                    recache = true;
                    const auto claim = tm_.claim_recache(digest, now);
                    winner = claim == storage::Index::RecacheClaim::winner;
                }
            }
            stats_.note_command(core::CommandKind::meta,
                                m ? core::CommandResult::success : core::CommandResult::miss);
            if (mc->rf_value) { // mg v -> stream the value with VA framing
                c->meta = true;
                c->get_with_cas = false;
                c->meta_quiet = quiet;
                c->meta_opaque = mopaque;
                c->meta_key = mkey;
                c->meta_return_order = mc->return_order;
                c->meta_binary_key = mc->key_binary;
                c->meta_recache = recache;
                c->meta_winner = winner;
                c->meta_stale = m && m->stale;
                c->meta_conditional_cas = has_cas ? cas : 0;
                c->meta_suppress_value = false;
                if (!begin_get(c, mkey, digest, /*record_access=*/!mc->no_update, now)) return;
                if (c->state != St::idle) break; // hit streaming (header or inlined body send)
                continue;                         // miss -> EN (or quiet)
            }
            const bool present = m.has_value();
            if (!present) { if (!quiet) c->out += kMetaMiss; }
            else {
                if (!mc->no_update) tm_.touch(digest);
                c->out += "HD";
                append_meta_rflags(c->out, mc->return_order, *m, mkey, mopaque, now,
                                   mc->key_binary, recache, winner, m->stale);
                c->out += "\r\n";
            }
            continue;
        }

        const auto cmd = parse_command(line);
        if (!cmd) {
            consume_in(c, eol + 2);
            c->out += command_parse_error_reply(cmd.error());
            continue;
        }
        // Snapshot scalars. Key is a view into `in` — use it before consume_in (which may clear `in`).
        const Verb verb = cmd->verb;
        const bool noreply = cmd->noreply;
        const std::uint32_t flags = cmd->flags;
        const std::int64_t exptime = cmd->exptime;
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
            stats_.note_command(core::CommandKind::delete_, erased ? core::CommandResult::success
                                                                  : core::CommandResult::miss);
            if (!noreply) c->out += (erased ? kDeleted : kNotFound);
        } else if (verb == Verb::touch) {
            const auto digest = crypto::hash_key(key_view);
            consume_in(c, eol + 2);
            const bool touched = tm_.touch_ttl(digest, exptime_to_expiry(exptime, now), now);
            stats_.note_command(core::CommandKind::touch, touched ? core::CommandResult::success
                                                                 : core::CommandResult::miss);
            if (!noreply) c->out += touched ? kTouched : kNotFound;
        } else if (verb == Verb::incr || verb == Verb::decr) {
            const auto digest = crypto::hash_key(key_view);
            const std::uint64_t delta = cmd->delta;
            consume_in(c, eol + 2);
            auto result = tm_.arithmetic(digest, delta, verb == Verb::decr, std::nullopt,
                                         std::nullopt, 0, write_mode_);
            const auto arithmetic_kind = verb == Verb::decr ? core::CommandKind::decr
                                                            : core::CommandKind::incr;
            stats_.note_command(arithmetic_kind,
                                result ? core::CommandResult::success
                                       : result.error().code == Errc::not_found
                                           ? core::CommandResult::miss
                                           : core::CommandResult::error);
            if (!noreply) {
                if (result) { append_u64(c->out, result->value); c->out += "\r\n"; }
                else if (result.error().code == Errc::not_found) c->out += kNotFound;
                else if (result.error().code == Errc::invalid_argument)
                    c->out += kClientErrorNonNumeric;
                else c->out += storage_failure_reply(result.error().code);
            }
        } else if (verb == Verb::flush_all) {
            const std::uint32_t delay = cmd->delay;
            consume_in(c, eol + 2);
            tm_.flush_all(delay);
            stats_.note_command(core::CommandKind::flush, core::CommandResult::success);
            if (!noreply) c->out += kOk;
        } else if (verb == Verb::get || verb == Verb::gets ||
                   verb == Verb::gat || verb == Verb::gats) {
            c->meta = false;                        // classic get -> VALUE/END framing
            c->meta_suppress_value = false;
            c->get_with_cas = (verb == Verb::gets || verb == Verb::gats);
            const std::optional<std::uint32_t> touch_expiry =
                (verb == Verb::gat || verb == Verb::gats)
                    ? std::optional(exptime_to_expiry(exptime, now)) : std::nullopt;
            if (!cmd->extra_keys.empty()) {
                // Command views point into `in`; retain the whole batch before consume_in may clear it.
                c->get_batch_keys.clear();
                c->get_batch_keys.reserve(1 + cmd->extra_keys.size());
                c->get_batch_keys.emplace_back(key_view);
                for (const std::string_view key : cmd->extra_keys)
                    c->get_batch_keys.emplace_back(key);
                c->get_batch_next = 0;
                c->get_batch_active = true;
                c->get_batch_expiry = touch_expiry;
                consume_in(c, eol + 2);
                continue; // the loop's batch branch starts the first key
            }
            if (touch_expiry)
                tm_.touch_ttl(crypto::hash_key(key_view), *touch_expiry, now);
            // Frame the VALUE header while key_view is still valid; begin_get copies on park.
            if (!begin_get(c, key_view, /*record_access=*/true, now)) {
                consume_in(c, eol + 2);
                return; // parked on read I/O pool (ADR-0011)
            }
            consume_in(c, eol + 2);
            if (c->state != St::idle) break; // hit: header stream or inlined full response in `out`
            // miss: kEnd already queued and state is idle -> keep parsing the pipeline
        } else if (verb == Verb::set || verb == Verb::add || verb == Verb::replace ||
                   verb == Verb::append || verb == Verb::prepend || verb == Verb::cas) {
            const auto digest = crypto::hash_key(key_view);
            consume_in(c, eol + 2);
            // Admission (an expired item counts as absent, lazy TTL, ADR-0007). Unconditional `set`
            // does not need a presence check — skip the index lookup on the common path.
            bool reject = false;
            Errc admission_error = Errc::ok;
            std::string_view reply = kNotStored;
            std::uint64_t cas_check = 0;
            storage::StoreCondition condition = storage::StoreCondition::unconditional;
            release_set_source(c);
            std::optional<storage::TierManager::Snapshot> source;
            const bool mutate = verb == Verb::append || verb == Verb::prepend;
            const auto command_kind = [&] {
                switch (verb) {
                    case Verb::add: return core::CommandKind::add;
                    case Verb::replace: return core::CommandKind::replace;
                    case Verb::append: return core::CommandKind::append;
                    case Verb::prepend: return core::CommandKind::prepend;
                    case Verb::cas: return core::CommandKind::cas;
                    default: return core::CommandKind::set;
                }
            }();
            if (mutate) source = tm_.open_snapshot(digest, /*record_access=*/false, now);
            if (verb == Verb::add) condition = storage::StoreCondition::add;
            else if (verb == Verb::replace || mutate)
                condition = storage::StoreCondition::replace;
            if (verb != Verb::set) {
                const auto exist = source ? std::optional(source->meta) : tm_.lookup_live(digest, now);
                const bool present = exist.has_value();
                if (verb == Verb::add && present) reject = true;           // add: only if absent
                else if (verb == Verb::replace && !present) reject = true; // replace: only if present
                else if (mutate && !present) reject = true;
                else if (verb == Verb::cas) {
                    if (!present) { reject = true; reply = kNotFound; }
                    else if (exist->etag != cmd_cas) { reject = true; reply = kExists; }
                    else cas_check = cmd_cas;
                }
            }
            Size store_size = nbytes;
            std::uint32_t store_flags = flags;
            std::int64_t store_exptime = exptime;
            if (!reject && mutate) {
                if (source->meta.size > std::numeric_limits<Size>::max() - nbytes) {
                    reject = true;
                    admission_error = Errc::too_large;
                }
                else store_size = source->meta.size + nbytes;
                store_flags = source->meta.flags;
                store_exptime = static_cast<std::int64_t>(source->meta.expiry);
                cas_check = source->meta.etag;
            }
            if (store_size > tm_.max_object_size()) {
                if (source && source->pin.valid) tm_.unpin_head(source->pin);
                stats_.set_rejected.fetch_add(1, rlx);
                stats_.note_command(command_kind, core::CommandResult::error);
                if (!noreply) c->out += kServerErrorTooLarge;
                c->quit_after = true; // do not consume the rejected body's advertised length
                break;
            }
            c->sh.reset();
            c->meta = false; // classic store -> STORED/NOT_STORED/EXISTS/NOT_FOUND framing
            c->set_digest = digest;
            c->set_remaining = nbytes;
            c->set_store_size = store_size;
            c->set_mode = verb == Verb::append ? 'A' : verb == Verb::prepend ? 'P' : 'S';
            c->set_source = std::move(source);
            c->set_source_copied = false;
            c->set_flags = store_flags;
            c->set_exptime = store_exptime;
            c->set_cas = cas_check;
            c->set_condition = condition;
            c->set_command = command_kind;
            c->set_metadata = {false, false};
            c->set_reply = reply;
            c->set_noreply = noreply;
            c->set_reject = reject;
            c->set_failed = false;
            c->set_error = admission_error;
            if (!reject) {
                auto h = tm_.begin_store(digest, store_size, write_mode_, condition);
                if (h) {
                    c->sh.emplace(std::move(*h));
                } else if (h.error().code == Errc::would_block) {
                    // Write-staging pool exhausted (ADR-0011/0016): park without posting a recv, so
                    // the kernel's socket buffer fills and TCP backpressures this client. The body is
                    // NOT drained into RAM. drain_set_waiters() retries begin_store once a buffer frees.
                    stats_.set_backpressure.fetch_add(1, rlx);
                    (void)park_set_waiter(c);
                    return;
                } else {
                    c->set_reject = true;
                    c->set_error = h.error().code;
                    c->set_reply = storage_failure_reply(h.error().code);
                }
            }
            c->state = St::set_body;
            continue; // consume the body in the set_body branch
        } else if (verb == Verb::stats) {
            consume_in(c, eol + 2);
            if (cmd->stats_verb == StatsVerb::reset) {
                if (reg_) reg_->reset();
                else stats_.reset();
                c->out += kReset;
            } else if (cmd->stats_verb == StatsVerb::settings) {
                c->out += format_settings();
            } else {
                c->out += format_stats(); // aggregated STAT lines (memcache channel only)
            }
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
