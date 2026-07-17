#include "mini_test.hpp"

#include "goblin/crypto/sha256.hpp"
#include "goblin/http/mirror.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <filesystem>
#include <netinet/in.h>
#include <poll.h>
#include <print>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace goblin;
using namespace goblin::http;
using namespace goblin::storage;

namespace {

struct Listener {
    int fd = -1;
    std::uint16_t port = 0;
};

Listener loopback_listener() {
    Listener out;
    out.fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (out.fd < 0) return out;
    const int one = 1;
    ::setsockopt(out.fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(out.fd, reinterpret_cast<sockaddr*>(&address), sizeof address) < 0 ||
        ::listen(out.fd, 4) < 0) {
        ::close(out.fd);
        out.fd = -1;
        return out;
    }
    socklen_t length = sizeof address;
    if (::getsockname(out.fd, reinterpret_cast<sockaddr*>(&address), &length) < 0) {
        ::close(out.fd);
        out.fd = -1;
        return out;
    }
    out.port = ntohs(address.sin_port);
    return out;
}

bool send_all(int fd, std::string_view data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t sent = ::send(fd, data.data() + offset, data.size() - offset, 0);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        offset += static_cast<std::size_t>(sent);
    }
    return true;
}

std::string read_request_head(int fd) {
    std::string request;
    char buffer[4096];
    while (request.find("\r\n\r\n") == std::string::npos) {
        const ssize_t got = ::recv(fd, buffer, sizeof buffer, 0);
        if (got <= 0) break;
        request.append(buffer, static_cast<std::size_t>(got));
    }
    return request;
}

Result<TierManager> mirror_tm(const std::string& root, Index& index) {
    fs::create_directories(root + "/ssd");
    TierSizes tiers;
    tiers.ram_head = 64 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig memory;
    memory.total_bytes = 8 * MiB;
    memory.block_bytes = 2 * MiB;
    memory.lock_memory = false;
    PoolConfig ssd;
    ssd.dirs = {root + "/ssd"};
    return TierManager::open(tiers, memory, EvictionConfig{}, ssd, PoolConfig{}, index,
                             64 * KiB, 2, false);
}

} // namespace

TEST("mirror service: miss streams one chunk at a time and publishes body plus HTTP metadata") {
    if (!MirrorService::available()) return;
    const Listener listener = loopback_listener();
    if (listener.fd < 0) {
        std::println("    (skipped: loopback sockets unavailable in this environment)");
        return;
    }
    const std::string body(32 * KiB, 'g');
    std::string received_request;
    std::thread origin([&] {
        const int client = ::accept(listener.fd, nullptr, nullptr);
        if (client < 0) return;
        char buffer[4096];
        while (received_request.find("\r\n\r\n") == std::string::npos) {
            const ssize_t got = ::recv(client, buffer, sizeof buffer, 0);
            if (got <= 0) break;
            received_request.append(buffer, static_cast<std::size_t>(got));
        }
        std::string head = "HTTP/1.1 200 OK\r\nContent-Length: " +
                           std::to_string(body.size()) +
                           "\r\nCache-Control: public, max-age=60\r\n"
                           "Content-Type: application/goblin\r\nETag: \"bug\"\r\n"
                           "Connection: close\r\n\r\n";
        (void)send_all(client, head);
        (void)send_all(client, body.substr(0, body.size() / 2));
        (void)send_all(client, body.substr(body.size() / 2));
        ::close(client);
    });

    const std::string root = (fs::temp_directory_path() /
        ("goblin-mirror-service-" + std::to_string(::getpid()))).string();
    Index index;
    auto tm = mirror_tm(root, index);
    CHECK(tm.has_value());
    if (!tm) {
        ::close(listener.fd);
        origin.join();
        fs::remove_all(root);
        return;
    }
    std::atomic<bool> shutdown{false};
    auto service = MirrorService::create(
        "http://127.0.0.1:" + std::to_string(listener.port) + "/base", *tm, index,
        &shutdown, 1);
    CHECK(service.has_value());
    if (!service) {
        ::close(listener.fd);
        origin.join();
        fs::remove_all(root);
        return;
    }

    MirrorRequest request;
    request.target = "/asset.bin?v=7";
    request.headers = {{"accept", "application/octet-stream"}};
    request.digest = crypto::hash_key("mirror-service-key");
    auto fetch = (*service)->fetch(request);
    CHECK(fetch.has_value());
    std::string streamed;
    bool saw_headers = false;
    bool done = false;
    for (unsigned iteration = 0; fetch && iteration < 100 && !done; ++iteration) {
        pollfd descriptor{(*fetch)->notification_fd(), POLLIN, 0};
        if (::poll(&descriptor, 1, 1000) <= 0) continue;
        (*fetch)->drain_notification();
        const auto view = (*fetch)->view();
        if (view.headers_published && !saw_headers) {
            saw_headers = view.response && view.response->status == 200;
            (*fetch)->acknowledge_headers();
        }
        if (view.chunk) {
            streamed.append(reinterpret_cast<const char*>(view.chunk->data()), view.chunk->size());
            (*fetch)->acknowledge_chunk(view.chunk_sequence);
        }
        done = view.done;
        CHECK(!view.failed);
    }

    origin.join();
    ::close(listener.fd);
    CHECK(done);
    CHECK(saw_headers);
    CHECK_EQ(streamed, body);
    CHECK(received_request.find("GET /base/asset.bin?v=7 HTTP/1.1") != std::string::npos);

    const auto record = index.lookup_with_http(request.digest);
    CHECK(record && record->meta.size == body.size() && record->http);
    if (record && record->http) {
        CHECK_EQ(record->http->status, std::uint16_t(200));
        CHECK_EQ(record->http->etag, "\"bug\"");
        CHECK_EQ(record->http->freshness_lifetime, std::uint64_t(60));
    }
    const auto resident = tm->head_view(request.digest);
    CHECK(resident && resident->size() == body.size());
    if (resident)
        CHECK(std::string_view(reinterpret_cast<const char*>(resident->data()), resident->size()) == body);

    service->reset();
    fs::remove_all(root);
}

