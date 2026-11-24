# Request filtering

**Status: macOS, native iOS and Android integration verified; broader rollout incomplete.**
The source contains the native ABI, Apple and Android backend support, managed
API and regenerated bindings. The `osx-arm64`, `ios-arm64`,
`iossimulator-arm64`, `android-arm64` and `android-x64` runtimes have been
rebuilt and staged. Mobile engine verification used ARM64 simulators/emulators,
not physical devices; managed end-to-end verification used macOS. Android x64
was cross-built and export-checked, not run. Other bundled runtimes still
predate this feature. Do not infer full cross-platform package readiness from
these results.

Navigation decisions and a disabled managed bridge do not prevent arbitrary
subresource or worker requests. Do not use this investigation as a network
isolation guarantee.

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

## Native contract and current coverage

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

| Source backend | Current implementation |
| --- | --- |
| macOS | `Limited`: HTTP(S) subresources, frame documents/subresources and per-view scope. `Kinds.All` only. No additional WebSocket, redirect or worker flags are advertised from the limited probes. |
| iOS | The same `Limited` base, backed by its own UIKit simulator probes. `Kinds.All` only; no additional WebSocket, redirect or worker flags. Tracked popups remain unsupported. |
| Android | `Limited`: HTTP(S) subresources, frame documents/subresources and per-view scope through native snapshots and the WebView request hook. `Kinds.All` only. Redirect destinations bypass the hook in the engine preflight; redirect, WebSocket and worker flags remain unset. |
| Windows | Common unsupported adapter: `None`. Nonempty filters are refused; empty filters are no-ops. The isolated integration candidate is not accepted because its failure path releases denied requests during view teardown; see Windows validation below. |
| GTK | Common unsupported adapter: `None`. Nonempty filters are refused; empty filters are no-ops. Implementation and runtime validation remain outstanding. |

On macOS, a tracked popup reuses the opener's immutable compiled rule entry
but gets its own content controller. An explicitly different nonempty filter
cannot be compiled within WebKit's synchronous popup callback and is refused.
An explicit empty filter removes inheritance. Replacing the child's filter
does not change its opener. Private, process-labelled temporary rule stores
remain alive through pending compiles and shared popup entries and are cleaned
up after their last owner.

UIKit shares that store/compile/install/cleanup implementation with Cocoa.
Its existing host-approved new-window behavior navigates the opener instead
of creating a tracked popup. That fallback checks the current local filter,
including when a deferred host decision outlives a filter update. A matching
rule prevents the fallback navigation; no unfiltered replacement view is made.

Android checks an owned immutable native snapshot directly on the request
thread, before custom-scheme handling. Projected custom-scheme URLs are mapped
back before matching so an HTTP(S) rule does not accidentally block local
custom content. A per-view Java gate drains a synchronous check before native
storage can be freed or reused; closed gates refuse requests without entering
JNI. No global service-worker client is installed. Local replacement does not
recall requests that already captured the previous snapshot.

## Reproduce the macOS probe

From the repository root on an arm64 Mac with the native build prerequisites:

```sh
cmake --preset macos-arm64-debug
cmake --build --preset macos-arm64-debug --target neoastra_macos_request_filter_probe
ctest --test-dir artifacts/native/macos-arm64-debug \
  -R '^neoastra_macos_request_filter_probe_baseline$' --output-on-failure
artifacts/native/macos-arm64-debug/neoastra_macos_request_filter_probe --gates
artifacts/native/macos-arm64-debug/neoastra_macos_request_filter_probe --api
```

The normal CTest baseline checks that a real NeoAstra view can request a local
resource, that a content rule blocks that fetch, and that another view sharing
the same website data store remains unaffected. It uses the public native-parent
API with an offscreen `NSView`, a private environment, and a temporary rule store.
It neither opens a visible window nor uses an existing browser profile.

