#include "goblin/http/key_derivation.hpp"

#include <array>
#include <cctype>
#include <string>
#include <vector>

namespace goblin::http {

namespace {
int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// True when `path` is already a canonical absolute path: leading '/', no percent-encoding, no
// empty segments (`//`), and no `.` / `..` segments. The common CDN-style request hits this path
// and can skip percent_decode + stack walk entirely.
bool is_simple_canonical_path(std::string_view path) {
    if (path.empty() || path.front() != '/') return false;
    for (std::size_t i = 0; i < path.size(); ++i) {
        const char c = path[i];
        if (c == '%') return false;
        if (c != '/') continue;
        // Inspect the segment that starts after this slash.
        const std::size_t seg = i + 1;
        if (seg >= path.size()) return true; // trailing '/' is fine for the simple check caller
        if (path[seg] == '/') return false;  // empty segment (`//`)
        if (path[seg] != '.') continue;
        // `/./` or trailing `/.`
        const std::size_t after_dot = seg + 1;
        if (after_dot >= path.size() || path[after_dot] == '/') return false;
        // `/../` or trailing `/..`
        if (path[after_dot] == '.' && (after_dot + 1 >= path.size() || path[after_dot + 1] == '/'))
            return false;
    }
    return true;
}
} // namespace

std::string percent_decode(std::string_view s) {
    // Fast path: nothing to decode — still returns a string (callers own the result), but skips
    // the scan-and-push loop when the input is already literal.
    if (s.find('%') == std::string_view::npos) return std::string(s);
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hexval(s[i + 1]);
            const int lo = hexval(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

std::string canonical_path(std::string_view path, bool decode) {
    // Only allocate a decoded buffer when percent-encoding is present.
    std::string decoded_storage;
    std::string_view sv = path;
    if (decode) {
        if (path.find('%') != std::string_view::npos) {
            decoded_storage = percent_decode(path);
            sv = decoded_storage;
        }
    }

    // Stack of path segments on the stack for typical depths; spill to the heap only if needed.
    std::array<std::string_view, 64> stack_buf{};
    std::vector<std::string_view> stack_heap;
    std::size_t depth = 0;
    auto push = [&](std::string_view seg) {
        if (depth < stack_buf.size() && stack_heap.empty()) {
            stack_buf[depth++] = seg;
            return;
        }
        if (stack_heap.empty()) {
            stack_heap.assign(stack_buf.begin(), stack_buf.begin() + static_cast<std::ptrdiff_t>(depth));
        }
        stack_heap.push_back(seg);
        depth = stack_heap.size();
    };
    auto pop = [&]() {
        if (!stack_heap.empty()) {
            stack_heap.pop_back();
            depth = stack_heap.size();
        } else if (depth) {
            --depth;
        }
    };
    auto at = [&](std::size_t i) -> std::string_view {
        return stack_heap.empty() ? stack_buf[i] : stack_heap[i];
    };

    std::size_t start = 0;
    while (true) {
        const std::size_t slash = sv.find('/', start);
        const std::string_view seg =
            (slash == std::string_view::npos) ? sv.substr(start) : sv.substr(start, slash - start);
        if (seg == "..") {
            if (depth) pop(); // cannot escape root
        } else if (!seg.empty() && seg != ".") {
            push(seg); // empty segments collapse duplicate '/'
        }
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    std::string out = "/";
    for (std::size_t k = 0; k < depth; ++k) {
        if (k) out.push_back('/');
        const std::string_view s = at(k);
        out.append(s.data(), s.size());
    }
    return out;
}

std::string normalize_host(std::string_view host) {
    std::string_view h = host;
    if (const auto c = h.find(':'); c != std::string_view::npos) h = h.substr(0, c); // strip :port
    while (!h.empty() && h.back() == '.') h.remove_suffix(1);                         // trailing dot
    std::string out(h);
    for (char& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

std::optional<std::string> derive_key(std::string_view host, std::string_view uri,
                                      const KeyOptions& opt) {
    std::string_view u = uri;
    if (const auto f = u.find('#'); f != std::string_view::npos) u = u.substr(0, f); // drop fragment

    std::string_view path_part = u;
    std::string_view query_part;
    if (const auto q = u.find('?'); q != std::string_view::npos) {
        path_part = u.substr(0, q);
        query_part = u.substr(q); // includes the leading '?'
    }

    // Directory index: a path ending in '/' (or an empty target) names a directory -> serve the
    // configured index file. Appended before canonicalization so dot-segments still resolve.
    const bool want_index =
        !opt.index_name.empty() && (path_part.empty() || path_part.back() == '/');

    // Hot path: already-canonical absolute path, no dir-index rewrite. Skip decode + segment walk.
    if (!want_index && is_simple_canonical_path(path_part)) {
        std::string key;
        if (opt.mode == KeyMode::vhost) {
            const std::string h = normalize_host(host);
            if (h.empty()) return std::nullopt;
            key.reserve(h.size() + path_part.size() +
                        (opt.keep_query ? query_part.size() : 0));
            key = h;
            key.append(path_part.data(), path_part.size());
        } else if (opt.strip_leading_slash) {
            // path_part is "/..." (or "/"); drop the leading '/'.
            key.assign(path_part.data() + 1, path_part.size() - 1);
        } else {
            key.assign(path_part.data(), path_part.size());
        }
        if (opt.keep_query && !query_part.empty())
            key.append(query_part.data(), query_part.size());
        return key;
    }

    std::string key;
    if (want_index) {
        std::string dir(path_part);
        dir += opt.index_name; // '/' -> '/index.html', '/blog/' -> '/blog/index.html'
        key = canonical_path(dir, /*decode=*/true);
    } else {
        key = canonical_path(path_part, /*decode=*/true);
    }
    if (opt.keep_query && !query_part.empty()) key.append(query_part.data(), query_part.size());

    if (opt.mode == KeyMode::vhost) {
        const std::string h = normalize_host(host);
        if (h.empty()) return std::nullopt; // vhost requires a Host header
        return h + key;                     // host first; `key` carries the leading '/'
    }
    if (opt.strip_leading_slash) return key.substr(1); // 'foo' instead of '/foo' (canonical_path => '/...')
    return key;
}

std::string derive_key_from_relpath(std::string_view relpath, const KeyOptions& opt) {
    if (opt.mode == KeyMode::vhost) {
        const std::size_t slash = relpath.find('/');
        const std::string_view host =
            (slash == std::string_view::npos) ? relpath : relpath.substr(0, slash);
        const std::string_view rest =
            (slash == std::string_view::npos) ? std::string_view{} : relpath.substr(slash);
        const std::string h = normalize_host(host);
        const std::string path = rest.empty() ? std::string("/") : canonical_path(rest, /*decode=*/false);
        return h + path;
    }
    std::string p = "/";
    p.append(relpath.data(), relpath.size());
    std::string key = canonical_path(p, /*decode=*/false); // filesystem names are literal — never percent-decode
    if (opt.strip_leading_slash) return key.substr(1); // line up with HTTP `GET /<relpath>` under the same flag
    return key;
}

} // namespace goblin::http
