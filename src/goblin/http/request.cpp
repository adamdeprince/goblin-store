#include "goblin/http/request.hpp"

#include "picohttpparser.h"

#include <algorithm>
#include <array>
#include <charconv>

namespace goblin::http {
namespace {

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

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

    struct Entry { std::string_view ext, type; };
    static constexpr Entry kTable[] = {
        {"html", "text/html; charset=utf-8"},   {"htm", "text/html; charset=utf-8"},
        {"css", "text/css; charset=utf-8"},      {"js", "text/javascript; charset=utf-8"},
        {"mjs", "text/javascript; charset=utf-8"}, {"json", "application/json; charset=utf-8"},
        {"map", "application/json; charset=utf-8"}, {"xml", "application/xml; charset=utf-8"},
        {"txt", "text/plain; charset=utf-8"},    {"csv", "text/csv; charset=utf-8"},
        {"md", "text/markdown; charset=utf-8"},  {"svg", "image/svg+xml; charset=utf-8"},
        {"png", "image/png"},                    {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"},                  {"gif", "image/gif"},
        {"webp", "image/webp"},                  {"avif", "image/avif"},
        {"ico", "image/x-icon"},                 {"bmp", "image/bmp"},
        {"woff", "font/woff"},                   {"woff2", "font/woff2"},
        {"ttf", "font/ttf"},                     {"otf", "font/otf"},
        {"eot", "application/vnd.ms-fontobject"}, {"wasm", "application/wasm"},
        {"pdf", "application/pdf"},               {"zip", "application/zip"},
        {"gz", "application/gzip"},               {"mp4", "video/mp4"},
        {"webm", "video/webm"},                  {"ogg", "audio/ogg"},
        {"mp3", "audio/mpeg"},                    {"wav", "audio/wav"},
        {"ics", "text/calendar; charset=utf-8"},
    };
    for (const auto& e : kTable)
        if (e.ext == ext) return e.type;
    return "application/octet-stream";
}

ParseResult parse_request(std::string_view buf, std::size_t prev_len) {
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

    for (std::size_t i = 0; i < num_headers; ++i) {
        const std::string_view name(headers[i].name, headers[i].name_len);
        const std::string_view value(headers[i].value, headers[i].value_len);
        if (iequals(name, "host")) req.host = trim(value);
        else if (iequals(name, "range")) req.range = parse_range(value);
        else if (iequals(name, "if-none-match")) req.if_none_match = trim(value);
        else if (iequals(name, "connection")) req.keep_alive = !iequals(trim(value), "close");
    }

    out.status = ParseStatus::ok;
    out.consumed = static_cast<std::size_t>(rc);
    out.req = req;
    return out;
}

} // namespace goblin::http
