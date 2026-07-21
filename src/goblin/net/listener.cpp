#include "goblin/net/listener.hpp"

#include "goblin/net/stream_io.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <stddef.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace goblin::net {
namespace {

Status prepare_listener(int fd, unsigned backlog, bool nonblocking) {
    if (::listen(fd, static_cast<int>(backlog)) < 0)
        return err(Errc::io_error, std::string("listen: ") + std::strerror(errno));
    if (nonblocking) {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
            return err(Errc::io_error,
                       std::string("make listener nonblocking: ") + std::strerror(errno));
    }
    return {};
}

} // namespace

bool wildcard_address(std::string_view address) noexcept {
    const std::string text(address);
    in_addr v4{};
    if (::inet_pton(AF_INET, text.c_str(), &v4) == 1)
        return v4.s_addr == htonl(INADDR_ANY);
    in6_addr v6{};
    return ::inet_pton(AF_INET6, text.c_str(), &v6) == 1 && IN6_IS_ADDR_UNSPECIFIED(&v6);
}

bool loopback_address(std::string_view address) noexcept {
    const std::string text(address);
    in_addr v4{};
    if (::inet_pton(AF_INET, text.c_str(), &v4) == 1)
        return (ntohl(v4.s_addr) >> 24) == 127;
    in6_addr v6{};
    return ::inet_pton(AF_INET6, text.c_str(), &v6) == 1 && IN6_IS_ADDR_LOOPBACK(&v6);
}

std::string format_endpoint(std::string_view address, std::uint16_t port) {
    if (address.find(':') != std::string_view::npos)
        return "[" + std::string(address) + "]:" + std::to_string(port);
    return std::string(address) + ":" + std::to_string(port);
}

Result<int> make_tcp_listener(std::string_view address, std::uint16_t port, unsigned backlog,
                              bool exasock, bool nonblocking) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    addrinfo* found = nullptr;
    const std::string host(address);
    const std::string service = std::to_string(port);
    const int gai = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &found);
    if (gai != 0)
        return err(Errc::invalid_argument,
                   std::string("invalid listener address: ") + ::gai_strerror(gai));

    int fd = -1;
    Error last{Errc::io_error, "socket"};
    for (addrinfo* ai = found; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) { last.detail = std::string("socket: ") + std::strerror(errno); continue; }
        const int one = 1;
        (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (ai->ai_family == AF_INET6) {
            const int v6_only = wildcard_address(address) ? 0 : 1;
            (void)::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6_only, sizeof v6_only);
        }
        if (exasock) {
            if (auto enabled = enable_exasock_socket(fd); !enabled) {
                last = enabled.error();
                ::close(fd); fd = -1; continue;
            }
        }
        if (::bind(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
            last.detail = std::string("bind: ") + std::strerror(errno);
            ::close(fd); fd = -1; continue;
        }
        if (auto ready = prepare_listener(fd, backlog, nonblocking); !ready) {
            last = ready.error();
            ::close(fd); fd = -1; continue;
        }
        break;
    }
    ::freeaddrinfo(found);
    if (fd < 0) return std::unexpected(std::move(last));
    return fd;
}

Result<int> make_unix_listener(std::string_view path, std::uint32_t mode, unsigned backlog,
                               bool nonblocking) {
    if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path))
        return err(Errc::invalid_argument, "Unix socket path is empty or too long");
    const std::string owned(path);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, owned.c_str(), owned.size() + 1);
    const socklen_t length = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + owned.size() + 1);
    struct stat existing{};
    if (::lstat(owned.c_str(), &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode))
            return err(Errc::invalid_argument,
                       "refusing to replace non-socket Unix path: " + owned);
        const int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (probe < 0)
            return err(Errc::io_error,
                       std::string("probe Unix socket: ") + std::strerror(errno));
        const int connected = ::connect(probe, reinterpret_cast<sockaddr*>(&address), length);
        const int connect_error = errno;
        ::close(probe);
        if (connected == 0)
            return err(Errc::invalid_argument, "Unix socket is already active: " + owned);
        if (connect_error != ECONNREFUSED && connect_error != ENOENT)
            return err(Errc::io_error,
                       std::string("refusing to replace unverified Unix socket: ") +
                           std::strerror(connect_error));
        if (::unlink(owned.c_str()) < 0)
            return err(Errc::io_error,
                       std::string("unlink old Unix socket: ") + std::strerror(errno));
    } else if (errno != ENOENT) {
        return err(Errc::io_error, std::string("inspect Unix socket: ") + std::strerror(errno));
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return err(Errc::io_error, std::string("Unix socket: ") + std::strerror(errno));
    const mode_t old_mask = ::umask(0777);
    const int bound = ::bind(fd, reinterpret_cast<sockaddr*>(&address), length);
    ::umask(old_mask);
    if (bound < 0) {
        const int saved = errno;
        ::close(fd);
        return err(Errc::io_error, std::string("bind Unix socket: ") + std::strerror(saved));
    }
    if (::chmod(owned.c_str(), static_cast<mode_t>(mode)) < 0) {
        const int saved = errno;
        ::close(fd); ::unlink(owned.c_str());
        return err(Errc::io_error, std::string("chmod Unix socket: ") + std::strerror(saved));
    }
    if (auto ready = prepare_listener(fd, backlog, nonblocking); !ready) {
        ::close(fd); ::unlink(owned.c_str());
        return std::unexpected(ready.error());
    }
    return fd;
}

} // namespace goblin::net
