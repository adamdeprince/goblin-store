#include "goblin/http/request.hpp"

#include "picohttpparser.h"

#include <algorithm>
#include <array>
#include <charconv>

namespace goblin::http {
namespace {

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// Case-insensitive equality against a lowercase literal (the common call pattern here).
bool ieq_lit(std::string_view a, std::string_view lit_lower) {
    if (a.size() != lit_lower.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lit_lower[i]) return false;
    return true;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

Method method_of(std::string_view m) {
    if (m == "GET") return Method::get;
    if (m == "HEAD") return Method::head;
    return Method::other;
}

bool to_u64(std::string_view s, Offset& out) {
    if (s.empty()) return false;
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && p == s.data() + s.size();
}

} // namespace

std::optional<RangeSpec> parse_range(std::string_view value) {
    value = trim(value);
    constexpr std::string_view kUnit = "bytes=";
    if (!value.starts_with(kUnit)) return std::nullopt; // only the bytes unit
    value.remove_prefix(kUnit.size());
    if (value.find(',') != std::string_view::npos) return std::nullopt; // single range only

    const auto dash = value.find('-');
    if (dash == std::string_view::npos) return std::nullopt;
    const std::string_view lo = trim(value.substr(0, dash));
    const std::string_view hi = trim(value.substr(dash + 1));

    RangeSpec r;
    if (lo.empty()) { // bytes=-n  (last n bytes)
        if (!to_u64(hi, r.a) || r.a == 0) return std::nullopt;
        r.kind = RangeSpec::Kind::suffix;
        return r;
    }
    if (!to_u64(lo, r.a)) return std::nullopt;
    if (hi.empty()) { // bytes=a-  (open-ended)
        r.kind = RangeSpec::Kind::from;
        return r;
    }
    if (!to_u64(hi, r.b) || r.b < r.a) return std::nullopt; // bytes=a-b
    r.kind = RangeSpec::Kind::closed;
    return r;
}

std::optional<std::pair<Offset, Size>> resolve_range(const RangeSpec& r, Size size) {
    switch (r.kind) {
        case RangeSpec::Kind::closed:
            if (r.a >= size) return std::nullopt; // start past EOF -> 416
            return std::pair<Offset, Size>{r.a, std::min<Offset>(r.b, size - 1) - r.a + 1};
        case RangeSpec::Kind::from:
            if (r.a >= size) return std::nullopt; // 416
            return std::pair<Offset, Size>{r.a, size - r.a};
        case RangeSpec::Kind::suffix:
            if (size == 0) return std::nullopt;
            const Offset n = std::min<Offset>(r.a, size); // last n bytes, clamped to the object
            return std::pair<Offset, Size>{size - n, n};
    }
    return std::nullopt;
}

std::string_view content_type_for(std::string_view key) {
    if (const auto q = key.find('?'); q != std::string_view::npos) key = key.substr(0, q); // drop query
    if (const auto s = key.rfind('/'); s != std::string_view::npos) key = key.substr(s + 1); // last segment
    const auto dot = key.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == key.size())
        return "application/octet-stream"; // no extension, dotfile, or trailing dot
    const std::string_view ext_raw = key.substr(dot + 1);
    if (ext_raw.size() > 8) return "application/octet-stream"; // no known type is this long
    std::array<char, 8> buf{};
    for (std::size_t i = 0; i < ext_raw.size(); ++i) buf[i] = lower(ext_raw[i]);
    const std::string_view ext(buf.data(), ext_raw.size());

