#include "mini_test.hpp"

#include "goblin/http/mirror_cache.hpp"

#include <limits>
#include <string>
#include <vector>

using namespace goblin;
using namespace goblin::http;

namespace {

std::vector<Request::Header> request_headers(
    std::initializer_list<std::pair<std::string_view, std::string_view>> input) {
    std::vector<Request::Header> out;
    for (const auto& [name, value] : input) out.push_back({name, value});
    return out;
}

OriginResponseHead response(unsigned status = 200, std::optional<Size> length = Size{123}) {
    OriginResponseHead out;
    out.status = status;
    out.reason = status == 200 ? "OK" : "Result";
    out.content_length = length;
    out.headers = {{"date", "Sun, 06 Nov 1994 08:49:37 GMT"},
                   {"cache-control", "public, max-age=60"},
                   {"etag", "\"origin-tag\""},
                   {"content-type", "text/plain"}};
    return out;
}

} // namespace

TEST("mirror URL: configured scheme/authority/base path are preserved") {
    const auto joined = mirror_origin_url("http://example.com/z", "/a/b.html?q=1");
    CHECK(joined.has_value());
    if (joined) CHECK_EQ(*joined, "http://example.com/z/a/b.html?q=1");

    const auto root = mirror_origin_url("https://example.com/", "/a");
    CHECK(root.has_value());
    if (root) CHECK_EQ(*root, "https://example.com/a");

    CHECK(!mirror_origin_url("https://example.com/z", "//attacker.test/a").has_value());
    CHECK(!mirror_origin_url("https://example.com/z?q=base", "/a").has_value());
}

TEST("mirror key: query and representation headers select entries; cache controls do not") {
    const auto base = request_headers({{"Accept-Language", "en"}, {"Cache-Control", "max-age=0"}});
    const auto same = request_headers({{"cache-control", "no-cache"}, {"accept-language", "en"}});
    const auto french = request_headers({{"Accept-Language", "fr"}});

    CHECK(mirror_cache_key("/a?q=1", base) == mirror_cache_key("/a?q=1", same));
    CHECK(mirror_cache_key("/a?q=1", base) != mirror_cache_key("/a?q=2", base));
    CHECK(mirror_cache_key("/a?q=1", base) != mirror_cache_key("/a?q=1", french));

    const auto repeated_ab = request_headers({{"X-Choice", "a"}, {"X-Choice", "b"}});
    const auto repeated_ba = request_headers({{"X-Choice", "b"}, {"X-Choice", "a"}});
    CHECK(mirror_cache_key("/a", repeated_ab) != mirror_cache_key("/a", repeated_ba));
}

TEST("mirror forwarding strips Host, hop-by-hop fields, and Connection-nominated fields") {
    const auto input = request_headers({{"Host", "frontend.test"},
                                        {"Connection", "keep-alive, X-Hop"},
                                        {"X-Hop", "remove"},
                                        {"Accept", "text/html"},
                                        {"Cookie", "a=b"}});
    const auto out = mirror_forward_headers(input);
    CHECK(!header_value(out, "host").has_value());
    CHECK(!header_value(out, "connection").has_value());
    CHECK(!header_value(out, "x-hop").has_value());
    CHECK(header_value(out, "accept") == std::optional<std::string_view>("text/html"));
    CHECK(header_value(out, "cookie") == std::optional<std::string_view>("a=b"));
    CHECK(header_value(out, "via").has_value());
}

TEST("mirror request cache-control parses shared-cache directives") {
    const auto input = request_headers({{"Cache-Control", "no-cache, only-if-cached, max-age=10, "
                                                          "min-fresh=3, max-stale"},
                                        {"Authorization", "Bearer secret"}});
    const auto policy = request_cache_policy(input);
    CHECK(policy.no_cache);
    CHECK(policy.only_if_cached);
    CHECK(policy.has_authorization);
    CHECK(policy.max_age == std::optional<std::uint64_t>(10));
    CHECK_EQ(policy.min_fresh, std::uint64_t(3));
    CHECK(policy.max_stale ==
          std::optional<std::uint64_t>(std::numeric_limits<std::uint64_t>::max()));
}

