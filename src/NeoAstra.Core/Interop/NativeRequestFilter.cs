// Copyright (c) Alexandre Mutel. All rights reserved.
// Licensed under the BSD-Clause 2 license.

using System.Runtime.InteropServices;
using NeoAstra.Interop.Generated;

namespace NeoAstra.Interop;

// Native copies the descriptor, array and strings during the entry point. This
// owner lasts only for that call, never for asynchronous compilation.
internal sealed unsafe class NativeRequestFilter : IDisposable
{
    private readonly Utf8StringArray _hosts;
    private readonly uint _count;
    private readonly ulong _flags;
    private NativeMethods.neoastra_request_rule_t* _rules;

    internal NativeRequestFilter(NeoRequestFilter snapshot)
    {
        _count = checked((uint)snapshot.Rules.Count);
        _flags = (ulong)snapshot.RequiredFlags;
        _hosts = new Utf8StringArray(snapshot.Rules.Select(static rule => rule.Host ?? string.Empty).ToArray());
        try
        {
            if (_count == 0) return;
            _rules = (NativeMethods.neoastra_request_rule_t*)NativeMemory.Alloc(_count, (nuint)sizeof(NativeMethods.neoastra_request_rule_t));
            for (var i = 0; i < snapshot.Rules.Count; i++)
            {
                var rule = snapshot.Rules[i];
                _rules[i] = new(new NativeMethods.neoastra_request_rule
                {
                    size = (uint)sizeof(NativeMethods.neoastra_request_rule),
                    version = 1,
                    host_match = (NativeMethods.neoastra_request_host_match)rule.HostMatch,
                    host = _hosts.Views[i],
                    port = rule.Port ?? -1,
                    schemes = (uint)rule.Schemes,
                    kinds = (uint)rule.Kinds,
                });
            }
        }
        catch { Dispose(); throw; }
    }

    internal NativeMethods.neoastra_request_filter_t Value => new(new NativeMethods.neoastra_request_filter
    {
        size = (uint)sizeof(NativeMethods.neoastra_request_filter),
        version = 1,
        rules = _rules,
        rule_count = _count,
        required_flags = _flags,
    });

    internal static uint ViewOptionsSize(bool hasRequestFilters)
    {
        NativeMethods.neoastra_view_options value = default;
        return hasRequestFilters ? (uint)sizeof(NativeMethods.neoastra_view_options)
            : checked((uint)((byte*)&value.request_filter - (byte*)&value));
    }

    public void Dispose()
    {
        NativeMemory.Free(_rules);
        _rules = null;
        _hosts.Dispose();
    }
}
