#include "goblin/protocol/memcache/server.hpp"
#include "goblin/protocol/memcache/auth.hpp"
#include "goblin/protocol/memcache/stats_format.hpp"

#include "goblin/core/stats.hpp"
#include "goblin/crypto/sha256.hpp"
#include "goblin/http/http_loop.hpp"
#include "goblin/http/https_loop.hpp"
#include "goblin/net/connection_dispatcher.hpp"
#include "goblin/net/listener.hpp"
#include "goblin/net/numa.hpp"
#include "goblin/net/rdma_server.hpp"
#include "goblin/net/stream_io.hpp"
#include "goblin/protocol/memcache/event_loop.hpp"
#include "goblin/protocol/memcache/protocol.hpp"
#include "goblin/protocol/memcache/tls_event_loop.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <optional>
#include <print>
#include <future>
#include <fcntl.h>
#include <format>
#include <latch>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <utility>
#include <unistd.h>
#include <vector>

namespace goblin::memcache {
namespace {

thread_local core::Stats* current_blocking_stats = nullptr;

class BlockingStatsScope {
public:
    explicit BlockingStatsScope(core::Stats& stats)
        : previous_(std::exchange(current_blocking_stats, &stats)) {}
    ~BlockingStatsScope() { current_blocking_stats = previous_; }
private:
    core::Stats* previous_;
};

class UnixSocketCleanup {
public:
    ~UnixSocketCleanup() { if (!path_.empty()) (void)::unlink(path_.c_str()); }
    void arm(std::string_view path) { path_.assign(path); }
private:
    std::string path_;
};

bool send_raw(int fd, const char* p, std::size_t left) {
    while (left > 0) {
        const ssize_t w = ::send(fd, p, left, MSG_NOSIGNAL);
        if (w <= 0) return false;
        if (current_blocking_stats)
            current_blocking_stats->bytes_served.fetch_add(static_cast<std::size_t>(w),
                                                            std::memory_order_relaxed);
        p += w;
        left -= static_cast<std::size_t>(w);
    }
    return true;
}
bool send_all(int fd, std::string_view s) { return send_raw(fd, s.data(), s.size()); }
bool send_all(int fd, ByteView b) {
    return send_raw(fd, reinterpret_cast<const char*>(b.data()), b.size());
}

bool recv_more(int fd, std::string& buf) {
    char tmp[16384];
    const ssize_t r = ::recv(fd, tmp, sizeof tmp, 0);
    if (r <= 0) return false; // EOF or error
    if (current_blocking_stats)
        current_blocking_stats->bytes_received.fetch_add(static_cast<std::size_t>(r),
                                                          std::memory_order_relaxed);
    buf.append(tmp, static_cast<std::size_t>(r));
    return true;
}

void sleep_interruptibly(const std::atomic<bool>& shutdown, unsigned milliseconds) {
    for (unsigned slept = 0;
         slept < milliseconds && !shutdown.load(std::memory_order_relaxed); slept += 100)
        std::this_thread::sleep_for(
            std::chrono::milliseconds(std::min(100u, milliseconds - slept)));
}

class HeadPinGuard {
public:
    HeadPinGuard(storage::TierManager& tm, storage::TierManager::HeadPin& pin)
        : tm_(&tm), pin_(&pin) {}
    ~HeadPinGuard() {
        if (pin_->valid) tm_->unpin_head(*pin_);
    }
    HeadPinGuard(const HeadPinGuard&) = delete;
    HeadPinGuard& operator=(const HeadPinGuard&) = delete;

private:
    storage::TierManager* tm_;
    storage::TierManager::HeadPin* pin_;
};

class SnapshotPinGuard {
public:
    SnapshotPinGuard(storage::TierManager& tm,
                     std::optional<storage::TierManager::Snapshot>& snapshot)
        : tm_(&tm), snapshot_(&snapshot) {}
    ~SnapshotPinGuard() {
        if (*snapshot_ && (*snapshot_)->pin.valid) tm_->unpin_head((*snapshot_)->pin);
    }
    void release() {
        if (*snapshot_ && (*snapshot_)->pin.valid) {
            tm_->unpin_head((*snapshot_)->pin);
            (*snapshot_)->pin.valid = false;
        }
        snapshot_->reset();
    }
private:
    storage::TierManager* tm_;
    std::optional<storage::TierManager::Snapshot>* snapshot_;
};

void append_u64(std::string& out, std::uint64_t value) {
    char buf[24];
    const auto [end, ec] = std::to_chars(buf, buf + sizeof buf, value);
    out.append(buf, static_cast<std::size_t>(end - buf));
}

void append_i64(std::string& out, std::int64_t value) {
    char buf[24];
    const auto [end, ec] = std::to_chars(buf, buf + sizeof buf, value);
    out.append(buf, static_cast<std::size_t>(end - buf));
}

void append_meta_tokens(std::string& out, const MetaCommand& command,
                        const storage::ObjectMeta& meta, std::uint32_t now,
                        bool recache = false, bool winner = false, bool stale = false) {
    bool claim_emitted = false;
    bool stale_emitted = false;
    for (const char flag : command.return_order) {
        switch (flag) {
            case 'f': out += " f"; append_u64(out, meta.flags); break;
            case 's': out += " s"; append_u64(out, meta.size); break;
            case 't':
                out += " t";
                append_i64(out, meta.expiry == 0 ? -1 :
                    std::max<std::int64_t>(0, std::int64_t(meta.expiry) - now));
                break;
            case 'c': out += " c"; append_u64(out, meta.etag); break;
            case 'k':
                out += " k"; out += command.key;
                if (command.key_binary) out += " b";
                break;
            case 'h': out += meta.fetched ? " h1" : " h0"; break;
            case 'l':
                out += " l";
                append_u64(out, now >= meta.last_access ? now - meta.last_access : 0);
                break;
            case 'O':
                if (!command.opaque.empty()) { out += " O"; out += command.opaque; }
                break;
            case 'N':
            case 'R':
                if (recache && !claim_emitted) {
                    if (stale && !stale_emitted) { out += " X"; stale_emitted = true; }
                    out += winner ? " W" : " Z";
                    claim_emitted = true;
                }
                break;
            default: break;
        }
    }
    if (stale && !stale_emitted) out += " X";
    if (recache && !claim_emitted) out += winner ? " W" : " Z";
    if (!command.opaque.empty() && command.return_order.find('O') == std::string::npos) {
        out += " O"; out += command.opaque;
    }
}

std::string format_stats(storage::TierManager& tm, const core::StatsRegistry& registry) {
    const auto stats = registry.aggregate(core::StatsSelection::memcache);
    return format_stats_response(tm, stats, registry.uptime_secs());
}

// Stream one immutable snapshot for the response's whole lifetime. Its fixed head locator and open
// generation fds cannot be mixed with a concurrent replacement between chunks (ADR-0018).
bool stream_value(int fd, storage::TierManager& tm, core::Reactor& reactor,
                  core::IoBufferPool& iobufs, storage::TierManager::Snapshot& snapshot) {
    if (snapshot.meta.size == 0) return true;
    if (!snapshot.rs) {
        return snapshot.pin.valid && send_all(fd, tm.pinned_bytes(snapshot.pin));
    }
    // Backpressure (ADR-0011): wait for a read buffer rather than dropping the GET. A blocking worker
    // serves one conn at a time, so io_buffers >= cores means this never spins.
    std::optional<MutBytes> chunk = iobufs.acquire();
    while (!chunk) {
        std::this_thread::yield();
        chunk = iobufs.acquire();
    }
    bool ok = true;
    Size pos = 0;
    while (pos < snapshot.meta.size) {
        const Size want = std::min<Size>(chunk->size(), snapshot.meta.size - pos);
        const auto n = snapshot.rs->read(reactor, pos, *chunk);
        if (!n || *n != want) { ok = false; break; }
        if (!send_all(fd, chunk->subspan(0, want))) { ok = false; break; }
        pos += want;
    }
    iobufs.release(*chunk);
    return ok;
}

bool handle_meta_command(int fd, const MetaCommand& command, std::string& buf,
                         storage::TierManager& tm, core::Reactor& reactor,
                         core::IoBufferPool& iobufs, WriteMode write_mode,
                         core::Stats& stats) {
    constexpr auto rlx = std::memory_order_relaxed;
    const std::uint32_t now = storage::now_unix();
    if (command.verb == MetaVerb::mn) return send_all(fd, kMetaNoop);
    const auto digest = crypto::hash_key(command.lookup_key());

    if (command.verb == MetaVerb::ms) {
        std::optional<storage::TierManager::Snapshot> source;
        SnapshotPinGuard source_guard(tm, source);
        if (command.mode == 'A' || command.mode == 'P')
            source = tm.open_snapshot(digest, /*record_access=*/false, now);
        const auto existing = source ? std::optional(source->meta) : tm.lookup_live(digest, now);
        const bool present = existing.has_value();
        bool admit = true;
        Errc admission_error = Errc::ok;
        std::string_view rejection = kMetaNotStored;
        std::uint64_t cas_expected = 0;
        storage::StoreCondition condition = storage::StoreCondition::unconditional;
        if (command.has_cas) {
            if (!present) { admit = false; rejection = kMetaNotFound; }
            else if (existing->etag != command.cas) { admit = false; rejection = kMetaExists; }
            else cas_expected = command.cas;
        }
        if (command.mode == 'E') condition = storage::StoreCondition::add;
        else if (command.mode == 'R' || command.mode == 'A' || command.mode == 'P')
            condition = storage::StoreCondition::replace;
        if (admit && command.mode == 'E' && present) admit = false;
        if (admit && (command.mode == 'R' || command.mode == 'A' || command.mode == 'P') &&
            !present) admit = false;
        if (command.mode != 'S' && command.mode != 'E' && command.mode != 'R' &&
            command.mode != 'A' && command.mode != 'P') admit = false;

        Size store_size = command.datalen;
        std::uint32_t flags = command.set_flags;
        std::uint32_t expiry = command.has_ttl
            ? exptime_to_expiry(command.ttl, now) : 0;
        if (admit && (command.mode == 'A' || command.mode == 'P')) {
            if (existing->size > std::numeric_limits<Size>::max() - command.datalen) {
                admit = false;
                admission_error = Errc::too_large;
            }
            else store_size = existing->size + command.datalen;
            flags = existing->flags;
            expiry = existing->expiry;
            cas_expected = existing->etag;
        }
        if (store_size > tm.max_object_size()) {
            stats.set_rejected.fetch_add(1, rlx);
            (void)send_all(fd, kServerErrorTooLarge);
            return false; // close without consuming an attacker-controlled oversized body
        }

        std::optional<storage::TierManager::StoreHandle> handle;
        Errc store_error = Errc::ok;
        if (admit) {
            for (;;) {
                auto opened = tm.begin_store(digest, store_size, write_mode, condition);
                if (opened) { handle.emplace(std::move(*opened)); break; }
                if (opened.error().code != Errc::would_block) {
                    store_error = opened.error().code;
                    break;
                }
                std::this_thread::yield();
            }
        }
        bool write_ok = true;
        if (handle && command.mode == 'A') {
            if (auto copied = tm.copy_snapshot(*handle, *source); !copied) {
                write_ok = false;
                store_error = copied.error().code;
                handle.reset();
            }
            source_guard.release();
        }
        std::uint64_t consumed = 0;
        while (consumed < command.datalen) {
            if (buf.empty() && !recv_more(fd, buf)) return false;
            const std::size_t take = std::min<std::size_t>(
                buf.size(), static_cast<std::size_t>(command.datalen - consumed));
            if (handle && write_ok) {
                auto written = handle->write(ByteView(
                    reinterpret_cast<const std::byte*>(buf.data()), take));
                if (!written) {
                    write_ok = false;
                    store_error = written.error().code;
                    handle.reset();
                } else {
                    stats.bytes_stored.fetch_add(take, rlx);
                }
            }
            consumed += take;
            buf.erase(0, take);
        }
        if (handle && command.mode == 'P') {
            if (auto copied = tm.copy_snapshot(*handle, *source); !copied) {
                write_ok = false;
                store_error = copied.error().code;
                handle.reset();
            }
            source_guard.release();
        }
        while (buf.size() < 2) if (!recv_more(fd, buf)) return false;
        const bool crlf = buf[0] == '\r' && buf[1] == '\n';
        buf.erase(0, 2);

        std::string reply;
        bool stored = false;
        std::uint64_t etag = 0;
        if (!admit) reply = admission_error == Errc::ok
            ? std::string(rejection) : std::string(storage_failure_reply(admission_error));
        else if (!crlf) reply = std::string(kBadDataChunk);
        else if (!handle || !write_ok) reply = std::string(storage_failure_reply(store_error));
        else {
            storage::TierManager::CommitMetadata state{command.invalidate, false};
            auto committed = handle->commit(flags, expiry, cas_expected, {}, state);
            if (!committed) {
                if ((command.mode == 'A' || command.mode == 'P') &&
                    committed.error().code == Errc::cas_mismatch)
                    reply = std::string(kMetaNotStored);
                else reply = std::string(commit_failure_reply(committed.error().code, true));
            } else {
                stored = true;
                etag = *committed;
            }
        }
        if (stored) stats.sets.fetch_add(1, rlx);
        else if (reply != kBadDataChunk) stats.set_rejected.fetch_add(1, rlx);
        if (stored && !command.quiet) {
            storage::ObjectMeta meta;
            meta.size = store_size;
            meta.flags = flags;
            meta.expiry = expiry;
            meta.etag = etag;
            reply = "HD";
            append_meta_tokens(reply, command, meta, now);
            reply += "\r\n";
        }
        return (stored && command.quiet) || send_all(fd, reply);
    }

    if (command.verb == MetaVerb::me) {
        const auto meta = tm.lookup_live(digest, now);
        if (!meta) return send_all(fd, kMetaMiss);
        std::string reply = "ME ";
        reply += command.key;
        if (command.key_binary) reply += " b";
        reply += " exp="; append_u64(reply, meta->expiry);
        reply += " la="; append_u64(reply, meta->last_access);
        reply += " cas="; append_u64(reply, meta->etag);
        reply += meta->fetched ? " fetch=yes" : " fetch=no";
        reply += " cls=0 size="; append_u64(reply, meta->size);
        reply += "\r\n";
        return send_all(fd, reply);
    }

    if (command.verb == MetaVerb::ma) {
        if (command.mode != 'I' && command.mode != 'D')
            return send_all(fd, "CLIENT_ERROR invalid arithmetic mode\r\n");
        const std::optional<std::uint64_t> initial = command.has_vivify
            ? std::optional(command.has_initial ? command.initial : 0) : std::nullopt;
        const std::optional<std::uint32_t> expiry = command.has_ttl
            ? std::optional(exptime_to_expiry(command.ttl, now))
            : command.has_vivify
                ? std::optional(exptime_to_expiry(command.vivify_ttl, now))
                : std::nullopt;
        auto result = tm.arithmetic(digest, command.delta, command.mode == 'D', initial, expiry,
                                    command.has_cas ? command.cas : 0, write_mode);
        if (!result) {
            if (result.error().code == Errc::not_found) return send_all(fd, kMetaNotFound);
            if (result.error().code == Errc::cas_mismatch) return send_all(fd, kMetaExists);
            if (result.error().code == Errc::invalid_argument)
                return send_all(fd, "CLIENT_ERROR invalid numeric value\r\n");
            return send_all(fd, storage_failure_reply(result.error().code));
        }
        if (command.quiet) return true;
        std::string reply = "VA ";
        append_u64(reply, result->value);
        append_meta_tokens(reply, command, result->meta, now);
        reply += "\r\n";
        return send_all(fd, reply);
    }

    if (command.verb == MetaVerb::md) {
        const auto meta = tm.lookup_live(digest, now);
        if (!meta) return command.quiet || send_all(fd, kMetaNotFound);
        if (command.has_cas && meta->etag != command.cas)
            return command.quiet || send_all(fd, kMetaExists);
        bool stored = false;
        Errc mutation_error = Errc::ok;
        if (command.invalidate) {
            const std::optional<std::uint32_t> expiry = command.has_ttl
                ? std::optional(exptime_to_expiry(command.ttl, now)) : std::nullopt;
            if (command.remove_value) {
                auto handle = tm.begin_store(digest, 0, write_mode, storage::StoreCondition::replace);
                if (handle) {
                    storage::TierManager::CommitMetadata state{true, false};
                    auto committed = handle->commit(meta->flags, expiry.value_or(meta->expiry),
                                                    meta->etag, {}, state);
                    stored = committed.has_value();
                    if (!committed) mutation_error = committed.error().code;
                } else mutation_error = handle.error().code;
            } else {
                const auto outcome =
                    tm.mark_stale(digest, command.has_cas ? command.cas : 0, expiry, now);
                stored = outcome == storage::Index::MetaMutation::stored;
                if (outcome == storage::Index::MetaMutation::cas_mismatch)
                    mutation_error = Errc::cas_mismatch;
            }
        } else {
            stored = tm.remove(digest);
        }
        if (!stored) {
            if (command.quiet) return true;
            if (mutation_error == Errc::cas_mismatch) return send_all(fd, kMetaExists);
            if (mutation_error != Errc::ok)
                return send_all(fd, storage_failure_reply(mutation_error));
            return send_all(fd, kMetaNotFound);
        }
        if (command.quiet) return true;
        std::string reply = "HD";
        append_meta_tokens(reply, command, *meta, now);
        reply += "\r\n";
        return send_all(fd, reply);
    }

    // mg: vivify and recache selection happen before the immutable response snapshot is opened.
    auto meta = tm.lookup_live(digest, now);
    bool recache = false;
    bool winner = false;
    Errc vivify_error = Errc::ok;
    if (!meta && command.has_vivify) {
        auto handle = tm.begin_store(digest, 0, write_mode, storage::StoreCondition::add);
        if (handle) {
            storage::TierManager::CommitMetadata state{false, true};
            auto committed = handle->commit(0, exptime_to_expiry(command.vivify_ttl, now),
                                            0, {}, state);
            if (committed) {
                recache = true;
                winner = true;
            } else if (committed.error().code != Errc::condition_not_met)
                vivify_error = committed.error().code;
        } else vivify_error = handle.error().code;
        meta = tm.lookup_live(digest, now);
        if (meta && !winner && meta->size == 0 && meta->recache_claimed) recache = true;
    }
    if (!meta && vivify_error != Errc::ok)
        return send_all(fd, storage_failure_reply(vivify_error));
    if (meta && command.has_ttl) {
        if (tm.touch_ttl(digest, exptime_to_expiry(command.ttl, now), now))
            meta = tm.lookup_live(digest, now);
    }
    if (meta) {
        const bool early = command.has_recache && meta->expiry != 0 && meta->expiry > now &&
                           meta->expiry - now < command.recache_ttl;
        if (meta->stale || early) {
            recache = true;
            winner = tm.claim_recache(digest, now) == storage::Index::RecacheClaim::winner;
        }
    }
    if (!meta) return command.quiet || send_all(fd, kMetaMiss);

    if (!command.rf_value) {
        if (!command.no_update) tm.touch(digest);
        std::string reply = "HD";
        append_meta_tokens(reply, command, *meta, now, recache, winner, meta->stale);
        reply += "\r\n";
        return send_all(fd, reply);
    }
    auto snapshot = tm.open_snapshot(digest, /*record_access=*/!command.no_update, now);
    if (!snapshot) return command.quiet || send_all(fd, kMetaMiss);
    HeadPinGuard pin_guard(tm, snapshot->pin);
    if (!command.no_update) tm.touch_policies(digest, snapshot->meta.size, snapshot->pin.valid);
    const bool conditional = command.has_cas && command.cas == snapshot->meta.etag;
    std::string header = conditional ? "HD" : "VA ";
    if (!conditional) append_u64(header, snapshot->meta.size);
    append_meta_tokens(header, command, snapshot->meta, now, recache, winner,
                       snapshot->meta.stale);
    header += "\r\n";
    stats.get_hits.fetch_add(1, rlx);
    if (!send_all(fd, header)) return false;
    if (conditional) return true;
    return stream_value(fd, tm, reactor, iobufs, *snapshot) && send_all(fd, "\r\n");
}

void handle_conn(int fd, storage::TierManager& tm, core::Reactor& reactor,
                 core::IoBufferPool& iobufs, WriteMode write_mode, core::Stats& stats,
                 core::StatsRegistry& registry, const Authenticator* authenticator = nullptr) {
    BlockingStatsScope stats_scope(stats);
    std::string buf;
    bool authenticated = authenticator == nullptr;
    while (true) {
        std::size_t eol = buf.find("\r\n");
        while (eol == std::string::npos) {
            if (!recv_more(fd, buf)) return;
            eol = buf.find("\r\n");
        }
        const std::string_view line(buf.data(), eol);
        if (!authenticated) {
            constexpr std::uint64_t kMaxAuthToken = 16 * 1024;
            const auto command = parse_command(line);
            if (!command || command->verb != Verb::set) {
                buf.erase(0, eol + 2);
                stats.auth_errors.fetch_add(1, std::memory_order_relaxed);
                if (!send_all(fd, kClientErrorUnauthenticated)) return;
                continue;
            }
            const std::uint64_t bytes = command->bytes;
            buf.erase(0, eol + 2);
            if (bytes > kMaxAuthToken) {
                stats.auth_cmds.fetch_add(1, std::memory_order_relaxed);
                stats.auth_errors.fetch_add(1, std::memory_order_relaxed);
                if (!send_all(fd, kClientErrorAuthTooLong)) return;
                continue;
            }
            while (buf.size() < bytes + 2) if (!recv_more(fd, buf)) return;
            const std::string_view token(buf.data(), static_cast<std::size_t>(bytes));
            const bool terminated = buf[bytes] == '\r' && buf[bytes + 1] == '\n';
            std::string_view reply;
            stats.auth_cmds.fetch_add(1, std::memory_order_relaxed);
            if (!terminated) reply = kClientErrorBadTermination;
            else if (token.find(' ') == std::string_view::npos) reply = kClientErrorAuthFormat;
            else if (!authenticator->authenticate(token)) reply = kClientErrorAuthFailure;
            else { authenticated = true; reply = kStored; }
            if (!authenticated) stats.auth_errors.fetch_add(1, std::memory_order_relaxed);
            buf.erase(0, static_cast<std::size_t>(bytes) + 2);
            if (!send_all(fd, reply)) return;
            continue;
        }
        if (line.size() >= 2 && line[0] == 'm' &&
            (line[1] == 'n' || line[1] == 'g' || line[1] == 's' || line[1] == 'd' ||
             line[1] == 'a' || line[1] == 'e') &&
            (line.size() == 2 || line[2] == ' ')) {
            auto command = parse_meta(line);
            if (!command) {
                buf.erase(0, eol + 2);
                if (!send_all(fd, "CLIENT_ERROR bad meta command\r\n")) return;
                continue;
            }
            // Copy all string_view-backed fields before erasing the command line.
            MetaCommand owned = *command;
            const std::string key(command->key);
            const std::string opaque(command->opaque);
            owned.key = key;
            owned.opaque = opaque;
            buf.erase(0, eol + 2);
            if (!handle_meta_command(fd, owned, buf, tm, reactor, iobufs, write_mode, stats))
                return;
            continue;
        }
        const auto cmd = parse_command(line);
        if (!cmd) {
            if (!send_all(fd, command_parse_error_reply(cmd.error()))) return;
            buf.erase(0, eol + 2);
            continue;
        }

        if (cmd->is_storage()) {
            const Verb verb = cmd->verb;
            const std::uint32_t flags = cmd->flags;
            const std::int64_t exptime = cmd->exptime;
            const std::uint64_t nbytes = cmd->bytes;
            const std::uint64_t cmd_cas = cmd->cas;
            const bool noreply = cmd->noreply;
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
            const auto digest = crypto::hash_key(cmd->key); // key still valid (buf intact here)
            buf.erase(0, eol + 2);                          // drop command line; data block follows

            // Admission, before opening files or writing anything. An expired item counts as absent
            // (lazy TTL, ADR-0007); cas must exist + match the current CAS.
            const bool mutation = verb == Verb::append || verb == Verb::prepend;
            std::optional<storage::TierManager::Snapshot> source;
            SnapshotPinGuard source_guard(tm, source);
            if (mutation) source = tm.open_snapshot(digest, /*record_access=*/false);
            const auto exist = source ? std::optional(source->meta) : tm.lookup_live(digest);
            const bool present = exist.has_value();
            bool admit = true;
            Errc admission_error = Errc::ok;
            std::string_view reject_reply = kNotStored;
            std::uint64_t cas_check = 0;
            storage::StoreCondition condition = storage::StoreCondition::unconditional;
            if (verb == Verb::add) condition = storage::StoreCondition::add;
            else if (verb == Verb::replace || mutation)
                condition = storage::StoreCondition::replace;
            if (verb == Verb::add && present) admit = false;
            else if (verb == Verb::replace && !present) admit = false;
            else if (mutation && !present) admit = false;
            else if (verb == Verb::cas) {
                if (!present) { admit = false; reject_reply = kNotFound; }
                else if (exist->etag != cmd_cas) { admit = false; reject_reply = kExists; }
                else cas_check = cmd_cas;
            }
            Size store_size = nbytes;
            std::uint32_t store_flags = flags;
            std::uint32_t store_expiry = exptime_to_expiry(exptime, storage::now_unix());
            if (admit && mutation) {
                if (exist->size > std::numeric_limits<Size>::max() - nbytes) {
                    admit = false;
                    admission_error = Errc::too_large;
                }
                else store_size = exist->size + nbytes;
                store_flags = exist->flags;
                store_expiry = exist->expiry;
                cas_check = exist->etag;
            }
            if (store_size > tm.max_object_size()) {
                stats.set_rejected.fetch_add(1, std::memory_order_relaxed);
                stats.note_command(command_kind, core::CommandResult::error);
                if (!noreply) (void)send_all(fd, kServerErrorTooLarge);
                return; // close without draining the rejected value
            }
            std::optional<storage::TierManager::StoreHandle> handle;
            Errc store_error = Errc::ok;
            if (admit) {
                // Blocking-mode backpressure (ADR-0011/0016): if the write-staging pool is exhausted,
                // retry rather than fail. A spinning worker holds no buffer, so a peer mid-SET will
                // commit and free one -> forward progress (size io_buffers >= cores to never spin).
                for (;;) {
                    auto h = tm.begin_store(digest, store_size, write_mode, condition);
                    if (h) { handle.emplace(std::move(*h)); break; }
                    if (h.error().code != Errc::would_block) {
                        store_error = h.error().code;
                        break;
                    }
                    std::this_thread::yield();
                }
            }
            bool write_ok = true;
            if (handle && verb == Verb::append) {
                if (auto copied = tm.copy_snapshot(*handle, *source); !copied) {
                    write_ok = false;
                    store_error = copied.error().code;
                    handle.reset();
                }
                source_guard.release();
            }

            // Stream the data block to disk in chunks (write it, or drain it if not storing).
            // RAM held = the recv buffer + the head — never the whole value (ADR-0016).
            std::uint64_t written = 0;
            while (written < nbytes) {
                if (buf.empty() && !recv_more(fd, buf)) return; // peer died mid-write
                const std::size_t take =
                    std::min<std::size_t>(buf.size(), static_cast<std::size_t>(nbytes - written));
                if (handle && write_ok) {
                    if (auto st = handle->write(
                            ByteView(reinterpret_cast<const std::byte*>(buf.data()), take));
                        !st) {
                        write_ok = false;
                        store_error = st.error().code;
                        handle.reset(); // drain framing without retaining scarce store resources
                    } else {
                        stats.bytes_stored.fetch_add(take, std::memory_order_relaxed);
                    }
                }
                written += take;
                buf.erase(0, take);
            }
            if (handle && verb == Verb::prepend) {
                if (auto copied = tm.copy_snapshot(*handle, *source); !copied) {
                    write_ok = false;
                    store_error = copied.error().code;
                    handle.reset();
                }
                source_guard.release();
            }
            while (buf.size() < 2) { // trailing CRLF
                if (!recv_more(fd, buf)) return;
            }
            const bool crlf_ok = buf[0] == '\r' && buf[1] == '\n';
            buf.erase(0, 2);

            std::string_view reply;
            if (!admit) reply = admission_error == Errc::ok
                ? reject_reply : storage_failure_reply(admission_error);
            else if (!crlf_ok) reply = kBadDataChunk;
            else if (!handle || !write_ok) reply = storage_failure_reply(store_error);
            else if (auto st = handle->commit(store_flags, store_expiry, cas_check); !st)
                reply = mutation && st.error().code == Errc::cas_mismatch
                    ? kNotStored : commit_failure_reply(st.error().code, false);
            else {
                reply = kStored; // st holds the new etag; unused on the sync path
                stats.sets.fetch_add(1, std::memory_order_relaxed);
            }
            if (reply != kStored && reply != kBadDataChunk)
                stats.set_rejected.fetch_add(1, std::memory_order_relaxed);
            const bool condition_reply = reply == kNotStored || reply == kExists ||
                                         reply == kNotFound;
            stats.note_command(command_kind,
                reply == kStored ? core::CommandResult::success
                : command_kind == core::CommandKind::cas && reply == kNotFound
                    ? core::CommandResult::miss
                : condition_reply ? core::CommandResult::condition
                                  : core::CommandResult::error);
            // (An uncommitted handle aborts on scope exit — the object stays unindexed/invisible.)
            if (!noreply && !send_all(fd, reply)) return;
            continue;
        }

        switch (cmd->verb) {
            case Verb::get:
            case Verb::gets:
            case Verb::gat:
            case Verb::gats: {
                const auto request_started = std::chrono::steady_clock::now();
                bool ttfb_recorded = false;
                const auto note_ttfb = [&] {
                    if (ttfb_recorded) return;
                    stats.ttfb.observe(static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - request_started).count()));
                    ttfb_recorded = true;
                };
                const bool with_cas = cmd->verb == Verb::gets || cmd->verb == Verb::gats;
                const bool get_and_touch = cmd->verb == Verb::gat || cmd->verb == Verb::gats;
                const std::uint32_t touch_expiry = get_and_touch
                    ? exptime_to_expiry(cmd->exptime, storage::now_unix()) : 0;
                const auto send_key = [&](std::string_view key) {
                    const auto digest = crypto::hash_key(key);
                    if (get_and_touch)
                        tm.touch_ttl(digest, touch_expiry, storage::now_unix());
                    auto snapshot = tm.open_snapshot(digest, /*record_access=*/true,
                                                     storage::now_unix());
                    if (!snapshot) {
                        stats.get_misses.fetch_add(1, std::memory_order_relaxed);
                        return true; // misses are omitted from a retrieval response
                    }
                    stats.get_hits.fetch_add(1, std::memory_order_relaxed);
                    HeadPinGuard pin_guard(tm, snapshot->pin);
                    tm.touch_policies(digest, snapshot->meta.size, snapshot->pin.valid);
                    if (!send_all(fd, with_cas
                                          ? value_header_cas(key, snapshot->meta.flags,
                                                             snapshot->meta.size, snapshot->meta.etag)
                                          : value_header(key, snapshot->meta.flags,
                                                         snapshot->meta.size)))
                        return false;
                    note_ttfb();
                    return stream_value(fd, tm, reactor, iobufs, *snapshot) &&
                           send_all(fd, std::string_view{"\r\n"});
                };
                if (!send_key(cmd->key)) return;
                for (const std::string_view key : cmd->extra_keys)
                    if (!send_key(key)) return;
                if (!send_all(fd, kEnd)) return; // exactly one terminator for the whole command
                note_ttfb();
                break;
            }
            case Verb::touch: {
                const bool touched = tm.touch_ttl(crypto::hash_key(cmd->key),
                    exptime_to_expiry(cmd->exptime, storage::now_unix()));
                stats.note_command(core::CommandKind::touch, touched ? core::CommandResult::success
                                                                    : core::CommandResult::miss);
                if (!cmd->noreply && !send_all(fd, touched ? kTouched : kNotFound)) return;
                break;
            }
            case Verb::incr:
            case Verb::decr: {
                auto result = tm.arithmetic(crypto::hash_key(cmd->key), cmd->delta,
                                            cmd->verb == Verb::decr, std::nullopt,
                                            std::nullopt, 0, write_mode);
                stats.note_command(cmd->verb == Verb::decr ? core::CommandKind::decr
                                                           : core::CommandKind::incr,
                    result ? core::CommandResult::success
                    : result.error().code == Errc::not_found ? core::CommandResult::miss
                                                             : core::CommandResult::error);
                if (!cmd->noreply) {
                    if (result) {
                        std::string reply;
                        append_u64(reply, result->value);
                        reply += "\r\n";
                        if (!send_all(fd, reply)) return;
                    } else if (!send_all(fd,
                        result.error().code == Errc::not_found ? kNotFound :
                        result.error().code == Errc::invalid_argument ? kClientErrorNonNumeric :
                        storage_failure_reply(result.error().code))) return;
                }
                break;
            }
            case Verb::del: {
                const bool erased = tm.remove(crypto::hash_key(cmd->key));
                stats.note_command(core::CommandKind::delete_, erased ? core::CommandResult::success
                                                                     : core::CommandResult::miss);
                if (!cmd->noreply && !send_all(fd, erased ? kDeleted : kNotFound)) return;
                break;
            }
            case Verb::flush_all:
                tm.flush_all(cmd->delay);
                stats.note_command(core::CommandKind::flush, core::CommandResult::success);
                if (!cmd->noreply && !send_all(fd, kOk)) return;
                break;
            case Verb::version:
                if (!send_all(fd, kVersion)) return;
                break;
            case Verb::stats:
                if (cmd->stats_verb == StatsVerb::reset) {
                    registry.reset();
                    if (!send_all(fd, kReset)) return;
                } else if (cmd->stats_verb == StatsVerb::settings) {
                    if (!send_all(fd, format_settings_response(registry.settings()))) return;
                } else if (!send_all(fd, format_stats(tm, registry))) return;
                break;
            case Verb::quit:
                return;
            default:
                if (!send_all(fd, kError)) return;
                break;
        }
        buf.erase(0, eol + 2);
    }
}

