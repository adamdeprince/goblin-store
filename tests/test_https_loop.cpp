#include "mini_test.hpp"

#include "goblin/http/https_loop.hpp"

#if GOBLIN_HAVE_TLS

#include "goblin/core/buffer_pool.hpp"
#include "goblin/core/reactor.hpp"
#include "goblin/crypto/sha256.hpp"
#include "goblin/http/key_derivation.hpp"
#include "goblin/storage/index.hpp"
#include "goblin/storage/tier_manager.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace goblin;
using namespace goblin::core;
using goblin::crypto::hash_key;
using goblin::http::HttpsLoop;
using goblin::http::KeyMode;
using goblin::http::KeyOptions;
using goblin::storage::Index;
using goblin::storage::TierManager;

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
        if (X509_EXTENSION* e =
                X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name, san.c_str())) {
            X509_add_ext(x, e, -1);
            X509_EXTENSION_free(e);
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
    ::listen(fd, 16);
    return {fd, ntohs(a.sin_port)};
}

static int dial(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    timeval tv{8, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) { ::close(fd); return -1; }
    return fd;
}

struct Resp {
    bool connected = false;
    int status = 0;
    std::string body;
};

// One HTTPS GET: TLS connect with `sni`, then `GET path` with the given Host header; Connection: close.
static Resp https_get(std::uint16_t port, const char* sni, const std::string& path, const char* host) {
    Resp out;
    const int fd = dial(port);
    if (fd < 0) return out;
    SSL_CTX* cctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_min_proto_version(cctx, TLS1_3_VERSION);
    SSL* s = SSL_new(cctx);
    if (sni) SSL_set_tlsext_host_name(s, sni);
    SSL_set_fd(s, fd);
    // A blocking socket only yields WANT_READ/WANT_WRITE when a signal interrupts the syscall (EINTR);
    // a real client just resumes, so retry rather than treating it as failure.
    auto again = [&](int rc) {
        const int e = SSL_get_error(s, rc);
        return e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE;
    };
    int cr;
    while ((cr = SSL_connect(s)) != 1 && again(cr)) {}
    if (cr == 1) {
        out.connected = true;
        const std::string req =
            "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
        for (std::size_t off = 0; off < req.size();) {
            const int w = SSL_write(s, req.data() + off, static_cast<int>(req.size() - off));
            if (w > 0) off += static_cast<std::size_t>(w);
            else if (!again(w)) break;
        }
        std::string buf;
        char tmp[4096];
        for (;;) {
            const int n = SSL_read(s, tmp, sizeof tmp);
            if (n > 0) buf.append(tmp, static_cast<std::size_t>(n));
            else if (!again(n)) break;
        }
        if (const auto sp = buf.find(' '); sp != std::string::npos)
            out.status = std::atoi(buf.c_str() + sp + 1);
        if (const auto he = buf.find("\r\n\r\n"); he != std::string::npos) out.body = buf.substr(he + 4);
    }
    SSL_free(s);
    SSL_CTX_free(cctx);
    ::close(fd);
    return out;
}

static std::string pattern(int seed, std::size_t n) {
    std::string s(n, '\0');
    for (std::size_t j = 0; j < n; ++j)
        s[j] = static_cast<char>((j * 7 + static_cast<std::size_t>(seed) * 131 + 3) & 0xFF);
    return s;
}

static Result<TierManager> open_tm(const std::string& base, Index& index) {
    PoolConfig ssd, hdd;
    ssd.stripe_unit = 64 * KiB;
    ssd.dirs = {base + "/s0", base + "/s1"};
    for (const auto& d : ssd.dirs) fs::create_directories(d);
    TierSizes tiers;
    tiers.ram_head = 4 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig mem;
    mem.total_bytes = 64 * MiB;
    mem.block_bytes = 1 * MiB;
    mem.lock_memory = false;
    EvictionConfig ev;
    return TierManager::open(tiers, mem, ev, ssd, hdd, index);
}

TEST("https loop: TLS GET served; strict SNI + Host==SNI per-tenant isolation") {
    if (!Reactor::available()) { std::println("    (skipped: built without liburing)"); return; }
    auto rc = Reactor::create();
    if (!rc) { std::println("    (skipped: io_uring unavailable: {})", rc.error().detail); return; }
    const std::string base =
        (fs::temp_directory_path() / ("goblin-https-" + std::to_string(::getpid()))).string();
    fs::create_directories(base);
    const std::string ac = base + "/a.crt", ak = base + "/a.key";
    const std::string bc = base + "/b.crt", bk = base + "/b.key";
    CHECK(write_self_signed(ac, ak, "a.test"));
    CHECK(write_self_signed(bc, bk, "b.test"));
    auto ctx = tls::Context::create({{ac, ak}, {bc, bk}});
    CHECK(ctx.has_value());
    if (!ctx) { fs::remove_all(base); return; }

    Index index;
    auto tm = open_tm(base, index);
    auto io = IoBufferPool::create(128 * KiB, 8, false);
    CHECK(tm.has_value() && io.has_value());
    if (!tm || !io) { fs::remove_all(base); return; }

    KeyOptions opt;
    opt.mode = KeyMode::vhost; // multi-tenant: key = host + path
    const std::string body = pattern(4, 20 * 1024); // head + disk tail, exercises the kTLS stream
    const std::string key = *http::derive_key("a.test", "/foo", opt); // -> "a.test/foo"
    CHECK(tm->store(hash_key(key), ByteView(reinterpret_cast<const std::byte*>(body.data()), body.size()), 0)
              .has_value());

    auto [lfd, port] = loopback_listener();
    CHECK(lfd >= 0);
    if (lfd < 0) { fs::remove_all(base); return; }
    HttpsLoop loop(*rc, lfd, *tm, index, *io, opt, *ctx, 0);
    std::thread th([&] { loop.run(); });

    const Resp good = https_get(port, "a.test", "/foo", "a.test");      // right tenant
    const Resp mismatch = https_get(port, "a.test", "/foo", "b.test");  // Host != SNI
    const Resp bad_sni = https_get(port, "c.test", "/foo", "c.test");   // no cert for this tenant

    loop.stop();
    th.join();
    ::close(lfd);
    fs::remove_all(base);

    CHECK(good.connected && good.status == 200 && good.body == body); // TLS GET over kTLS
    CHECK(mismatch.connected && mismatch.status == 421);              // Host != SNI -> 421
    CHECK(!bad_sni.connected);                                        // strict SNI refuses the handshake
}

#endif // GOBLIN_HAVE_TLS
