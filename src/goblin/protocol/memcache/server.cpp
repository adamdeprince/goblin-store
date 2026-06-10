#include "goblin/protocol/memcache/server.hpp"

#include "goblin/crypto/sha256.hpp"
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
    const auto chunk = iobufs.acquire();
    if (!chunk) return false; // no free I/O buffer
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
                if (auto h = tm.begin_store(digest, nbytes)) handle.emplace(std::move(*h));
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
    EventLoop loop(*reactor, *lfd, tm, index, *iobufs);
    loop.run();
    ::close(*lfd);
}

} // namespace

Status serve(const ServerConfig& cfg, storage::TierManager& tm, storage::Index& index) {
    unsigned n = cfg.cores ? cfg.cores : std::thread::hardware_concurrency();
    if (n == 0) n = 1;
    std::vector<std::thread> workers;
    workers.reserve(n);

    if (cfg.net == NetMode::async) {
        // Each worker: its own io_uring ring + I/O pool + SO_REUSEPORT listener (ADR-0002).
        for (unsigned i = 0; i < n; ++i)
            workers.emplace_back([&cfg, &tm, &index, i] { async_worker(cfg, tm, index, i); });
        for (auto& t : workers) t.join();
        return {};
    }

    // Blocking thread-per-core: one shared listener, the kernel hands each conn to a free worker.
    auto lfd = make_listener(cfg.memcache_port);
    if (!lfd) return std::unexpected(lfd.error());
    const int fd = *lfd;
    for (unsigned i = 0; i < n; ++i)
        workers.emplace_back([fd, &cfg, &tm, &index, i] { worker_loop(fd, cfg, tm, index, i); });
    for (auto& t : workers) t.join();
    ::close(fd);
    return {};
}

} // namespace goblin::memcache
