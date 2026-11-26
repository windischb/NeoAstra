// Copyright (c) Alexandre Mutel. All rights reserved.
// Licensed under the BSD-Clause 2 license.

using System.Collections.ObjectModel;
using System.Globalization;
using System.Net;
using System.Net.Sockets;
using System.Text;

namespace NeoAstra;

/// <summary>Selects which request hosts a <see cref="NeoRequestRule"/> matches.</summary>
public enum NeoHostMatch
{
    /// <summary>Any host; <see cref="NeoRequestRule.Host"/> must be <see langword="null"/>.</summary>
    Any = 0,
    /// <summary>Exactly the DNS name or IP literal given by <see cref="NeoRequestRule.Host"/>.</summary>
    Exact = 1,
    /// <summary>The DNS name given by <see cref="NeoRequestRule.Host"/> and every dot-delimited subdomain of it; IP literals are not allowed.</summary>
    Suffix = 2,
    /// <summary>Loopback hosts after URL canonicalization (<c>localhost</c> and its subdomains, <c>127.0.0.0/8</c>, <c>0.0.0.0</c>, <c>::</c>, <c>::1</c> and IPv4-mapped <c>127.0.0.0/8</c>); <see cref="NeoRequestRule.Host"/> must be <see langword="null"/>. DNS aliases that resolve to loopback are not matched.</summary>
    Loopback = 3,
}

/// <summary>Selects the URL schemes a <see cref="NeoRequestRule"/> matches.</summary>
[Flags]
public enum NeoRequestSchemes : uint
{
    /// <summary>No scheme; not valid in a rule.</summary>
    None = 0,
    /// <summary><c>http:</c> URLs, with default port 80.</summary>
    Http = 1,
    /// <summary><c>https:</c> URLs, with default port 443.</summary>
    Https = 2,
    /// <summary><c>ws:</c> URLs, with default port 80.</summary>
    Ws = 4,
    /// <summary><c>wss:</c> URLs, with default port 443.</summary>
    Wss = 8,
}

/// <summary>Selects the browser resource categories a <see cref="NeoRequestRule"/> matches. Bit <c>n</c> corresponds to <see cref="NeoResourceKind"/> value <c>n</c>.</summary>
[Flags]
public enum NeoResourceKinds : uint
{
    /// <summary>No resource kind; not valid in a rule.</summary>
    None = 0,
    /// <summary>Unclassified resources.</summary>
    Other = 1u << 0,
    /// <summary>Documents, including frame documents.</summary>
    Document = 1u << 1,
    /// <summary>Stylesheets.</summary>
    Stylesheet = 1u << 2,
    /// <summary>Images.</summary>
    Image = 1u << 3,
    /// <summary>Audio or video media.</summary>
    Media = 1u << 4,
    /// <summary>Fonts.</summary>
    Font = 1u << 5,
    /// <summary>Scripts.</summary>
    Script = 1u << 6,
    /// <summary>XMLHttpRequest requests.</summary>
    XmlHttpRequest = 1u << 7,
    /// <summary>Fetch API requests.</summary>
    Fetch = 1u << 8,
    /// <summary>Text tracks.</summary>
    TextTrack = 1u << 9,
    /// <summary>Event sources.</summary>
    EventSource = 1u << 10,
    /// <summary>WebSocket connections.</summary>
    WebSocket = 1u << 11,
    /// <summary>Web application manifests.</summary>
    Manifest = 1u << 12,
    /// <summary>Every resource kind, including kinds an engine reports that have no portable value.</summary>
    All = uint.MaxValue,
}

/// <summary>Request classes a <see cref="NeoRequestFilter"/> requires the backend to cover.</summary>
[Flags]
public enum NeoRequestFilterFlags : ulong
{
    /// <summary>No requirement beyond the base, which is always required.</summary>
    None = 0,
    /// <summary>Subresource requests and HTTP(S) main documents. Part of the base; does not imply worker or WebSocket coverage.</summary>
    Subresources = 1ul << 0,
    /// <summary>Frame documents and their subresource requests. Part of the base.</summary>
    Frames = 1ul << 1,
    /// <summary>WebSocket connections.</summary>
    WebSockets = 1ul << 2,
    /// <summary>Each hop of an HTTP redirect.</summary>
    Redirects = 1ul << 3,
    /// <summary>Requests of service workers.</summary>
    ServiceWorkers = 1ul << 4,
    /// <summary>Requests of shared workers.</summary>
    SharedWorkers = 1ul << 5,
    /// <summary>The filter applies to its view only, not to other views of the same profile. Part of the base.</summary>
    PerView = 1ul << 6,
    /// <summary>Reserved for reporting refused requests; not implemented.</summary>
    ReportsRefusals = 1ul << 7,
    /// <summary>Rules may select precise <see cref="NeoResourceKinds"/> instead of <see cref="NeoResourceKinds.All"/>.</summary>
    ResourceKinds = 1ul << 8,
    /// <summary>Requests of dedicated workers.</summary>
    DedicatedWorkers = 1ul << 9,
}

