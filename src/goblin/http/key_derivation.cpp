#include "goblin/http/key_derivation.hpp"

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
} // namespace

std::string percent_decode(std::string_view s) {
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
    const std::string decoded = decode ? percent_decode(path) : std::string(path);
    const std::string_view sv(decoded);
    std::vector<std::string_view> stack;
    std::size_t start = 0;
    while (true) {
        const std::size_t slash = sv.find('/', start);
        const std::string_view seg =
            (slash == std::string_view::npos) ? sv.substr(start) : sv.substr(start, slash - start);
        if (seg == "..") {
            if (!stack.empty()) stack.pop_back(); // cannot escape root
        } else if (!seg.empty() && seg != ".") {
            stack.push_back(seg); // empty segments collapse duplicate '/'
        }
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    std::string out = "/";
    for (std::size_t k = 0; k < stack.size(); ++k) {
        if (k) out.push_back('/');
        out.append(stack[k].data(), stack[k].size());
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

    std::string key = canonical_path(path_part, /*decode=*/true);
    if (opt.keep_query && !query_part.empty()) key.append(query_part.data(), query_part.size());

    if (opt.mode == KeyMode::vhost) {
        const std::string h = normalize_host(host);
        if (h.empty()) return std::nullopt; // vhost requires a Host header
        return h + key;                     // host first; `key` carries the leading '/'
    }
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
    return canonical_path(p, /*decode=*/false); // filesystem names are literal — never percent-decode
}

} // namespace goblin::http
