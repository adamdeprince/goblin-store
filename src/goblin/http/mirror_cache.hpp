#pragma once

#include "goblin/common/error.hpp"
#include "goblin/http/request.hpp"
#include "goblin/storage/index.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace goblin::http {

struct OwnedHeader {
    std::string name;
    std::string value;
};

struct RequestCachePolicy {
    bool no_store = false;
    bool no_cache = false;
    bool only_if_cached = false;
    bool has_authorization = false;
    std::optional<std::uint64_t> max_age;
    std::uint64_t min_fresh = 0;
    std::optional<std::uint64_t> max_stale; // nullopt absent; UINT64_MAX means no limit
};

struct OriginResponseHead {
    unsigned status = 0;
    std::string reason;
    std::vector<OwnedHeader> headers;
    std::optional<Size> content_length;
};

struct CacheDecision {
    bool cacheable = false;
    std::string why_not;
    std::shared_ptr<storage::HttpCacheMetadata> metadata;
};

// Append an origin-form request target to a configured mirror base. The base path is always treated
// as a directory prefix: base http://h/z plus /a?q=1 becomes http://h/z/a?q=1. The configured scheme
// is preserved and an origin-form target can never replace the configured authority.
Result<std::string> mirror_origin_url(std::string_view base, std::string_view target);

// Conservative representation key: URI (including query) plus all end-to-end request fields which
// can select a representation. Over-keying before the response's Vary list is known is safe; cache
// directives, conditionals, framing, and hop-by-hop fields are deliberately excluded.
std::string mirror_cache_key(std::string_view target, std::span<const Request::Header> headers);

std::vector<OwnedHeader> mirror_forward_headers(std::span<const Request::Header> headers);
RequestCachePolicy request_cache_policy(std::span<const Request::Header> headers);

CacheDecision evaluate_cacheability(const OriginResponseHead&, const RequestCachePolicy&,
                                    std::uint64_t request_time, std::uint64_t response_time);
bool cache_is_fresh(const storage::HttpCacheMetadata&, const RequestCachePolicy&, std::uint64_t now);
bool cache_allows_stale_on_error(const storage::HttpCacheMetadata&, std::uint64_t now);

std::optional<std::string_view> header_value(std::span<const OwnedHeader>, std::string_view name);

} // namespace goblin::http