/// <summary>Refuses an HTTP(S) or WebSocket request when all its criteria match.</summary>
/// <remarks>Criteria are validated and the host normalized when the filter is installed, not when the rule is created.</remarks>
public sealed record NeoRequestRule
{
    /// <summary>Gets how <see cref="Host"/> is matched. The default is <see cref="NeoHostMatch.Any"/>.</summary>
    public NeoHostMatch HostMatch { get; init; } = NeoHostMatch.Any;

    /// <summary>Gets the host for <see cref="NeoHostMatch.Exact"/> and <see cref="NeoHostMatch.Suffix"/>: a DNS name (Unicode names are converted to IDNA ASCII), an IPv4 address or an IPv6 address with or without brackets. Must be <see langword="null"/> for the other modes.</summary>
    public string? Host { get; init; }

    /// <summary>Gets the effective port to match, from 1 to 65535, or <see langword="null"/> for any port. Implicit default ports (80 for HTTP/WS, 443 for HTTPS/WSS) match like explicit ones.</summary>
    public int? Port { get; init; }

    /// <summary>Gets the schemes to match. The default is <see cref="NeoRequestSchemes.Http"/> | <see cref="NeoRequestSchemes.Https"/>.</summary>
    public NeoRequestSchemes Schemes { get; init; } = NeoRequestSchemes.Http | NeoRequestSchemes.Https;

    /// <summary>Gets the resource kinds to match. The default is <see cref="NeoResourceKinds.All"/>.</summary>
    public NeoResourceKinds Kinds { get; init; } = NeoResourceKinds.All;
}

/// <summary>An immutable, refuse-only set of request rules for a view. A request is refused when any rule matches.</summary>
/// <remarks>The filter is an additional protection layer: it does not promise that no TCP connection is opened, nor that a replacement applies atomically in every engine process.</remarks>
public sealed class NeoRequestFilter
{
    internal const int MaximumRules = 256;
    internal const int MaximumHostLength = 253;
    internal const NeoRequestFilterFlags BaseFlags = NeoRequestFilterFlags.Subresources | NeoRequestFilterFlags.Frames | NeoRequestFilterFlags.PerView;

    private const NeoRequestFilterFlags KnownFlags = (NeoRequestFilterFlags)((1ul << 10) - 1);
    private const NeoRequestSchemes KnownSchemes = NeoRequestSchemes.Http | NeoRequestSchemes.Https | NeoRequestSchemes.Ws | NeoRequestSchemes.Wss;
    private const NeoResourceKinds KnownKinds = (NeoResourceKinds)((1u << 13) - 1);

    private static readonly ReadOnlyCollection<NeoRequestRule> NoRules = new([]);
    private readonly ReadOnlyCollection<NeoRequestRule> _rules = NoRules;
    private readonly bool _isSnapshot;

    /// <summary>Initializes a filter without rules that requires the base request classes.</summary>
    public NeoRequestFilter()
    {
    }

    private NeoRequestFilter(ReadOnlyCollection<NeoRequestRule> rules, NeoRequestFilterFlags flags)
    {
        _rules = rules;
        RequiredFlags = flags;
        _isSnapshot = true;
    }

    /// <summary>Gets a filter without rules, which refuses nothing.</summary>
    public static NeoRequestFilter Empty { get; } = new(NoRules, BaseFlags);

    /// <summary>Gets the rules; a request is refused when any rule matches. The setter copies the given collection. An empty list removes the filter.</summary>
    /// <exception cref="ArgumentNullException">The value is <see langword="null"/>.</exception>
    public IReadOnlyList<NeoRequestRule> Rules
    {
        get => _rules;
        init
        {
            ArgumentNullException.ThrowIfNull(value);
            _rules = value.Count == 0 ? NoRules : new ReadOnlyCollection<NeoRequestRule>([.. value]);
        }
    }

    /// <summary>Gets the request classes the backend must cover, or installation fails with <see cref="NotSupportedException"/>. The default is <see cref="NeoRequestFilterFlags.Subresources"/> | <see cref="NeoRequestFilterFlags.Frames"/> | <see cref="NeoRequestFilterFlags.PerView"/>, the base that is always required even when omitted here.</summary>
    public NeoRequestFilterFlags RequiredFlags { get; init; } = BaseFlags;

