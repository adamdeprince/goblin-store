#include "goblin/http/https_loop.hpp"

#if GOBLIN_HAVE_TLS

#include <fcntl.h>
#include <poll.h>
#include <string>

namespace goblin::http {
namespace {
std::string lower(const char* s) {
    std::string r(s ? s : "");
    for (char& c : r)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return r;
}
} // namespace

void HttpsLoop::on_connection(Conn* c) {
    const int fl = ::fcntl(c->fd, F_GETFL, 0); // non-blocking so SSL_* yields WANT_READ/WANT_WRITE
    ::fcntl(c->fd, F_SETFL, fl | O_NONBLOCK);
    SSL* ssl = SSL_new(ctx_.default_ctx());
    if (!ssl) { close_conn(c); return; }
    SSL_set_fd(ssl, c->fd); // BIO_NOCLOSE: the loop owns the fd, SSL_free won't close it
    tls_.emplace(c, TlsState{ssl, true});
    drive_handshake(c);
}

void HttpsLoop::drive_handshake(Conn* c) {
    if (c->closing) return;
    SSL* ssl = tls_.at(c).ssl;
    const int r = SSL_accept(ssl);
    if (r == 1) { // handshake done -> kTLS TX live; capture SNI, then read the request
        if (const char* sni = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name)) c->sni = lower(sni);
        tls_.at(c).handshaking = false;
        start_recv(c);
        return;
    }
    switch (SSL_get_error(ssl, r)) {
        case SSL_ERROR_WANT_READ: arm_poll(c, POLLIN); break;
        case SSL_ERROR_WANT_WRITE: arm_poll(c, POLLOUT); break;
        default: close_conn(c); break; // handshake failed (incl. a strict-SNI fatal alert)
    }
}

void HttpsLoop::ssl_read_step(Conn* c) {
    if (c->closing) return;
    SSL* ssl = tls_.at(c).ssl;
    char buf[16 * 1024];
    const int r = SSL_read(ssl, buf, sizeof buf);
    if (r > 0) {
        c->in.append(buf, static_cast<std::size_t>(r));
        process(c); // HttpLoop::process; its tail re-enters start_recv() to drain buffered records
        return;
    }
    switch (SSL_get_error(ssl, r)) {
        case SSL_ERROR_WANT_READ: arm_poll(c, POLLIN); break;
        case SSL_ERROR_WANT_WRITE: arm_poll(c, POLLOUT); break;
        default: close_conn(c); break; // peer close-notify or error
    }
}

void HttpsLoop::start_recv(Conn* c) { ssl_read_step(c); } // RX = SSL_read; TX stays io_uring (kTLS)

void HttpsLoop::arm_poll(Conn* c, unsigned events) {
    if (c->closing) return;
    if (r_.submit_poll(c->fd, events, tag(c, kPoll)))
        ++c->inflight;
    else
        close_conn(c);
}

void HttpsLoop::on_poll(Conn* c, int /*revents*/) {
    if (c->closing) return;
    const auto it = tls_.find(c);
    if (it == tls_.end()) { close_conn(c); return; }
    if (it->second.handshaking)
        drive_handshake(c);
    else
        ssl_read_step(c);
}

void HttpsLoop::on_destroy(Conn* c) {
    if (const auto it = tls_.find(c); it != tls_.end()) {
        SSL_free(it->second.ssl); // BIO_NOCLOSE -> does not touch the (already-closed) fd
        tls_.erase(it);
    }
}

} // namespace goblin::http
#endif
