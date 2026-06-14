#include "mini_test.hpp"

#include "goblin/tls/context.hpp"

#if GOBLIN_HAVE_TLS

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace goblin;

// Throwaway P-256 self-signed cert + key (CN + DNS SAN = host, ~10y) to PEM files.
static bool write_self_signed(const std::string& cert, const std::string& key, const char* host) {
    EVP_PKEY* pkey = EVP_EC_gen("prime256v1");
    if (!pkey) return false;
    X509* x = X509_new();
    bool ok = x != nullptr;
    if (ok) {
        ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
        X509_gmtime_adj(X509_getm_notBefore(x), 0);
        X509_gmtime_adj(X509_getm_notAfter(x), 60L * 60 * 24 * 3650);
        X509_set_pubkey(x, pkey);
        X509_NAME* nm = X509_get_subject_name(x);
        X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>(host), -1, -1, 0);
        X509_set_issuer_name(x, nm);
        const std::string san = std::string("DNS:") + host;
        if (X509_EXTENSION* ext =
                X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name, san.c_str())) {
            X509_add_ext(x, ext, -1);
            X509_EXTENSION_free(ext);
        }
        ok = X509_sign(x, pkey, EVP_sha256()) > 0;
    }
    if (ok) {
        FILE* kf = std::fopen(key.c_str(), "wb");
        ok = kf && PEM_write_PrivateKey(kf, pkey, nullptr, nullptr, 0, nullptr, nullptr) > 0;
        if (kf) std::fclose(kf);
        FILE* cf = std::fopen(cert.c_str(), "wb");
        ok = ok && cf && PEM_write_X509(cf, x) > 0;
        if (cf) std::fclose(cf);
    }
    X509_free(x);
    EVP_PKEY_free(pkey);
    return ok;
}

static std::string cert_cn(X509* x) {
    X509_NAME* n = X509_get_subject_name(x);
    const int idx = X509_NAME_get_index_by_NID(n, NID_commonName, -1);
    if (idx < 0) return {};
    const ASN1_STRING* s = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(n, idx));
    return std::string(reinterpret_cast<const char*>(ASN1_STRING_get0_data(s)), ASN1_STRING_length(s));
}

static std::pair<int, std::uint16_t> loopback_listener() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
    socklen_t al = sizeof a;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &al);
    ::listen(fd, 8);
    return {fd, ntohs(a.sin_port)};
}

static int dial(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) { ::close(fd); return -1; }
    return fd;
}

// One handshake attempt against `ctx` with the given SNI (nullptr = none). Reports whether the client
// handshake succeeded, the CN of the cert it received, and whether the server got kTLS TX.
struct Attempt {
    bool client_ok = false;
    std::string cert_cn;
    bool server_ktls = false;
};
static Attempt handshake(tls::Context& sctx, const char* sni) {
    Attempt r;
    auto [lfd, port] = loopback_listener();
    const int cfd = dial(port);
    const int sfd = ::accept(lfd, nullptr, nullptr);

    std::thread srv([&] {
        SSL* ss = SSL_new(sctx.default_ctx());
        SSL_set_fd(ss, sfd);
        if (SSL_accept(ss) == 1) {
            r.server_ktls = BIO_get_ktls_send(SSL_get_wbio(ss)) != 0;
            SSL_write(ss, "ok", 2);
        }
        SSL_free(ss);
    });

    SSL_CTX* cctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_min_proto_version(cctx, TLS1_3_VERSION);
    SSL* cs = SSL_new(cctx);
    if (sni) SSL_set_tlsext_host_name(cs, sni);
    SSL_set_fd(cs, cfd);
    if (SSL_connect(cs) == 1) {
        r.client_ok = true;
        if (X509* peer = SSL_get1_peer_certificate(cs)) {
            r.cert_cn = cert_cn(peer);
            X509_free(peer);
        }
    }
    srv.join();
    SSL_free(cs);
    SSL_CTX_free(cctx);
    ::close(cfd);
    ::close(sfd);
    ::close(lfd);
    return r;
}

TEST("tls: strict SNI picks the per-host cert, rejects unknown/missing SNI, kTLS engages") {
    const std::string base =
        (fs::temp_directory_path() / ("goblin-tls-" + std::to_string(::getpid()))).string();
    fs::create_directories(base);
    const std::string ac = base + "/a.crt", ak = base + "/a.key";
    const std::string bc = base + "/b.crt", bk = base + "/b.key";
    CHECK(write_self_signed(ac, ak, "a.test"));
    CHECK(write_self_signed(bc, bk, "b.test"));

    auto ctx = tls::Context::create({{ac, ak}, {bc, bk}});
    CHECK(ctx.has_value());
    if (!ctx) { fs::remove_all(base); return; }

    const Attempt a = handshake(*ctx, "a.test");
    const Attempt b = handshake(*ctx, "b.test");
    const Attempt unknown = handshake(*ctx, "c.test"); // no cert -> rejected
    const Attempt none = handshake(*ctx, nullptr);     // no SNI -> rejected

    CHECK(a.client_ok && a.cert_cn == "a.test"); // SNI a.test -> cert a
    CHECK(b.client_ok && b.cert_cn == "b.test"); // SNI b.test -> cert b (different tenant)
    CHECK(!unknown.client_ok);                   // strict: unknown tenant refused
    CHECK(!none.client_ok);                      // strict: no SNI refused
    CHECK(a.server_ktls);                        // kTLS TX active on the data path

    fs::remove_all(base);
}

#endif // GOBLIN_HAVE_TLS
