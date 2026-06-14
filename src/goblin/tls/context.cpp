#include "goblin/tls/context.hpp"

#if GOBLIN_HAVE_TLS

#include <openssl/x509v3.h>

namespace goblin::tls {
namespace {

char lc(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
std::string lower(std::string s) {
    for (char& c : s) c = lc(c);
    return s;
}

// The DNS SAN names plus the CN of a leaf certificate — the hostnames it is valid for.
std::vector<std::string> hostnames(X509* cert) {
    std::vector<std::string> out;
    if (auto* san = static_cast<GENERAL_NAMES*>(
            X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr))) {
        for (int i = 0, n = sk_GENERAL_NAME_num(san); i < n; ++i) {
            const GENERAL_NAME* g = sk_GENERAL_NAME_value(san, i);
            if (g->type == GEN_DNS) {
                const int len = ASN1_STRING_length(g->d.dNSName);
                const unsigned char* d = ASN1_STRING_get0_data(g->d.dNSName);
                if (d && len > 0) out.emplace_back(reinterpret_cast<const char*>(d), len);
            }
        }
        GENERAL_NAMES_free(san);
    }
    X509_NAME* subj = X509_get_subject_name(cert);
    const int idx = X509_NAME_get_index_by_NID(subj, NID_commonName, -1);
    if (idx >= 0) {
        const ASN1_STRING* s = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(subj, idx));
        const int len = ASN1_STRING_length(s);
        const unsigned char* d = ASN1_STRING_get0_data(s);
        if (d && len > 0) out.emplace_back(reinterpret_cast<const char*>(d), len);
    }
    return out;
}

// Strict per-tenant SNI: present the cert whose SAN/CN matches the SNI; refuse a missing or unknown
// SNI outright (a fatal unrecognized_name alert) rather than falling back to a default cert.
int sni_select(SSL* ssl, int* al, void* arg) {
    const auto* by_host = static_cast<const std::unordered_map<std::string, SSL_CTX*>*>(arg);
    if (const char* sni = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name)) {
        if (const auto it = by_host->find(lower(sni)); it != by_host->end()) {
            SSL_set_SSL_CTX(ssl, it->second);
            return SSL_TLSEXT_ERR_OK;
        }
    }
    if (al) *al = SSL_AD_UNRECOGNIZED_NAME;
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

} // namespace

Result<Context> Context::create(const std::vector<CertKey>& certs) {
    if (certs.empty()) return err(Errc::invalid_argument, "no TLS certificates configured");
    Context self;
    for (const auto& ck : certs) {
        SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
        if (!ctx) return err(Errc::unsupported, "SSL_CTX_new failed");
        std::unique_ptr<SSL_CTX, Free> owned(ctx); // free on any early return below

        SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION); // TLS 1.3 only — no renegotiation
        SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
        SSL_CTX_set_options(ctx, SSL_OP_ENABLE_KTLS); // kernel TLS for the data path (ADR-0011)
        if (SSL_CTX_use_certificate_chain_file(ctx, ck.cert_path.c_str()) <= 0)
            return err(Errc::invalid_argument, "load certificate: " + ck.cert_path);
        if (SSL_CTX_use_PrivateKey_file(ctx, ck.key_path.c_str(), SSL_FILETYPE_PEM) <= 0)
            return err(Errc::invalid_argument, "load private key: " + ck.key_path);
        if (SSL_CTX_check_private_key(ctx) <= 0)
            return err(Errc::invalid_argument, "key does not match certificate: " + ck.cert_path);

        X509* cert = SSL_CTX_get0_certificate(ctx);
        if (!cert) return err(Errc::invalid_argument, "no certificate in " + ck.cert_path);
        for (const auto& h : hostnames(cert)) (*self.by_host_)[lower(h)] = ctx;
        if (!self.default_) self.default_ = ctx; // first cert bootstraps every handshake
        self.ctxs_.push_back(std::move(owned));
    }
    // The handshake starts on default_; the callback then enforces strict per-tenant selection.
    SSL_CTX_set_tlsext_servername_callback(self.default_, sni_select);
    SSL_CTX_set_tlsext_servername_arg(self.default_, self.by_host_.get());
    return self;
}

} // namespace goblin::tls

#endif
