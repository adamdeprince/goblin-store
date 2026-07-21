#pragma once

#include "goblin/protocol/memcache/event_loop.hpp"
#include "goblin/tls/context.hpp"

#if GOBLIN_HAVE_TLS
#include <unordered_map>

namespace goblin::memcache {

// Memcache text/meta over TLS 1.3. OpenSSL drives handshake and receive decryption; kTLS keeps the
// shared head-first io_uring transmit path unchanged after the handshake.
class TlsEventLoop : public EventLoop {
public:
    TlsEventLoop(core::Reactor& reactor, int listener_fd, storage::TierManager& tm,
                 storage::Index& index, core::IoBufferPool& iobufs, tls::Context& context,
                 unsigned io_timeout_ms = 0, core::StatsRegistry* registry = nullptr,
                 WriteMode write_mode = WriteMode::evict)
        : EventLoop(reactor, listener_fd, tm, index, iobufs, io_timeout_ms, registry, write_mode),
          context_(context) {}
    TlsEventLoop(core::Reactor& reactor, net::ConnectionInbox& inbox,
                 storage::TierManager& tm, storage::Index& index,
                 core::IoBufferPool& iobufs, tls::Context& context,
                 unsigned io_timeout_ms = 0, core::StatsRegistry* registry = nullptr,
                 WriteMode write_mode = WriteMode::evict)
        : EventLoop(reactor, inbox, tm, index, iobufs, io_timeout_ms, registry, write_mode),
          context_(context) {}

protected:
    void on_connection(Conn*) override;
    void on_poll(Conn*, int revents) override;
    void on_destroy(Conn*) override;
    void start_recv(Conn*) override;

private:
    struct TlsState { SSL* ssl = nullptr; bool handshaking = true; };
    void drive_handshake(Conn*);
    void ssl_read_step(Conn*);
    void arm_poll(Conn*, unsigned events);

    tls::Context& context_;
    std::unordered_map<Conn*, TlsState> tls_;
};

} // namespace goblin::memcache
#endif
