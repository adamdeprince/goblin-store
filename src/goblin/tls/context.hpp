// TLS server context (ADR-0005): one TLS 1.3 SSL_CTX per certificate, with SNI-based selection so a
// single HTTPS listener serves many domains (per-host certs, no wildcard). Each cert registers under
// its DNS SAN names (and CN); during the handshake the SNI callback swaps to the matching cert, with
// the first cert as the default/fallback. kTLS is enabled, so the post-handshake data path runs
// through the kernel (ADR-0011). Compiled only with OpenSSL (GOBLIN_HAVE_TLS).
#pragma once

#include "goblin/common/error.hpp"

#include <string>
#include <vector>

#ifndef GOBLIN_HAVE_TLS
#define GOBLIN_HAVE_TLS 0
#endif

#if GOBLIN_HAVE_TLS
#include <openssl/ssl.h>

#include <memory>
#include <unordered_map>
#endif

namespace goblin::tls {

constexpr bool available() noexcept { return GOBLIN_HAVE_TLS; }

#if GOBLIN_HAVE_TLS
class Context {
public:
    struct CertKey {
        std::string cert_path; // PEM certificate chain
        std::string key_path;  // PEM private key
    };

    // Build from one-or-more cert/key pairs. The first is the default (used to start every handshake
    // and for an unrecognized SNI); each cert's DNS SAN names (and CN) route their hostname to it.
    // TLS 1.3 only, kTLS enabled. Move-only.
    static Result<Context> create(const std::vector<CertKey>& certs);

    // Start a handshake here: SSL_new(default_ctx()); the internal SNI callback swaps the per-host cert.
    SSL_CTX* default_ctx() const noexcept { return default_; }

private:
    struct Free {
        void operator()(SSL_CTX* c) const noexcept { SSL_CTX_free(c); }
    };
    Context() : by_host_(std::make_unique<std::unordered_map<std::string, SSL_CTX*>>()) {}

    std::vector<std::unique_ptr<SSL_CTX, Free>> ctxs_; // owns every per-cert ctx
    // host -> ctx; heap-allocated so its address is stable for the SNI callback arg across moves.
    std::unique_ptr<std::unordered_map<std::string, SSL_CTX*>> by_host_;
    SSL_CTX* default_ = nullptr;
};
#endif

} // namespace goblin::tls
