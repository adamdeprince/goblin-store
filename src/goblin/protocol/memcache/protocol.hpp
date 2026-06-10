// memcache text protocol (ADR-0005): the implemented subset only — get/gets, set/add/replace,
// delete, version, quit. No incr/decr, no CAS-as-coordination. Parsing a command LINE is pure
// logic (here); wire framing (reading the data block for storage commands) is the connection's job.
#pragma once

#include "goblin/common/error.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace goblin::memcache {

enum class Verb { get, gets, set, add, replace, del, version, quit, unknown };

struct Command {
    Verb verb = Verb::unknown;
    std::string_view key;      // view into the parsed line; first key only (v1: no multi-get)
    std::uint32_t flags = 0;
    std::uint32_t exptime = 0; // 0 = never
    std::uint64_t bytes = 0;   // payload length, for storage commands
    bool noreply = false;

    bool is_storage() const noexcept {
        return verb == Verb::set || verb == Verb::add || verb == Verb::replace;
    }
};

// Parse a single command line (WITHOUT the trailing CRLF). `key` views into `line`.
Result<Command> parse_command(std::string_view line);

// Fixed text-protocol responses.
inline constexpr std::string_view kStored = "STORED\r\n";
inline constexpr std::string_view kNotStored = "NOT_STORED\r\n";
inline constexpr std::string_view kDeleted = "DELETED\r\n";
inline constexpr std::string_view kNotFound = "NOT_FOUND\r\n";
inline constexpr std::string_view kEnd = "END\r\n";
inline constexpr std::string_view kError = "ERROR\r\n";
inline constexpr std::string_view kBadDataChunk = "CLIENT_ERROR bad data chunk\r\n";
inline constexpr std::string_view kVersion = "VERSION goblincache 0.0.1\r\n";

// "VALUE <key> <flags> <bytes>\r\n" — followed on the wire by <data>\r\n.
std::string value_header(std::string_view key, std::uint32_t flags, std::uint64_t bytes);

} // namespace goblin::memcache
