# Request filtering

A host can give a view declarative, refuse-only rules: requests to matching
hosts and ports are refused before they reach their destination. The rules are
matched natively from an immutable snapshot, without a managed callback or a DNS
lookup in the request path, and can be replaced or cleared while the view runs.

It is an **additional protection layer, not a network sandbox**. A typical use
is an embedded browser view that must not reach a few known local ports (a local
tool or control server, for example) while other local servers stay reachable.
Keep the server's own access checks in place: a filter does not replace them.

Not promised on any backend: that no TCP connection, DNS lookup or TLS handshake
happens; that requests already in flight are recalled; that an update reaches
every engine process at once or within a fixed time. Navigation decisions and a
disabled managed bridge do not prevent subresource or worker requests either.

## Managed use

```csharp
var support = environment.GetCapability(NeoCapability.RequestFilter);
if (support.SupportLevel != NeoSupportLevel.None)
{
    var filter = new NeoRequestFilter
    {
        Rules = [new NeoRequestRule { Port = 2582 }, new NeoRequestRule { Port = 2583 }],
    };
    await using var view = await environment.CreateWebViewAsync(
        NeoAstraHost.FillWindow(window), new NeoAstraOptions { RequestFilter = filter });

    await view.SetRequestFilterAsync(new NeoRequestFilter
    {
        Rules = [new NeoRequestRule { Port = 2582 }],
    });
    // Last locally installed, immutable snapshot; not a global engine acknowledgement.
    var installed = view.RequestFilter;
    await view.SetRequestFilterAsync(NeoRequestFilter.Empty);
}
```

`Any` host plus a port covers that port irrespective of DNS spelling, including
on remote hosts. Defaults are HTTP/HTTPS and `Kinds.All`; explicit WS/WSS or
narrow resource kinds require the corresponding capability. `RequiredFlags`
can require additional coverage beyond `Subresources | Frames | PerView`.
Missing coverage throws rather than accepting a nonempty no-op filter.

The setter is callable from any thread and always dispatches, so a UI-thread
call cannot overtake an already-posted setter. Input lists and normalized hosts
are snapshotted before dispatch. Cancellation waits for the native outcome:
if local installation won, its successful result and snapshot win as well.
Initial options are snapshotted independently of later mutations to the options
object. Where tracked popups are supported, they inherit their opener's snapshot
unless an explicit filter (including `Empty`) is supplied.

An older native library is detected before referencing the new export: it
reports `None`, rejects nonempty filters, and accepts clearing as a no-op.

## Native contract

`NEOASTRA_BUILD_FEATURE_REQUEST_FILTER` identifies the new export and view
options V2. `NEOASTRA_CAPABILITY_REQUEST_FILTER` separately reports backend
coverage. Version 1 view options remain accepted at their original 104-byte
size on the supported 64-bit targets; V2 appends the filter pointer.

The initial descriptor is copied before creation returns and installed before
the native view is published to its caller. `neoastra_view_set_request_filter_async`
copies inputs and serializes updates per view. Completion means installation
through the local public engine API, not a global engine-process barrier.
Cancellation before the local installation point prevents it; after that
point the installation result wins. Disposing a view settles pending
operations, and late compile callbacks cannot install into the disposed view.
Preparation failures preserve the old filter; an uncertain installation failure
tears down the view instead of leaving it unprotected.

## Coverage by platform

Every backend reports `Limited` with `Subresources | Frames | PerView`;
requesting a class that a backend does not advertise fails rather than silently
promising it. Windows reports `None` on a WebView2 runtime without
`ICoreWebView2_22`.

- **Yes:** implemented and passed the positive, refusal and clearing checks of
  the tested cases; not proof of every runtime or request variant.
- **Restricted:** usable only under the condition in the cell.
- **No:** not offered by the NeoAstra filter API on that backend.
- **Unverified:** no end-to-end evidence either way.

| Feature | macOS | iOS | Android | Windows | Linux (GTK) |
| --- | --- | --- | --- | --- | --- |
| Filter HTTP page navigation | Yes | Yes | Yes | Yes | Yes |
| Filter HTTP fetch/XHR, images and scripts | Yes | Yes | Yes | Yes | Yes |
| Filter HTTP frame documents and requests from frames | Yes | Yes | Yes | Yes | Yes |
| Filter HTTPS page/frame requests | Yes | Unverified | Yes | Unverified; the native suites use HTTP | Yes |
| Replace or remove an installed filter | Yes | Yes | Yes | Yes | Yes |
| Leave direct page requests of an unfiltered view alone | Yes | Yes | Yes | Yes | Yes |
| Leave another view's shared/service-worker traffic alone | Unverified | Unverified | Unverified | Unverified; worker requests are not offered to the view's handler | Restricted: with an explicitly ephemeral profile per view |
| Filter an independent tracked popup | Restricted: inherits the compiled rules | No tracked popup; the fallback navigation is filtered | No tracked popup; the fallback navigation is filtered | Yes: inherits the snapshot before its first request | No; a filtered tracked popup is refused |
| Choose individual resource kinds | No; `Kinds.All` only | No | No | No | No |
| Block every HTTP redirect hop | No | No | No; redirect targets bypass the hook | No | No |
| Filter WebSocket / WSS | No | No | No | No | No |
| Filter dedicated-worker requests | No | No | No | No | No |
| Filter shared/service-worker requests | No | No | No | No; they bypass the view's handler | No |

## Backend behavior and restrictions

**macOS and iOS.** Each update compiles into a `WKContentRuleList` in a private,
temporary rule store that lives through pending compiles and shared popup
entries and is removed after its last owner. On macOS a tracked popup reuses its
opener's compiled rules but gets its own content controller; an explicitly
different nonempty filter cannot be compiled within WebKit's synchronous popup
callback and is refused, and an explicit empty filter removes inheritance.
UIKit has no tracked popups: a host-approved new window navigates the opener,
and that fallback checks the current filter, also when a deferred decision
outlives a filter update.

