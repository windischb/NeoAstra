#pragma once

#include "neoastra.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace neo_request_filter {

enum class host_match : uint32_t { any, exact, suffix, loopback };
enum scheme : uint32_t { http = 1, https = 2, ws = 4, wss = 8 };
inline constexpr uint32_t all_schemes = http | https | ws | wss;
inline constexpr uint32_t all_kinds = UINT32_MAX;
inline constexpr size_t maximum_rules = 256;
inline constexpr size_t maximum_host_length = 253;
inline constexpr size_t maximum_compiled_rules = 16384;
inline constexpr size_t maximum_json_bytes = 4 * 1024 * 1024;

struct rule {
    host_match match{host_match::any};
    // Canonical ASCII DNS name or IP literal, without IPv6 brackets.
    std::string host;
    int32_t port{-1};
    uint32_t schemes{http | https};
    uint32_t kinds{all_kinds};
    bool operator==(const rule&) const = default;
};

// A deliberately bounded parser for engine-supplied absolute network URLs, not
// a replacement for the browser's URL parser. Engines supply ASCII/IDNA hosts.
struct target {
    std::string host;
    uint32_t scheme{};
    uint16_t port{};
    bool loopback{};
};
enum class parse_result { network, unrelated, invalid };
parse_result parse_target(std::string_view uri, target& output);

// Copies and validates the caller's rules once. Callers share only const
// snapshots; replacement/lifetime belongs to the view, not to this pure core.
class filter final {
public:
    explicit filter(std::span<const rule> rules);
    std::span<const rule> rules() const noexcept { return _rules; }
    bool empty() const noexcept { return _rules.empty(); }
    bool refuses(std::string_view uri, neoastra_resource_kind_t kind) const;
    // Throws for criteria WebKit cannot represent exactly, or bounded expansion
    // limits. JSON compilation never silently drops or broadens a criterion.
    std::string webkit_json() const;

private:
    std::vector<rule> _rules;
};

} // namespace neo_request_filter
