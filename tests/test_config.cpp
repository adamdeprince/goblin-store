#include "mini_test.hpp"

#include "goblin/common/config.hpp"

#include <string_view>

using namespace goblin;

namespace {

ServerConfig rdma_config() {
    ServerConfig config;
    config.rdma.enabled = true;
    config.rdma.address = "127.0.0.1";
    config.ssd.dirs.push_back("/not-opened-by-config-validation");
    return config;
}

bool detail_contains(const Status& status, std::string_view text) {
    return !status && status.error().detail.find(text) != std::string::npos;
}

} // namespace

TEST("Network listeners default to loopback-only") {
    CHECK_EQ(ServerConfig{}.listen_address, std::string("127.0.0.1"));
}

TEST("RDMA config requires a numeric address") {
    auto config = rdma_config();
    config.rdma.address = "cache.example.com";
    const auto status = validate(config);
    CHECK(detail_contains(status, "numeric IPv4 or IPv6"));
}

TEST("RDMA config rejects an undiscoverable ephemeral port") {
    auto config = rdma_config();
    config.rdma.port = 0;
    const auto status = validate(config);
    CHECK(detail_contains(status, "--rdma-port"));
}

TEST("RDMA config enforces registered bulk geometry") {
    auto config = rdma_config();
    config.rdma.bulk_window_bytes = 6 * KiB;
    const auto status = validate(config);
    CHECK(detail_contains(status, "power of two"));

    config = rdma_config();
    config.rdma.bulk_window_count = 1;
    const auto one_window = validate(config);
    CHECK(detail_contains(one_window, "at least 2"));
}

TEST("Default native RDMA geometry validates") {
    const auto status = validate(rdma_config());
    CHECK(status.has_value());
}

TEST("TCP listen address accepts exact numeric IPv4 and IPv6") {
    ServerConfig config;
    config.ssd.dirs.push_back("/not-opened-by-config-validation");
    config.listen_address = "cache.example.com";
    CHECK(detail_contains(validate(config), "numeric IPv4 or IPv6"));

    config.listen_address = "::1";
    CHECK(validate(config).has_value());

    config.listen_address = "127.1"; // getaddrinfo accepts this shorthand; inet_pton does not.
    CHECK(detail_contains(validate(config), "numeric IPv4 or IPv6"));

    config.listen_address = "127.0.0.1";
    CHECK(validate(config).has_value());
}

TEST("Overload limits are finite and reject invalid connection, backlog, and object bounds") {
    ServerConfig config;
    config.ssd.dirs.push_back("/not-opened-by-config-validation");
    CHECK(config.max_connections > 0);
    CHECK(config.listen_backlog > 0);
    CHECK(config.idle_timeout_ms > 0);
    CHECK(config.queue_timeout_ms > 0);
    CHECK(config.max_get_waiters > 0);
    CHECK(config.max_set_waiters > 0);
    CHECK_EQ(config.file_handle_cache, 128U);
    CHECK_EQ(config.max_object_size, kMaxObjectSize);
    CHECK(validate(config).has_value());

    config.max_connections = 0;
    CHECK(detail_contains(validate(config), "--max-connections"));
    config.max_connections = 1;
    config.listen_backlog = 0;
    CHECK(detail_contains(validate(config), "--listen-backlog"));
    config.listen_backlog = 1;
    config.max_object_size = 0;
    CHECK(detail_contains(validate(config), "--max-object-size"));
    config.max_object_size = kMaxObjectSize + 1;
    CHECK(detail_contains(validate(config), "4 GiB hard limit"));

    config.max_object_size = kMaxObjectSize;
    config.file_handle_cache = 3;
    CHECK(detail_contains(validate(config), "--file-handle-cache"));
    config.file_handle_cache = 0;
    CHECK(detail_contains(validate(config), "--file-handle-cache"));
}

TEST("Disk reclaim watermarks require a strict low-to-high interval") {
    auto config = rdma_config();
    config.eviction.low_watermark = 0.80;
    config.eviction.high_watermark = 0.90;
    CHECK(validate(config).has_value());
    config.eviction.low_watermark = 0.90;
    CHECK(!validate(config).has_value());
    config.eviction.low_watermark = 0.0;
    CHECK(!validate(config).has_value());
    config.eviction.low_watermark = 0.80;
    config.eviction.high_watermark = 1.01;
    CHECK(!validate(config).has_value());
}

TEST("ExaSock requires an exact address and a plaintext listener") {
    ServerConfig config;
    config.ssd.dirs.push_back("/not-opened-by-config-validation");
    config.net = NetMode::exasock;
    config.listen_address = "0.0.0.0";
    CHECK(detail_contains(validate(config), "non-wildcard"));

    config.listen_address = "192.0.2.10";
    CHECK(validate(config).has_value());

    config.enable_memcache = false;
    config.enable_http = false;
    config.enable_https = true;
    config.tls_cert_paths.push_back("cert.pem");
    config.tls_key_paths.push_back("key.pem");
    CHECK(detail_contains(validate(config), "plaintext HTTP"));
}

TEST("Unix memcache may be the only listener and validates its mode") {
    ServerConfig config;
    config.ssd.dirs.push_back("/not-opened-by-config-validation");
    config.enable_memcache = false;
    config.enable_http = false;
    config.memcache_socket = "/tmp/goblin-test.sock";
    CHECK(validate(config).has_value());
    config.memcache_socket_mode = 01000;
    CHECK(detail_contains(validate(config), "socket-mode"));
}

TEST("Memcache TLS needs certificates and the async TCP backend") {
    ServerConfig config;
    config.ssd.dirs.push_back("/not-opened-by-config-validation");
    config.memcache_tls = true;
    CHECK(detail_contains(validate(config), "TLS require"));
    config.tls_cert_paths.push_back("cert.pem");
    config.tls_key_paths.push_back("key.pem");
    CHECK(validate(config).has_value());
    config.net = NetMode::blocking;
    CHECK(detail_contains(validate(config), "requires the default --net async"));
}

TEST("ASCII auth cannot leave native RDMA unauthenticated") {
    auto config = rdma_config();
    config.memcache_auth_file = "/secret/users";
    CHECK(detail_contains(validate(config), "protects TCP and Unix memcache"));
}

TEST("Mirror requires HTTP, validates its base URL, and excludes virtual-host mode") {
    ServerConfig config;
    config.ssd.dirs.push_back("/not-opened-by-config-validation");
    config.mirror_url = "https://origin.example/assets";
    CHECK(validate(config).has_value());

    config.http_vhost = true;
    CHECK(detail_contains(validate(config), "mutually exclusive"));

    config.http_vhost = false;
    config.mirror_url = "ftp://origin.example/assets";
    CHECK(detail_contains(validate(config), "absolute http"));

    config.mirror_url = "https://origin.example/assets?base=bad";
    CHECK(detail_contains(validate(config), "query or fragment"));

    config.mirror_url = "https://origin.example/assets";
    config.enable_http = false;
    config.enable_https = false;
    CHECK(detail_contains(validate(config), "requires the HTTP"));
}

TEST("Native io_uring mirror client accepts only plaintext HTTP origins") {
    ServerConfig config;
    config.ssd.dirs.push_back("/not-opened-by-config-validation");
    config.mirror_client = MirrorClient::uring;
    config.mirror_url = "https://origin.example/assets";
    CHECK(detail_contains(validate(config), "requires an http:// origin"));

    config.mirror_url = "http://origin.example/assets";
    CHECK(validate(config).has_value());
}