TEST("mirror service: a disconnected leader keeps filling and one same-key follower reuses it") {
    if (!MirrorService::available()) return;
    const Listener listener = loopback_listener();
    if (listener.fd < 0) {
        std::println("    (skipped: loopback sockets unavailable in this environment)");
        return;
    }
    const std::string body(192 * KiB, 'd');
    std::atomic<unsigned> origin_requests{0};
    std::thread origin([&] {
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            pollfd descriptor{listener.fd, POLLIN, 0};
            const int timeout_ms = attempt == 0 ? 2000 : 500;
            if (::poll(&descriptor, 1, timeout_ms) <= 0) return;
            const int client = ::accept(listener.fd, nullptr, nullptr);
            if (client < 0) return;
            origin_requests.fetch_add(1, std::memory_order_relaxed);
            (void)read_request_head(client);
            const std::string head =
                "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
                "\r\nCache-Control: public, max-age=60\r\nConnection: close\r\n\r\n";
            (void)send_all(client, head);
            (void)send_all(client, body);
            ::close(client);
        }
    });

    const std::string root = (fs::temp_directory_path() /
        ("goblin-mirror-detach-" + std::to_string(::getpid()))).string();
    Index index;
    auto tm = mirror_tm(root, index);
    CHECK(tm.has_value());
    if (!tm) {
        ::close(listener.fd);
        origin.join();
        fs::remove_all(root);
        return;
    }
    std::atomic<bool> shutdown{false};
    auto service = MirrorService::create(
        "http://127.0.0.1:" + std::to_string(listener.port), *tm, index, &shutdown, 2);
    CHECK(service.has_value());
    if (!service) {
        ::close(listener.fd);
        origin.join();
        fs::remove_all(root);
        return;
    }

    MirrorRequest request;
    request.target = "/detached.bin";
    request.digest = crypto::hash_key("mirror-detached-key");
    auto leader = (*service)->fetch(request);
    CHECK(leader.has_value());
    std::shared_ptr<MirrorFetch> follower;
    bool detached = false;
    for (unsigned iteration = 0; leader && iteration < 100 && !detached; ++iteration) {
        pollfd descriptor{(*leader)->notification_fd(), POLLIN, 0};
        if (::poll(&descriptor, 1, 1000) <= 0) continue;
        (*leader)->drain_notification();
        const auto view = (*leader)->view();
        if (view.headers_published) (*leader)->acknowledge_headers();
        if (view.chunk) {
            auto joined = (*service)->fetch(request);
            CHECK(joined.has_value());
            if (joined) follower = std::move(*joined);
            (*leader)->detach_client(); // releases the current send gate without cancelling the fill
            detached = true;
        }
    }
    CHECK(detached);
    bool follower_ready = false;
    for (unsigned iteration = 0; follower && iteration < 100 && !follower_ready; ++iteration) {
        pollfd descriptor{follower->notification_fd(), POLLIN, 0};
        if (::poll(&descriptor, 1, 1000) <= 0) continue;
        follower->drain_notification();
        const auto view = follower->view();
        follower_ready = view.done && view.cache_ready && !view.failed;
    }

    origin.join();
    ::close(listener.fd);
    CHECK(follower_ready);
    CHECK_EQ(origin_requests.load(std::memory_order_relaxed), 1u);
    const auto record = index.lookup_with_http(request.digest);
    CHECK(record && record->http && record->meta.size == body.size());
    const auto resident = tm->head_view(request.digest);
    CHECK(resident && resident->size() == 64 * KiB);
    if (resident)
        CHECK(std::string_view(reinterpret_cast<const char*>(resident->data()), resident->size()) ==
              std::string_view(body).substr(0, resident->size()));

    service->reset();
    fs::remove_all(root);
}

