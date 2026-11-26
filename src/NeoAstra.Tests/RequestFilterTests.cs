// Copyright (c) Alexandre Mutel. All rights reserved.
// Licensed under the BSD-Clause 2 license.

using NeoAstra.Interop;
using NeoAstra.Interop.Generated;

namespace NeoAstra.Tests;

[TestClass]
public sealed class RequestFilterTests
{
    private const NeoRequestFilterFlags Base = NeoRequestFilterFlags.Subresources | NeoRequestFilterFlags.Frames | NeoRequestFilterFlags.PerView;

    [TestMethod]
    public void OldLibrariesRefuseRulesWithoutReferencingANewExport()
    {
        var old = new NeoRuntimeInfo("test", "1", "1", "test", "arm64", 63, false);
        var current = old with { BuildFeatures = 127 };
        var filter = NeoRequestFilter.CreateSnapshot(new NeoRequestFilter { Rules = [new NeoRequestRule { Port = 2582 }] });
        Assert.IsFalse(old.HasRequestFilters);
        Assert.IsTrue(current.HasRequestFilters);
        Assert.IsInstanceOfType<NotSupportedException>(NeoAstra.CheckRequestFilterSupport(old, filter));
        Assert.IsNull(NeoAstra.CheckRequestFilterSupport(old, NeoRequestFilter.Empty));
        Assert.IsNull(NeoAstra.CheckRequestFilterSupport(current, filter));
        Assert.AreEqual(NeoSupportLevel.None, NeoEnvironment.CapabilityUnknownToLibrary(old, NeoCapability.RequestFilter)!.SupportLevel);
        Assert.IsNull(NeoEnvironment.CapabilityUnknownToLibrary(current, NeoCapability.RequestFilter));
        Assert.AreEqual(104u, NativeRequestFilter.ViewOptionsSize(false));
        Assert.AreEqual(112u, NativeRequestFilter.ViewOptionsSize(true));
    }

    [TestMethod]
    public unsafe void NativeDescriptorContainsTheNormalizedOwnedRules()
    {
        var snapshot = NeoRequestFilter.CreateSnapshot(new NeoRequestFilter
        {
            Rules =
            [
                new NeoRequestRule { HostMatch = NeoHostMatch.Exact, Host = "Bücher.Example.", Port = 2582 },
                new NeoRequestRule { HostMatch = NeoHostMatch.Loopback },
            ],
        });
        using var native = new NativeRequestFilter(snapshot);
        var descriptor = native.Value.Value;
        Assert.AreEqual(32u, descriptor.size);
        Assert.AreEqual(1u, descriptor.version);
        Assert.AreEqual(2u, descriptor.rule_count);
        Assert.AreEqual((ulong)Base, descriptor.required_flags);
        var exact = descriptor.rules[0].Value;
        Assert.AreEqual(48u, exact.size);
        Assert.AreEqual(NativeMethods.neoastra_request_host_match.NEOASTRA_REQUEST_HOST_EXACT, exact.host_match.Value);
        Assert.AreEqual("xn--bcher-kva.example", Utf8String.Decode(exact.host));
        Assert.AreEqual(2582, exact.port);
        Assert.AreEqual(3u, exact.schemes);
        Assert.AreEqual(uint.MaxValue, exact.kinds);
        var loopback = descriptor.rules[1].Value;
        Assert.AreEqual(-1, loopback.port);
        Assert.AreEqual(0UL, loopback.host.Value.length);
        using var empty = new NativeRequestFilter(NeoRequestFilter.Empty);
        Assert.AreEqual(0u, empty.Value.Value.rule_count);
        Assert.IsTrue(empty.Value.Value.rules is null);
    }

    [TestMethod]
    public void DefaultsAreTheDocumentedBase()
    {
        var rule = new NeoRequestRule();
        Assert.AreEqual(NeoHostMatch.Any, rule.HostMatch);
        Assert.IsNull(rule.Host);
        Assert.IsNull(rule.Port);
        Assert.AreEqual(NeoRequestSchemes.Http | NeoRequestSchemes.Https, rule.Schemes);
        Assert.AreEqual(NeoResourceKinds.All, rule.Kinds);

        var filter = new NeoRequestFilter();
        Assert.AreEqual(0, filter.Rules.Count);
        Assert.AreEqual(Base, filter.RequiredFlags);
        Assert.AreEqual(0, NeoRequestFilter.Empty.Rules.Count);
        Assert.AreEqual(Base, NeoRequestFilter.Empty.RequiredFlags);
        Assert.AreSame(NeoRequestFilter.Empty, NeoRequestFilter.CreateSnapshot(null));
        Assert.AreSame(NeoRequestFilter.Empty, NeoRequestFilter.CreateSnapshot(filter));
        Assert.AreSame(NeoRequestFilter.Empty, NeoRequestFilter.CreateSnapshot(new NeoRequestFilter { RequiredFlags = NeoRequestFilterFlags.None }));
    }

