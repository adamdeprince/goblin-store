// HTTP key derivation (ADR-0015): turn a request (or a --source file) into a Goblin key.
// One rule, two modes, used by BOTH the HTTP front-end and the --source preloader so their
// keys line up exactly. Pure logic — no I/O.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace goblin::http {

enum class KeyMode {
    path,  // key = canonical URI path                (leading '/')
    vhost, // key = normalized Host + canonical path   (host first, NO leading '/')
};

struct KeyOptions {
    KeyMode mode = KeyMode::path;
    bool keep_query = false; // false => strip the query string (default, ADR-0015)
    // path mode only: drop the canonical key's leading '/', so HTTP `GET /foo` and a raw memcache
    // `set foo` resolve to the same key (default off => key is the literal path, '/foo'). vhost mode
    // is unaffected — there the leading '/' separates host from path (`example.com/foo`).
    bool strip_leading_slash = false;
    // HTTP directory index (request path only): when non-empty, a request whose path ends in '/'
    // (e.g. '/' or '/blog/') has this filename appended before canonicalization, so `GET /` resolves
    // to the `<index_name>` object. Honoured only in derive_key, which the HTTP/HTTPS loops call —
    // memcache uses raw keys (never derive_key) and the --source preloader leaves this empty, so the
    // mapping is HTTP-request-only.
    std::string index_name = {}; // default member init so partial aggregate inits don't warn
};

// Derive the key for an HTTP request. `host` is the raw Host header (may carry a port); it is
// used only in vhost mode. Returns nullopt in vhost mode if the host is empty.
std::optional<std::string> derive_key(std::string_view host, std::string_view uri,
                                      const KeyOptions& opt);

// Derive the key for a file at `relpath` (relative to a --source root), per the same rule:
//   path  : "/" + relpath              vhost : relpath (first component is the host)
// Filesystem names are literal, so relpaths are NOT percent-decoded.
std::string derive_key_from_relpath(std::string_view relpath, const KeyOptions& opt);

// --- canonicalization helpers (exposed for testing) ---
std::string percent_decode(std::string_view s);
std::string canonical_path(std::string_view path, bool decode = true); // collapse //, resolve ./..
std::string normalize_host(std::string_view host);                     // lowercase, strip :port + trailing '.'

} // namespace goblin::http