The additional `--gates` run is diagnostic. It now evaluates application
requests, not the absence of TCP connections. Exit 1 means at least one
observed case failed or was inconclusive; exit 0 does not advertise a library
capability. Harness/setup failures abort separately. In particular, a
successful run cannot prove a global engine update barrier.

The `--api` mode tests the public C entry points: initial installation, required
coverage rejection, cancellation, clearing, another view of the same
environment, popup inheritance/independent replacement, disposal during
compilation and temporary-store cleanup. It is also registered as
`neoastra_macos_request_filter_api_tests`.

The shared source is `native/tests/apple_request_filter_probe.mm`. The UIKit
app runs the same loopback probes in its own application sandbox and attaches
NeoAstra to UIKit's running event loop. Its iOS API cases do not claim tracked
popup support, which the UIKit backend does not implement.

With an installed iOS simulator runtime, run:

```sh
python3 eng/run_ios_request_filter_probe.py \
  --runtime com.apple.CoreSimulator.SimRuntime.iOS-27-2
```

The runner builds a simulator-only test app, creates a new simulator, runs
baseline/gates/API modes, and shuts down and deletes only that simulator.
`--mode baseline`, `--mode gates` and `--mode api` select individual runs.
Success requires a fresh result written by the app, not merely successful
process launch. Logs are in `artifacts/native/ios-request-filter-probe/`.
Loopback HTTP allowance belongs only to the test app's plist; no production
transport-security setting is changed.

The server binds only to IPv4 loopback on OS-assigned ports. Each case has a
fresh target and source listener, `Connection: close`, and no-store responses.
It counts TCP accepts, received bytes, completed HTTP headers, and WebSocket
upgrade requests independently. It also accepts connections which send no
headers, rather than waiting for a request before counting the connection.
An observation handshake drains server work already queued before a snapshot;
this is not a synchronization mechanism with WebKit.

Every negative case has a matching unfiltered positive control. Frame fetches
use the same source origin and a second origin (`localhost` instead of
`127.0.0.1`). A redirect is an HTTP 302 through the allowed source to the
filtered target. The WebSocket endpoint records the upgrade and returns HTTP
400; it tests whether the handshake reaches the target, not successful
WebSocket messaging. The worker case creates a dedicated worker from a blob
and asks it to fetch the target.

The first probe used a broad, port-only regex. It now compiles its rules through
the shared native core in `native/src/common/request_filter.*`. Its host-rule
cases compare that matcher with real WebKit loads using exact/suffix hosts and
numeric loopback spellings, with an unfiltered control for each.

## Reproduce the Android integration probe

With the installed Android SDK/NDK and cached sample AndroidX dependencies:

```sh
python3 eng/run_android_request_filter_probe.py --api 36 --api 37.0
python3 eng/run_android_request_filter_probe.py --https --api 36 --api 37.0
python3 -m unittest discover -s eng/tests -p '*request_filter_probe.py'
```

The runner builds the real backend Java sources and native library together
with a C-ABI/JNI test driver. It downloads nothing, creates disposable ARM64
AVDs, uses explicit emulator serials and maps three host loopback listeners
with `adb reverse`. It removes only its own emulator/AVD resources. The test
manifest's cleartext allowance does not change production network policy.
Both a fresh in-app PASS result and host request counters must pass. Partial
HTTP headers fail verification; empty transport connections are diagnostic.
With `--https`, OpenSSL generates a disposable CA and loopback server certificate.
Only the temporary test APK trusts that CA, scoped to `127.0.0.1`; no device or
host trust store is modified. The server counts decoded HTTP bytes after TLS
separately from successful handshakes. Unit tests require the fixture CA and
reject an incorrect server hostname.

On 24 November 2026, both API 36 / WebView **133.0.6943.137** and API 37.0 /
WebView **145.0.7632.218** passed the expanded integrated probe over both HTTP
and HTTPS. A source page marker first confirms working transport. Filtered A
and unfiltered B share the same environment/profile. Verified cases include:

