#include "goblin/http/mirror_cache.hpp"

#include "goblin/common/types.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <limits>
#include <set>

#if GOBLIN_HAVE_CURL
#include <curl/curl.h>
#endif

namespace goblin::http {
namespace {

char ascii_lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string lower(std::string_view value) {
    std::string out(value);
    for (char& c : out) c = ascii_lower(c);
    return out;
}

std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return value;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    return true;
}

bool token_list_contains(std::string_view list, std::string_view wanted) {
    while (!list.empty()) {
        const auto comma = list.find(',');
        const std::string_view token = trim(list.substr(0, comma));
        if (iequals(token, wanted)) return true;
        if (comma == std::string_view::npos) break;
        list.remove_prefix(comma + 1);
    }
    return false;
}

std::optional<std::uint64_t> u64(std::string_view value) {
    value = trim(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value.remove_prefix(1);
        value.remove_suffix(1);
    }
    std::uint64_t parsed = 0;
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (ec != std::errc{} || end != value.data() + value.size()) return std::nullopt;
    return parsed;
}

std::uint64_t saturating_add(std::uint64_t a, std::uint64_t b) noexcept {
    return a > std::numeric_limits<std::uint64_t>::max() - b
        ? std::numeric_limits<std::uint64_t>::max() : a + b;
}

struct Directives {
    bool no_store = false;
    bool no_cache = false;
    bool private_ = false;
    bool public_ = false;
    bool must_revalidate = false;
    bool proxy_revalidate = false;
    bool only_if_cached = false;
    std::optional<std::uint64_t> max_age;
    std::optional<std::uint64_t> s_maxage;
    std::optional<std::uint64_t> min_fresh;
    std::optional<std::uint64_t> max_stale;
    std::optional<std::uint64_t> stale_if_error;
};

Directives parse_directives(std::string_view value) {
    Directives out;
    while (!value.empty()) {
        const auto comma = value.find(',');
        std::string_view item = trim(value.substr(0, comma));
        const auto equals = item.find('=');
        const std::string name = lower(trim(item.substr(0, equals)));
        const std::string_view argument = equals == std::string_view::npos
            ? std::string_view{} : trim(item.substr(equals + 1));
        if (name == "no-store") out.no_store = true;
        else if (name == "no-cache") out.no_cache = true;
        else if (name == "private") out.private_ = true;
        else if (name == "public") out.public_ = true;
        else if (name == "must-revalidate") out.must_revalidate = true;
        else if (name == "proxy-revalidate") out.proxy_revalidate = true;
        else if (name == "only-if-cached") out.only_if_cached = true;
        else if (name == "max-age") out.max_age = u64(argument);
        else if (name == "s-maxage") out.s_maxage = u64(argument);
        else if (name == "min-fresh") out.min_fresh = u64(argument);
        else if (name == "max-stale")
            out.max_stale = equals == std::string_view::npos
                ? std::optional<std::uint64_t>(std::numeric_limits<std::uint64_t>::max())
                : u64(argument);
        else if (name == "stale-if-error") out.stale_if_error = u64(argument);
        if (comma == std::string_view::npos) break;
        value.remove_prefix(comma + 1);
    }
    return out;
}

bool hop_by_hop(std::string_view name) {
    return iequals(name, "connection") || iequals(name, "proxy-connection") ||
           iequals(name, "keep-alive") || iequals(name, "proxy-authenticate") ||
           iequals(name, "proxy-authorization") || iequals(name, "te") ||
           iequals(name, "trailer") || iequals(name, "transfer-encoding") ||
           iequals(name, "upgrade");
}

bool key_excluded(std::string_view name) {
    return hop_by_hop(name) || iequals(name, "host") || iequals(name, "cache-control") ||
           iequals(name, "pragma") || iequals(name, "range") || iequals(name, "if-range") ||
           iequals(name, "if-match") || iequals(name, "if-none-match") ||
           iequals(name, "if-modified-since") || iequals(name, "if-unmodified-since");
}

bool forwarded_but_not_keyed(std::string_view name) {
    return iequals(name, "cache-control") || iequals(name, "pragma") ||
           iequals(name, "range") || iequals(name, "if-range") ||
           iequals(name, "if-match") || iequals(name, "if-none-match") ||
           iequals(name, "if-modified-since") || iequals(name, "if-unmodified-since");
}

std::set<std::string> connection_tokens(std::span<const Request::Header> headers) {
    std::set<std::string> out;
    for (const auto& header : headers) {
        if (!iequals(header.name, "connection")) continue;
        std::string_view value = header.value;
        while (!value.empty()) {
            const auto comma = value.find(',');
            const auto token = trim(value.substr(0, comma));
            if (!token.empty()) out.insert(lower(token));
            if (comma == std::string_view::npos) break;
            value.remove_prefix(comma + 1);
        }
    }
    return out;
}

std::optional<std::uint64_t> date_seconds(std::string_view value) {
#if GOBLIN_HAVE_CURL
    std::string copy(value);
    const auto parsed = ::curl_getdate(copy.c_str(), nullptr);
    if (parsed < 0) return std::nullopt;
    return static_cast<std::uint64_t>(parsed);
#else
    (void)value;
    return std::nullopt;
#endif
}

bool cacheable_by_default(unsigned status) {
    switch (status) {
        case 200: case 203: case 204: case 300: case 301: case 308:
        case 404: case 405: case 410: case 414: case 501: return true;
        default: return false;
    }
}

std::optional<std::string_view> raw_header(std::span<const OwnedHeader> headers,
                                           std::string_view name) {
    for (const auto& header : headers)
        if (iequals(header.name, name)) return header.value;
    return std::nullopt;
}

} // namespace

