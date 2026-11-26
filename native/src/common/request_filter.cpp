#include "request_filter.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <stdexcept>

namespace neo_request_filter {
namespace {

constexpr uint32_t known_kinds = (1u << (NEOASTRA_RESOURCE_MANIFEST + 1)) - 1;

char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }
bool digit(char c) noexcept { return c >= '0' && c <= '9'; }
bool letter(char c) noexcept { return c >= 'a' && c <= 'z'; }
int hex(char c) noexcept {
    if (digit(c)) return c - '0';
    c = lower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

void append_number(std::string& text, uint32_t value, int base = 10) {
    std::array<char, 16> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, base);
    text.append(buffer.data(), result.ptr);
}

// The network URL IPv4 forms (decimal, octal, hexadecimal and shortened).
// Syntactically valid overflow is still numeric, not a DNS-name fallback.
bool ipv4_number(std::string_view text, uint64_t& value) noexcept {
    if (text.empty()) return false;
    unsigned base = 10;
    if (text.size() >= 2 && text[0] == '0' && lower(text[1]) == 'x') {
        base = 16;
        text.remove_prefix(2);
    } else if (text.size() >= 2 && text[0] == '0') {
        base = 8;
        text.remove_prefix(1);
    }
    value = 0;
    for (const char c : text) {
        const auto n = hex(c);
        if (n < 0 || static_cast<unsigned>(n) >= base) return false;
        const auto maximum = std::numeric_limits<uint64_t>::max();
        value = value > (maximum - static_cast<unsigned>(n)) / base
            ? maximum : value * base + static_cast<unsigned>(n);
    }
    return true;
}

struct host {
    std::string name;
    bool ip{};
    bool loopback{};
};

bool normalize_host(std::string_view input, host& result) {
    if (input.empty() || input.size() > maximum_host_length * 3) return false;
    std::string name;
    name.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        if (c == '%') {
            if (i + 2 >= input.size() || hex(input[i + 1]) < 0 || hex(input[i + 2]) < 0) return false;
            c = static_cast<unsigned char>((hex(input[i + 1]) << 4) | hex(input[i + 2]));
            i += 2;
        }
        if (c <= 0x20 || c >= 0x7f) return false; // IDNA belongs to the engine/managed boundary.
        name.push_back(lower(static_cast<char>(c)));
    }
    if (name.size() > maximum_host_length) return false;
    if (name.find(':') != std::string::npos) {
        std::array<unsigned char, 16> address{};
        if (inet_pton(AF_INET6, name.c_str(), address.data()) != 1) return false;
        const bool zero_prefix = std::all_of(address.begin(), address.begin() + 15, [](auto n) { return n == 0; });
        const bool mapped = std::all_of(address.begin(), address.begin() + 10, [](auto n) { return n == 0; })
            && address[10] == 0xff && address[11] == 0xff && address[12] == 127;
        result.loopback = (zero_prefix && address[15] <= 1) || mapped;
        result.ip = true;
        // Use the URL's hexadecimal IPv6 form on every platform, including
        // IPv4-mapped addresses (inet_ntop may instead use dotted IPv4).
        std::array<uint16_t, 8> words{};
        for (size_t i = 0; i < words.size(); ++i)
            words[i] = static_cast<uint16_t>((address[2 * i] << 8) | address[2 * i + 1]);
        size_t start = 8, length = 0;
        for (size_t i = 0; i < words.size();) {
            if (words[i] != 0) { ++i; continue; }
            const auto first = i;
            while (i < words.size() && words[i] == 0) ++i;
            if (i - first > length && i - first >= 2) { start = first; length = i - first; }
        }
        result.name.clear();
        for (size_t i = 0; i < words.size();) {
            if (i == start) { result.name += "::"; i += length; continue; }
            if (!result.name.empty() && result.name.back() != ':') result.name += ':';
            append_number(result.name, words[i++], 16);
        }
        return true;
    }

