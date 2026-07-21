// Memcache basic and meta text protocol parsing (ADR-0005). Parsing a command LINE is pure logic;
// wire framing and streaming are the connection backend's job.
#pragma once

#include "goblin/common/error.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace goblin::memcache {

enum class Verb {
    get, gets, gat, gats,
    set, add, replace, append, prepend, cas,
    touch, incr, decr, del, flush_all,
    version, quit, stats, unknown
};
enum class StatsVerb { read, reset, settings };

struct Command {
    Verb verb = Verb::unknown;
    std::string_view key;      // view into the parsed line; first key for get/gets
    // Additional get/gets keys, also viewing the parsed line. Kept separate so the overwhelmingly
    // common single-key request neither allocates nor changes its hot-path representation.
    std::vector<std::string_view> extra_keys;
    std::uint32_t flags = 0;
    std::int64_t exptime = 0;  // 0 = never; negative classic exptime = immediately expired
    std::uint64_t bytes = 0;   // payload length, for storage commands
    std::uint64_t cas = 0;     // expected CAS for the `cas` command (0 otherwise)
    std::uint64_t delta = 0;   // incr/decr amount
    std::uint32_t delay = 0;   // flush_all delay
    StatsVerb stats_verb = StatsVerb::read;
    bool noreply = false;

    bool is_storage() const noexcept {
        return verb == Verb::set || verb == Verb::add || verb == Verb::replace ||
               verb == Verb::append || verb == Verb::prepend || verb == Verb::cas;
    }
};

// Parse a single command line (WITHOUT the trailing CRLF). `key` views into `line`.
Result<Command> parse_command(std::string_view line);

// ---- meta text protocol (mn/mg/ms/md/ma/me) ----
enum class MetaVerb { mn, mg, ms, md, ma, me, unknown };

// A parsed meta command. Flag tokens are <letter>[arg]; `key`/`opaque` view into `line`.
struct MetaCommand {
    MetaVerb verb = MetaVerb::unknown;
    std::string_view key;      // wire token; lookup_key() selects its decoded form when b is set
    std::uint64_t datalen = 0; // ms: data block length
    // requested return data (mg; echoed on ms):
    bool rf_value = false, rf_flags = false, rf_size = false, rf_ttl = false, rf_cas = false,
         rf_key = false, rf_hit = false, rf_last_access = false;
    std::string return_order;                                  // requested response flags, wire order
    bool quiet = false;                                      // q: suppress miss (mg) / success (ms)
    bool no_update = false;                                  // u: do not touch LRU/access metadata
    bool key_binary = false;                                 // b: key token is strict base64
    std::string decoded_key;                                 // populated for b, may contain NUL bytes
    bool has_set_flags = false; std::uint32_t set_flags = 0; // F (ms)
    bool has_ttl = false;       std::int64_t  ttl = 0;       // T (ms set TTL / mg+md touch)
    bool has_cas = false;       std::uint64_t cas = 0;       // C (compare-and-swap)
    char mode = 'S';                                         // M (ms): S set, E add, R replace
    bool has_vivify = false;    std::int64_t vivify_ttl = 0; // N: miss stub/autocreate TTL
    bool has_recache = false;   std::uint32_t recache_ttl = 0; // R: early refresh threshold
    bool invalidate = false;                                 // I: mark stale (md/ms)
    bool remove_value = false;                               // x: tombstone value (md)
    bool has_initial = false;   std::uint64_t initial = 0;   // J: meta arithmetic initial value
    std::uint64_t delta = 1;                                 // D: meta arithmetic delta
    std::string_view opaque;                                 // O<token>, echoed verbatim

    std::string_view lookup_key() const noexcept {
        return key_binary ? std::string_view(decoded_key) : key;
    }
};

// Parse a meta command LINE (without the trailing CRLF). err(invalid_protocol) on an unknown verb,
// missing key/datalen, or an unsupported flag.
Result<MetaCommand> parse_meta(std::string_view line);

// Convert a memcache exptime to an absolute Unix expiry (0 = never): 0 -> never; <= 30 days ->
// relative (now + exptime); larger -> already an absolute Unix timestamp.
std::uint32_t exptime_to_expiry(std::int64_t exptime, std::uint32_t now);

// Fixed text-protocol responses.
inline constexpr std::string_view kStored = "STORED\r\n";
inline constexpr std::string_view kNotStored = "NOT_STORED\r\n";
inline constexpr std::string_view kExists = "EXISTS\r\n"; // cas: the item changed under us (mismatch)
inline constexpr std::string_view kDeleted = "DELETED\r\n";
inline constexpr std::string_view kNotFound = "NOT_FOUND\r\n";
inline constexpr std::string_view kTouched = "TOUCHED\r\n";
inline constexpr std::string_view kOk = "OK\r\n";
inline constexpr std::string_view kEnd = "END\r\n";
inline constexpr std::string_view kError = "ERROR\r\n";
inline constexpr std::string_view kClientErrorBadCommand = "CLIENT_ERROR bad command line format\r\n";
inline constexpr std::string_view kClientErrorUnauthenticated = "CLIENT_ERROR unauthenticated\r\n";
inline constexpr std::string_view kClientErrorAuthTooLong = "CLIENT_ERROR auth token too long\r\n";
inline constexpr std::string_view kClientErrorAuthFormat =
    "CLIENT_ERROR bad authentication token format\r\n";
inline constexpr std::string_view kClientErrorAuthFailure =
    "CLIENT_ERROR authentication failure\r\n";
inline constexpr std::string_view kClientErrorBadTermination =
    "CLIENT_ERROR bad command line termination\r\n";
inline constexpr std::string_view kClientErrorNonNumeric =
    "CLIENT_ERROR cannot increment or decrement non-numeric value\r\n";
inline constexpr std::string_view kBadDataChunk = "CLIENT_ERROR bad data chunk\r\n";
inline constexpr std::string_view kServerErrorStorage = "SERVER_ERROR storage I/O failure\r\n";
inline constexpr std::string_view kServerErrorTooLarge = "SERVER_ERROR object too large\r\n";
inline constexpr std::string_view kServerErrorOutOfSpace = "SERVER_ERROR out of space\r\n";
inline constexpr std::string_view kServerErrorReadOnly = "SERVER_ERROR storage is read-only\r\n";
inline constexpr std::string_view kServerErrorOutOfMemory = "SERVER_ERROR out of memory\r\n";
inline constexpr std::string_view kReset = "RESET\r\n";
inline constexpr std::string_view kVersion = "VERSION goblin-store 0.0.3\r\n";

// Bare ERROR is reserved for an unknown/unsupported command. A recognized command with malformed
// syntax is a client error. Storage-condition failures are NOT_STORED/NS; actual inability to store
// is a server error.
std::string_view command_parse_error_reply(const Error&) noexcept;
std::string_view storage_failure_reply(Errc) noexcept;
std::string_view commit_failure_reply(Errc, bool meta) noexcept;

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