    [TestMethod]
    public void EnumValuesMatchTheNativeContract()
    {
        // Bit n of NeoResourceKinds is NeoResourceKind n, the native neoastra_resource_kind_t value.
        foreach (var kind in Enum.GetValues<NeoResourceKind>())
            Assert.AreEqual(1u << (int)kind, (uint)Enum.Parse<NeoResourceKinds>(kind.ToString()), kind.ToString());
        CollectionAssert.AreEqual(new uint[] { 0, 1, 2, 3 }, Enum.GetValues<NeoHostMatch>().Select(static value => (uint)value).ToArray());
        CollectionAssert.AreEqual(new uint[] { 0, 1, 2, 4, 8 }, Enum.GetValues<NeoRequestSchemes>().Select(static value => (uint)value).ToArray());
        CollectionAssert.AreEqual(
            new ulong[] { 0, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512 },
            Enum.GetValues<NeoRequestFilterFlags>().Select(static value => (ulong)value).ToArray());
    }

    [TestMethod]
    public void SnapshotAddsTheBaseAndKeepsExtraFlags()
    {
        var snapshot = NeoRequestFilter.CreateSnapshot(new NeoRequestFilter
        {
            Rules = [new NeoRequestRule { Port = 2582 }],
            RequiredFlags = NeoRequestFilterFlags.WebSockets,
        });
        Assert.AreEqual(Base | NeoRequestFilterFlags.WebSockets, snapshot.RequiredFlags);
        Assert.IsTrue(snapshot.IsSnapshot);
        Assert.AreSame(snapshot, NeoRequestFilter.CreateSnapshot(snapshot));

        var flagsOnly = NeoRequestFilter.CreateSnapshot(new NeoRequestFilter { RequiredFlags = NeoRequestFilterFlags.Redirects });
        Assert.AreEqual(0, flagsOnly.Rules.Count);
        Assert.AreEqual(Base | NeoRequestFilterFlags.Redirects, flagsOnly.RequiredFlags);
    }

    [TestMethod]
    public void RulesAreCopiedAndSnapshotsCannotBeMutated()
    {
        var first = new NeoRequestRule { HostMatch = NeoHostMatch.Exact, Host = "Example.COM." };
        var list = new List<NeoRequestRule> { first };
        var filter = new NeoRequestFilter { Rules = list };
        list.Add(new NeoRequestRule());
        list[0] = new NeoRequestRule { Port = 1 };
        Assert.AreEqual(1, filter.Rules.Count);
        Assert.AreSame(first, filter.Rules[0]);

        var snapshot = NeoRequestFilter.CreateSnapshot(filter);
        Assert.AreNotSame(first, snapshot.Rules[0]);
        Assert.AreEqual("example.com", snapshot.Rules[0].Host);
        Assert.AreEqual("Example.COM.", first.Host);
        Assert.IsNotInstanceOfType<NeoRequestRule[]>(snapshot.Rules);
        Assert.IsNotInstanceOfType<List<NeoRequestRule>>(snapshot.Rules);
        var asList = (IList<NeoRequestRule>)snapshot.Rules;
        Assert.IsTrue(asList.IsReadOnly);
        Assert.ThrowsExactly<NotSupportedException>(() => asList[0] = new NeoRequestRule());
        Assert.ThrowsExactly<NotSupportedException>(() => asList.Add(new NeoRequestRule()));
        Assert.IsTrue(((IList<NeoRequestRule>)NeoRequestFilter.Empty.Rules).IsReadOnly);

        var array = new[] { new NeoRequestRule { Port = 80 } };
        var fromArray = NeoRequestFilter.CreateSnapshot(new NeoRequestFilter { Rules = array });
        array[0] = new NeoRequestRule { Port = 81 };
        Assert.AreEqual(80, fromArray.Rules[0].Port);
    }