    /// <summary>Validates <paramref name="filter"/> and returns an immutable copy with canonical hosts and the base flags added, or <see cref="Empty"/> when it is <see langword="null"/> or has no rules and no extra flags. A snapshot is returned as is.</summary>
    /// <exception cref="ArgumentException">The filter has more than 256 rules, a <see langword="null"/> rule, unknown flag bits, an undefined host mode, a host that is missing, forbidden or not valid for the mode, a port outside 1-65535, or an empty or unknown scheme or kind mask.</exception>
    internal static NeoRequestFilter CreateSnapshot(NeoRequestFilter? filter)
    {
        if (filter is null) return Empty;
        if (filter._isSnapshot) return filter;
        if ((filter.RequiredFlags & ~KnownFlags) != 0)
            throw new ArgumentException($"Unknown request filter flags 0x{(ulong)filter.RequiredFlags:x}.", nameof(filter));
        var source = filter._rules;
        if (source.Count > MaximumRules)
            throw new ArgumentException($"A request filter has at most {MaximumRules} rules.", nameof(filter));
        var flags = filter.RequiredFlags | BaseFlags;
        if (source.Count == 0 && flags == BaseFlags) return Empty;
        var rules = new NeoRequestRule[source.Count];
        for (var i = 0; i < rules.Length; i++) rules[i] = NormalizeRule(source[i], i);
        return new NeoRequestFilter(rules.Length == 0 ? NoRules : new ReadOnlyCollection<NeoRequestRule>(rules), flags);
    }

    /// <summary>Gets whether this instance is a validated, normalized snapshot from <see cref="CreateSnapshot"/>.</summary>
    internal bool IsSnapshot => _isSnapshot;

    private static NeoRequestRule NormalizeRule(NeoRequestRule? rule, int index)
    {
        if (rule is null) throw new ArgumentException($"Request filter rule {index} is null.", "filter");
        if (rule.Port is { } port && (port < 1 || port > 65535))
            throw RuleError(index, $"port {port} is outside 1-65535");
        if (rule.Schemes == 0 || (rule.Schemes & ~KnownSchemes) != 0)
            throw RuleError(index, $"scheme mask 0x{(uint)rule.Schemes:x} is empty or unknown");
        if (rule.Kinds == 0 || (rule.Kinds != NeoResourceKinds.All && (rule.Kinds & ~KnownKinds) != 0))
            throw RuleError(index, $"resource kind mask 0x{(uint)rule.Kinds:x} is empty or unknown");
        switch (rule.HostMatch)
        {
            case NeoHostMatch.Any:
            case NeoHostMatch.Loopback:
                if (rule.Host is not null) throw RuleError(index, $"{rule.HostMatch} does not take a host");
                return rule with { };
            case NeoHostMatch.Exact:
            case NeoHostMatch.Suffix:
                if (rule.Host is null) throw RuleError(index, $"{rule.HostMatch} requires a host");
                if (!TryNormalizeHost(rule.Host, out var host, out var ip))
                    throw RuleError(index, "the host is not a valid DNS name or IP address");
                if (ip && rule.HostMatch == NeoHostMatch.Suffix)
                    throw RuleError(index, "a host suffix must be a DNS name, not an IP address");
                return rule with { Host = host };
            default:
                throw RuleError(index, $"host match {(int)rule.HostMatch} is undefined");
        }
    }

    private static ArgumentException RuleError(int index, string message) => new($"Request filter rule {index}: {message}.", "filter");

