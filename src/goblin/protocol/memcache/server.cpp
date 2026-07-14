#include "goblin/protocol/memcache/server.hpp"

#include "goblin/core/stats.hpp"
#include "goblin/crypto/sha256.hpp"
#include "goblin/http/http_loop.hpp"
#include "goblin/http/https_loop.hpp"
#include "goblin/protocol/memcache/event_loop.hpp"
#include "goblin/protocol/memcache/protocol.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace goblin::memcache {
namespace {

bool send_raw(int fd, const char* p, std::size_t left) {
    while (left > 0) {
        const ssize_t w = ::send(fd, p, left, MSG_NOSIGNAL);
        if (w <= 0) return false;
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
    buf.append(tmp, static_cast<std::size_t>(r));
    return true;
}

void sleep_interruptibly(const std::atomic<bool>& shutdown, unsigned milliseconds) {
    for (unsigned slept = 0;
         slept < milliseconds && !shutdown.load(std::memory_order_relaxed); slept += 100)
        std::this_thread::sleep_for(
            std::chrono::milliseconds(std::min(100u, milliseconds - slept)));
}

// Stream an object's value in fixed I/O-pool chunks, head-first (ADR-0006/0016). read() copies the
// head out under the storage read-lock (ADR-0018); the zero-copy head returns once heads can be
// pinned against concurrent eviction.
bool stream_value(int fd, storage::TierManager& tm, core::Reactor& reactor,
                  core::IoBufferPool& iobufs, const crypto::Digest& digest,
                  const storage::ObjectMeta& meta) {
    if (meta.size == 0) return true;
    // Backpressure (ADR-0011): wait for a read buffer rather than dropping the GET. A blocking worker
    // serves one conn at a time, so io_buffers >= cores means this never spins.
    std::optional<MutBytes> chunk = iobufs.acquire();
    while (!chunk) {
        std::this_thread::yield();
        chunk = iobufs.acquire();
    }
    bool ok = true;
    Size pos = 0;
    while (pos < meta.size) {
        const Size want = std::min<Size>(chunk->size(), meta.size - pos);
        const auto n = tm.read(reactor, digest, pos, chunk->subspan(0, want),
                               /*record_access=*/false);
        if (!n || *n != want) { ok = false; break; }
        if (!send_all(fd, chunk->subspan(0, want))) { ok = false; break; }
        pos += want;
    }
    iobufs.release(*chunk);
    return ok;
}

void handle_conn(int fd, storage::TierManager& tm, storage::Index& index, core::Reactor& reactor,
                 core::IoBufferPool& iobufs) {
    std::string buf;
    while (true) {
        std::size_t eol = buf.find("\r\n");
        while (eol == std::string::npos) {
            if (!recv_more(fd, buf)) return;
            eol = buf.find("\r\n");
        }
        const auto cmd = parse_command(std::string_view(buf.data(), eol));
        if (!cmd) {
            if (!send_all(fd, kError)) return;
            buf.erase(0, eol + 2);
            continue;
        }

        if (cmd->is_storage()) {
            const Verb verb = cmd->verb;
            const std::uint32_t flags = cmd->flags;
            const std::uint32_t exptime = cmd->exptime;
            const std::uint64_t nbytes = cmd->bytes;
            const std::uint64_t cmd_cas = cmd->cas;
            const bool noreply = cmd->noreply;
            const auto digest = crypto::hash_key(cmd->key); // key still valid (buf intact here)
            buf.erase(0, eol + 2);                          // drop command line; data block follows

            // Admission, before opening files or writing anything. An expired item counts as absent
            // (lazy TTL, ADR-0007); cas must exist + match the current CAS.
            const auto exist = index.lookup(digest);
            const bool present = exist && !storage::is_expired(*exist, storage::now_unix());
            bool admit = true;
            std::string_view reject_reply = kNotStored;
            std::uint64_t cas_check = 0;
            if (verb == Verb::add && present) admit = false;
            else if (verb == Verb::replace && !present) admit = false;
            else if (verb == Verb::cas) {
                if (!present) { admit = false; reject_reply = kNotFound; }
                else if (exist->etag != cmd_cas) { admit = false; reject_reply = kExists; }
                else cas_check = cmd_cas;
            }
            std::optional<storage::TierManager::StoreHandle> handle;
            if (admit) {
                // Blocking-mode backpressure (ADR-0011/0016): if the write-staging pool is exhausted,
                // retry rather than fail. A spinning worker holds no buffer, so a peer mid-SET will
                // commit and free one -> forward progress (size io_buffers >= cores to never spin).
                for (;;) {
                    auto h = tm.begin_store(digest, nbytes);
                    if (h) { handle.emplace(std::move(*h)); break; }
                    if (h.error().code != Errc::would_block) break; // real failure -> NOT_STORED
                    std::this_thread::yield();
                }
            }
            bool write_ok = true;

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
                        !st)
                        write_ok = false;
                }
                written += take;
                buf.erase(0, take);
            }
            while (buf.size() < 2) { // trailing CRLF
                if (!recv_more(fd, buf)) return;
            }
            const bool crlf_ok = buf[0] == '\r' && buf[1] == '\n';
            buf.erase(0, 2);

            std::string_view reply;
            if (!admit) reply = reject_reply; // NOT_STORED / NOT_FOUND / EXISTS
            else if (!crlf_ok) reply = kBadDataChunk;
            else if (!handle || !write_ok) reply = kNotStored;
            else if (auto st = handle->commit(flags, exptime_to_expiry(exptime, storage::now_unix()),
                                              cas_check);
                     !st)
                reply = (st.error().code == Errc::cas_mismatch) ? kExists : kNotStored;
            else reply = kStored;
            // (An uncommitted handle aborts on scope exit — the object stays unindexed/invisible.)
            if (!noreply && !send_all(fd, reply)) return;
            continue;
        }