Result<int> make_listener(std::string_view address, std::uint16_t port, unsigned backlog,
                          bool exasock = false, bool nonblocking = false) {
    return net::make_tcp_listener(address, port, backlog, exasock, nonblocking);
}

using StartGate = std::shared_ptr<std::latch>;

Status bind_protocol_worker(const ServerConfig& cfg, const net::ConnectionInbox& inbox) {
    if (!cfg.numa_enabled || !cfg.numa_node || inbox.worker_cpu() < 0) return {};
    const unsigned cpu = static_cast<unsigned>(inbox.worker_cpu());
    return net::bind_numa_worker(*cfg.numa_node, std::span<const unsigned>(&cpu, 1));
}

// Blocking-mode workers retain the original shared listener. The async default instead uses the
// application-owned dispatcher below; blocking accept cannot use that handoff without changing the
// fallback's one-connection-at-a-time semantics.
void worker_loop(int lfd, const ServerConfig& cfg, storage::TierManager& tm,
                 core::StatsRegistry& registry, const std::atomic<bool>& shutdown, unsigned id,
                 net::ConnectionBudget& connection_budget,
                 const Authenticator* authenticator, std::promise<Status> startup) {
    (void)id;
    auto reactor = core::Reactor::create();
    if (!reactor) {
        startup.set_value(std::unexpected(reactor.error()));
        return;
    }
    auto iobufs =
        core::IoBufferPool::create(cfg.io_chunk_bytes, cfg.io_buffers, cfg.memory.lock_memory,
                                   cfg.memory.use_hugepages, cfg.memory.hugetlb_page_bytes);
    if (!iobufs) {
        startup.set_value(std::unexpected(iobufs.error()));
        return;
    }
    core::Stats stats;
    registry.add(&stats);
    startup.set_value(Status{});
    bool listener_paused = false;

    while (!shutdown.load(std::memory_order_relaxed)) {
        if (connection_budget.full()) {
            if (!listener_paused) {
                listener_paused = true;
                registry.note_listener_disabled();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        listener_paused = false;
        pollfd pfd{lfd, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, 200); // wake every 200 ms to observe shutdown (blocking path)
        if (pr <= 0) continue;
        const int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (!connection_budget.try_acquire()) {
            registry.note_connection_rejected();
            if (!listener_paused) registry.note_listener_disabled();
            listener_paused = true;
            ::close(cfd);
            continue;
        }
        const int one = 1;
        ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); // no Nagle (small-reply latency)
        const unsigned receive_timeout = cfg.io_timeout_ms && cfg.idle_timeout_ms
            ? std::min(cfg.io_timeout_ms, cfg.idle_timeout_ms)
            : std::max(cfg.io_timeout_ms, cfg.idle_timeout_ms);
        if (receive_timeout) { // bounds both an idle keepalive and a stalled request body
            timeval tv{};
            tv.tv_sec = receive_timeout / 1000;
            tv.tv_usec = (receive_timeout % 1000) * 1000;
            ::setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        }
        if (cfg.io_timeout_ms) {
            timeval tv{};
            tv.tv_sec = cfg.io_timeout_ms / 1000;
            tv.tv_usec = (cfg.io_timeout_ms % 1000) * 1000;
            ::setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        }
        stats.conns.fetch_add(1, std::memory_order_relaxed);
        stats.curr_conns.fetch_add(1, std::memory_order_relaxed);
        handle_conn(cfd, tm, *reactor, *iobufs, cfg.memcache_write_mode, stats, registry,
                    authenticator);
        stats.curr_conns.fetch_sub(1, std::memory_order_relaxed);
        ::close(cfd);
        connection_budget.release();
    }
    registry.remove(&stats);
}

// Async worker (ADR-0002): its own io_uring ring + I/O pool. A NAPI-aware coordinator assigns a
// connection once, after which this EventLoop owns recv/parse/stream/send for its whole lifetime.
void async_worker(const ServerConfig& cfg, storage::TierManager& tm, storage::Index& index,
                  core::StatsRegistry& reg, const std::atomic<bool>& shutdown,
                  net::ConnectionInbox& inbox, StartGate start_gate,
                  const Authenticator* authenticator, std::promise<Status> startup) {
    if (auto bound = bind_protocol_worker(cfg, inbox); !bound) {
        startup.set_value(std::unexpected(bound.error()));
        return;
    }
    auto reactor = core::Reactor::create();
    if (!reactor) {
        startup.set_value(std::unexpected(reactor.error()));
        return;
    }
    auto iobufs =
        core::IoBufferPool::create(cfg.io_chunk_bytes, cfg.io_buffers, cfg.memory.lock_memory,
                                   cfg.memory.use_hugepages, cfg.memory.hugetlb_page_bytes);
    if (!iobufs) {
        startup.set_value(std::unexpected(iobufs.error()));
        return;
    }
    EventLoop loop(*reactor, inbox, tm, index, *iobufs, cfg.io_timeout_ms, &reg,
                   cfg.memcache_write_mode);
    loop.set_authenticator(authenticator);
    loop.set_read_ahead(cfg.read_ahead);
    loop.set_overload_limits(cfg.idle_timeout_ms, cfg.queue_timeout_ms,
                             cfg.max_get_waiters, cfg.max_set_waiters);
    loop.set_shutdown(&shutdown, cfg.shutdown_grace_ms);
    startup.set_value(Status{});
    start_gate->wait();
    loop.run();
}

#if GOBLIN_HAVE_TLS
void tls_memcache_worker(const ServerConfig& cfg, storage::TierManager& tm,
                         storage::Index& index, tls::Context& context,
                         core::StatsRegistry& registry, const std::atomic<bool>& shutdown,
                         net::ConnectionInbox& inbox, StartGate start_gate,
                         const Authenticator* authenticator, std::promise<Status> startup) {
    if (auto bound = bind_protocol_worker(cfg, inbox); !bound) {
        startup.set_value(std::unexpected(bound.error()));
        return;
    }
    auto reactor = core::Reactor::create();
    if (!reactor) {
        startup.set_value(std::unexpected(reactor.error()));
        return;
    }
    auto buffers =
        core::IoBufferPool::create(cfg.io_chunk_bytes, cfg.io_buffers, cfg.memory.lock_memory,
                                   cfg.memory.use_hugepages, cfg.memory.hugetlb_page_bytes);
    if (!buffers) {
        startup.set_value(std::unexpected(buffers.error()));
        return;
    }
    TlsEventLoop loop(*reactor, inbox, tm, index, *buffers, context, cfg.io_timeout_ms,
                      &registry, cfg.memcache_write_mode);
    loop.set_authenticator(authenticator);
    loop.set_read_ahead(cfg.read_ahead);
    loop.set_overload_limits(cfg.idle_timeout_ms, cfg.queue_timeout_ms,
                             cfg.max_get_waiters, cfg.max_set_waiters);
    loop.set_shutdown(&shutdown, cfg.shutdown_grace_ms);
    startup.set_value(Status{});
    start_gate->wait();
    loop.run();
}
#endif

// ExaSock worker: socket operations go through standard nonblocking calls in the readiness adapter
// so the preload library can accelerate them. Disk reads remain on this worker's io_uring reactor.
void exasock_memcache_worker(const ServerConfig& cfg, storage::TierManager& tm,
                             storage::Index& index, core::StatsRegistry& reg,
                             const std::atomic<bool>& shutdown,
                             net::ConnectionInbox& inbox, StartGate start_gate,
                             const Authenticator* authenticator,
                             std::promise<Status> startup) {
    if (auto bound = bind_protocol_worker(cfg, inbox); !bound) {
        startup.set_value(std::unexpected(bound.error()));
        return;
    }
    auto reactor = core::Reactor::create();
    if (!reactor) {
        startup.set_value(std::unexpected(reactor.error()));
        return;
    }
    auto stream_io = net::make_readiness_stream_io(*reactor,
                                                    /*require_exasock_connections=*/true);
    if (!stream_io) {
        startup.set_value(std::unexpected(stream_io.error()));
        return;
    }
    auto iobufs =
        core::IoBufferPool::create(cfg.io_chunk_bytes, cfg.io_buffers, cfg.memory.lock_memory,
                                   cfg.memory.use_hugepages, cfg.memory.hugetlb_page_bytes);
    if (!iobufs) {
        startup.set_value(std::unexpected(iobufs.error()));
        return;
    }
    EventLoop loop(**stream_io, inbox, tm, index, *iobufs, cfg.io_timeout_ms, &reg,
                   cfg.memcache_write_mode);
    loop.set_authenticator(authenticator);
    loop.set_read_ahead(cfg.read_ahead);
    loop.set_overload_limits(cfg.idle_timeout_ms, cfg.queue_timeout_ms,
                             cfg.max_get_waiters, cfg.max_set_waiters);
    loop.set_shutdown(&shutdown, cfg.shutdown_grace_ms);
    startup.set_value(Status{});
    start_gate->wait();
    loop.run();
}

// HTTP object server (ADR-0005/0015): its own io_uring ring + read pool. HTTP retains a separate
// dispatcher and buffer budget so slow downloads cannot starve memcache.
void http_worker(const ServerConfig& cfg, storage::TierManager& tm, storage::Index& index,
                 core::StatsRegistry& reg, const std::atomic<bool>& shutdown,
                 net::ConnectionInbox& inbox, StartGate start_gate,
                 http::MirrorService* mirror, std::promise<Status> startup) {
    if (auto bound = bind_protocol_worker(cfg, inbox); !bound) {
        startup.set_value(std::unexpected(bound.error()));
        return;
    }
    auto reactor = core::Reactor::create();
    if (!reactor) {
        startup.set_value(std::unexpected(reactor.error()));
        return;
    }
    auto iobufs =
        core::IoBufferPool::create(cfg.io_chunk_bytes, cfg.io_buffers, cfg.memory.lock_memory,
                                   cfg.memory.use_hugepages, cfg.memory.hugetlb_page_bytes);
    if (!iobufs) {
        startup.set_value(std::unexpected(iobufs.error()));
        return;
    }
    http::KeyOptions keyopt;
    keyopt.mode = cfg.http_vhost ? http::KeyMode::vhost : http::KeyMode::path;
    keyopt.keep_query = cfg.key_on_query;
    keyopt.strip_leading_slash = cfg.key_strip_slash;
    keyopt.index_name = cfg.http_index; // HTTP-only directory index (memcache + --source unaffected)
    http::HttpLoop loop(*reactor, inbox, tm, index, *iobufs, keyopt, cfg.io_timeout_ms, &reg,
                        mirror);
    loop.set_read_ahead(cfg.read_ahead);
    loop.set_overload_limits(cfg.idle_timeout_ms, cfg.queue_timeout_ms,
                             cfg.max_get_waiters, cfg.max_set_waiters);
    loop.set_shutdown(&shutdown, cfg.shutdown_grace_ms);
    startup.set_value(Status{});
    start_gate->wait();
    loop.run();
}

void exasock_http_worker(const ServerConfig& cfg, storage::TierManager& tm,
                         storage::Index& index, core::StatsRegistry& reg,
                         const std::atomic<bool>& shutdown,
                         net::ConnectionInbox& inbox, StartGate start_gate,
                         http::MirrorService* mirror, std::promise<Status> startup) {
    if (auto bound = bind_protocol_worker(cfg, inbox); !bound) {
        startup.set_value(std::unexpected(bound.error()));
        return;
    }
    auto reactor = core::Reactor::create();
    if (!reactor) {
        startup.set_value(std::unexpected(reactor.error()));
        return;
    }
    auto stream_io = net::make_readiness_stream_io(*reactor,
                                                    /*require_exasock_connections=*/true);
    if (!stream_io) {
        startup.set_value(std::unexpected(stream_io.error()));
        return;
    }
    auto iobufs =
        core::IoBufferPool::create(cfg.io_chunk_bytes, cfg.io_buffers, cfg.memory.lock_memory,
                                   cfg.memory.use_hugepages, cfg.memory.hugetlb_page_bytes);
    if (!iobufs) {
        startup.set_value(std::unexpected(iobufs.error()));
        return;
    }
    http::KeyOptions keyopt;
    keyopt.mode = cfg.http_vhost ? http::KeyMode::vhost : http::KeyMode::path;
    keyopt.keep_query = cfg.key_on_query;
    keyopt.strip_leading_slash = cfg.key_strip_slash;
    keyopt.index_name = cfg.http_index;
    http::HttpLoop loop(**stream_io, inbox, tm, index, *iobufs, keyopt, cfg.io_timeout_ms, &reg,
                        mirror);
    loop.set_read_ahead(cfg.read_ahead);
    loop.set_overload_limits(cfg.idle_timeout_ms, cfg.queue_timeout_ms,
                             cfg.max_get_waiters, cfg.max_set_waiters);
    loop.set_shutdown(&shutdown, cfg.shutdown_grace_ms);
    startup.set_value(Status{});
    start_gate->wait();
    loop.run();
}

#if GOBLIN_HAVE_TLS
// HTTPS object server: own ring + read pool behind the HTTPS dispatcher, sharing one tls::Context
// (per-host certs, SNI selection) across all workers. The handshake runs on the loop;
// after it, kTLS makes the data path the ordinary HttpLoop flow (ADR-0005/0011).
void https_worker(const ServerConfig& cfg, storage::TierManager& tm, storage::Index& index,
                  tls::Context& ctx, core::StatsRegistry& reg, const std::atomic<bool>& shutdown,
                  net::ConnectionInbox& inbox, StartGate start_gate,
                  http::MirrorService* mirror, std::promise<Status> startup) {
    if (auto bound = bind_protocol_worker(cfg, inbox); !bound) {
        startup.set_value(std::unexpected(bound.error()));
        return;
    }
    auto reactor = core::Reactor::create();
    if (!reactor) {
        startup.set_value(std::unexpected(reactor.error()));
        return;
    }
    auto iobufs =
        core::IoBufferPool::create(cfg.io_chunk_bytes, cfg.io_buffers, cfg.memory.lock_memory,
                                   cfg.memory.use_hugepages, cfg.memory.hugetlb_page_bytes);
    if (!iobufs) {
        startup.set_value(std::unexpected(iobufs.error()));
        return;
    }
    http::KeyOptions keyopt;
    keyopt.mode = cfg.http_vhost ? http::KeyMode::vhost : http::KeyMode::path;
    keyopt.keep_query = cfg.key_on_query;
    keyopt.strip_leading_slash = cfg.key_strip_slash;
    keyopt.index_name = cfg.http_index; // HTTP-only directory index (memcache + --source unaffected)
    http::HttpsLoop loop(*reactor, inbox, tm, index, *iobufs, keyopt, ctx, cfg.io_timeout_ms, &reg,
                         mirror);
    loop.set_read_ahead(cfg.read_ahead);
    loop.set_overload_limits(cfg.idle_timeout_ms, cfg.queue_timeout_ms,
                             cfg.max_get_waiters, cfg.max_set_waiters);
    loop.set_shutdown(&shutdown, cfg.shutdown_grace_ms);
    startup.set_value(Status{});
    start_gate->wait();
    loop.run();
}
#endif

} // namespace