    if (name.back() == '.') name.pop_back();
    if (name.empty()) return false;
    const std::string_view text(name);
    if (text.front() == '.' || text.back() == '.' || text.find("..") != std::string_view::npos
        || !std::all_of(text.begin(), text.end(), [](char c) {
            return letter(c) || digit(c) || c == '-' || c == '_' || c == '.';
        })) return false;
    const auto dot = text.rfind('.');
    const auto tail = text.substr(dot == std::string_view::npos ? 0 : dot + 1);
    uint64_t last{};
    const bool numeric = ipv4_number(tail, last) || std::all_of(tail.begin(), tail.end(), digit);
    if (numeric) {
        std::array<std::string_view, 4> parts{};
        size_t count = 0, start = 0;
        for (;;) {
            if (count == parts.size()) return false;
            const auto end = text.find('.', start);
            parts[count++] = text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
            if (end == std::string_view::npos) break;
            start = end + 1;
        }
        uint64_t address = 0;
        for (size_t i = 0; i < count; ++i) {
            uint64_t number{};
            if (!ipv4_number(parts[i], number)) return false;
            if (i + 1 == count) {
                if (number >= (uint64_t{1} << (8 * (5 - count)))) return false;
                address += number;
            } else {
                if (number > 255) return false;
                address += number << (8 * (3 - i));
            }
        }
        result.ip = true;
        result.loopback = (address >> 24) == 127 || address == 0;
        result.name.clear();
        for (int shift = 24; shift >= 0; shift -= 8) {
            if (!result.name.empty()) result.name += '.';
            append_number(result.name, static_cast<uint32_t>((address >> shift) & 255));
        }
    } else {
        result.name = std::move(name);
        result.ip = false;
        result.loopback = result.name == "localhost" || result.name.ends_with(".localhost");
    }
    return true;
}

bool equal_ascii(std::string_view text, std::string_view expected) noexcept {
    return text.size() == expected.size() && std::equal(text.begin(), text.end(), expected.begin(),
        [](char a, char b) { return lower(a) == b; });
}

void validate(const rule& value) {
    if (value.match > host_match::loopback || value.port < -1 || value.port == 0 || value.port > 65535
        || value.schemes == 0 || (value.schemes & ~all_schemes) != 0
        || value.kinds == 0 || (value.kinds != all_kinds && (value.kinds & ~known_kinds) != 0))
        throw std::invalid_argument("Invalid request filter criterion");
    if (value.match == host_match::any || value.match == host_match::loopback) {
        if (!value.host.empty()) throw std::invalid_argument("Any/Loopback rules do not take a host");
        return;
    }
    host canonical;
    if (value.host.size() > maximum_host_length || !normalize_host(value.host, canonical) || canonical.name != value.host)
        throw std::invalid_argument("Request filter host must be canonical ASCII, without IPv6 brackets");
    if (value.match == host_match::suffix && canonical.ip)
        throw std::invalid_argument("A host suffix must be a DNS name");
}

std::string regex_literal(std::string_view text) {
    std::string result;
    for (const auto c : text) {
        if (std::string_view(R"(\.^$*+?()[]{}|)").find(c) != std::string_view::npos) result += '\\';
        result += c;
    }
    return result;
}

std::vector<std::string> host_patterns(const rule& value) {
    switch (value.match) {
    case host_match::any:
        return {R"([^/?#@:\[\]]+)", R"(\[[0-9a-f:.]+\])"};
    case host_match::loopback:
        // WebKit matches engine-canonical URLs. Decimal/hex/octal IPv4 input
        // and expanded/mapped IPv6 are checked against actual engines separately.
        return {R"(localhost\.?)", R"(([^/?#@:.]+\.)+localhost\.?)",
            R"(127\.[0-9]+\.[0-9]+\.[0-9]+\.?)", R"(0\.0\.0\.0\.?)",
            R"(\[::\])", R"(\[::1\])", R"(\[::ffff:7f[0-9a-f][0-9a-f]:[0-9a-f]+\])"};
    case host_match::exact:
        if (value.host.find(':') != std::string::npos) return {R"(\[)" + regex_literal(value.host) + R"(\])"};
        return {regex_literal(value.host) + R"(\.?)"};
    case host_match::suffix:
        return {R"(([^/?#@:.]+\.)*)" + regex_literal(value.host) + R"(\.?)"};
    }
    throw std::invalid_argument("Unknown host match");
}

void append_json_string(std::string& output, std::string_view value) {
    output += '"';
    for (const auto c : value) {
        if (c == '\\' || c == '"') output += '\\';
        output += c; // Patterns consist solely of validated printable ASCII.
    }
    output += '"';
}

} // namespace

