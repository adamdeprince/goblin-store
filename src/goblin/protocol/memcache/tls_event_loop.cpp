#include "goblin/protocol/memcache/tls_event_loop.hpp"

#if GOBLIN_HAVE_TLS

#include <fcntl.h>
#include <openssl/bio.h>
#include <poll.h>

namespace goblin::memcache {

void TlsEventLoop::on_connection(Conn* connection) {
    const int flags = ::fcntl(connection->fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(connection->fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close_conn(connection);
        return;
    }
    SSL* ssl = SSL_new(context_.default_ctx());
    if (!ssl) { close_conn(connection); return; }
    SSL_set_fd(ssl, connection->fd);
    tls_.emplace(connection, TlsState{ssl, true});
    drive_handshake(connection);
}

void TlsEventLoop::drive_handshake(Conn* connection) {
    if (connection->closing) return;
    SSL* ssl = tls_.at(connection).ssl;
    const int result = SSL_accept(ssl);
    if (result == 1) {
        // StreamLoop sends headers, resident heads, and tails with raw io_uring send operations.
        // That is correct only once the kernel owns TLS record generation for this socket.
        if (!BIO_get_ktls_send(SSL_get_wbio(ssl))) { close_conn(connection); return; }
        tls_.at(connection).handshaking = false;
        start_recv(connection);
        return;
    }
    switch (SSL_get_error(ssl, result)) {
        case SSL_ERROR_WANT_READ: arm_poll(connection, POLLIN); break;
        case SSL_ERROR_WANT_WRITE: arm_poll(connection, POLLOUT); break;
        default: close_conn(connection); break;
    }
}

void TlsEventLoop::ssl_read_step(Conn* connection) {
    if (connection->closing) return;
    SSL* ssl = tls_.at(connection).ssl;
    char buffer[16 * 1024];
    const int result = SSL_read(ssl, buffer, sizeof buffer);
    if (result > 0) {
        stats_.bytes_received.fetch_add(static_cast<std::uint64_t>(result),
                                        std::memory_order_relaxed);
        if (connection->in_off) {
            connection->in.erase(0, connection->in_off);
            connection->in_off = 0;
        }
        connection->in.append(buffer, static_cast<std::size_t>(result));
        process(connection);
        return;
    }
    switch (SSL_get_error(ssl, result)) {
        case SSL_ERROR_WANT_READ: arm_poll(connection, POLLIN); break;
        case SSL_ERROR_WANT_WRITE: arm_poll(connection, POLLOUT); break;
        default: close_conn(connection); break;
    }
}

void TlsEventLoop::start_recv(Conn* connection) { ssl_read_step(connection); }

void TlsEventLoop::arm_poll(Conn* connection, unsigned events) {
    if (connection->closing) return;
    if (io_.submit_poll(connection->fd, events, tag(connection, kPoll)))
        ++connection->inflight;
    else
        close_conn(connection);
}

void TlsEventLoop::on_poll(Conn* connection, int) {
    if (connection->closing) return;
    const auto found = tls_.find(connection);
    if (found == tls_.end()) { close_conn(connection); return; }
    if (found->second.handshaking) drive_handshake(connection);
    else ssl_read_step(connection);
}

void TlsEventLoop::on_destroy(Conn* connection) {
    if (const auto found = tls_.find(connection); found != tls_.end()) {
        SSL_free(found->second.ssl);
        tls_.erase(found);
    }
    EventLoop::on_destroy(connection);
}

} // namespace goblin::memcache
#endif