    /// <summary>Converts a host to the canonical ASCII form the native filter requires: IDNA ASCII lowercase DNS name without trailing dot, dotted-decimal IPv4 (URL numeric forms accepted), or compressed lowercase hexadecimal IPv6 without brackets, IPv4-mapped addresses included.</summary>
    internal static bool TryNormalizeHost(string host, out string canonical, out bool ip)
    {
        canonical = string.Empty;
        ip = false;
        if (string.IsNullOrEmpty(host) || host.Length > MaximumHostLength * 4) return false;
        var ascii = true;
        foreach (var c in host)
        {
            if (char.IsControl(c) || char.IsWhiteSpace(c) || c == '%') return false;
            if (c > 0x7f) ascii = false;
        }

        var text = host;
        if (text.StartsWith('[') && text.EndsWith(']')) text = text[1..^1];
        if (text.Contains(':'))
        {
            // IPAddress accepts "[::1]:80" and zone ids; the native contract does not.
            if (!ascii || text.AsSpan().IndexOfAny('[', ']') >= 0) return false;
            if (!IPAddress.TryParse(text, out var address) || address.AddressFamily != AddressFamily.InterNetworkV6) return false;
            canonical = FormatIPv6(address);
            ip = true;
            return true;
        }
        if (!ReferenceEquals(text, host)) return false; // Brackets only around IPv6.

        if (!ascii)
        {
            try { text = new IdnMapping().GetAscii(text); }
            catch (ArgumentException) { return false; }
        }
        text = text.ToLowerInvariant();
        if (text.EndsWith('.')) text = text[..^1];
        if (text.Length == 0 || text.Length > MaximumHostLength || text[0] == '.' || text[^1] == '.' || text.Contains("..", StringComparison.Ordinal)) return false;
        foreach (var c in text)
        {
            if (!(char.IsAsciiLetterLower(c) || char.IsAsciiDigit(c) || c == '-' || c == '_' || c == '.')) return false;
        }

        var tail = text.AsSpan(text.LastIndexOf('.') + 1);
        if (!TryParseIPv4Number(tail, out _) && !IsAllDigits(tail))
        {
            canonical = text;
            return true;
        }
        // A numeric last label makes the host an IPv4 address in URL parsing, never a DNS name.
        if (!TryParseIPv4(text, out var value)) return false;
        canonical = string.Create(CultureInfo.InvariantCulture, $"{value >> 24}.{(value >> 16) & 255}.{(value >> 8) & 255}.{value & 255}");
        ip = true;
        return true;
    }

    private static bool IsAllDigits(ReadOnlySpan<char> text)
    {
        foreach (var c in text) if (!char.IsAsciiDigit(c)) return false;
        return true;
    }

    // URL IPv4 number: decimal, 0-prefixed octal or 0x-prefixed hexadecimal; overflow saturates.
    private static bool TryParseIPv4Number(ReadOnlySpan<char> text, out ulong value)
    {
        value = 0;
        if (text.IsEmpty) return false;
        var radix = 10u;
        if (text.Length >= 2 && text[0] == '0' && text[1] == 'x') { radix = 16; text = text[2..]; }
        else if (text.Length >= 2 && text[0] == '0') { radix = 8; text = text[1..]; }
        foreach (var c in text)
        {
            var digit = char.IsAsciiDigit(c) ? c - '0' : c is >= 'a' and <= 'f' ? c - 'a' + 10 : -1;
            if (digit < 0 || (uint)digit >= radix) return false;
            value = value > (ulong.MaxValue - (uint)digit) / radix ? ulong.MaxValue : value * radix + (uint)digit;
        }
        return true;
    }

    private static bool TryParseIPv4(string text, out uint address)
    {
        address = 0;
        var parts = text.Split('.');
        if (parts.Length > 4) return false;
        ulong result = 0;
        for (var i = 0; i < parts.Length; i++)
        {
            if (!TryParseIPv4Number(parts[i], out var number)) return false;
            if (i == parts.Length - 1)
            {
                if (number >= 1ul << (8 * (5 - parts.Length))) return false;
                result += number;
            }
            else
            {
                if (number > 255) return false;
                result += number << (8 * (3 - i));
            }
        }
        address = (uint)result;
        return true;
    }

    // RFC 5952 form in hexadecimal words only: IPAddress.ToString writes IPv4-mapped and
    // IPv4-compatible addresses with a dotted suffix, which the native contract rejects.
    private static string FormatIPv6(IPAddress address)
    {
        Span<byte> bytes = stackalloc byte[16];
        address.TryWriteBytes(bytes, out _);
        Span<ushort> words = stackalloc ushort[8];
        for (var i = 0; i < 8; i++) words[i] = (ushort)((bytes[2 * i] << 8) | bytes[2 * i + 1]);
        int start = 8, length = 0;
        for (var i = 0; i < 8;)
        {
            if (words[i] != 0) { i++; continue; }
            var first = i;
            while (i < 8 && words[i] == 0) i++;
            if (i - first > length && i - first >= 2) { start = first; length = i - first; }
        }
        var builder = new StringBuilder(39);
        for (var i = 0; i < 8;)
        {
            if (i == start) { builder.Append("::"); i += length; continue; }
            if (builder.Length > 0 && builder[^1] != ':') builder.Append(':');
            builder.Append(words[i++].ToString("x", CultureInfo.InvariantCulture));
        }
        return builder.ToString();
    }
}
