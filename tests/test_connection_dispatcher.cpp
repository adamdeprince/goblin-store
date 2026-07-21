#include "mini_test.hpp"

#include "goblin/net/connection_dispatcher.hpp"
#include "goblin/core/stats.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace goblin;
using namespace goblin::net;

TEST("connection router: least-loaded placement evenly fills workers") {
    ConnectionRouter router(std::vector<int>(12, -1));
    std::vector<std::uint64_t> loads(12, 0);
    for (unsigned connection = 0; connection < 16; ++connection) {
        const auto route = router.choose(loads);
        CHECK(route.worker < loads.size());
        ++loads[route.worker];
    }
    const auto [low, high] = std::minmax_element(loads.begin(), loads.end());
    CHECK_EQ(*low, 1u);
    CHECK_EQ(*high, 2u);
}

TEST("connection budget has an atomic hard ceiling and balanced release") {
    ConnectionBudget budget(2);
    CHECK(budget.try_acquire());
    CHECK(budget.try_acquire());
    CHECK(!budget.try_acquire());
    CHECK(budget.full());
    CHECK_EQ(budget.current(), 2u);
    budget.release();
    CHECK(!budget.full());
    CHECK(budget.try_acquire());
    budget.release();
    budget.release();
    budget.release(); // defensive underflow guard
    CHECK_EQ(budget.current(), 0u);
}

TEST("connection router: load wins and incoming CPU breaks only equal-load ties") {
    ConnectionRouter router({10, 11, 12});
    std::vector<std::uint64_t> loads = {0, 0, 0};
    auto route = router.choose(loads, /*incoming_cpu=*/11);
    CHECK_EQ(route.worker, 1u);
    CHECK(route.incoming_cpu_match);

    loads[1] = 1;
    route = router.choose(loads, /*incoming_cpu=*/11);
    CHECK(route.worker != 1u);
    CHECK(!route.incoming_cpu_match);
}

TEST("connection router: NAPI affinity is retained only among least-loaded workers") {
    ConnectionRouter router({-1, -1, -1});
    std::vector<std::uint64_t> loads = {0, 0, 0};
    const auto first = router.choose(loads, /*incoming_cpu=*/-1, /*napi_id=*/73);
    CHECK(first.worker < loads.size());

    loads = {1, 1, 1};
    auto route = router.choose(loads, /*incoming_cpu=*/-1, /*napi_id=*/73);
    CHECK_EQ(route.worker, first.worker);
    CHECK(route.napi_affinity_match);

    loads[first.worker] = 2;
    route = router.choose(loads, /*incoming_cpu=*/-1, /*napi_id=*/73);
    CHECK(route.worker != first.worker);
    CHECK(!route.napi_affinity_match);
}

TEST("connection dispatcher: a burst is handed evenly to worker inboxes") {
#if !defined(__linux__)
    return;
#else
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    if (listener < 0) return;
    const int one = 1;
    (void)::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    CHECK(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
    CHECK(::listen(listener, 128) == 0);
    const int flags = ::fcntl(listener, F_GETFL, 0);
    CHECK(flags >= 0 && ::fcntl(listener, F_SETFL, flags | O_NONBLOCK) == 0);
    socklen_t address_length = sizeof address;
    CHECK(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_length) == 0);

    auto dispatcher = ConnectionDispatcher::create(
        listener, std::vector<int>(4, -1), "test dispatcher");
    CHECK(dispatcher.has_value());
    if (!dispatcher) return; // create() owns and closes listener on failure

    std::atomic<bool> shutdown{false};
    std::thread acceptor([&] { (*dispatcher)->run(shutdown); });
    std::vector<int> clients;
    for (unsigned connection = 0; connection < 17; ++connection) {
        const int client = ::socket(AF_INET, SOCK_STREAM, 0);
        CHECK(client >= 0);
        if (client < 0) continue;
        CHECK(::connect(client, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
        clients.push_back(client);
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::uint64_t accepted = 0;
    do {
        accepted = 0;
        for (std::size_t worker = 0; worker < (*dispatcher)->worker_count(); ++worker)
            accepted += (*dispatcher)->inbox(worker).accepted_connections();
        if (accepted == clients.size()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    CHECK_EQ(accepted, clients.size());

    shutdown.store(true, std::memory_order_relaxed);
    acceptor.join();
    std::vector<std::uint64_t> counts;
    for (std::size_t worker = 0; worker < (*dispatcher)->worker_count(); ++worker) {
        auto& inbox = (*dispatcher)->inbox(worker);
        counts.push_back(inbox.accepted_connections());
        auto server_fds = inbox.take_connections();
        for (const int fd : server_fds) {
            ::close(fd);
            inbox.release_connection();
        }
    }
    const auto [low, high] = std::minmax_element(counts.begin(), counts.end());
    CHECK(*high - *low <= 1);
    for (const int fd : clients) ::close(fd);
#endif
}

TEST("connection dispatcher pauses acceptance at the shared connection ceiling") {
#if !defined(__linux__)
    return;
#else
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    if (listener < 0) return;
    const int one = 1;
    (void)::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    CHECK(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
    CHECK(::listen(listener, 8) == 0);
    const int flags = ::fcntl(listener, F_GETFL, 0);
    CHECK(flags >= 0 && ::fcntl(listener, F_SETFL, flags | O_NONBLOCK) == 0);
    socklen_t address_length = sizeof address;
    CHECK(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_length) == 0);

    auto budget = std::make_shared<ConnectionBudget>(1);
    core::StatsRegistry stats;
    auto dispatcher = ConnectionDispatcher::create(
        listener, {-1}, "limited dispatcher", false, budget, &stats);
    CHECK(dispatcher.has_value());
    if (!dispatcher) return;

    std::atomic<bool> shutdown{false};
    std::thread acceptor([&] { (*dispatcher)->run(shutdown); });
    const int first = ::socket(AF_INET, SOCK_STREAM, 0);
    const int second = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(first >= 0 && second >= 0);
    CHECK(::connect(first, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);

    const auto wait_until = [](const auto& predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!predicate() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return predicate();
    };
    CHECK(wait_until([&] { return budget->current() == 1; }));
    CHECK(::connect(second, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
    CHECK(wait_until([&] { return stats.aggregate().listen_disabled_num >= 1; }));
    CHECK_EQ((*dispatcher)->inbox(0).accepted_connections(), 1u);

    auto accepted = (*dispatcher)->inbox(0).take_connections();
    CHECK_EQ(accepted.size(), 1u);
    for (const int fd : accepted) {
        ::close(fd);
        (*dispatcher)->inbox(0).release_connection();
    }
    CHECK(wait_until([&] { return (*dispatcher)->inbox(0).accepted_connections() == 2; }));
    CHECK_EQ(budget->current(), 1u);

    shutdown.store(true, std::memory_order_relaxed);
    acceptor.join();
    accepted = (*dispatcher)->inbox(0).take_connections();
    for (const int fd : accepted) {
        ::close(fd);
        (*dispatcher)->inbox(0).release_connection();
    }
    CHECK_EQ(budget->current(), 0u);
    if (first >= 0) ::close(first);
    if (second >= 0) ::close(second);
#endif
}