void serve_blocking_connection(int fd, storage::TierManager& tm, core::Reactor& reactor,
                               core::IoBufferPool& iobufs, WriteMode write_mode,
                               const Authenticator* authenticator) {
    core::StatsRegistry registry;
    core::Stats stats;
    registry.add(&stats);
    stats.conns.fetch_add(1, std::memory_order_relaxed);
    stats.curr_conns.fetch_add(1, std::memory_order_relaxed);
    handle_conn(fd, tm, reactor, iobufs, write_mode, stats, registry, authenticator);
    stats.curr_conns.fetch_sub(1, std::memory_order_relaxed);
    registry.remove(&stats);
}

Status serve(const ServerConfig& cfg, storage::TierManager& tm, storage::Index& index,
             std::atomic<bool>& shutdown) {
    UnixSocketCleanup unix_socket_cleanup;
    if (cfg.memcache_auth_file && cfg.rdma.enabled)
        return err(Errc::invalid_argument,
                   "authentication cannot leave the native RDMA endpoint unprotected");
    if (cfg.memcache_tls && (!cfg.enable_memcache || cfg.net != NetMode::async))
        return err(Errc::invalid_argument,
                   "memcache TLS requires the async memcache TCP listener");
    // main() binds itself before calling serve(), so every thread created below inherits the
    // selected NUMA node's CPU mask. Direct library callers may leave numa_cpus unresolved.
    unsigned n = cfg.cores ? cfg.cores : static_cast<unsigned>(cfg.numa_cpus.size());
    if (n == 0) n = std::thread::hardware_concurrency();
    if (n == 0) n = 1;
    std::optional<Authenticator> authenticator;
    if (cfg.memcache_auth_file) {
        auto loaded = Authenticator::load(*cfg.memcache_auth_file);
        if (!loaded) return std::unexpected(loaded.error());
        authenticator.emplace(std::move(*loaded));
    }
    const Authenticator* const auth = authenticator ? &*authenticator : nullptr;
    std::unique_ptr<http::MirrorService> mirror;
    if (cfg.mirror_url) {
        auto created = http::MirrorService::create(*cfg.mirror_url, tm, index, &shutdown, n,
                                                   cfg.mirror_client);
        if (!created) return std::unexpected(created.error());
        mirror = std::move(*created);
    }
    http::MirrorService* const mirror_service = mirror.get();
    std::vector<int> protocol_worker_cpus(n, -1);
    if (cfg.numa_enabled && cfg.numa_node && !cfg.numa_cpus.empty())
        for (unsigned worker = 0; worker < n; ++worker)
            protocol_worker_cpus[worker] =
                static_cast<int>(cfg.numa_cpus[worker % cfg.numa_cpus.size()]);

    // The same budget covers queued and live TCP/Unix connections across memcache, HTTP, HTTPS,
    // and ExaSock. Native RDMA retains its separate CM backlog and registered-memory admission.
    auto connection_budget = std::make_shared<net::ConnectionBudget>(cfg.max_connections);
    // One registry shared by every loop and acceptor. Slots and overload counters retain their
    // protocol domain so compatible memcache hit-rate and connection figures exclude HTTP(S).
    core::StatsRegistry stats_reg;
    ServerConfig effective_settings = cfg;
    effective_settings.cores = n;
    stats_reg.set_settings(effective_settings);
    std::unique_ptr<net::ConnectionDispatcher> memcache_dispatcher;
    std::unique_ptr<net::ConnectionDispatcher> unix_dispatcher;
    std::unique_ptr<net::ConnectionDispatcher> http_dispatcher;
    std::unique_ptr<net::ConnectionDispatcher> https_dispatcher;
    std::vector<std::thread> acceptors;
    std::vector<std::thread> workers;
    int blocking_lfd = -1; // shared listener for blocking-mode memcache; closed after join
    std::optional<Error> rdma_runtime_failure;

    auto join_workers = [&] {
        // Once shutdown is visible, stop every producer before a worker drains its inbox. This
        // makes the final queue drain stable: no accepted fd can arrive behind a departing loop.
        for (auto& acceptor : acceptors) {
            if (acceptor.joinable()) acceptor.join();
        }
        for (auto& worker : workers) {
            if (worker.joinable()) worker.join();
        }
        if (blocking_lfd >= 0) {
            ::close(blocking_lfd);
            blocking_lfd = -1;
        }
        if (memcache_dispatcher) memcache_dispatcher->report();
        if (unix_dispatcher) unix_dispatcher->report();
        if (http_dispatcher) http_dispatcher->report();
        if (https_dispatcher) https_dispatcher->report();
    };
    auto abort_startup = [&](std::string_view endpoint, Error error) -> Status {
        shutdown.store(true, std::memory_order_relaxed);
        join_workers();
        if (!endpoint.empty()) {
            if (error.detail.empty()) error.detail.assign(endpoint);
            else error.detail = std::string(endpoint) + ": " + error.detail;
        }
        return std::unexpected(std::move(error));
    };

    if (cfg.rdma.enabled && !net::rdma_server_available())
        return err(Errc::unsupported,
                   "--rdma requested, but this build has no libibverbs/librdmacm support");
    if (cfg.net == NetMode::exasock && !net::exasock_compiled())
        return err(Errc::unsupported,
                   "--net exasock requested, but this build has no ExaSock support");

    auto create_dispatcher = [&](std::string label, std::uint16_t port, bool exasock,
                                 core::StatsDomain domain)
        -> Result<std::unique_ptr<net::ConnectionDispatcher>> {
        auto listener = make_listener(cfg.listen_address, port, cfg.listen_backlog, exasock,
                                      /*nonblocking=*/true);
        if (!listener) return std::unexpected(listener.error());
        return net::ConnectionDispatcher::create(*listener, protocol_worker_cpus,
                                                 std::move(label), exasock,
                                                 connection_budget, &stats_reg, domain);
    };

    // Complete every fallible setup that can run on the coordinator before launching a thread.
    // In particular, an RDMA listener must never be left live when a later TCP bind or TLS-context
    // construction fails.
    if (cfg.enable_memcache && cfg.net == NetMode::blocking) {
        auto listener = make_listener(cfg.listen_address, cfg.memcache_port,
                                      cfg.listen_backlog);
        if (!listener) return std::unexpected(listener.error());
        blocking_lfd = *listener;
    } else if (cfg.enable_memcache) {
        auto dispatcher = create_dispatcher(
            cfg.net == NetMode::exasock ? "ExaSock memcache" : "memcache",
            cfg.memcache_port, cfg.net == NetMode::exasock, core::StatsDomain::memcache_tcp);
        if (!dispatcher) return std::unexpected(dispatcher.error());
        memcache_dispatcher = std::move(*dispatcher);
    }
    if (cfg.memcache_socket) {
        auto listener = net::make_unix_listener(*cfg.memcache_socket,
                                                cfg.memcache_socket_mode,
                                                cfg.listen_backlog,
                                                /*nonblocking=*/true);
        if (!listener) return std::unexpected(listener.error());
        unix_socket_cleanup.arm(*cfg.memcache_socket);
        auto dispatcher = net::ConnectionDispatcher::create(
            *listener, protocol_worker_cpus, "Unix memcache", false, connection_budget,
            &stats_reg, core::StatsDomain::memcache_tcp);
        if (!dispatcher) {
            (void)::unlink(cfg.memcache_socket->c_str());
            return std::unexpected(dispatcher.error());
        }
        unix_dispatcher = std::move(*dispatcher);
    }
    if (cfg.enable_http) {
        auto dispatcher = create_dispatcher(
            cfg.net == NetMode::exasock ? "ExaSock HTTP" : "HTTP", cfg.http_port,
            cfg.net == NetMode::exasock, core::StatsDomain::http);
        if (!dispatcher) {
            if (blocking_lfd >= 0) ::close(blocking_lfd);
            return std::unexpected(dispatcher.error());
        }
        http_dispatcher = std::move(*dispatcher);
    }
    if (cfg.enable_https) {
        auto dispatcher = create_dispatcher("HTTPS", cfg.https_port, /*exasock=*/false,
                                            core::StatsDomain::https);
        if (!dispatcher) {
            if (blocking_lfd >= 0) ::close(blocking_lfd);
            return std::unexpected(dispatcher.error());
        }
        https_dispatcher = std::move(*dispatcher);
    }

#if GOBLIN_HAVE_TLS
    std::optional<tls::Context> https_ctx; // outlives every HTTPS worker
    std::optional<tls::Context> memcache_ctx; // optional-SNI context for ordinary clients
    if (cfg.memcache_tls) {
        std::vector<tls::Context::CertKey> certs;
        for (std::size_t i = 0; i < cfg.tls_cert_paths.size(); ++i)
            certs.push_back({cfg.tls_cert_paths[i], cfg.tls_key_paths[i]});
        auto context = tls::Context::create(certs, tls::Context::SniPolicy::optional);
        if (!context) return abort_startup("memcache TLS context", context.error());
        memcache_ctx.emplace(std::move(*context));
    }
    if (cfg.enable_https) {
        std::vector<tls::Context::CertKey> certs;
        for (std::size_t i = 0; i < cfg.tls_cert_paths.size(); ++i)
            certs.push_back({cfg.tls_cert_paths[i], cfg.tls_key_paths[i]});
        auto context = tls::Context::create(certs);
        if (!context) {
            if (blocking_lfd >= 0) ::close(blocking_lfd);
            return std::unexpected(context.error());
        }
        https_ctx.emplace(std::move(*context));
    }
#else
    if (cfg.enable_https || cfg.memcache_tls) {
        if (blocking_lfd >= 0) ::close(blocking_lfd);
        return err(Errc::unsupported, "built without OpenSSL — TLS unavailable");
    }
#endif

    // Native RDMA is a distinct reliable-connected endpoint, not TCP over IPoIB. CM acceptance and
    // each connection's verbs/io_uring progress run outside the TCP worker loops while sharing the
    // same canonical index, TierManager, write policy, and stats registry.
    if (cfg.rdma.enabled) {
        std::promise<Status> startup;
        auto ready = startup.get_future();
        workers.emplace_back([&cfg, &tm, &index, &stats_reg, &shutdown,
                              &rdma_runtime_failure, startup = std::move(startup)]() mutable {
            bool announced = false;
            auto status = net::serve_rdma(
                cfg.rdma, cfg.memcache_write_mode, tm, index, stats_reg, shutdown,
                [&startup, &announced](Status value) {
                    announced = true;
                    startup.set_value(std::move(value));
                });
            if (!status && announced) {
                rdma_runtime_failure.emplace(status.error());
                shutdown.store(true, std::memory_order_relaxed);
                std::println(stderr, "RDMA listener: {}", status.error().detail);
            }
        });
        Status opened = ready.get();
        if (!opened) return abort_startup("RDMA listener", opened.error());
    }

    // memcache: async io_uring loops, or the blocking thread-per-core fallback (--net blocking).
    if (cfg.enable_memcache) {
        if (cfg.net != NetMode::blocking) {
            auto start_gate = std::make_shared<std::latch>(1);
            for (unsigned i = 0; i < n; ++i) {
                std::promise<Status> startup;
                auto ready = startup.get_future();
                auto* const inbox = &memcache_dispatcher->inbox(i);
                if (cfg.memcache_tls) {
#if GOBLIN_HAVE_TLS
                    auto* const context = &*memcache_ctx;
                    workers.emplace_back(
                        [&cfg, &tm, &index, &stats_reg, &shutdown, inbox, start_gate,
                         context, auth, startup = std::move(startup)]() mutable {
                            tls_memcache_worker(cfg, tm, index, *context, stats_reg, shutdown,
                                                *inbox, start_gate, auth, std::move(startup));
                        });
#endif
                } else if (cfg.net == NetMode::async)
                    workers.emplace_back(
                        [&cfg, &tm, &index, &stats_reg, &shutdown, inbox, start_gate, auth,
                         startup = std::move(startup)]() mutable {
                            async_worker(cfg, tm, index, stats_reg, shutdown, *inbox, start_gate,
                                         auth, std::move(startup));
                        });
                else
                    workers.emplace_back(
                        [&cfg, &tm, &index, &stats_reg, &shutdown, inbox, start_gate, auth,
                         startup = std::move(startup)]() mutable {
                            exasock_memcache_worker(cfg, tm, index, stats_reg, shutdown, *inbox,
                                                    start_gate, auth, std::move(startup));
                        });
                Status opened = ready.get();
                if (!opened) {
                    start_gate->count_down();
                    return abort_startup(
                        (cfg.net == NetMode::exasock ? "ExaSock memcache worker "
                                                    : "memcache worker ") +
                            std::to_string(i),
                        opened.error());
                }
            }
            start_gate->count_down();
            acceptors.emplace_back([&shutdown, dispatcher = memcache_dispatcher.get()] {
                dispatcher->run(shutdown);
            });
        } else if (cfg.net == NetMode::blocking) {
            for (unsigned i = 0; i < n; ++i) {
                std::promise<Status> startup;
                auto ready = startup.get_future();
                workers.emplace_back([&cfg, &tm, &stats_reg, &shutdown, &connection_budget,
                                      i, fd = blocking_lfd, auth,
                                      startup = std::move(startup)]() mutable {
                    worker_loop(fd, cfg, tm, stats_reg, shutdown, i, *connection_budget,
                                auth, std::move(startup));
                });
                Status opened = ready.get();
                if (!opened)
                    return abort_startup("blocking memcache worker " + std::to_string(i),
                                         opened.error());
            }
        }
    }
    // Unix-domain memcache gets an ordinary async worker set even when TCP uses blocking or
    // ExaSock. This keeps AF_UNIX independent of provider-specific TCP behavior.
    if (cfg.memcache_socket) {
        auto start_gate = std::make_shared<std::latch>(1);
        for (unsigned i = 0; i < n; ++i) {
            std::promise<Status> startup;
            auto ready = startup.get_future();
            auto* const inbox = &unix_dispatcher->inbox(i);
            workers.emplace_back(
                [&cfg, &tm, &index, &stats_reg, &shutdown, inbox, start_gate, auth,
                 startup = std::move(startup)]() mutable {
                    async_worker(cfg, tm, index, stats_reg, shutdown, *inbox, start_gate,
                                 auth, std::move(startup));
                });
            Status opened = ready.get();
            if (!opened) {
                start_gate->count_down();
                return abort_startup("Unix memcache worker " + std::to_string(i),
                                     opened.error());
            }
        }
        start_gate->count_down();
        acceptors.emplace_back([&shutdown, dispatcher = unix_dispatcher.get()] {
            dispatcher->run(shutdown);
        });
    }
    // HTTP uses the selected ExaSock readiness loop when requested; otherwise it retains its
    // io_uring loop. The historical --net blocking selector remains memcache-only.
    if (cfg.enable_http) {
        auto start_gate = std::make_shared<std::latch>(1);
        for (unsigned i = 0; i < n; ++i) {
            std::promise<Status> startup;
            auto ready = startup.get_future();
            auto* const inbox = &http_dispatcher->inbox(i);
            if (cfg.net == NetMode::exasock)
                workers.emplace_back(
                    [&cfg, &tm, &index, &stats_reg, &shutdown, inbox, start_gate, mirror_service,
                     startup = std::move(startup)]() mutable {
                        exasock_http_worker(cfg, tm, index, stats_reg, shutdown, *inbox,
                                            start_gate, mirror_service, std::move(startup));
                    });
            else
                workers.emplace_back(
                    [&cfg, &tm, &index, &stats_reg, &shutdown, inbox, start_gate, mirror_service,
                     startup = std::move(startup)]() mutable {
                        http_worker(cfg, tm, index, stats_reg, shutdown, *inbox, start_gate,
                                    mirror_service, std::move(startup));
                    });
            Status opened = ready.get();
            if (!opened) {
                start_gate->count_down();
                return abort_startup(cfg.net == NetMode::exasock
                                         ? "ExaSock HTTP worker " + std::to_string(i)
                                         : "HTTP worker " + std::to_string(i),
                                     opened.error());
            }
        }
        start_gate->count_down();
        acceptors.emplace_back([&shutdown, dispatcher = http_dispatcher.get()] {
            dispatcher->run(shutdown);
        });
    }

    // HTTPS: the shared context was built before any listener thread; now start every ring and
    // release them together before the dispatcher begins consuming the shared accept queue.
#if GOBLIN_HAVE_TLS
    if (cfg.enable_https) {
        tls::Context& ctx = *https_ctx;
        auto start_gate = std::make_shared<std::latch>(1);
        for (unsigned i = 0; i < n; ++i) {
            std::promise<Status> startup;
            auto ready = startup.get_future();
            auto* const inbox = &https_dispatcher->inbox(i);
            workers.emplace_back(
                [&cfg, &tm, &index, &ctx, &stats_reg, &shutdown, inbox, start_gate,
                 mirror_service, startup = std::move(startup)]() mutable {
                    https_worker(cfg, tm, index, ctx, stats_reg, shutdown, *inbox, start_gate,
                                 mirror_service, std::move(startup));
                });
            Status opened = ready.get();
            if (!opened) {
                start_gate->count_down();
                return abort_startup("HTTPS worker " + std::to_string(i), opened.error());
            }
        }
        start_gate->count_down();
        acceptors.emplace_back([&shutdown, dispatcher = https_dispatcher.get()] {
            dispatcher->run(shutdown);
        });
    }
#endif

    // TTL reaper: periodically reclaim expired objects (reads already lazy-skip them). Runs for the
    // process lifetime alongside the workers; a no-op until some object is stored with a TTL.
    if (cfg.ttl_reap_ms > 0)
        workers.emplace_back([&cfg, &tm, &shutdown] {
            while (!shutdown.load(std::memory_order_relaxed)) {
                // Sleep in short slices so shutdown does not wait for the maintenance interval.
                sleep_interruptibly(shutdown, cfg.ttl_reap_ms);
                if (!shutdown.load(std::memory_order_relaxed)) tm.reap_expired();
            }
        });

    // Proactive capacity maintenance: fstatvfs each configured filesystem and reclaim local
    // whole-object victims from high to low watermark before foreground writers hit ENOSPC.
    // TierManager bounds each pass so shutdown and protocol workers cannot be starved by a badly
    // overcommitted filesystem; subsequent intervals continue toward the target.
    if (cfg.eviction.reclaim_interval_ms > 0)
        workers.emplace_back([&cfg, &tm, &shutdown] {
            while (!shutdown.load(std::memory_order_relaxed)) {
                sleep_interruptibly(shutdown, cfg.eviction.reclaim_interval_ms);
                if (!shutdown.load(std::memory_order_relaxed))
                    (void)tm.reclaim_to_watermarks();
            }
        });

    // Access-score aging is independent of NUMA: every live key loses the same fraction once/minute.
    // TierManager gives this thread priority over promotion once it announces a pending rescore.
    workers.emplace_back([&tm, &shutdown] {
        while (!shutdown.load(std::memory_order_relaxed)) {
            sleep_interruptibly(shutdown, 60'000);
            if (!shutdown.load(std::memory_order_relaxed)) tm.decay_access_scores();
        }
    });

    // Region zero is local. Drain current score inversions as quickly as block copies permit, except
    // while a rescore is pending/running; once sorted (or yielding to rescore), poll in one second.
    if (cfg.numa_promotion && cfg.memory.numa_regions.size() > 1)
        workers.emplace_back([&tm, &shutdown] {
            while (!shutdown.load(std::memory_order_relaxed)) {
                if (tm.promote_hot_remote_block()) continue;
                sleep_interruptibly(shutdown, 1'000);
            }
        });

    join_workers();
    if (rdma_runtime_failure) return std::unexpected(std::move(*rdma_runtime_failure));
    return {};
}

} // namespace goblin::memcache