**Android.** The request thread checks an owned, immutable native snapshot
before custom-scheme handling. Projected custom-scheme URLs are mapped back
before matching, so an HTTP(S) rule does not block local custom content. A
per-view Java gate drains a running check before native storage is freed;
closed gates refuse requests without entering JNI. No global service-worker
client is installed. Replacing a filter does not recall requests that already
captured the previous snapshot.

**Linux (WebKitGTK).** Each update compiles into a separate mode-0700 temporary
store with a unique list identifier; installation adds the new list before
removing the old one, and the store is removed after its last owner. A tracked
popup reuses its opener's engine controller, so a filtered tracked popup is
refused, also while the opener has a pending update. Unfiltered tracked popups
remain available, but both views afterwards reject nonempty filters with
`NotSupportedException`, even after one of them is disposed. Worker requests
stay per view only when each view uses an explicitly ephemeral profile, which
owns a `WebKitWebContext` of its own; views sharing a profile or session can
affect each other's worker requests.

**Windows (WebView2).** Each filtered view registers one `ICoreWebView2_22`
filter for document request sources and refuses, in `WebResourceRequested`, the
offered requests that its snapshot matches. WebView2 stops offering some of an
opener's requests to the handler after a window request that is answered
without a window, so a filtered view avoids that answer:

- A tracked popup gets the opener's snapshot before its first request. One
  whose filter cannot be registered after its assignment is sent the offline
  switch below and disposed; it may start loading before either takes effect.
- Every other window request is answered with a hidden popup sink: a WebView in
  the opener's profile without script that cancels its navigation and closes
  after it. A sink not created within 10 s fails its request closed; an
  assigned one closes at the latest 10 s after its assignment. At most 16 sinks
  per view and 64 per application are alive or being created.
- Without a sink, the view is sent a DevTools offline switch, then the request
  is answered. The switch lasts until the view is disposed and does not cover
  service workers. It is not awaited, because waiting while the request is
  deferred stalls the page, so requests issued before it takes effect can still
  go out. It needs the DevTools Network domain, which is enabled on every
  filtered view. The library logs `Request filter fail-closed` once the switch
  is confirmed and `Request filter degraded` otherwise; there is no host event.

A filtered view being disposed is sent the offline switch first, then unloads
its document through `about:blank` while its handler refuses every offered
request, and closes 1 s after that navigation succeeded, otherwise when its 3 s
timer fires. `Close()` alone released in-flight pagehide requests in the tested
runtime. At most 16 views unload at a time; when unloading cannot start, the
view closes at once, protected only by the offline switch, and the library
logs a warning.

## Verification

| Backend | Tested on | Tests |
| --- | --- | --- |
| macOS | arm64 | `neoastra_macos_request_filter_probe_baseline` and `_api_tests` in CTest; managed tests and the conformance harness |
| iOS | iOS 27.2 simulator (arm64) | `eng/run_ios_request_filter_probe.py` |
| Android | API 36 (WebView 133) and API 37.0 (WebView 145) arm64 emulators, HTTP and HTTPS | `eng/run_android_request_filter_probe.py` |
| Linux | GTK 4.22.4, WebKitGTK 2.52.6, Ubuntu 26.04 arm64 | `neoastra_linux_request_filter_*` in CTest |
| Windows | WebView2 158 and 157 (hosted CI runner), x64 | `neoastra_windows_request_filter_tests` and `_failure_tests` (CTest label `browser`), `_unsupported_tests` |

The tests count requests, received bytes and partial headers at loopback
servers; each refused case has an unfiltered control that must reach its
server. JavaScript results alone are not taken as evidence: a no-cors fetch or a
beacon can appear to succeed when the server received nothing. A mutant whose
matcher never refuses is detected by the Windows main suite. Not verified:
physical iOS and Android devices, Android x64 execution, and the HTTPS cells
marked above.

### Reproduce the macOS probe

```sh
cmake --preset macos-arm64-debug
cmake --build --preset macos-arm64-debug --target neoastra_macos_request_filter_probe
ctest --test-dir artifacts/native/macos-arm64-debug -R request_filter --output-on-failure
artifacts/native/macos-arm64-debug/neoastra_macos_request_filter_probe --gates
```

The CTest baseline checks that a view can request a local resource, that a rule
blocks that request, and that another view sharing the same website data store
is unaffected; `--api` covers the public C entry points. `--gates` is a
diagnostic run of further request classes; it does not change capability flags.

With an installed iOS simulator runtime, the same probe runs in a test app:

```sh
python3 eng/run_ios_request_filter_probe.py --runtime com.apple.CoreSimulator.SimRuntime.iOS-27-2
```

The runner creates a new simulator, runs the baseline, gates and API modes, and
deletes only that simulator.

### Reproduce the Android integration probe

With the Android SDK/NDK and the cached sample dependencies installed:

```sh
python3 eng/run_android_request_filter_probe.py --api 36 --api 37.0
python3 eng/run_android_request_filter_probe.py --https --api 36 --api 37.0
```

The runner builds the real Java backend and native library with a JNI test
driver, creates disposable arm64 emulators, and maps loopback servers with
`adb reverse`. With `--https` only the test APK trusts a disposable CA. It
removes only its own emulator resources and changes no device or host trust
store.

### Linux and Windows suites

The Linux suites run in the ordinary native CTest run (they need a display, for
example Xvfb). On Windows, `python eng/build_native.py --rid win-x64 --browser-tests`
runs the two suites labelled `browser`; they use hidden windows and loopback
servers and take several minutes.