- Initial navigation, fetch/XHR, image, script, CSS, EventSource, audio metadata,
  Beacon and no-cors POST.
- Frame documents, same-/cross-origin frame fetches and images, frame form
  GET/POST, main-frame form GET and same-origin POST, and reload after installation.
  The main-frame POST starts from an already-loaded target-origin page: the
  existing cross-origin navigation policy must not masquerade as filter protection.
- Host-approved popup fallback and a deferred popup decision answered after a
  filter update, each with an unfiltered control. The test enables script-opened
  windows only on its fixture views; production gesture settings are unchanged.
- Cancellation retaining the old filter, unsupported flags, replacement and
  clearing; projected custom-scheme content still loads.
- A request-thread/disposal race through the actual JNI path, pending-operation
  disposal and a stale Java client after new-view creation.

All four runs passed the 55 host-counter expectations. Positive controls
reached their servers; blocked cases sent no application bytes. No-cors fetch
and Beacon can appear successful in JavaScript even when the host received
nothing; their JavaScript results alone are not used as blocking evidence.

Artifacts are in `artifacts/native/android-request-filter-probe/api36/` and
`api37.0/`, with `-https` appended for TLS runs: `device.txt`, `nafilter.txt`,
`summary.json` and `server-records.jsonl`. Superseded runs remain under
`superseded/`. The earlier standalone engine preflight is separate:
it observed subresource redirect destinations reaching the target despite the
filter. Those results are not relabelled as redirect protection.

These integration fixtures use loopback origins, not a public-to-local
network origin. They do not establish coverage for every resource type,
older Android/WebView versions, physical devices, x64 runtime execution,
Android managed-host packaging or CodeAlta's server guard. The first two
API 36 attempts failed even the unfiltered transport control with
`ERR_ADDRESS_UNREACHABLE`; only the corrected loopback-tunnel runs above
count as integration evidence. The first API 36 TLS run had passing app and
host results but no captured Logcat verdict and was therefore failed. The
runner now requests a compact, on-device tag-filtered dump and checks the adb
exit status; all four expanded reruns include the required verdict.

## Windows validation (integration not accepted)

### Standalone engine preflight

A standalone Win32/WebView2 probe on VINBBWI completed its second run on
24 November 2026 with nine passing cases and four separate diagnostics, using
runtime `158.0.4370.33` and SDK `1.0.4078.44`. Querying `_22` and installing the
broad document-source filter before navigation succeeded. Filtered and
unfiltered views shared an environment/profile; document/frame/subresource
controls, same-origin POST and sequential remove/add/replace were exercised.
The blocked initial document opened two TCP connections but sent zero HTTP bytes.

Redirect targets and dedicated-worker fetches were observed through the hook.
Shared-worker fetches and service-worker traffic bypassed its document-only
source registration. These samples do not establish additional capability flags
or an atomic update barrier. The fixture used HTTP loopback and its own port/path
matcher, **not the NeoAstra matcher or C ABI**. Source integration, lifecycle,
popup, failure-path and TLS checks remain separate work.

Evidence is under `C:/Users/bwi/neoastra-probe/out` on VINBBWI, including
`summary.json`, `host_counters.json`, `host_request_log.json` and `hooks.jsonl`.
The first run under `out_run1` had incomplete JavaScript diagnostics and is not
the final result. The delayed report was interrupted by the agent provider's
quota; it was not a test timeout. Windows still reports `None` in this worktree
until its actual backend integration is implemented and verified.

### C-ABI review candidate: blocked

On 24 November 2026, the isolated Windows integration candidate completed its
native build and ordinary integration run with 217 host-counter assertions,
the simulated missing-interface run with 46 assertions, and eight non-GUI
native tests. Disabling the refusal decision made the integration test fail.
The received logs confirm these results; the build also contains `STL4029`
deprecation warnings for the shared-pointer atomic helper functions.

