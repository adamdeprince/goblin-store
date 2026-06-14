#include "goblin/protocol/memcache/server.hpp"

#include "goblin/crypto/sha256.hpp"
#include "goblin/http/http_loop.hpp"
#include "goblin/http/https_loop.hpp"
#include "goblin/protocol/memcache/event_loop.hpp"
#include "goblin/protocol/memcache/protocol.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
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
        const auto n = tm.read(reactor, digest, pos, chunk->subspan(0, want));
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
            const std::uint64_t nbytes = cmd->bytes;
            const bool noreply = cmd->noreply;
            const auto digest = crypto::hash_key(cmd->key); // key still valid (buf intact here)
            buf.erase(0, eol + 2);                          // drop command line; data block follows

            // ADD/REPLACE admission, before opening files or writing anything.
            const bool admit = !((verb == Verb::add && index.contains(digest)) ||
                                 (verb == Verb::replace && !index.contains(digest)));
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
            if (!admit) reply = kNotStored;
            else if (!crlf_ok) reply = kBadDataChunk;
            else if (!handle || !write_ok) reply = kNotStored;
            else if (auto st = handle->commit(flags); !st) reply = kNotStored;
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
                if (!meta) {
                    if (!send_all(fd, kEnd)) return; // miss
                    break;
                }
                tm.touch(digest); // record the hit for eviction (no-op if head not cached)
                if (!send_all(fd, value_header(cmd->key, meta->flags, meta->size))) return;
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
                 unsigned id) {
    auto reactor = core::Reactor::create();
    if (!reactor) { std::println(stderr, "worker {}: {}", id, reactor.error().detail); return; }
    auto iobufs =
        core::IoBufferPool::create(cfg.io_chunk_bytes, cfg.io_buffers, cfg.memory.lock_memory);
    if (!iobufs) { std::println(stderr, "worker {}: {}", id, iobufs.error().detail); return; }

    while (true) {
        const int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }
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
                  unsigned id) {
    auto reactor = core::Reactor::create();
    if (!reactor) { std::println(stderr, "worker {}: {}", id, reactor.error().detail); return; }
    auto iobufs =
        core::IoBufferPool::create(cfg.io_chunk_bytes, cfg.io_buffers, cfg.memory.lock_memory);
    if (!iobufs) { std::println(stderr, "worker {}: {}", id, iobufs.error().detail); return; }
    auto lfd = make_listener(cfg.memcache_port, /*reuseport=*/true);
    if (!lfd) { std::println(stderr, "worker {}: {}", id, lfd.error().detail); return; }
    EventLoop loop(*reactor, *lfd, tm, index, *iobufs, cfg.io_timeout_ms);
    loop.run();
    ::close(*lfd);
}

// HTTP object server (ADR-0005/0015): its own io_uring ring + read pool + SO_REUSEPORT listener on
// the HTTP port, so slow HTTP downloads draw from a separate buffer budget and can't starve memcache.
void http_worker(const ServerConfig& cfg, storage::TierManager& tm, storage::Index& index,
                 unsigned id) {
    auto reactor = core::Reactor::create();
    if (!reactor) { std::println(stderr, "http worker {}: {}", id, reactor.error().detail); return; }
    auto iobufs =
        core::IoBufferPool::create(cfg.io_chunk_bytes, cfg.io_buffers, cfg.memory.lock_memory);
    if (!iobufs) { std::println(stderr, "http worker {}: {}", id, iobufs.error().detail); return; }
    auto lfd = make_listener(cfg.http_port, /*reuseport=*/true);
    if (!lfd) { std::println(stderr, "http worker {}: {}", id, lfd.error().detail); return; }
    http::KeyOptions keyopt;
    keyopt.mode = cfg.http_vhost ? http::KeyMode::vhost : http::KeyMode::path;
    keyopt.keep_query = cfg.key_on_query;
    http::HttpLoop loop(*reactor, *lfd, tm, index, *iobufs, keyopt, cfg.io_timeout_ms);
    loop.run();
    ::close(*lfd);
}

#if GOBLIN_HAVE_TLS
// HTTPS object server: own ring + read pool + SO_REUSEPORT listener on the HTTPS port, sharing one
// tls::Context (per-host certs, SNI selection) across all workers. The handshake runs on the loop;
// after it, kTLS makes the data path the ordinary HttpLoop flow (ADR-0005/0011).
void https_worker(const ServerConfig& cfg, storage::TierManager& tm, storage::Index& index,
                  tls::Context& ctx, unsigned id) {
    auto reactor = core::Reactor::create();
    if (!reactor) { std::println(stderr, "https worker {}: {}", id, reactor.error().detail); return; }
    auto iobufs =
        core::IoBufferPool::create(cfg.io_chunk_bytes, cfg.io_buffers, cfg.memory.lock_memory);
    if (!iobufs) { std::println(stderr, "https worker {}: {}", id, iobufs.error().detail); return; }
    auto lfd = make_listener(cfg.https_port, /*reuseport=*/true);
    if (!lfd) { std::println(stderr, "https worker {}: {}", id, lfd.error().detail); return; }
    http::KeyOptions keyopt;
    keyopt.mode = cfg.http_vhost ? http::KeyMode::vhost : http::KeyMode::path;
    keyopt.keep_query = cfg.key_on_query;
    http::HttpsLoop loop(*reactor, *lfd, tm, index, *iobufs, keyopt, ctx, cfg.io_timeout_ms);
    loop.run();
    ::close(*lfd);
}
#endif

} // namespace

Status serve(const ServerConfig& cfg, storage::TierManager& tm, storage::Index& index) {
    unsigned n = cfg.cores ? cfg.cores : std::thread::hardware_concurrency();
    if (n == 0) n = 1;
    std::vector<std::thread> workers;
    int blocking_lfd = -1; // shared listener for blocking-mode memcache; closed after join

    // memcache: async io_uring loops, or the blocking thread-per-core fallback (--net blocking).
    if (cfg.enable_memcache) {
        if (cfg.net == NetMode::async) {
            for (unsigned i = 0; i < n; ++i)
                workers.emplace_back([&cfg, &tm, &index, i] { async_worker(cfg, tm, index, i); });
        } else {
            auto lfd = make_listener(cfg.memcache_port); // one shared listener, kernel load-balances
            if (!lfd) return std::unexpected(lfd.error());
            blocking_lfd = *lfd;
            for (unsigned i = 0; i < n; ++i)
                workers.emplace_back(
                    [&cfg, &tm, &index, i, fd = blocking_lfd] { worker_loop(fd, cfg, tm, index, i); });
        }
    }
    // HTTP: always async, on its own loops + read pool (per-protocol isolation, ADR-0011).
    if (cfg.enable_http) {
        for (unsigned i = 0; i < n; ++i)
            workers.emplace_back([&cfg, &tm, &index, i] { http_worker(cfg, tm, index, i); });
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
            workers.emplace_back([&cfg, &tm, &index, &ctx, i] { https_worker(cfg, tm, index, ctx, i); });
    }
#else
    if (cfg.enable_https) return err(Errc::unsupported, "built without OpenSSL — HTTPS unavailable");
#endif

    for (auto& t : workers) t.join();
    if (blocking_lfd >= 0) ::close(blocking_lfd);
    return {};
}

} // namespace goblin::memcache