        switch (cmd->verb) {
            case Verb::get:
            case Verb::gets: {
                const auto digest = crypto::hash_key(cmd->key);
                const auto meta = index.lookup(digest);
                if (!meta || storage::is_expired(*meta, storage::now_unix())) {
                    if (!send_all(fd, kEnd)) return; // miss (or expired)
                    break;
                }
                tm.touch(digest); // record the hit for eviction (no-op if head not cached)
                if (!send_all(fd, cmd->verb == Verb::gets
                                      ? value_header_cas(cmd->key, meta->flags, meta->size, meta->etag)
                                      : value_header(cmd->key, meta->flags, meta->size)))
                    return;
                if (!stream_value(fd, tm, reactor, iobufs, digest, *meta)) return;
                if (!send_all(fd, std::string_view{"\r\n"})) return;
                if (!send_all(fd, kEnd)) return;
                break;
            }
            case Verb::del: {
                const bool erased = tm.remove(crypto::hash_key(cmd->key));
                if (!cmd->noreply && !send_all(fd, erased ? kDeleted : kNotFound)) return;
                break;
            }
            case Verb::version:
                if (!send_all(fd, kVersion)) return;
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

Result<int> make_listener(std::uint16_t port, bool reuseport = false) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return err(Errc::io_error, "socket");
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (reuseport) ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one); // async per-core
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        ::close(fd);
        return err(Errc::io_error, "bind");
    }
    if (::listen(fd, 128) < 0) {
        ::close(fd);
        return err(Errc::io_error, "listen");
    }
    return fd;
}

// One worker per core (ADR-0001/0018): its own io_uring reactor + I/O-buffer pool, all accepting
// from one shared listen socket so the kernel hands each new connection to a free worker (no
// hash-collision starvation under the blocking one-connection-per-worker model). The index + tier
// manager are shared and thread-safe. (SO_REUSEPORT per-core listeners return with the io_uring
// multishot-accept async loop, where a worker multiplexes many connections.)
void worker_loop(int lfd, const ServerConfig& cfg, storage::TierManager& tm, storage::Index& index,
                 const std::atomic<bool>& shutdown, unsigned id) {
    auto reactor = core::Reactor::create();
    if (!reactor) { std::println(stderr, "worker {}: {}", id, reactor.error().detail); return; }
    auto iobufs =
        core::IoBufferPool::create(cfg.io_chunk_bytes, cfg.io_buffers, cfg.memory.lock_memory,
                                   cfg.memory.use_hugepages, cfg.memory.block_bytes);
    if (!iobufs) { std::println(stderr, "worker {}: {}", id, iobufs.error().detail); return; }

    while (!shutdown.load(std::memory_order_relaxed)) {
        pollfd pfd{lfd, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, 200); // wake every 200 ms to observe shutdown (blocking path)
        if (pr <= 0) continue;
        const int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        const int one = 1;
        ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); // no Nagle (small-reply latency)
        if (cfg.io_timeout_ms) { // a slow client errors out instead of blocking this worker forever
            timeval tv{};
            tv.tv_sec = cfg.io_timeout_ms / 1000;
            tv.tv_usec = (cfg.io_timeout_ms % 1000) * 1000;
            ::setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            ::setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        }
        handle_conn(cfd, tm, index, *reactor, *iobufs);
        ::close(cfd);
    }
}