The candidate covers initial installation, replacement/removal, per-view
isolation, popup inheritance/disposal, and requests from a running, allowed
third-origin frame. Positive controls include cross-origin OPTIONS/POST and
decoded request bodies. A prepared 403 response also prevented target requests
when construction or assignment of the fresh response was fault-injected.

**The remaining failure path does not satisfy the contract.** When neither
response can be assigned, a deferral holds the request while the view lives.
Disposing that view nevertheless sends GETs for `/b_s.js` and `/b_img.png` to
the denied origin. Instrumented traces also record 587 request bytes for
`/b_s.js` during controller teardown. This is application traffic, not merely
a TCP connection. Holding the deferral indefinitely is not a disposal-safe fix.

Skipping handler removal, skipping controller `Close()`, and retaining the
deferral beyond state destruction did not establish a safe teardown path.
Immediate `Stop()` also failed. Diagnostic runs with 600 ms of message pumping
after `Stop()` or `about:blank` observed no target requests, but no reliable
abort-completion signal was established. These delays were not adopted.
The candidate instead notifies the host with `WEB_PROCESS_TERMINATED` and
`RECREATE_VIEW`; recreating the view still entails the unsafe teardown.

The failure-test executable reports 18 assertions and exits successfully
because it asserts the hold before disposal and only logs the subsequent
release. That diagnostic success is **not** successful security conformance.
The original construction-failure and deferred-popup regression failures
remain recorded separately from their corrected runs.

The parent received and hash-checked the patch and all 36 manifest-listed
log/provenance files. Evidence is under
`artifacts/windows-request-filter-handoff/r2-review-logs/`, particularly
`logs/{before,final}/failure.log` and `logs/after6/failure.log`.
The patch has not been applied and no Windows runtime has been staged.
TLS, managed integration, a real older runtime, failure of the deferral itself,
and popup-allocation fault injection remain unverified. No additional worker,
redirect or WebSocket capability is claimed.

## Earlier observations under the strict contract

Observed on **24 November 2026**, macOS **27.0.1 (26A434)**, WebKit framework
**22625.1.29.11.28**, arm64, SDK **27.2**:

| Case | Observation |
| --- | --- |
| First main document, content rule installed before navigation | Control reached the target. Filtered navigation failed, but the target still accepted one TCP connection, with zero HTTP requests and zero received bytes. **Fails the no-connection requirement.** |
| First main document with content rule and native navigation cancellation | The native `NavigationRequested` callback was invoked and answered `Cancel` (asserted by the probe). The target still accepted one TCP connection, with zero requests and bytes. |
| Navigation started by the already-loaded page | Unfiltered controls reached the target. Filtered attempts produced zero target accepts and bytes, both with and without the additional native cancellation. |
| Fetch, XHR, image and script | Positive controls each reached the target; filtered attempts reported error with zero target accepts and zero bytes. |
| Fetch from same-origin and cross-origin frames | Positive controls reached the target; filtered attempts reported error with zero target accepts and zero bytes. |
| Frame document | Control loaded and reached the target. The filtered frame produced neither load nor error within five seconds, with zero target accepts and bytes. **Inconclusive**, not counted as a passing gate. |
| HTTP 302 redirect followed by fetch | Control reached the redirected target; filtered attempt reported error with zero target accepts and bytes. |
| WebSocket handshake | Control sent an upgrade request and received the test endpoint's rejection. Filtered attempt reported error without a connection or upgrade at the target. |
| Dedicated-worker fetch | Control reached the target; filtered attempt reported error with zero target accepts and bytes. |
| Two views in the same environment/data store | The filtered view's fetch was refused; the unfiltered view still reached the target. |
| Fetch immediately after adding/removing a compiled list | In the observed run, add blocked the fetch and remove allowed it. **This is an example, not a proven cut-over barrier.** |