    [TestMethod]
    public void RuleCountIsLimited()
    {
        Assert.ThrowsExactly<ArgumentNullException>(() => new NeoRequestFilter { Rules = null! });
        var limit = Enumerable.Range(1, 256).Select(port => new NeoRequestRule { Port = port }).ToArray();
        Assert.AreEqual(256, NeoRequestFilter.CreateSnapshot(new NeoRequestFilter { Rules = limit }).Rules.Count);
        var over = Enumerable.Range(1, 257).Select(port => new NeoRequestRule { Port = port }).ToArray();
        AssertRejected(new NeoRequestFilter { Rules = over });
        AssertRejected(new NeoRequestFilter { Rules = [new NeoRequestRule(), null!] });
    }

    [TestMethod]
    public void MalformedCriteriaAreRejected()
    {
        AssertRejected(new NeoRequestFilter { RequiredFlags = (NeoRequestFilterFlags)(1ul << 10) });
        AssertRejected(new NeoRequestFilter { RequiredFlags = (NeoRequestFilterFlags)(1ul << 63) });
        AssertRuleRejected(new NeoRequestRule { HostMatch = (NeoHostMatch)4 });
        AssertRuleRejected(new NeoRequestRule { HostMatch = (NeoHostMatch)(-1) });
        AssertRuleRejected(new NeoRequestRule { Schemes = NeoRequestSchemes.None });
        AssertRuleRejected(new NeoRequestRule { Schemes = (NeoRequestSchemes)16 });
        AssertRuleRejected(new NeoRequestRule { Kinds = NeoResourceKinds.None });
        AssertRuleRejected(new NeoRequestRule { Kinds = (NeoResourceKinds)(1u << 13) });
        AssertRuleRejected(new NeoRequestRule { Kinds = NeoResourceKinds.Fetch | (NeoResourceKinds)(1u << 31) });
        foreach (var port in new[] { 0, -1, -2, 65536, int.MaxValue })
            AssertRuleRejected(new NeoRequestRule { Port = port });

        var accepted = NeoRequestFilter.CreateSnapshot(new NeoRequestFilter
        {
            Rules =
            [
                new NeoRequestRule { Port = 1, Schemes = NeoRequestSchemes.Ws | NeoRequestSchemes.Wss, Kinds = NeoResourceKinds.Document | NeoResourceKinds.Manifest },
                new NeoRequestRule { HostMatch = NeoHostMatch.Loopback, Port = 65535 },
            ],
        });
        Assert.AreEqual(2, accepted.Rules.Count);
    }

    [TestMethod]
    public void HostPresenceDependsOnTheMode()
    {
        AssertRuleRejected(new NeoRequestRule { HostMatch = NeoHostMatch.Any, Host = "example.com" });
        AssertRuleRejected(new NeoRequestRule { HostMatch = NeoHostMatch.Any, Host = "" });
        AssertRuleRejected(new NeoRequestRule { HostMatch = NeoHostMatch.Loopback, Host = "localhost" });
        AssertRuleRejected(new NeoRequestRule { HostMatch = NeoHostMatch.Exact });
        AssertRuleRejected(new NeoRequestRule { HostMatch = NeoHostMatch.Suffix });
        AssertRuleRejected(new NeoRequestRule { HostMatch = NeoHostMatch.Exact, Host = "" });
    }

    [TestMethod]
    [DataRow("example.com", "example.com")]
    [DataRow("EXAMPLE.com.", "example.com")]
    [DataRow("my_host.local", "my_host.local")]
    [DataRow("xn--bcher-kva.example", "xn--bcher-kva.example")]
    [DataRow("Bücher.Example", "xn--bcher-kva.example")]
    [DataRow("bücher.example.", "xn--bcher-kva.example")]
    [DataRow("localhost", "localhost")]
    [DataRow("127.0.0.1", "127.0.0.1")]
    [DataRow("127.1", "127.0.0.1")]
    [DataRow("2130706433", "127.0.0.1")]
    [DataRow("0x7F.0.0.1", "127.0.0.1")]
    [DataRow("0177.0.0.01", "127.0.0.1")]
    [DataRow("192.168.0.1.", "192.168.0.1")]
    [DataRow("::1", "::1")]
    [DataRow("[::1]", "::1")]
    [DataRow("0:0:0:0:0:0:0:1", "::1")]
    [DataRow("::FFFF:127.0.0.1", "::ffff:7f00:1")]
    [DataRow("::ffff:7f00:1", "::ffff:7f00:1")]
    [DataRow("2001:DB8:0:0:1:0:0:1", "2001:db8::1:0:0:1")]
    [DataRow("2001:db8:0:1:1:1:1:1", "2001:db8:0:1:1:1:1:1")]
    [DataRow("fe80:0000::00ab", "fe80::ab")]
    public void ExactHostsAreCanonical(string host, string expected)
    {
        var snapshot = NeoRequestFilter.CreateSnapshot(new NeoRequestFilter { Rules = [new NeoRequestRule { HostMatch = NeoHostMatch.Exact, Host = host }] });
        Assert.AreEqual(expected, snapshot.Rules[0].Host);
        // Canonical output is a fixed point, as the native validator requires.
        Assert.IsTrue(NeoRequestFilter.TryNormalizeHost(expected, out var again, out _));
        Assert.AreEqual(expected, again);
    }

