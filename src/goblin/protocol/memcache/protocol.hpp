// memcache text protocol (ADR-0005): the implemented subset only — get/gets, set/add/replace/cas,
// delete, version, quit. No incr/decr. CAS is supported (gets returns it, cas stores on a match); the
// CAS value is the object's per-store generation (ObjectMeta::etag). Parsing a command LINE is pure
// logic (here); wire framing (reading the data block for storage commands) is the connection's job.
#pragma once

#include "goblin/common/error.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace goblin::memcache {

enum class Verb { get, gets, set, add, replace, cas, del, version, quit, stats, unknown };

struct Command {
    Verb verb = Verb::unknown;
    std::string_view key;      // view into the parsed line; first key only (v1: no multi-get)
    std::uint32_t flags = 0;
    std::uint32_t exptime = 0; // 0 = never
    std::uint64_t bytes = 0;   // payload length, for storage commands
    std::uint64_t cas = 0;     // expected CAS for the `cas` command (0 otherwise)
    bool noreply = false;

    bool is_storage() const noexcept {
        return verb == Verb::set || verb == Verb::add || verb == Verb::replace || verb == Verb::cas;
    }
};

// Parse a single command line (WITHOUT the trailing CRLF). `key` views into `line`.
Result<Command> parse_command(std::string_view line);

// ---- meta text protocol (mn/mg/ms/md), the "Core + CAS" subset ----
enum class MetaVerb { mn, mg, ms, md, unknown };

// A parsed meta command. Flag tokens are <letter>[arg]; `key`/`opaque` view into `line`.
struct MetaCommand {
    MetaVerb verb = MetaVerb::unknown;
    std::string_view key;      // not base64-decoded ('b' is unsupported)
    std::uint64_t datalen = 0; // ms: data block length
    // requested return data (mg; echoed on ms):
    bool rf_value = false, rf_flags = false, rf_size = false, rf_ttl = false, rf_cas = false,
         rf_key = false;
    bool quiet = false;                                      // q: suppress miss (mg) / success (ms)
    bool has_set_flags = false; std::uint32_t set_flags = 0; // F (ms)
    bool has_ttl = false;       std::int64_t  ttl = 0;       // T (ms set TTL / mg+md touch)
    bool has_cas = false;       std::uint64_t cas = 0;       // C (compare-and-swap)
    char mode = 'S';                                         // M (ms): S set, E add, R replace
    std::string_view opaque;                                 // O<token>, echoed verbatim
};

// Parse a meta command LINE (without the trailing CRLF). err(invalid_protocol) on an unknown verb,
// missing key/datalen, or an unsupported flag.
Result<MetaCommand> parse_meta(std::string_view line);

// Convert a memcache exptime to an absolute Unix expiry (0 = never): 0 -> never; <= 30 days ->
// relative (now + exptime); larger -> already an absolute Unix timestamp.
std::uint32_t exptime_to_expiry(std::uint32_t exptime, std::uint32_t now);

// Fixed text-protocol responses.
inline constexpr std::string_view kStored = "STORED\r\n";
inline constexpr std::string_view kNotStored = "NOT_STORED\r\n";
inline constexpr std::string_view kExists = "EXISTS\r\n"; // cas: the item changed under us (mismatch)
inline constexpr std::string_view kDeleted = "DELETED\r\n";
inline constexpr std::string_view kNotFound = "NOT_FOUND\r\n";
inline constexpr std::string_view kEnd = "END\r\n";
inline constexpr std::string_view kError = "ERROR\r\n";
inline constexpr std::string_view kBadDataChunk = "CLIENT_ERROR bad data chunk\r\n";
inline constexpr std::string_view kVersion = "VERSION goblin-store 0.0.2\r\n";

// Fixed meta-protocol replies (the flagged forms — VA/HD with return flags — are built dynamically).
inline constexpr std::string_view kMetaNoop = "MN\r\n";      // mn
inline constexpr std::string_view kMetaMiss = "EN\r\n";      // mg miss
inline constexpr std::string_view kMetaOk = "HD\r\n";        // stored / deleted (no return flags)
inline constexpr std::string_view kMetaNotStored = "NS\r\n"; // ms mode condition not met
inline constexpr std::string_view kMetaExists = "EX\r\n";    // cas mismatch
inline constexpr std::string_view kMetaNotFound = "NF\r\n";  // absent (md / cas on missing)

// "VALUE <key> <flags> <bytes>\r\n" — followed on the wire by <data>\r\n.
// Prefer the append_* forms on the hot path (no temporary std::string).
void append_value_header(std::string& out, std::string_view key, std::uint32_t flags,
                         std::uint64_t bytes);
void append_value_header_cas(std::string& out, std::string_view key, std::uint32_t flags,
                             std::uint64_t bytes, std::uint64_t cas);
std::string value_header(std::string_view key, std::uint32_t flags, std::uint64_t bytes);
// "VALUE <key> <flags> <bytes> <cas>\r\n" — the gets/cas form, carrying the CAS unique.
std::string value_header_cas(std::string_view key, std::uint32_t flags, std::uint64_t bytes,
                             std::uint64_t cas);

} // namespace goblin::memcache