The first-document result is a counterexample to treating a content-blocking
error as proof that no connection was opened. It does not show that HTTP data
escaped. Checking a host-supplied navigation before calling the engine could
prevent that particular attempt; canceling the later navigation callback did
not. The tested page-initiated navigation behaved differently and did not
connect. These results do not establish coverage for every navigation source,
popup, redirect or speculative connection.

These are narrowly scoped measurements, not a complete security conformance
matrix. Still unverified: full URL/alias/default-port matching, HTTPS/WSS,
all resource types, preconnect/DNS-prefetch, shared/service workers, popups,
multiple redirect status codes and chains, and concurrent update/cancellation/
disposal. Cross-origin loopback tests are not public-to-private network tests.
There are no runtime results here for Windows, GTK, iOS or Android.

## Engine propagation: limit of the public API

The earlier API proposal required `SetRequestFilterAsync` to complete only
after new requests from already-running documents, frames and supported
workers used the replacement rules. Sleeping, retrying, or observing one
blocked request does not establish that stronger contract.

A read-only inspection of upstream WebKit source found the following:

- Public `WKUserContentController` add/remove methods return `void`.
  The compilation completion belongs to `WKContentRuleListStore`, not to
  propagation of the installed list.
- `WebUserContentControllerProxy::addContentRuleList` and the remove paths
  update the UI-process state, then send rule updates to WebContent and network
  processes without a reply.
- `NetworkContentRuleListManager::contentExtensionsBackend` can wait for a
  missing backend, but an existing backend is returned immediately. That
  distinction matters for replacing or removing already installed rules.
- `NetworkLoadChecker::processContentRuleListsForLoad` consults the network-side
  manager. A WebContent round trip alone does not establish ordering on the
  separate connection to the network process.

These findings are from upstream `main`, **not** a source-to-binary match for the
installed macOS build. The resulting risk is an inference: the inspected public
path does not establish the required cross-process acknowledgement, so a
network-side copy could still be stale. No public barrier was identified.
No timing race was reproduced by the local sequential add/fetch sample, and
neither that sample nor the source review proves that a different supported
mechanism cannot exist.

The earlier plan stopped at this gate. The approved revision instead defines
completion as local installation through the public engine APIs, not a
cross-process cut-over. This missing acknowledgement is therefore no longer
an implementation blocker. Neither private SPI, forced reloads nor a fixed
sleep is used to pretend a stronger guarantee exists.

In that earlier run, the native Debug build and all 16 CTest cases, including
the baseline, passed. The baseline also passed five consecutive runs. The
then-stricter diagnostic gate returned exit 1 because of its no-connection
requirement and the inconclusive frame-document case. Those results are not
retroactively changed.

Primary source paths under the WebKit repository, inspected on the same date:

- `Source/WebKit/UIProcess/API/Cocoa/WKUserContentController.h` and `.mm`
- `Source/WebKit/UIProcess/API/Cocoa/WKContentRuleListStore.h`
- `Source/WebKit/UIProcess/UserContent/WebUserContentControllerProxy.cpp`
- `Source/WebKit/NetworkProcess/NetworkContentRuleListManager.cpp`
- `Source/WebKit/NetworkProcess/NetworkLoadChecker.cpp`

Repository: `https://github.com/WebKit/WebKit`; source files were read from
`https://raw.githubusercontent.com/WebKit/WebKit/main/` with those paths.

## Revised request-contract run

On 24 November 2026, after approval of the revised plan, the modified probe
and its baseline passed on the same macOS/WebKit versions. `--gates` returned
exit 0 under the request contract:

- The initial document still caused one TCP accept, but no HTTP request or
  application byte. TCP accepts remain visible in the output.
- The frame-document probe now records its exact navigation URI in the native
  navigation callback. Its unfiltered control reaches the target. With the
  content list, that native callback proves the attempt occurred, while the
  five-second observation window records no application bytes. No load/error
  event is required; a timeout without that additional evidence stays inconclusive.
