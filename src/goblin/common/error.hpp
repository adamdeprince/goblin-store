// Error handling: std::expected-based, exception-free hot paths (ADR-0004).
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace goblin {

enum class Errc : std::uint16_t {
    ok = 0,
    not_found,
    already_exists,
    out_of_space,         // a disk tier is full (relevant in BLOCK mode, ADR-0010)
    out_of_memory,        // RAM budget exhausted (ADR-0008)
    too_large,            // object exceeds the configured maximum
    invalid_argument,     // bad config / bad API use
    invalid_protocol,     // malformed request on the wire
    io_error,
    unsupported,
    backpressure_timeout, // BLOCK-mode write waited too long for space
};

std::string_view to_string(Errc) noexcept;

struct Error {
    Errc code = Errc::ok;
    std::string detail;   // optional human context; kept empty on hot paths

    Error() = default;
    Error(Errc c, std::string d = {}) : code(c), detail(std::move(d)) {}
};

// Result<T> for value-returning calls; Status for void calls.
template <class T>
using Result = std::expected<T, Error>;
using Status = std::expected<void, Error>;

inline std::unexpected<Error> err(Errc c, std::string detail = {}) {
    return std::unexpected(Error{c, std::move(detail)});
}

} // namespace goblin
