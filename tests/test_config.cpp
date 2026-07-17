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

TEST("TCP listen address must be numeric IPv4") {
    ServerConfig config;
    config.ssd.dirs.push_back("/not-opened-by-config-validation");
    config.listen_address = "cache.example.com";
    CHECK(detail_contains(validate(config), "numeric IPv4"));

    config.listen_address = "::1";
    CHECK(detail_contains(validate(config), "numeric IPv4"));

    config.listen_address = "127.1"; // getaddrinfo accepts this shorthand; inet_pton does not.
    CHECK(detail_contains(validate(config), "numeric IPv4"));

    config.listen_address = "127.0.0.1";
    CHECK(validate(config).has_value());
}

TEST("ExaSock requires an exact address and a plaintext listener") {
    ServerConfig config;
    config.ssd.dirs.push_back("/not-opened-by-config-validation");
    config.net = NetMode::exasock;
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