TEST("mirror cacheability honors no-store/private/Vary star and known body size") {
    RequestCachePolicy request;
    auto ok = evaluate_cacheability(response(), request, 1'000, 1'001);
    CHECK(ok.cacheable && ok.metadata);

    auto no_length = evaluate_cacheability(response(200, std::nullopt), request, 1'000, 1'001);
    CHECK(!no_length.cacheable);

    CHECK(evaluate_cacheability(response(200, kMaxObjectSize), request, 1'000, 1'001).cacheable);
    CHECK(!evaluate_cacheability(response(200, kMaxObjectSize + 1), request, 1'000, 1'001)
               .cacheable);

    auto standalone_not_modified = response(304, Size{0});
    CHECK(!evaluate_cacheability(standalone_not_modified, request, 1'000, 1'001).cacheable);

    auto no_store_response = response();
    no_store_response.headers.push_back({"cache-control", "no-store"});
    CHECK(!evaluate_cacheability(no_store_response, request, 1'000, 1'001).cacheable);

    auto private_response = response();
    private_response.headers.push_back({"cache-control", "private"});
    CHECK(!evaluate_cacheability(private_response, request, 1'000, 1'001).cacheable);

    auto vary = response();
    vary.headers.push_back({"vary", "Accept-Encoding, *"});
    CHECK(!evaluate_cacheability(vary, request, 1'000, 1'001).cacheable);

    auto vary_range = response();
    vary_range.headers.push_back({"vary", "Range"});
    CHECK(!evaluate_cacheability(vary_range, request, 1'000, 1'001).cacheable);
}

TEST("mirror freshness applies Age, request max-age/min-fresh, and stale-if-error") {
    RequestCachePolicy request;
    auto origin = response();
    origin.headers.push_back({"age", "10"});
    origin.headers.push_back({"cache-control", "max-age=100, stale-if-error=30"});
    auto decision = evaluate_cacheability(origin, request, 1'000, 1'002);
    CHECK(decision.cacheable && decision.metadata);
    if (!decision.metadata) return;
    // corrected initial age is at least Age(10) + response delay(2).
    CHECK(cache_is_fresh(*decision.metadata, request, 1'050));
    CHECK(!cache_is_fresh(*decision.metadata, request, 1'100));
    CHECK(cache_allows_stale_on_error(*decision.metadata, 1'110));
    CHECK(!cache_allows_stale_on_error(*decision.metadata, 1'140));

    RequestCachePolicy strict;
    strict.max_age = 20;
    strict.min_fresh = 5;
    CHECK(!cache_is_fresh(*decision.metadata, strict, 1'020));

    auto immediately_stale = response();
    immediately_stale.headers.push_back({"cache-control", "max-age=0"});
    auto stale_decision = evaluate_cacheability(immediately_stale, request, 2'000, 2'000);
    CHECK(stale_decision.metadata &&
          !cache_is_fresh(*stale_decision.metadata, request, 2'000));

    auto enormous_age = response();
    enormous_age.headers.push_back({"age", "18446744073709551615"});
    auto enormous_decision = evaluate_cacheability(enormous_age, request, 1, 2);
    CHECK(enormous_decision.metadata &&
          !cache_is_fresh(*enormous_decision.metadata, request, 2));
}

TEST("mirror authorized responses require explicit shared-cache permission") {
    RequestCachePolicy request;
    request.has_authorization = true;
    auto unmarked = response();
    unmarked.headers.clear();
    unmarked.headers.push_back({"cache-control", "max-age=60"});
    CHECK(!evaluate_cacheability(unmarked, request, 1'000, 1'001).cacheable);

    unmarked.headers[0].value = "public, max-age=60";
    CHECK(evaluate_cacheability(unmarked, request, 1'000, 1'001).cacheable);
}
