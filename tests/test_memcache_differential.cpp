#include "mini_test.hpp"

#include "goblin/core/buffer_pool.hpp"
#include "goblin/core/reactor.hpp"
#include "goblin/protocol/memcache/event_loop.hpp"
#include "goblin/protocol/memcache/auth.hpp"
#include "goblin/protocol/memcache/server.hpp"
#include "goblin/storage/index.hpp"
#include "goblin/storage/tier_manager.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <netinet/in.h>
#include <regex>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace goblin;

#if defined(__linux__)
namespace {

Result<storage::TierManager> open_protocol_store(const fs::path& base, storage::Index& index) {
    PoolConfig pool;
    pool.dirs = {(base / "data").string()};
    pool.stripe_unit = 64 * KiB;
    fs::create_directories(pool.dirs.front());
    TierSizes tiers;
    tiers.ram_head = 64 * KiB;
    tiers.ssd_prefix = 1 * MiB;
    MemoryConfig memory;
    memory.total_bytes = 32 * MiB;
    memory.block_bytes = 2 * MiB;
    memory.lock_memory = false;
    EvictionConfig eviction;
    return storage::TierManager::open(tiers, memory, eviction, pool, {}, index,
                                      64 * KiB, 8, /*direct_io=*/false);
}

bool write_all(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
        const ssize_t n = ::send(fd, bytes.data(), bytes.size(), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        bytes.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
}

std::string read_all(int fd) {
    std::string out;
    char bytes[4096];
    for (;;) {
        const ssize_t n = ::recv(fd, bytes, sizeof bytes, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        out.append(bytes, static_cast<std::size_t>(n));
    }
    return out;
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
        ::listen(fd, 16) != 0) {
        ::close(fd);
        return {-1, 0};
    }
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

std::string normalize_volatile(std::string wire) {
    wire = std::regex_replace(wire, std::regex("STAT ([^ ]+) [^\\r]+\\r\\n"),
                              "STAT $1 #\r\n");
    wire = std::regex_replace(wire,
        std::regex("(ME [^ ]+)( b)? exp=[0-9]+ la=[0-9]+ cas=[0-9]+"),
        "$1$2 exp=# la=# cas=#");
    wire = std::regex_replace(wire, std::regex(" t-?[0-9]+"), " t#");
    wire = std::regex_replace(wire, std::regex(" l[0-9]+"), " l#");
    return wire;
}

constexpr std::string_view kTranscript =
    "set a 7 0 3\r\none\r\n"
    "set b 8 0 3\r\ntwo\r\n"
    "get a absent b\r\n"
    "append a 0 0 1\r\n!\r\n"
    "prepend a 0 0 1\r\n>\r\n"
    "get a\r\n"
    "set num 0 0 2\r\n10\r\n"
    "incr num 5\r\n"
    "decr num 20\r\n"
    "touch b 60\r\n"
    "gats 60 b absent\r\n"
    "flush_all\r\n"
    "get a\r\n"
    "set after 0 0 2\r\nok\r\n"
    "ms m 3 F9 c k Omset\r\nhey\r\n"
    "mg m v C9 c k Ocond\r\n"
    "mg m v f s c k h l u Oread\r\n"
    "mg dog v k N60 Ofirst\r\n"
    "mg dog v k N60 Osecond\r\n"
    "ms stale 3\r\nold\r\n"
    "md stale I\r\n"
    "mg stale v k\r\n"
    "ma count N60 J5 D2 k\r\n"
    "ma count D2 k\r\n"
    "me count\r\n"
    "ms YmluAGtleQ== 1 b k c\r\nx\r\n"
    "mg YmluAGtleQ== b v k\r\n"
    "stats\r\n"
    "stats reset\r\n"
    "quit\r\n";

std::string run_blocking(const fs::path& base) {
    storage::Index index;
    auto tm = open_protocol_store(base, index);
    auto reactor = core::Reactor::create();
    auto buffers = core::IoBufferPool::create(64 * KiB, 8, false);
    if (!tm || !reactor || !buffers) return {};
    int sockets[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) return {};
    std::thread server([&] {
        memcache::serve_blocking_connection(sockets[0], *tm, *reactor, *buffers);
        ::close(sockets[0]);
    });
    if (!write_all(sockets[1], kTranscript)) ::shutdown(sockets[1], SHUT_RDWR);
    const std::string result = read_all(sockets[1]);
    ::close(sockets[1]);
    server.join();
    return result;
}

std::string run_async(const fs::path& base) {
    storage::Index index;
    auto tm = open_protocol_store(base, index);
    auto reactor = core::Reactor::create();
    auto buffers = core::IoBufferPool::create(64 * KiB, 8, false);
    auto [listen_fd, port] = listener();
    if (!tm || !reactor || !buffers || listen_fd < 0) return {};
    memcache::EventLoop loop(*reactor, listen_fd, *tm, index, *buffers);
    std::thread server([&] { loop.run(); });
    const int client = connect_to(port);
    std::string result;
    if (client >= 0) {
        if (!write_all(client, kTranscript)) ::shutdown(client, SHUT_RDWR);
        result = read_all(client);
        ::close(client);
    }
    loop.stop();
    server.join();
    ::close(listen_fd);
    return result;
}

} // namespace
#endif

TEST("memcache differential: async and blocking execute one classic/meta wire transcript") {
#if !defined(__linux__)
    return;
#else
    if (!core::Reactor::available()) return;
    const fs::path root = fs::temp_directory_path() /
        ("goblin-memcache-differential-" + std::to_string(::getpid()));
    fs::remove_all(root);
    const std::string blocking = run_blocking(root / "blocking");
    const std::string async = run_async(root / "async");
    CHECK(!blocking.empty());
    CHECK(!async.empty());
    CHECK_EQ(normalize_volatile(blocking), normalize_volatile(async));
    fs::remove_all(root);
#endif
}

TEST("memcache ASCII authentication gates commands and permits retry") {
#if !defined(__linux__)
    return;
#else
    if (!core::Reactor::available()) return;
    const fs::path root = fs::temp_directory_path() /
        ("goblin-memcache-auth-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    const fs::path credentials = root / "users";
    { std::ofstream file(credentials); file << "alice:secret\n"; }
    ::chmod(credentials.c_str(), 0600);
    auto auth = memcache::Authenticator::load(credentials.string());
    storage::Index index;
    auto tm = open_protocol_store(root, index);
    auto reactor = core::Reactor::create();
    auto buffers = core::IoBufferPool::create(64 * KiB, 8, false);
    CHECK(auth && tm && reactor && buffers);
    if (!auth || !tm || !reactor || !buffers) { fs::remove_all(root); return; }
    int sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    if (sockets[0] < 0) { fs::remove_all(root); return; }
    std::thread server([&] {
        memcache::serve_blocking_connection(sockets[0], *tm, *reactor, *buffers,
                                             WriteMode::evict, &*auth);
        ::close(sockets[0]);
    });
    constexpr std::string_view transcript =
        "version\r\n"
        "set ignored 0 0 11\r\nalice wrong\r\n"
        "set ignored 99 999 12\r\nalice secret\r\n"
        "version\r\nquit\r\n";
    CHECK(write_all(sockets[1], transcript));
    const std::string result = read_all(sockets[1]);
    ::close(sockets[1]);
    server.join();
    CHECK_EQ(result,
        std::string("CLIENT_ERROR unauthenticated\r\n"
                    "CLIENT_ERROR authentication failure\r\n"
                    "STORED\r\nVERSION goblin-store 0.0.2\r\n"));
    fs::remove_all(root);
#endif
}
