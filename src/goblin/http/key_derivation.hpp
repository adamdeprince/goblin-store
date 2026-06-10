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
