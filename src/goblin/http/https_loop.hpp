// HTTPS serving (ADR-0005/0011): HTTP/1.1 over TLS 1.3, on the shared streaming loop. Subclasses
// HttpLoop, so once the handshake completes the request/response path is the ordinary HTTP flow. The
// data path uses kTLS: replies stream via io_uring send on the kTLS socket (kernel encrypts, no
// userspace copy); requests are decrypted with SSL_read driven by readiness polls (small, the
// handshake path reused). Strict per-tenant isolation: SNI selects the cert (tls::Context) and the
// Host==SNI check lives in HttpLoop::process. Compiled only with OpenSSL (GOBLIN_HAVE_TLS).
#pragma once

#include "goblin/http/http_loop.hpp"
#include "goblin/tls/context.hpp"

#if GOBLIN_HAVE_TLS
#include <unordered_map>

namespace goblin::http {

class HttpsLoop : public HttpLoop {
public:
    HttpsLoop(core::Reactor& reactor, int listen_fd, storage::TierManager& tm, storage::Index& index,
              core::IoBufferPool& iobufs, KeyOptions keyopt, tls::Context& ctx,
              unsigned io_timeout_ms = 0, core::StatsRegistry* reg = nullptr,
              MirrorService* mirror = nullptr)
        : HttpLoop(reactor, listen_fd, tm, index, iobufs, keyopt, io_timeout_ms, reg, mirror,
                   core::StatsDomain::https),
          ctx_(ctx) {}
    HttpsLoop(core::Reactor& reactor, net::ConnectionInbox& inbox,
              storage::TierManager& tm, storage::Index& index,
              core::IoBufferPool& iobufs, KeyOptions keyopt, tls::Context& ctx,
              unsigned io_timeout_ms = 0, core::StatsRegistry* reg = nullptr,
              MirrorService* mirror = nullptr)
        : HttpLoop(reactor, inbox, tm, index, iobufs, keyopt, io_timeout_ms, reg, mirror,
                   core::StatsDomain::https),
          ctx_(ctx) {}

protected:
    void on_connection(Conn*) override; // start the TLS handshake instead of an immediate recv
    void on_poll(Conn*, int revents) override; // a readiness poll fired -> resume handshake or SSL_read
    void on_destroy(Conn*) override;    // free this conn's SSL
    void start_recv(Conn*) override;    // post-handshake read = poll-driven SSL_read

private:
    void drive_handshake(Conn*);     // SSL_accept until done / want-read/write / failure
    void ssl_read_step(Conn*);       // one SSL_read -> append + process, or poll for readiness
    void arm_poll(Conn*, unsigned events); // submit a one-shot readiness poll (counts as in-flight)

    struct TlsState {
        SSL* ssl = nullptr;
        bool handshaking = true;
    };
    tls::Context& ctx_;
    std::unordered_map<Conn*, TlsState> tls_; // per-conn SSL, keyed by Conn (freed in on_destroy)
};

} // namespace goblin::http
#endif