- The other previously observed blocking and isolation cases still passed.
- After add and remove, the test samples requests until it observes the
  intended effect or its test deadline expires, recording the per-sample
  counter differences. In this run both effects appeared in the first sample.
  This is test observation, not a production waiting algorithm or a promised
  propagation duration.

This diagnostic run alone is not a complete backend capability matrix.
The subsequent ABI/operation tests and macOS API mode exercise installation
and lifetime separately. Managed integration is verified below; full platform
conformance remains outstanding.
CodeAlta's server-side guard remains responsible for rejecting page-originated
MCP commands, including during port discovery and engine propagation. Its
real-browser integration tests are a separate host acceptance task.

## UIKit verification

On 24 November 2026, all three probe modes passed in a fresh **iOS 27.2
(24B8117)** ARM64 simulator with **WebKit 8625.2.8.10.7**. Positive controls
covered the request paths before checking their filtered counterparts.
The diagnostic gates included initial/page navigation, fetch/XHR, images,
scripts, frame documents and frame fetches, host aliases, view isolation and
observed add/remove effects. Diagnostic redirect/WebSocket/worker samples
do not enable those capability flags.

The public API checks passed initial installation, rejection of missing
coverage, cancellation, nonempty replacement, clearing, view isolation,
disposal during compilation and private-store cleanup. A regression first
reproduced a deferred popup decision starting an opener navigation after a
filter update; the current-snapshot check now prevents it. The complete
baseline/gates/API run passed after that fix.

The device archive was cross-built and export-checked, not run on a physical
iPhone. Neither the iOS 15 deployment target nor older iOS releases were
runtime-tested. These native UIKit tests are not an iOS managed-host test.

## Shared native core

`request_filter.hpp/.cpp` owns and validates copied rule lists, parses bounded
ASCII network targets, matches host/port/scheme/resource kind, and generates
bounded WebKit content-list JSON. It handles the browser's canonical URL form
and common numeric IPv4/IPv6 loopback spellings; it is not a general URL parser
or a DNS resolver. Native rule hosts must already be canonical ASCII. The
managed boundary normalizes IDNs before passing them to this core.

The WebKit compiler currently accepts `Kinds.All` only, rejecting narrower
kind selections rather than broadening them to WebKit's `raw` category. It
emits separate alternatives without regex alternation and escapes authority
boundaries, hosts and JSON strings. Expansion is bounded independently of
the 256-input-rule limit.

`neoastra_request_filter_tests` covers validation, owned snapshots, exact and
suffix boundaries, effective ports, loopback spellings, malformed-network-URL
rejection, resource masks, a JSON golden case and expansion limits. Numeric
IPv4 cases were checked against the primary Web Platform Tests URL vectors:
`https://raw.githubusercontent.com/web-platform-tests/wpt/master/url/resources/urltestdata.json`.
The actual JSON compiler is also exercised by the macOS probe. The rebuilt
native library exposes only the conservative Apple base capability described
above, not every class sampled by the diagnostic probe.

`neoastra_request_filter_operation_tests` uses a controllable backend to check
V1/V2 layout handling, deep copying, FIFO installation, preparation failure,
cancellation/commit ordering, throwing callbacks, creation delayed until
filter installation, disposal, shutdown and late callbacks. The independent
ABI-1.7 consumer continues to test an actual old struct allocation against the
rebuilt library.

The final native Debug run on 24 November 2026 passed all 19 CTest cases and
the diagnostic host-alias gate. ASan/UBSan passed the common tests, matcher,
operation-lifetime tests and real macOS API tests; leak detection was disabled,
as in the project's existing sanitizer configuration. A creation-path
argument-validation regression found by the full suite was fixed and rechecked.

The final macOS Release staging run also passed all 19 native tests and retained
the runtime's existing minimum deployment target of macOS 26.0 (built with SDK
27.2). This deployment target is not evidence of a test run on macOS 26.
All three restaged Apple runtime manifests match their binaries' SHA-256 hashes.