TEST("mirror service: storage admission failure still streams the complete origin response") {
    if (!MirrorService::available()) return;
    const Listener listener = loopback_listener();
    if (listener.fd < 0) {
        std::println("    (skipped: loopback sockets unavailable in this environment)");
        return;
    }
    const std::string body(96 * KiB, 'e');
    std::thread origin([&] {
        const int client = ::accept(listener.fd, nullptr, nullptr);
        if (client < 0) return;
        (void)read_request_head(client);
        const std::string head =
            "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
            "\r\nCache-Control: public, max-age=60\r\nConnection: close\r\n\r\n";
        (void)send_all(client, head);
        (void)send_all(client, body);
        ::close(client);
    });

    const std::string root = (fs::temp_directory_path() /
        ("goblin-mirror-store-fail-" + std::to_string(::getpid()))).string();
    Index index;
    auto tm = mirror_tm(root, index);
    CHECK(tm.has_value());
    if (!tm) {
        ::close(listener.fd);
        origin.join();
        fs::remove_all(root);
        return;
    }
    // Pool keeps an open directory descriptor. Removing its directory makes the subsequent
    // openat(O_CREAT) admission fail while leaving the TierManager itself valid for the test.
    fs::remove_all(root);
    std::atomic<bool> shutdown{false};
    auto service = MirrorService::create(
        "http://127.0.0.1:" + std::to_string(listener.port), *tm, index, &shutdown, 1);
    CHECK(service.has_value());
    if (!service) {
        ::close(listener.fd);
        origin.join();
        return;
    }

    MirrorRequest request;
    request.target = "/uncached.bin";
    request.digest = crypto::hash_key("mirror-store-failure-key");
    auto fetch = (*service)->fetch(request);
    CHECK(fetch.has_value());
    std::string streamed;
    bool done = false;
    for (unsigned iteration = 0; fetch && iteration < 100 && !done; ++iteration) {
        pollfd descriptor{(*fetch)->notification_fd(), POLLIN, 0};
        if (::poll(&descriptor, 1, 1000) <= 0) continue;
        (*fetch)->drain_notification();
        const auto view = (*fetch)->view();
        if (view.headers_published) (*fetch)->acknowledge_headers();
        if (view.chunk) {
            streamed.append(reinterpret_cast<const char*>(view.chunk->data()), view.chunk->size());
            (*fetch)->acknowledge_chunk(view.chunk_sequence);
        }
        done = view.done;
        CHECK(!view.failed);
    }

    origin.join();
    ::close(listener.fd);
    CHECK(done);
    CHECK_EQ(streamed, body);
    CHECK(!index.lookup(request.digest));
    service->reset();
}
