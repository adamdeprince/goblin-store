// TLS server context (ADR-0005): a TLS 1.3 SSL_CTX from a PEM certificate chain + private key, with
// kTLS enabled so the post-handshake data path runs through the kernel (ADR-0011) — io_uring send/recv
// on the socket, the kernel does the record crypto. Compiled only with OpenSSL (GOBLIN_HAVE_TLS);
// HTTPS is unavailable otherwise.
#pragma once

#include "goblin/common/error.hpp"

#include <string>

#ifndef GOBLIN_HAVE_TLS
#define GOBLIN_HAVE_TLS 0
#endif

#if GOBLIN_HAVE_TLS
#include <openssl/ssl.h>

#include <memory>
#endif

namespace goblin::tls {

constexpr bool available() noexcept { return GOBLIN_HAVE_TLS; }

#if GOBLIN_HAVE_TLS
class Context {
public:
    // Build a TLS 1.3 server context from a PEM cert chain + private key; kTLS enabled. Move-only.
    static Result<Context> create(const std::string& cert_path, const std::string& key_path);
    SSL_CTX* raw() const noexcept { return ctx_.get(); }

private:
    struct Free {
        void operator()(SSL_CTX* c) const noexcept { SSL_CTX_free(c); }
    };
    explicit Context(SSL_CTX* c) : ctx_(c) {}
    std::unique_ptr<SSL_CTX, Free> ctx_;
};
#endif

} // namespace goblin::tls