After the expanded HTTPS checks, the Release solution build with SDK `10.0.114`
passed without warnings or errors: 670 .NET tests passed and 82 were skipped,
including the Java lifetime-gate and TLS-server regressions. All five restaged runtime manifests match
their binaries' SHA-256 hashes. The focused managed conformance run after Apple
staging passed snapshot capture, cross-thread dispatch,
FIFO installation, cancellation, disposal and tracked-popup inheritance. A
separate process loaded the original native library from the repository's HEAD
and passed rejection of unsupported rules and the compatible empty-filter path.

The managed filter-only mode also uses disposable HTTPS loopback servers
on backends with TLS-decision support. On macOS it passed source/target positive
controls, same- and cross-origin frame fetches, per-view refusal with zero
additional decoded HTTP bytes, and clearing.
Its certificate allowance exists only on these fixture views; no system trust
store or engine-wide TLS setting is changed. Some backends omit the challenge
URI, so this test callback is not a certificate-policy example for untrusted
content. Server tests separately reject the untrusted certificate and verify
that partial HTTP bytes after TLS still count.

Run the managed checks without other automation or device scenarios:

```sh
dotnet run --project src/NeoAstra.Conformance -c Release -- --run --request-filter-only
```

The broader `--run` conformance process exited 137 after a pre-existing
microphone-capture case; its complete run is not counted as passed. The exact
termination cause was not established. Explicit media-test opt-in is proposed
separately as task `rx38`; the request-filter-only mode does not run those cases.

## CodeAlta host acceptance handoff

G5 remains a separate CodeAlta task, not a claim established by these library
tests. In an explicitly authorized disposable CodeAlta worktree/profile, use
the real `DesktopMcpGuard` and middleware before a fake MCP tool dispatcher:

- Establish a positive non-browser client control that increments a fake-tool
  counter; use no real application-control tools.
- From real browser pages, test remote/local origins, same-origin fixtures,
  opaque origins, frames, fetch/XHR, form/no-cors POST, preflight, WebSocket
  upgrade and available workers. Record actual headers and guard decisions.
- Test without the filter first, then with it. Start a new ephemeral MCP port
  while pages are alive, before the port list/filter learns it.
- Require zero unauthorized tool invocations. A browser-side refusal that
  prevents a request from reaching the guard is not itself a guard test.

Do not copy a simplified guard into NeoAstra or target a running CodeAlta
instance. The tested host code must determine behavior when Fetch Metadata is
absent; local `Origin` values alone were not universally rejected in the
inspected compatibility path.

## Remaining execution prerequisites

The earlier .NET commands exited 155 because SDK `10.0.114` was absent. After
explicit approval, that exact SDK was installed under
`~/.dotnet/neoastra-sdk-10.0.114`, with the official archive's SHA-512 verified.
`global.json` and the system SDK installation were not changed. Builds in this
session use that installation explicitly:

```sh
export DOTNET_ROOT="$HOME/.dotnet/neoastra-sdk-10.0.114"
export DOTNET_ROOT_ARM64="$DOTNET_ROOT"
export PATH="$DOTNET_ROOT:$PATH"
```

Interop and iOS symbol declarations were regenerated with it. Both iOS
archives were rebuilt and export-checked after UIKit integration. Simulator
engine evidence is described above; physical-device and iOS managed-host
verification remain outstanding.

The initial Windows Fleet check was refused with `machine.policyDenied` /
`agent_sessions_off`. After owner authorization, VINBBWI accepted the preflight.
The missing build prerequisites were installed with explicit permission in
isolated user directories, including .NET SDK `10.0.114`, clang-cl/LLVM, CMake,
Ninja, Python and the pinned WebView2 SDK. No existing checkout, system PATH,
trust store or production service was modified. Windows integration remains
blocked by the failure-path evidence above; Linux/WebKitGTK gates still require
actual runtime tests. A build for another RID does not replace them. Android's
verified integration and remaining test boundaries are recorded above; it does
not substitute for these desktop or CodeAlta host checks.
