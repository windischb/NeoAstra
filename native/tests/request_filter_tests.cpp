// Pure matching/validation/compiler tests. Actual engine URL serialization and
// request interception are exercised separately, not inferred from these tests.
#include "request_filter.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using namespace neo_request_filter;

namespace {

filter one(rule value) { return filter(std::span<const rule>(&value, 1)); }

template<typename Exception, typename Action>
void throws(Action action) {
    try { action(); }
    catch (const Exception&) { return; }
    assert(false && "expected exception");
}

void validation() {
    assert(filter({}).empty());
    std::vector<rule> rules(maximum_rules);
    assert(filter(rules).rules().size() == maximum_rules);
    rules.emplace_back();
    throws<std::invalid_argument>([&] { filter value(rules); });
    for (const auto port : {-2, 0, 65536}) {
        rule value; value.port = port;
        throws<std::invalid_argument>([&] { one(value); });
    }
    for (const auto host : {"", "LOCALHOST", "localhost.", "127.1", "[::1]", "foo/bar", "x@y", "https://x", "é.example"}) {
        rule value; value.match = host_match::exact; value.host = host;
        throws<std::invalid_argument>([&] { one(value); });
    }
    for (const auto host : {"localhost", "xn--bcher-kva.example", "127.0.0.1", "::1", "::ffff:7f00:1"}) {
        rule value; value.match = host_match::exact; value.host = host;
        assert(one(value).rules()[0].host == host);
    }
    rule value;
    value.host = "ignored.example";
    throws<std::invalid_argument>([&] { one(value); });
    value.host.clear();
    value.match = static_cast<host_match>(99);
    throws<std::invalid_argument>([&] { one(value); });
    value.match = host_match::any;
    value.schemes = 0;
    throws<std::invalid_argument>([&] { one(value); });
    value.schemes = 16;
    throws<std::invalid_argument>([&] { one(value); });
    value.schemes = http;
    for (const auto mask : {0u, 1u << 31}) {
        value.kinds = mask;
        throws<std::invalid_argument>([&] { one(value); });
    }
    value.kinds = all_kinds;
    value.match = host_match::suffix; value.host = "127.0.0.1";
    throws<std::invalid_argument>([&] { one(value); });
    value.match = host_match::exact; value.host.assign(254, 'a');
    throws<std::invalid_argument>([&] { one(value); });
    value.host = std::string("local\0host", 10);
    throws<std::invalid_argument>([&] { one(value); });
    value.host.assign(maximum_host_length, 'a');
    assert(one(value).rules()[0].host.size() == maximum_host_length);

    value = {}; value.port = 2582;
    auto copied = one(value);
    value.port = 5173;
    assert(copied.rules()[0].port == 2582);
}

void matching() {
    rule value; value.port = 2582;
    const auto ports = one(value);
    for (const auto uri : {"http://127.0.0.1:2582/", "https://elsewhere.example:2582/?q",
             "HTTP://USER:PASS@LOCALHOST:2582", "http://[::1]:2582#fragment"}) {
        assert(ports.refuses(uri, NEOASTRA_RESOURCE_FETCH));
    }
    for (const auto uri : {"http://localhost:5173/", "https://elsewhere.example/", "ws://localhost:2582/",
             "http://user:2582@elsewhere.example/", "http://elsewhere.example/?url=http://localhost:2582/",
             "data:text/plain,http://localhost:2582/", "blob:http://localhost:2582/id"}) {
        assert(!ports.refuses(uri, NEOASTRA_RESOURCE_DOCUMENT));
    }
    value.port = 80; value.schemes = all_schemes;
    auto defaults = one(value);
    assert(defaults.refuses("http://example.com/", NEOASTRA_RESOURCE_OTHER));
    assert(defaults.refuses("ws://example.com/", NEOASTRA_RESOURCE_WEBSOCKET));
    assert(defaults.refuses("https://example.com:80/", NEOASTRA_RESOURCE_IMAGE));
    assert(defaults.refuses("http://example.com:00080/", NEOASTRA_RESOURCE_IMAGE));
    assert(!defaults.refuses("https://example.com/", NEOASTRA_RESOURCE_IMAGE));
    value.port = 443;
    assert(one(value).refuses("wss://example.com/", NEOASTRA_RESOURCE_WEBSOCKET));

    value = {}; value.match = host_match::suffix; value.host = "example.com";
    auto suffix = one(value);
    for (const auto uri : {"http://example.com", "https://a.b.example.com./", "http://EXAMPLE.COM:443/"})
        assert(suffix.refuses(uri, NEOASTRA_RESOURCE_DOCUMENT));
    for (const auto uri : {"http://notexample.com", "https://example.com.evil.test/", "http://example.com@elsewhere.test/"})
        assert(!suffix.refuses(uri, NEOASTRA_RESOURCE_DOCUMENT));
    value.match = host_match::exact;
    assert(!one(value).refuses("https://a.example.com", NEOASTRA_RESOURCE_DOCUMENT));
    value.kinds = 1u << NEOASTRA_RESOURCE_IMAGE;
    assert(one(value).refuses("http://example.com", NEOASTRA_RESOURCE_IMAGE));
    assert(!one(value).refuses("http://example.com", NEOASTRA_RESOURCE_FETCH));
    value.kinds = all_kinds;
    assert(one(value).refuses("http://example.com", static_cast<neoastra_resource_kind_t>(99)));

    value = {}; value.match = host_match::loopback; value.schemes = all_schemes;
    const auto loopback = one(value);
    for (const auto uri : {"http://localhost", "https://foo.localhost./", "http://127.0.0.2",
             "http://2130706433", "http://0x7f.1", "http://0177.0.0.1", "http://%31%32%37.1",
             "http://0.0.0.0", "http://[::]", "http://[0:0:0:0:0:0:0:1]",
             "http://[::ffff:127.0.0.2]", "http://[::ffff:7fff:ffff]", "ws://LOCALHOST.:2582/"}) {
        target parsed;
        assert(parse_target(uri, parsed) == parse_result::network);
        assert(parsed.loopback);
        assert(loopback.refuses(uri, NEOASTRA_RESOURCE_OTHER));
    }
    for (const auto uri : {"http://localhost.example", "http://localtest.me", "http://128.0.0.1",
             "http://[::ffff:8000:1]", "http://[2001:db8::1]", "http://0xffffffff", "http://0x7f.0.0.0x7g"}) {
        target parsed;
        assert(parse_target(uri, parsed) == parse_result::network);
        assert(!parsed.loopback);
        assert(!loopback.refuses(uri, NEOASTRA_RESOURCE_OTHER));
    }
    for (const auto uri : {"http:/localhost", "http://", "https://[::1", "http://localhost:65536",
             "http://127.0.0.999/", "http://0xffffffff1", "http://%00localhost", "http://a b/"}) {
        target parsed;
        assert(parse_target(uri, parsed) == parse_result::invalid);
        assert(loopback.refuses(uri, NEOASTRA_RESOURCE_OTHER));
        assert(!filter({}).refuses(uri, NEOASTRA_RESOURCE_OTHER));
    }
}

void compiler() {
    assert(filter({}).webkit_json() == "[]");
    rule value; value.match = host_match::exact; value.host = "example.com"; value.schemes = http;
    const auto json = one(value).webkit_json();
    assert(json == R"([{"trigger":{"url-filter":"^http://([^/?#]*@)?example\\.com\\.?(:[0-9]+)?[/?#]","url-filter-is-case-sensitive":false},"action":{"type":"block"}},{"trigger":{"url-filter":"^http://([^/?#]*@)?example\\.com\\.?(:[0-9]+)?$","url-filter-is-case-sensitive":false},"action":{"type":"block"}}])");
    value = {}; value.match = host_match::loopback; value.schemes = all_schemes; value.port = 80;
    const auto loopback = one(value).webkit_json();
    assert(loopback.find('|') == std::string::npos);
    assert(loopback.find("{n}") == std::string::npos);
    assert(loopback.find("wss://") != std::string::npos);
    assert(loopback.find("ffff:7f") != std::string::npos);
    value.kinds = 1u << NEOASTRA_RESOURCE_IMAGE;
    throws<std::invalid_argument>([&] { one(value).webkit_json(); });
    value.kinds = all_kinds;
    std::vector<rule> many(maximum_rules, value);
    throws<std::length_error>([&] { filter(many).webkit_json(); });
    value.match = host_match::suffix;
    value.host.assign(maximum_host_length, 'a');
    many.assign(maximum_rules, value);
    assert(filter(many).webkit_json().size() <= maximum_json_bytes);
}

} // namespace

int main() {
    validation();
    matching();
    compiler();
    std::puts("Request filter core tests passed");
}