parse_result parse_target(std::string_view uri, target& output) {
    output = {};
    const auto colon = uri.find(':');
    if (colon == std::string_view::npos || colon == 0) return parse_result::invalid;
    const auto scheme_name = uri.substr(0, colon);
    for (size_t i = 0; i < scheme_name.size(); ++i) {
        const auto c = lower(scheme_name[i]);
        if (!(letter(c) || (i != 0 && (digit(c) || c == '+' || c == '-' || c == '.')))) return parse_result::invalid;
    }
    uint16_t default_port;
    if (equal_ascii(scheme_name, "http")) { output.scheme = http; default_port = 80; }
    else if (equal_ascii(scheme_name, "https")) { output.scheme = https; default_port = 443; }
    else if (equal_ascii(scheme_name, "ws")) { output.scheme = ws; default_port = 80; }
    else if (equal_ascii(scheme_name, "wss")) { output.scheme = wss; default_port = 443; }
    else return parse_result::unrelated;
    uri.remove_prefix(colon + 1);
    if (!uri.starts_with("//")) return parse_result::invalid;
    uri.remove_prefix(2);
    auto authority = uri.substr(0, uri.find_first_of("/?#"));
    if (const auto at = authority.rfind('@'); at != std::string_view::npos) authority.remove_prefix(at + 1);
    if (authority.empty()) return parse_result::invalid;
    std::string_view name, port;
    if (authority.front() == '[') {
        const auto end = authority.find(']');
        if (end == std::string_view::npos) return parse_result::invalid;
        name = authority.substr(1, end - 1);
        if (name.find(':') == std::string_view::npos) return parse_result::invalid;
        const auto rest = authority.substr(end + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') return parse_result::invalid;
            port = rest.substr(1);
        }
    } else {
        const auto end = authority.find(':');
        name = authority.substr(0, end);
        if (end != std::string_view::npos) port = authority.substr(end + 1);
    }
    output.port = default_port;
    if (!port.empty()) {
        uint32_t parsed = 0;
        for (const auto c : port) {
            if (!digit(c)) return parse_result::invalid;
            parsed = parsed * 10 + static_cast<unsigned>(c - '0');
            if (parsed > 65535) return parse_result::invalid;
        }
        output.port = static_cast<uint16_t>(parsed);
    }
    host canonical;
    if (!normalize_host(name, canonical)) return parse_result::invalid;
    output.host = std::move(canonical.name);
    output.loopback = canonical.loopback;
    return parse_result::network;
}

filter::filter(std::span<const rule> rules) {
    if (rules.size() > maximum_rules) throw std::invalid_argument("Request filter has more than 256 rules");
    for (const auto& value : rules) validate(value);
    _rules.assign(rules.begin(), rules.end());
}

bool filter::refuses(std::string_view uri, neoastra_resource_kind_t kind) const {
    if (empty()) return false;
    target parsed;
    const auto result = parse_target(uri, parsed);
    if (result == parse_result::unrelated) return false;
    if (result == parse_result::invalid) return true; // No malformed-network-URL bypass.
    for (const auto& value : _rules) {
        if ((value.schemes & parsed.scheme) == 0 || (value.port != -1 && value.port != parsed.port)) continue;
        if (value.kinds != all_kinds && (static_cast<uint32_t>(kind) >= 32 || (value.kinds & (1u << kind)) == 0)) continue;
        switch (value.match) {
        case host_match::any: return true;
        case host_match::loopback: if (parsed.loopback) return true; break;
        case host_match::exact: if (parsed.host == value.host) return true; break;
        case host_match::suffix:
            if (parsed.host == value.host || (parsed.host.size() > value.host.size()
                && parsed.host.ends_with(value.host) && parsed.host[parsed.host.size() - value.host.size() - 1] == '.')) return true;
            break;
        }
    }
    return false;
}

std::string filter::webkit_json() const {
    struct scheme_info { std::string_view name; uint32_t flag; int port; };
    constexpr scheme_info schemes[] = {{"http", http, 80}, {"https", https, 443}, {"ws", ws, 80}, {"wss", wss, 443}};
    std::string output = "[";
    size_t count = 0;
    for (const auto& value : _rules) {
        // WebKit's `raw` conflates kinds, unlike the portable resource enum.
        // The initial backend therefore offers All only, not ResourceKinds.
        if (value.kinds != all_kinds) throw std::invalid_argument("WebKit cannot preserve this resource-kind selection");
        const auto hosts = host_patterns(value);
        for (const auto& scheme : schemes) {
            if ((value.schemes & scheme.flag) == 0) continue;
            std::vector<std::string> ports;
            if (value.port == -1) ports.emplace_back("(:[0-9]+)?");
            else {
                ports.emplace_back(":" + std::to_string(value.port));
                if (value.port == scheme.port) ports.emplace_back("");
            }
            for (const auto& host : hosts) for (const auto& port : ports) for (const auto ending : {"[/?#]", "$"}) {
                if (++count > maximum_compiled_rules) throw std::length_error("Too many compiled request filter rules");
                if (count > 1) output += ',';
                output += R"({"trigger":{"url-filter":)";
                append_json_string(output, "^" + std::string(scheme.name) + "://([^/?#]*@)?" + host + port + ending);
                output += R"(,"url-filter-is-case-sensitive":false},"action":{"type":"block"}})";
                if (output.size() + 1 > maximum_json_bytes) throw std::length_error("Compiled request filter exceeds 4 MiB");
            }
        }
    }
    output += ']';
    return output;
}

} // namespace neo_request_filter
