#include "mini_test.hpp"

#include "goblin/http/request.hpp"

#include <string>
#include <string_view>

using namespace goblin;
using namespace goblin::http;

TEST("http request: simple GET — method, target (query kept), host, keep-alive") {
    const std::string_view s = "GET /foo/bar?q=1 HTTP/1.1\r\nHost: example.com\r\n\r\n";
    const auto r = parse_request(s);
    CHECK(r.status == ParseStatus::ok);
    CHECK(r.req.method == Method::get);
    CHECK(r.req.target == "/foo/bar?q=1");
    CHECK(r.req.host == "example.com");
    CHECK(r.req.keep_alive);
    CHECK(!r.req.range.has_value());
    CHECK_EQ(r.consumed, s.size()); // whole buffer was the request head
}

TEST("http request: HEAD recognized; unknown method -> other") {
    CHECK(parse_request("HEAD / HTTP/1.1\r\n\r\n").req.method == Method::head);
    CHECK(parse_request("PURGE / HTTP/1.1\r\n\r\n").req.method == Method::other);
}

TEST("http request: partial head -> partial, full -> ok") {
    const std::string_view partial = "GET / HTTP/1.1\r\nHost: x\r\n"; // no terminating CRLF yet
    CHECK(parse_request(partial).status == ParseStatus::partial);
    CHECK(parse_request("not a real request line\r\n\r\n").status == ParseStatus::bad);
}

TEST("http request: keep-alive defaults + Connection override") {
    CHECK(parse_request("GET / HTTP/1.1\r\n\r\n").req.keep_alive);                       // 1.1 default
    CHECK(!parse_request("GET / HTTP/1.0\r\n\r\n").req.keep_alive);                      // 1.0 default
    CHECK(parse_request("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n").req.keep_alive);
    CHECK(!parse_request("GET / HTTP/1.1\r\nConnection: close\r\n\r\n").req.keep_alive);
}

TEST("http request: pipelined — consumed bounds the first request, remainder parses") {
    const std::string_view two = "GET /a HTTP/1.1\r\nHost: h\r\n\r\nGET /b HTTP/1.1\r\n\r\n";
    const auto r1 = parse_request(two);
    CHECK(r1.status == ParseStatus::ok && r1.req.target == "/a");
    const auto r2 = parse_request(two.substr(r1.consumed));
    CHECK(r2.status == ParseStatus::ok && r2.req.target == "/b");
}

TEST("http request: Range header lands on the parsed request") {
    const auto r = parse_request("GET /f HTTP/1.1\r\nHost: h\r\nRange: bytes=10-20\r\n\r\n");
    CHECK(r.status == ParseStatus::ok && r.req.range.has_value());
    CHECK(r.req.range->kind == RangeSpec::Kind::closed && r.req.range->a == 10 && r.req.range->b == 20);
}

TEST("http range: parse closed / open-ended / suffix; reject junk") {
    CHECK(parse_range("bytes=0-99")->kind == RangeSpec::Kind::closed);
    CHECK(parse_range("bytes=500-")->kind == RangeSpec::Kind::from);
    CHECK(parse_range("bytes=-256")->kind == RangeSpec::Kind::suffix);
    CHECK(!parse_range("items=0-9").has_value());   // non-bytes unit
    CHECK(!parse_range("bytes=0-9,20-29").has_value()); // multi-range unsupported
    CHECK(!parse_range("bytes=9-2").has_value());   // inverted
    CHECK(!parse_range("bytes=-0").has_value());    // empty suffix
    CHECK(!parse_range("bytes=abc").has_value());
}

TEST("http range: resolve against object size (clamp + 416)") {
    const Size sz = 1000;
    auto closed = resolve_range(*parse_range("bytes=0-99"), sz);
    CHECK(closed && closed->first == 0 && closed->second == 100);
    auto from = resolve_range(*parse_range("bytes=900-"), sz);
    CHECK(from && from->first == 900 && from->second == 100);
    auto suffix = resolve_range(*parse_range("bytes=-50"), sz);
    CHECK(suffix && suffix->first == 950 && suffix->second == 50);
    auto over = resolve_range(*parse_range("bytes=-5000"), sz); // suffix clamps to whole object
    CHECK(over && over->first == 0 && over->second == 1000);
    auto end_clamp = resolve_range(*parse_range("bytes=990-100000"), sz); // last clamps to EOF
    CHECK(end_clamp && end_clamp->first == 990 && end_clamp->second == 10);
    CHECK(!resolve_range(*parse_range("bytes=2000-3000"), sz).has_value()); // start past EOF -> 416
}

TEST("http content-type: extension mapping, case-insensitive, query/dir stripped") {
    CHECK(content_type_for("/style.css") == "text/css; charset=utf-8");
    CHECK(content_type_for("/app.js") == "text/javascript; charset=utf-8");
    CHECK(content_type_for("/a/b/logo.PNG") == "image/png");          // case-insensitive
    CHECK(content_type_for("/photo.jpeg") == "image/jpeg");
    CHECK(content_type_for("/font.woff2") == "font/woff2");
    CHECK(content_type_for("/archive.tar.gz") == "application/gzip"); // last extension wins
    CHECK(content_type_for("/app.js?v=2") == "text/javascript; charset=utf-8"); // query ignored
    CHECK(content_type_for("example.com/blog/index.html") == "text/html; charset=utf-8"); // vhost key
}

TEST("http content-type: unknown / missing extension -> octet-stream") {
    CHECK(content_type_for("/data.bin") == "application/octet-stream");  // unknown ext
    CHECK(content_type_for("/README") == "application/octet-stream");    // no extension
    CHECK(content_type_for("/.bashrc") == "application/octet-stream");   // dotfile, no extension
    CHECK(content_type_for("/archive.") == "application/octet-stream");  // trailing dot
    CHECK(content_type_for("/") == "application/octet-stream");          // root / empty segment
}