// Async worker (ADR-0002): its own io_uring ring + I/O pool + SO_REUSEPORT listener; the EventLoop
// multiplexes many connections on the one ring (accept / recv+parse / GET stream / SET ingest / send).
void async_worker(const ServerConfig& cfg, storage::TierManager& tm, storage::Index& index,
                  core::StatsRegistry& reg, const std::atomic<bool>& shutdown, unsigned id) {
    auto reactor = core::Reactor::create();
    if (!reactor) { std::println(stderr, "worker {}: {}", id, reactor.error().detail); return; }
    auto iobufs =
        core::IoBufferPool::create(cfg.io_chunk_bytes, cfg.io_buffers, cfg.memory.lock_memory,
                                   cfg.memory.use_hugepages, cfg.memory.block_bytes);
    if (!iobufs) { std::println(stderr, "worker {}: {}", id, iobufs.error().detail); return; }
    auto lfd = make_listener(cfg.memcache_port, /*reuseport=*/true);
    if (!lfd) { std::println(stderr, "worker {}: {}", id, lfd.error().detail); return; }
    EventLoop loop(*reactor, *lfd, tm, index, *iobufs, cfg.io_timeout_ms, &reg);
    loop.set_read_ahead(cfg.read_ahead);
    loop.set_shutdown(&shutdown, cfg.shutdown_grace_ms);
    loop.run();
    ::close(*lfd);
}

// HTTP object server (ADR-0005/0015): its own io_uring ring + read pool + SO_REUSEPORT listener on
// the HTTP port, so slow HTTP downloads draw from a separate buffer budget and can't starve memcache.
void http_worker(const ServerConfig& cfg, storage::TierManager& tm, storage::Index& index,
                 core::StatsRegistry& reg, const std::atomic<bool>& shutdown, unsigned id) {
    auto reactor = core::Reactor::create();
    if (!reactor) { std::println(stderr, "http worker {}: {}", id, reactor.error().detail); return; }
    auto iobufs =
        core::IoBufferPool::create(cfg.io_chunk_bytes, cfg.io_buffers, cfg.memory.lock_memory,
                                   cfg.memory.use_hugepages, cfg.memory.block_bytes);
    if (!iobufs) { std::println(stderr, "http worker {}: {}", id, iobufs.error().detail); return; }
    auto lfd = make_listener(cfg.http_port, /*reuseport=*/true);
    if (!lfd) { std::println(stderr, "http worker {}: {}", id, lfd.error().detail); return; }
    http::KeyOptions keyopt;
    keyopt.mode = cfg.http_vhost ? http::KeyMode::vhost : http::KeyMode::path;
    keyopt.keep_query = cfg.key_on_query;
    keyopt.strip_leading_slash = cfg.key_strip_slash;
    keyopt.index_name = cfg.http_index; // HTTP-only directory index (memcache + --source unaffected)
    http::HttpLoop loop(*reactor, *lfd, tm, index, *iobufs, keyopt, cfg.io_timeout_ms, &reg);
    loop.set_read_ahead(cfg.read_ahead);
    loop.set_shutdown(&shutdown, cfg.shutdown_grace_ms);
    loop.run();
    ::close(*lfd);
}

#if GOBLIN_HAVE_TLS
// HTTPS object server: own ring + read pool + SO_REUSEPORT listener on the HTTPS port, sharing one
// tls::Context (per-host certs, SNI selection) across all workers. The handshake runs on the loop;
// after it, kTLS makes the data path the ordinary HttpLoop flow (ADR-0005/0011).
void https_worker(const ServerConfig& cfg, storage::TierManager& tm, storage::Index& index,
                  tls::Context& ctx, core::StatsRegistry& reg, const std::atomic<bool>& shutdown,
                  unsigned id) {
    auto reactor = core::Reactor::create();
    if (!reactor) { std::println(stderr, "https worker {}: {}", id, reactor.error().detail); return; }
    auto iobufs =
        core::IoBufferPool::create(cfg.io_chunk_bytes, cfg.io_buffers, cfg.memory.lock_memory,
                                   cfg.memory.use_hugepages, cfg.memory.block_bytes);
    if (!iobufs) { std::println(stderr, "https worker {}: {}", id, iobufs.error().detail); return; }
    auto lfd = make_listener(cfg.https_port, /*reuseport=*/true);
    if (!lfd) { std::println(stderr, "https worker {}: {}", id, lfd.error().detail); return; }
    http::KeyOptions keyopt;
    keyopt.mode = cfg.http_vhost ? http::KeyMode::vhost : http::KeyMode::path;
    keyopt.keep_query = cfg.key_on_query;
    keyopt.strip_leading_slash = cfg.key_strip_slash;
    keyopt.index_name = cfg.http_index; // HTTP-only directory index (memcache + --source unaffected)
    http::HttpsLoop loop(*reactor, *lfd, tm, index, *iobufs, keyopt, ctx, cfg.io_timeout_ms, &reg);
    loop.set_read_ahead(cfg.read_ahead);
    loop.set_shutdown(&shutdown, cfg.shutdown_grace_ms);
    loop.run();
    ::close(*lfd);
}
#endif

} // namespace

