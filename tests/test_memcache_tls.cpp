#include "mini_test.hpp"

#include "goblin/core/buffer_pool.hpp"
#include "goblin/core/reactor.hpp"
#include "goblin/protocol/memcache/auth.hpp"
#include "goblin/protocol/memcache/protocol.hpp"
#include "goblin/protocol/memcache/tls_event_loop.hpp"
#include "goblin/storage/index.hpp"
#include "goblin/storage/tier_manager.hpp"

#if GOBLIN_HAVE_TLS

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace goblin;

#if defined(__linux__)
namespace {

bool write_certificate(const std::string& cert, const std::string& key) {
    EVP_PKEY* pkey = EVP_EC_gen("prime256v1");
    X509* certificate = X509_new();
    bool ok = pkey && certificate;
    if (ok) {
        ASN1_INTEGER_set(X509_get_serialNumber(certificate), 1);
        X509_gmtime_adj(X509_getm_notBefore(certificate), 0);
        X509_gmtime_adj(X509_getm_notAfter(certificate), 60L * 60 * 24);
        X509_set_pubkey(certificate, pkey);
        X509_NAME* name = X509_get_subject_name(certificate);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("cache.test"),
                                   -1, -1, 0);
        X509_set_issuer_name(certificate, name);
        ok = X509_sign(certificate, pkey, EVP_sha256()) > 0;
    }
    if (ok) {
        FILE* key_file = std::fopen(key.c_str(), "wb");
        ok = key_file && PEM_write_PrivateKey(key_file, pkey, nullptr, nullptr, 0,
                                              nullptr, nullptr) > 0;
        if (key_file) std::fclose(key_file);
        FILE* cert_file = std::fopen(cert.c_str(), "wb");
        ok = ok && cert_file && PEM_write_X509(cert_file, certificate) > 0;
        if (cert_file) std::fclose(cert_file);
    }
    X509_free(certificate);
    EVP_PKEY_free(pkey);
    return ok;
}

Result<storage::TierManager> open_store(const fs::path& root, storage::Index& index) {
    PoolConfig pool;
    pool.dirs = {(root / "data").string()};
    pool.stripe_unit = 64 * KiB;
    fs::create_directories(pool.dirs.front());
    TierSizes tiers;
    tiers.ram_head = 64 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig memory;
    memory.total_bytes = 32 * MiB;
    memory.block_bytes = 2 * MiB;
    memory.lock_memory = false;
    return storage::TierManager::open(tiers, memory, EvictionConfig{}, pool, {}, index,
                                      64 * KiB, 8, false);
}

std::pair<int, std::uint16_t> listener() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return {-1, 0};
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 ||
        ::listen(fd, 8) != 0) { ::close(fd); return {-1, 0}; }
    socklen_t length = sizeof address;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length);
    return {fd, ntohs(address.sin_port)};
}

int connect_to(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

} // namespace
#endif

TEST("memcache TLS accepts no-SNI clients and applies ASCII authentication") {
#if !defined(__linux__)
    return; // the streaming TLS transmit path requires Linux kTLS
#else
    if (!core::Reactor::available()) return;
    auto reactor = core::Reactor::create();
    if (!reactor) return;
    const fs::path root = fs::temp_directory_path() /
        ("goblin-memcache-tls-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    const std::string cert = (root / "server.crt").string();
    const std::string key = (root / "server.key").string();
    const std::string users = (root / "users").string();
    CHECK(write_certificate(cert, key));
    { std::ofstream file(users); file << "alice:secret\n"; }
    ::chmod(users.c_str(), 0600);
    auto context = tls::Context::create({{cert, key}}, tls::Context::SniPolicy::optional);
    auto auth = memcache::Authenticator::load(users);
    storage::Index index;
    auto store = open_store(root, index);
    auto buffers = core::IoBufferPool::create(64 * KiB, 8, false);
    auto [listen_fd, port] = listener();
    CHECK(context && auth && store && buffers && listen_fd >= 0);
    if (!context || !auth || !store || !buffers || listen_fd < 0) {
        fs::remove_all(root);
        return;
    }

    memcache::TlsEventLoop loop(*reactor, listen_fd, *store, index, *buffers, *context);
    loop.set_authenticator(&*auth);
    std::thread server([&] { loop.run(); });

    bool passed = false;
    const int fd = connect_to(port);
    SSL_CTX* client_context = SSL_CTX_new(TLS_client_method());
    if (client_context) SSL_CTX_set_min_proto_version(client_context, TLS1_3_VERSION);
    SSL* client = client_context ? SSL_new(client_context) : nullptr;
    if (fd >= 0 && client) {
        SSL_set_fd(client, fd); // deliberately no SNI
        if (SSL_connect(client) == 1) {
            constexpr std::string_view request =
                "version\r\nset ignored 0 0 12\r\nalice secret\r\nversion\r\n";
            if (SSL_write(client, request.data(), static_cast<int>(request.size())) > 0) {
                std::string response;
                char bytes[1024];
                while (response.find(memcache::kVersion) == std::string::npos) {
                    const int count = SSL_read(client, bytes, sizeof bytes);
                    if (count <= 0) break;
                    response.append(bytes, static_cast<std::size_t>(count));
                }
                passed = response == "CLIENT_ERROR unauthenticated\r\nSTORED\r\n" +
                                       std::string(memcache::kVersion);
            }
        }
    }
    if (client) SSL_free(client);
    SSL_CTX_free(client_context);
    if (fd >= 0) ::close(fd);
    loop.stop();
    server.join();
    ::close(listen_fd);
    fs::remove_all(root);
    CHECK(passed);
#endif
}

#endif // GOBLIN_HAVE_TLS