Result<std::string> mirror_origin_url(std::string_view base, std::string_view target) {
    if (!(base.starts_with("http://") || base.starts_with("https://")))
        return err(Errc::invalid_argument, "mirror origin must use http or https");
    if (target.empty() || target.front() != '/' || target.starts_with("//") ||
        target.find_first_of("\r\n#") != std::string_view::npos)
        return err(Errc::invalid_argument, "mirror request target must be origin-form");
    if (base.find('#') != std::string_view::npos || base.find('?') != std::string_view::npos)
        return err(Errc::invalid_argument, "mirror base URL cannot contain a query or fragment");

    while (base.size() > 1 && base.back() == '/') base.remove_suffix(1);
    std::string out;
    out.reserve(base.size() + target.size());
    out.append(base);
    out.append(target);
    return out;
}

std::string mirror_cache_key(std::string_view target, std::span<const Request::Header> headers) {
    std::vector<std::pair<std::string, std::string>> selected;
    const auto connection = connection_tokens(headers);
    selected.reserve(headers.size());
    for (const auto& header : headers) {
        std::string name = lower(header.name);
        if (key_excluded(name) || connection.contains(name)) continue;
        selected.emplace_back(std::move(name), std::string(trim(header.value)));
    }
    // Header-line order for the same field name can be significant. Group names for a stable key,
    // but preserve the original order of repeated field values rather than sorting those values.
    std::stable_sort(selected.begin(), selected.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    std::string key("\0goblin-mirror-v1\0", 18);
    key.append(target);
    key.push_back('\0');
    for (const auto& [name, value] : selected) {
        key += name;
        key.push_back(':');
        key += value;
        key.push_back('\0');
    }
    return key;
}

std::vector<OwnedHeader> mirror_forward_headers(std::span<const Request::Header> headers) {
    const auto connection = connection_tokens(headers);
    std::vector<OwnedHeader> out;
    out.reserve(headers.size() + 1);
    for (const auto& header : headers) {
        std::string name = lower(header.name);
        if (hop_by_hop(name) || name == "host" || connection.contains(name)) continue;
        out.push_back({std::move(name), std::string(trim(header.value))});
    }
    out.push_back({"via", "1.1 goblin-store"});
    return out;
}

RequestCachePolicy request_cache_policy(std::span<const Request::Header> headers) {
    RequestCachePolicy out;
    for (const auto& header : headers) {
        if (iequals(header.name, "authorization")) out.has_authorization = true;
        if (iequals(header.name, "pragma") && token_list_contains(header.value, "no-cache"))
            out.no_cache = true;
        if (!iequals(header.name, "cache-control")) continue;
        const Directives parsed = parse_directives(header.value);
        out.no_store = out.no_store || parsed.no_store;
        out.no_cache = out.no_cache || parsed.no_cache;
        out.only_if_cached = out.only_if_cached || parsed.only_if_cached;
        if (parsed.max_age) out.max_age = parsed.max_age;
        if (parsed.min_fresh) out.min_fresh = *parsed.min_fresh;
        if (parsed.max_stale) out.max_stale = parsed.max_stale;
    }
    return out;
}

std::optional<std::string_view> header_value(std::span<const OwnedHeader> headers,
                                             std::string_view name) {
    return raw_header(headers, name);
}

CacheDecision evaluate_cacheability(const OriginResponseHead& response,
                                    const RequestCachePolicy& request,
                                    std::uint64_t request_time,
                                    std::uint64_t response_time) {
    CacheDecision out;
    auto reject = [&](std::string why) {
        out.why_not = std::move(why);
        return out;
    };
    if (request.no_store) return reject("request cache-control: no-store");
    if (!response.content_length) return reject("origin response has no Content-Length");
    if (*response.content_length > kMaxObjectSize) return reject("origin response exceeds maximum object size");
    if (response.status == 304) return reject("standalone 304 has no representation body");
    if (response.status == 206) return reject("partial origin responses are not stored");

    Directives cc;
    bool vary_star = false;
    bool vary_unkeyed = false;
    for (const auto& header : response.headers) {
        if (iequals(header.name, "cache-control")) {
            const Directives one = parse_directives(header.value);
            cc.no_store = cc.no_store || one.no_store;
            cc.no_cache = cc.no_cache || one.no_cache;
            cc.private_ = cc.private_ || one.private_;
            cc.public_ = cc.public_ || one.public_;
            cc.must_revalidate = cc.must_revalidate || one.must_revalidate;
            cc.proxy_revalidate = cc.proxy_revalidate || one.proxy_revalidate;
            if (one.max_age) cc.max_age = one.max_age;
            if (one.s_maxage) cc.s_maxage = one.s_maxage;
            if (one.stale_if_error) cc.stale_if_error = one.stale_if_error;
        } else if (iequals(header.name, "vary")) {
            std::string_view value = header.value;
            while (!value.empty()) {
                const auto comma = value.find(',');
                const std::string_view field = trim(value.substr(0, comma));
                vary_star = vary_star || field == "*";
                vary_unkeyed = vary_unkeyed || forwarded_but_not_keyed(field);
                if (comma == std::string_view::npos) break;
                value.remove_prefix(comma + 1);
            }
        }
    }
    if (cc.no_store) return reject("response cache-control: no-store");
    if (cc.private_) return reject("response cache-control: private");
    if (vary_star) return reject("response Vary: *");
    if (vary_unkeyed) return reject("response Vary names a request control field");
    const bool explicit_freshness = cc.s_maxage || cc.max_age || raw_header(response.headers, "expires");
    if (!cacheable_by_default(response.status) && !explicit_freshness && !cc.public_)
        return reject("response status is not cacheable");
    if (request.has_authorization && !(cc.public_ || cc.s_maxage || cc.must_revalidate))
        return reject("authorized response lacks shared-cache permission");

    auto meta = std::make_shared<storage::HttpCacheMetadata>();
    meta->status = static_cast<std::uint16_t>(response.status);
    meta->reason = response.reason;
    meta->response_time = response_time;
    meta->revalidate_always = cc.no_cache;
    meta->must_revalidate = cc.must_revalidate || cc.proxy_revalidate;
    meta->stale_if_error = cc.stale_if_error.value_or(0);

    std::set<std::string> nominated;
    for (const auto& header : response.headers)
        if (iequals(header.name, "connection")) {
            std::string_view value = header.value;
            while (!value.empty()) {
                const auto comma = value.find(',');
                const auto token = trim(value.substr(0, comma));
                if (!token.empty()) nominated.insert(lower(token));
                if (comma == std::string_view::npos) break;
                value.remove_prefix(comma + 1);
            }
        }
    for (const auto& header : response.headers) {
        const std::string name = lower(header.name);
        if (hop_by_hop(name) || nominated.contains(name) || name == "content-length" || name == "age")
            continue;
        meta->headers.push_back({name, std::string(trim(header.value))});
        if (name == "etag") meta->etag = std::string(trim(header.value));
        else if (name == "last-modified") meta->last_modified = std::string(trim(header.value));
    }

    const std::uint64_t date = raw_header(response.headers, "date")
        .and_then(date_seconds).value_or(response_time);
    const std::uint64_t apparent_age = response_time > date ? response_time - date : 0;
    const std::uint64_t age_value = raw_header(response.headers, "age").and_then(u64).value_or(0);
    const std::uint64_t response_delay = response_time > request_time ? response_time - request_time : 0;
    meta->corrected_initial_age =
        saturating_add(std::max(apparent_age, age_value), response_delay);

    if (cc.s_maxage) meta->freshness_lifetime = *cc.s_maxage;
    else if (cc.max_age) meta->freshness_lifetime = *cc.max_age;
    else if (const auto expires = raw_header(response.headers, "expires").and_then(date_seconds))
        meta->freshness_lifetime = *expires > date ? *expires - date : 0;
    else if (const auto modified = raw_header(response.headers, "last-modified").and_then(date_seconds)) {
        // RFC 9111 permits heuristic freshness. Ten percent, capped at 24 hours, is conservative.
        const std::uint64_t apparent = date > *modified ? date - *modified : 0;
        meta->freshness_lifetime = std::min<std::uint64_t>(apparent / 10, 24 * 60 * 60);
    }

    out.cacheable = true;
    out.metadata = std::move(meta);
    return out;
}

bool cache_is_fresh(const storage::HttpCacheMetadata& meta, const RequestCachePolicy& request,
                    std::uint64_t now) {
    if (request.no_cache || meta.revalidate_always) return false;
    const std::uint64_t resident = now > meta.response_time ? now - meta.response_time : 0;
    const std::uint64_t age = saturating_add(meta.corrected_initial_age, resident);
    std::uint64_t usable_lifetime = meta.freshness_lifetime;
    if (request.max_age) usable_lifetime = std::min(usable_lifetime, *request.max_age);
    // RFC freshness is strict: max-age=0 is stale even during the response's first wall-clock
    // second. Avoid subtraction until the strict comparison has proved it cannot underflow.
    if (age < usable_lifetime && request.min_fresh <= usable_lifetime - age) return true;
    if (!request.max_stale || meta.must_revalidate) return false;
    const std::uint64_t staleness = age > usable_lifetime ? age - usable_lifetime : 0;
    return staleness <= *request.max_stale;
}

bool cache_allows_stale_on_error(const storage::HttpCacheMetadata& meta, std::uint64_t now) {
    if (meta.stale_if_error == 0) return false;
    const std::uint64_t resident = now > meta.response_time ? now - meta.response_time : 0;
    const std::uint64_t age = saturating_add(meta.corrected_initial_age, resident);
    const std::uint64_t stale = age > meta.freshness_lifetime ? age - meta.freshness_lifetime : 0;
    return stale <= meta.stale_if_error;
}

} // namespace goblin::http
