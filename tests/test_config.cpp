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