Status serve(const ServerConfig& cfg, storage::TierManager& tm, storage::Index& index,
             const std::atomic<bool>& shutdown) {
    // main() binds itself before calling serve(), so every thread created below inherits the
    // selected NUMA node's CPU mask. Direct library callers may leave numa_cpus unresolved.
    unsigned n = cfg.cores ? cfg.cores : static_cast<unsigned>(cfg.numa_cpus.size());
    if (n == 0) n = std::thread::hardware_concurrency();
    if (n == 0) n = 1;
    std::vector<std::thread> workers;
    int blocking_lfd = -1; // shared listener for blocking-mode memcache; closed after join
    // One registry shared by every async loop (memcache + HTTP + HTTPS). Each loop registers its own
    // per-worker Stats slot; the memcache `stats` command aggregates them. Outlives the workers.
    core::StatsRegistry stats_reg;

    // memcache: async io_uring loops, or the blocking thread-per-core fallback (--net blocking).
    if (cfg.enable_memcache) {
        if (cfg.net == NetMode::async) {
            for (unsigned i = 0; i < n; ++i)
                workers.emplace_back([&cfg, &tm, &index, &stats_reg, &shutdown, i] {
                    async_worker(cfg, tm, index, stats_reg, shutdown, i);
                });
        } else {
            auto lfd = make_listener(cfg.memcache_port); // one shared listener, kernel load-balances
            if (!lfd) return std::unexpected(lfd.error());
            blocking_lfd = *lfd;
            for (unsigned i = 0; i < n; ++i)
                workers.emplace_back([&cfg, &tm, &index, &shutdown, i, fd = blocking_lfd] {
                    worker_loop(fd, cfg, tm, index, shutdown, i);
                });
        }
    }
    // HTTP: always async, on its own loops + read pool (per-protocol isolation, ADR-0011).
    if (cfg.enable_http) {
        for (unsigned i = 0; i < n; ++i)
            workers.emplace_back([&cfg, &tm, &index, &stats_reg, &shutdown, i] {
                http_worker(cfg, tm, index, stats_reg, shutdown, i);
            });
    }

    // HTTPS: build one shared per-host cert context (SNI) and spawn TLS loops on the HTTPS port.
#if GOBLIN_HAVE_TLS
    std::optional<tls::Context> https_ctx; // outlives the workers; freed after the join below
    if (cfg.enable_https) {
        std::vector<tls::Context::CertKey> certs;
        for (std::size_t i = 0; i < cfg.tls_cert_paths.size(); ++i)
            certs.push_back({cfg.tls_cert_paths[i], cfg.tls_key_paths[i]});
        auto c = tls::Context::create(certs);
        if (!c) return std::unexpected(c.error());
        https_ctx.emplace(std::move(*c));
        tls::Context& ctx = *https_ctx;
        for (unsigned i = 0; i < n; ++i)
            workers.emplace_back([&cfg, &tm, &index, &ctx, &stats_reg, &shutdown, i] {
                https_worker(cfg, tm, index, ctx, stats_reg, shutdown, i);
            });
    }
#else
    if (cfg.enable_https) return err(Errc::unsupported, "built without OpenSSL — HTTPS unavailable");
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
    if (cfg.memory.numa_regions.size() > 1)
        workers.emplace_back([&tm, &shutdown] {
            while (!shutdown.load(std::memory_order_relaxed)) {
                if (tm.promote_hot_remote_block()) continue;
                sleep_interruptibly(shutdown, 1'000);
            }
        });

    for (auto& t : workers) t.join();
    if (blocking_lfd >= 0) ::close(blocking_lfd);
    return {};
}

} // namespace goblin::memcache
