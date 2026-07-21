#pragma once

#include "goblin/common/error.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace goblin::net {

// Bind an exact numeric IPv4/IPv6 stream listener. `::` is deliberately dual-stack when the
// platform permits it; exact IPv6 addresses remain IPv6-only.
Result<int> make_tcp_listener(std::string_view address, std::uint16_t port, unsigned backlog,
                              bool exasock = false, bool nonblocking = false);

// Bind an AF_UNIX stream listener. An existing socket at `path` is replaced, but a non-socket is
// never removed. The caller owns both the descriptor and unlinking the pathname at shutdown.
Result<int> make_unix_listener(std::string_view path, std::uint32_t mode, unsigned backlog,
                               bool nonblocking = false);

bool wildcard_address(std::string_view address) noexcept;
bool loopback_address(std::string_view address) noexcept;
std::string format_endpoint(std::string_view address, std::uint16_t port);

} // namespace goblin::net
