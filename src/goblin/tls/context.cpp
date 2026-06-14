#include "goblin/tls/context.hpp"

#if GOBLIN_HAVE_TLS

namespace goblin::tls {

Result<Context> Context::create(const std::string& cert_path, const std::string& key_path) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) return err(Errc::unsupported, "SSL_CTX_new failed");
    std::unique_ptr<SSL_CTX, Free> guard(ctx); // free on any early return

    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION); // TLS 1.3 only — no renegotiation
    SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_options(ctx, SSL_OP_ENABLE_KTLS); // kernel TLS for the post-handshake data path

    if (SSL_CTX_use_certificate_chain_file(ctx, cert_path.c_str()) <= 0)
        return err(Errc::invalid_argument, "load certificate: " + cert_path);
    if (SSL_CTX_use_PrivateKey_file(ctx, key_path.c_str(), SSL_FILETYPE_PEM) <= 0)
        return err(Errc::invalid_argument, "load private key: " + key_path);
    if (SSL_CTX_check_private_key(ctx) <= 0)
        return err(Errc::invalid_argument, "private key does not match certificate");

    return Context(guard.release());
}

} // namespace goblin::tls

#endif
