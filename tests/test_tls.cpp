#include "mini_test.hpp"

#include "goblin/tls/context.hpp"

#if GOBLIN_HAVE_TLS

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

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

// Write a throwaway P-256 self-signed cert + key to PEM files (CN=localhost, ~10y).
static bool write_self_signed(const std::string& cert, const std::string& key) {
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
                                   reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
        X509_set_issuer_name(x, nm);
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

TEST("tls: TLS 1.3 handshake over TCP, kTLS TX engages, application data round-trips") {
    const std::string base =
        (fs::temp_directory_path() / ("goblin-tls-" + std::to_string(::getpid()))).string();
    fs::create_directories(base);
    const std::string cert = base + "/cert.pem", key = base + "/key.pem";
    CHECK(write_self_signed(cert, key));

    auto ctx = tls::Context::create(cert, key);
    CHECK(ctx.has_value());
    if (!ctx) { fs::remove_all(base); return; }

    auto [lfd, port] = loopback_listener();
    const int cfd = dial(port);
    const int sfd = ::accept(lfd, nullptr, nullptr);
    CHECK(cfd >= 0 && sfd >= 0);
    if (cfd < 0 || sfd < 0) { fs::remove_all(base); return; }

    // Client drives its handshake + read in a thread; server runs in the test thread.
    std::atomic<bool> client_ok{false};
    std::string got;
    std::thread cli([&] {
        SSL_CTX* cctx = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_min_proto_version(cctx, TLS1_3_VERSION);
        SSL* s = SSL_new(cctx);
        SSL_set_fd(s, cfd);
        if (SSL_connect(s) == 1) {
            char buf[16] = {};
            const int n = SSL_read(s, buf, sizeof buf);
            if (n > 0) got.assign(buf, static_cast<std::size_t>(n));
            client_ok = true;
        }
        SSL_free(s);
        SSL_CTX_free(cctx);
    });

    SSL* ss = SSL_new(ctx->raw());
    SSL_set_fd(ss, sfd);
    const bool hs = SSL_accept(ss) == 1;
    bool ktls = false, wrote = false;
    if (hs) {
        ktls = BIO_get_ktls_send(SSL_get_wbio(ss)) != 0; // kernel TLS on the send path (ADR-0011)
        wrote = SSL_write(ss, "ping", 4) == 4;
    }
    cli.join();

    CHECK(hs);                                   // TLS 1.3 handshake completed
    CHECK(ktls);                                 // kTLS TX active -> data path is kernel/io_uring
    CHECK(wrote && client_ok.load() && got == "ping"); // encrypted application data round-trips

    SSL_free(ss);
    ::close(sfd);
    ::close(cfd);
    ::close(lfd);
    fs::remove_all(base);
}

#endif // GOBLIN_HAVE_TLS
