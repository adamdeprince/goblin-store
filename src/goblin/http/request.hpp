// HTTP/1.1 request parsing (ADR-0005): incremental, zero-copy over the recv buffer via picohttpparser.
// Extracts only what Goblin needs to serve an object — method, request-target, Host, Range, keep-alive
// — plus RFC 7233 single-range parse/resolve. Pure logic, no I/O; the front-end derives the key
// (ADR-0015) from target+host and serves through the tier manager.
#pragma once

#include "goblin/common/types.hpp"

#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace goblin::http {

enum class Method { get, head, other };

using RangeSpec = ByteRange; // RFC 7233 single byte-range (defined in common/types.hpp)

struct Request {
    struct Header {
        std::string_view name;
        std::string_view value;
    };
    Method method = Method::other;
    std::string_view target;        // request-target, origin-form "/path?query" (views into the buffer)
    std::string_view host;          // raw Host header (may include :port); empty if absent
    std::optional<RangeSpec> range; // present iff a parseable single "bytes=" Range header was sent
    std::string_view if_none_match; // raw If-None-Match header (conditional GET); empty if absent
    bool keep_alive = true;         // HTTP/1.1 default; false on "Connection: close"
    int minor_version = 1;          // HTTP/1.<minor>
    // Populated only when parse_request(..., capture_headers=true). Views have the same lifetime as
    // target/host and let mirror mode apply forwarding, Vary, and request cache-control rules without
    // imposing allocations on the ordinary object-serving hot path.
    std::vector<Header> headers;
};

enum class ParseStatus { ok, partial, bad };

struct ParseResult {
    ParseStatus status = ParseStatus::partial;
    std::size_t consumed = 0; // bytes of the request head (request line + headers + CRLFCRLF); 0 unless ok
    Request req;
};

// Parse one request head from the front of `buf`. `prev_len` is the buffer length at the previous
// call for this connection (picohttpparser resumes its scan there); pass 0 the first time.
ParseResult parse_request(std::string_view buf, std::size_t prev_len = 0,
                          bool capture_headers = false);

// Parse a Range header value: "bytes=a-b" / "bytes=a-" / "bytes=-n". nullopt if it's not a single
// bytes range (multi-range, non-bytes unit, or malformed). Exposed for testing.
std::optional<RangeSpec> parse_range(std::string_view value);

// Resolve a RangeSpec against a known object size -> (offset, length), clamped per RFC 7233.
// nullopt means unsatisfiable (the caller answers 416). Exposed for testing; used by the serving path.
std::optional<std::pair<Offset, Size>> resolve_range(const RangeSpec&, Size object_size);

// Content-Type for an object, derived from its key's file extension (case-insensitive; any query
// suffix and directory part are ignored). The key already reflects the directory-index mapping, so
// `GET /blog/` -> `.../index.html` -> text/html. Unknown/missing extension -> application/octet-stream.
std::string_view content_type_for(std::string_view key);

} // namespace goblin::http