    [TestMethod]
    [DataRow("example.com.")]
    [DataRow("Sub.Example.com")]
    [DataRow("bücher.example")]
    [DataRow("localhost")]
    public void SuffixAcceptsDnsNames(string host)
    {
        var snapshot = NeoRequestFilter.CreateSnapshot(new NeoRequestFilter { Rules = [new NeoRequestRule { HostMatch = NeoHostMatch.Suffix, Host = host }] });
        Assert.IsFalse(snapshot.Rules[0].Host!.EndsWith('.'));
    }

    [TestMethod]
    [DataRow("127.0.0.1")]
    [DataRow("127.1")]
    [DataRow("0x7f000001")]
    [DataRow("::1")]
    [DataRow("[::ffff:127.0.0.1]")]
    [DataRow("example.1")]
    public void SuffixRejectsIpLiterals(string host)
    {
        AssertRuleRejected(new NeoRequestRule { HostMatch = NeoHostMatch.Suffix, Host = host });
        // The same literal is valid for an exact match unless it is no address at all.
        if (host != "example.1")
            NeoRequestFilter.CreateSnapshot(new NeoRequestFilter { Rules = [new NeoRequestRule { HostMatch = NeoHostMatch.Exact, Host = host }] });
    }

    [TestMethod]
    [DataRow(".")]
    [DataRow("..")]
    [DataRow(".example.com")]
    [DataRow("example..com")]
    [DataRow("example.com..")]
    [DataRow("exa mple.com")]
    [DataRow(" example.com")]
    [DataRow("example.com\0")]
    [DataRow("exam\tple.com")]
    [DataRow("ex%61mple.com")]
    [DataRow("user@example.com")]
    [DataRow("example.com/path")]
    [DataRow("example.com:80")]
    [DataRow("example.com?x")]
    [DataRow("exa*mple.com")]
    [DataRow("[example.com]")]
    [DataRow("[127.0.0.1]")]
    [DataRow("256.0.0.1")]
    [DataRow("1.2.3.4.5")]
    [DataRow("1.2.3.256")]
    [DataRow("1.2.65536")]
    [DataRow("4294967296")]
    [DataRow("08.0.0.1")]
    [DataRow("1.x.3.4")]
    [DataRow("::1::")]
    [DataRow("[::1]:80")]
    [DataRow("::1]")]
    [DataRow("[::1")]
    [DataRow("fe80::1%eth0")]
    [DataRow("1:2:3:4:5:6:7:8:9")]
    public void MalformedHostsAreRejected(string host)
    {
        AssertRuleRejected(new NeoRequestRule { HostMatch = NeoHostMatch.Exact, Host = host });
    }

    [TestMethod]
    public void HostLengthIsLimited()
    {
        var label = new string('a', 63);
        var host253 = $"{label}.{label}.{label}.{new string('b', 61)}";
        Assert.AreEqual(253, host253.Length);
        var snapshot = NeoRequestFilter.CreateSnapshot(new NeoRequestFilter { Rules = [new NeoRequestRule { HostMatch = NeoHostMatch.Exact, Host = host253 + "." }] });
        Assert.AreEqual(host253, snapshot.Rules[0].Host);
        AssertRuleRejected(new NeoRequestRule { HostMatch = NeoHostMatch.Exact, Host = host253 + "b" });
        AssertRuleRejected(new NeoRequestRule { HostMatch = NeoHostMatch.Suffix, Host = new string('a', 10_000) });
    }

    private static void AssertRuleRejected(NeoRequestRule rule) => AssertRejected(new NeoRequestFilter { Rules = [rule] });

    private static void AssertRejected(NeoRequestFilter filter)
    {
        var error = Assert.ThrowsExactly<ArgumentException>(() => NeoRequestFilter.CreateSnapshot(filter));
        Assert.AreEqual("filter", error.ParamName);
    }
}