    // Branch on length first so most unknowns exit after one integer compare; within each bucket
    // the set is tiny (often 1–3), so linear compare is cheaper than a hash.
    switch (ext.size()) {
        case 2:
            if (ext == "js") return "text/javascript; charset=utf-8";
            if (ext == "md") return "text/markdown; charset=utf-8";
            if (ext == "gz") return "application/gzip";
            break;
        case 3:
            if (ext == "css") return "text/css; charset=utf-8";
            if (ext == "htm") return "text/html; charset=utf-8";
            if (ext == "mjs") return "text/javascript; charset=utf-8";
            if (ext == "xml") return "application/xml; charset=utf-8";
            if (ext == "txt") return "text/plain; charset=utf-8";
            if (ext == "csv") return "text/csv; charset=utf-8";
            if (ext == "svg") return "image/svg+xml; charset=utf-8";
            if (ext == "png") return "image/png";
            if (ext == "jpg") return "image/jpeg";
            if (ext == "gif") return "image/gif";
            if (ext == "ico") return "image/x-icon";
            if (ext == "bmp") return "image/bmp";
            if (ext == "ttf") return "font/ttf";
            if (ext == "otf") return "font/otf";
            if (ext == "eot") return "application/vnd.ms-fontobject";
            if (ext == "pdf") return "application/pdf";
            if (ext == "zip") return "application/zip";
            if (ext == "mp4") return "video/mp4";
            if (ext == "ogg") return "audio/ogg";
            if (ext == "mp3") return "audio/mpeg";
            if (ext == "wav") return "audio/wav";
            if (ext == "ics") return "text/calendar; charset=utf-8";
            if (ext == "map") return "application/json; charset=utf-8";
            break;
        case 4:
            if (ext == "html") return "text/html; charset=utf-8";
            if (ext == "json") return "application/json; charset=utf-8";
            if (ext == "jpeg") return "image/jpeg";
            if (ext == "webp") return "image/webp";
            if (ext == "avif") return "image/avif";
            if (ext == "woff") return "font/woff";
            if (ext == "wasm") return "application/wasm";
            if (ext == "webm") return "video/webm";
            break;
        case 5:
            if (ext == "woff2") return "font/woff2";
            break;
        default:
            break;
    }
    return "application/octet-stream";
}

ParseResult parse_request(std::string_view buf, std::size_t prev_len, bool capture_headers) {
    const char* method = nullptr;
    const char* path = nullptr;
    std::size_t method_len = 0, path_len = 0;
    int minor = 0;
    std::array<phr_header, 64> headers{};
    std::size_t num_headers = headers.size();

    const int rc =
        phr_parse_request(buf.data(), buf.size(), &method, &method_len, &path, &path_len, &minor,
                          headers.data(), &num_headers, std::min(prev_len, buf.size()));

    ParseResult out;
    if (rc == -2) { out.status = ParseStatus::partial; return out; }
    if (rc < 0) { out.status = ParseStatus::bad; return out; }

    Request req;
    req.method = method_of(std::string_view(method, method_len));
    req.target = std::string_view(path, path_len);
    req.minor_version = minor;
    req.keep_alive = minor >= 1; // HTTP/1.1 keep-alive by default; HTTP/1.0 closes unless told otherwise
    if (capture_headers) req.headers.reserve(num_headers);

    // Size-first match: only the four headers Goblin cares about. Unknown names exit after one
    // integer compare; known ones compare against a lowercase literal (no dual-tolower).
    for (std::size_t i = 0; i < num_headers; ++i) {
        const std::string_view name(headers[i].name, headers[i].name_len);
        const std::string_view value(headers[i].value, headers[i].value_len);
        if (capture_headers) req.headers.push_back({name, trim(value)});
        switch (name.size()) {
            case 4:
                if (ieq_lit(name, "host")) req.host = trim(value);
                break;
            case 5:
                if (ieq_lit(name, "range")) req.range = parse_range(value);
                break;
            case 10:
                if (ieq_lit(name, "connection")) req.keep_alive = !iequals(trim(value), "close");
                break;
            case 13:
                if (ieq_lit(name, "if-none-match")) req.if_none_match = trim(value);
                break;
            default:
                break;
        }
    }

    out.status = ParseStatus::ok;
    out.consumed = static_cast<std::size_t>(rc);
    out.req = req;
    return out;
}

} // namespace goblin::http
