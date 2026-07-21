#include "mini_test.hpp"

#include "goblin/net/listener.hpp"
#include "goblin/protocol/memcache/auth.hpp"

#include <filesystem>
#include <fstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace goblin;

TEST("memcache auth file uses standard user:password records") {
    const fs::path path = fs::temp_directory_path() /
        ("goblin-auth-" + std::to_string(::getpid()));
    {
        std::ofstream file(path);
        file << "alice:correct horse battery staple\nrobot:s3:cret\n";
    }
    ::chmod(path.c_str(), 0600);
    auto auth = memcache::Authenticator::load(path.string());
    CHECK(auth.has_value());
    if (auth) {
        CHECK_EQ(auth->size(), std::size_t(2));
        CHECK(auth->authenticate("alice correct horse battery staple"));
        CHECK(auth->authenticate("robot s3:cret"));
        CHECK(!auth->authenticate("alice wrong"));
        CHECK(!auth->authenticate("missing s3:cret"));
        CHECK(!auth->authenticate("robot  s3:cret"));
    }
    fs::remove(path);
}

TEST("memcache auth file rejects credentials exposed to other users") {
    const fs::path path = fs::temp_directory_path() /
        ("goblin-auth-mode-" + std::to_string(::getpid()));
    { std::ofstream file(path); file << "alice:secret\n"; }
    ::chmod(path.c_str(), 0644);
    const auto auth = memcache::Authenticator::load(path.string());
    CHECK(!auth.has_value());
    if (!auth) CHECK(auth.error().detail.find("chmod 600") != std::string::npos);
    fs::remove(path);
}

TEST("network listener binds IPv6 loopback and formats bracketed endpoints") {
    auto listener = net::make_tcp_listener("::1", 0, 8);
    if (!listener) return; // IPv6 may be disabled by the test host/container.
    sockaddr_storage address{};
    socklen_t length = sizeof address;
    CHECK(::getsockname(*listener, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    CHECK_EQ(address.ss_family, static_cast<sa_family_t>(AF_INET6));
    CHECK_EQ(net::format_endpoint("::1", 11211), std::string("[::1]:11211"));
    ::close(*listener);
}

TEST("Unix listener refuses non-sockets and applies the requested mode") {
    const fs::path path = fs::path("/tmp") /
        ("goblin-unix-" + std::to_string(::getpid()));
    { std::ofstream file(path); file << "do not remove"; }
    CHECK(!net::make_unix_listener(path.string(), 0600, 8).has_value());
    CHECK(fs::is_regular_file(path));
    fs::remove(path);

    auto listener = net::make_unix_listener(path.string(), 0620, 8);
    if (!listener && listener.error().detail.find("Operation not permitted") != std::string::npos) {
        fs::remove(path);
        return; // filesystem sandbox denies AF_UNIX bind; exercised on Linux CI/hosts
    }
    CHECK(listener.has_value());
    if (listener) {
        struct stat info{};
        CHECK(::stat(path.c_str(), &info) == 0);
        CHECK_EQ(info.st_mode & 0777, static_cast<mode_t>(0620));
        const auto duplicate = net::make_unix_listener(path.string(), 0600, 8);
        CHECK(!duplicate.has_value());
        if (!duplicate)
            CHECK(duplicate.error().detail.find("already active") != std::string::npos);
        ::close(*listener);
        ::unlink(path.c_str());
    }
}
