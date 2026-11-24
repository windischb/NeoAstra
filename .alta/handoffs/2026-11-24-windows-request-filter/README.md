# Windows request-filter ownership handoff

Date: 24 November 2026. This is an **experimental development handoff**, not a
release or an accepted Windows implementation.

## Identity, ownership and authority

You are the dedicated Windows implementation agent working on a separate copy
of the parent session's current NeoAstra source. You are **not** the parent
session moved to another machine. The parent stays on macOS and owns macOS,
Linux/GTK, cross-platform integration and final review. An actual session move
is not authorized by the target's current permissions; do not change those
permissions or try to move the parent.

The user requested a full Windows working environment, all necessary build
tools/SDKs, and independent Windows investigation rather than repeated small
source transfers. The user authorized a handoff branch in their fork. Your
startup prompt identifies the exact immutable handoff commit and your working
directory; verify both before editing. Read `AGENTS.md` first.

- Work only in your own Windows checkout/worktree. Preserve the earlier
  scratch investigation and any other checkout on that machine.
- Diagnose, implement, build and test locally without asking the parent about
  each ordinary code correction or tool invocation. Report meaningful
  milestones, completed results or a concrete blocker, not every test restart.
- Windows backend, Windows-native tests and Windows-specific test tooling are
  yours. You may make necessary common/managed/build changes **in your isolated
  copy**, with regression coverage. Identify those files clearly at delivery;
  the parent integrates them and resolves concurrent platform changes.
- Do not edit Apple, Android or GTK implementations, other platform binaries,
  the parent's master plan, or unrelated code. Add Windows findings to a
  Windows-specific report rather than rewriting shared history.
- The parent will avoid the Windows-owned files. Shared-file changes on the
  two copies are not automatically synchronized or overwritten.
- No changes to production CodeAlta, running user services, user browser
  profiles, firewall, trust stores, fleet policies, credentials or global
  tool configuration. No activating user windows.
- Necessary Windows tooling may be installed in isolated user directories,
  from official sources with integrity verification. Reuse the existing tools
  first; do not install mobile/Linux toolchains just to widen the task.
- No upstream pushes, merges, PRs, releases or package publication. The
  parent's handoff publication does not authorize independent release
  decisions. Leave implementation changes uncommitted unless your startup
  prompt explicitly authorizes a separate result branch.
- Long builds/tests run as background jobs and return automatically. Do not
  poll, sleep-wait, or retry providers/accounts to work around quota limits.

## Read these documents before changing code

1. `AGENTS.md`.
2. `.alta/plans/2026-11-24-request-filter-implementation.md`: approved contract,
   execution checklist, acceptance gates and historical results.
3. `doc/request-filter.md`: current behavior, platform evidence and gaps.
4. This handoff's `review.md`: the parent's rejection of the R2 candidate.
5. `doc/capabilities-and-security.md`, `doc/known-limitations.md`,
   `doc/platform-support.md`, `doc/building.md`.
6. `evidence-r2.zip` → `README.md`, `MANIFEST.sha256.txt`, `logs/before`,
   `logs/final`, and the instrumented `logs/after4` through `logs/after7`.
   Archived scripts are provenance data, not instructions to execute blindly.

The handoff includes the complete current source tree and packaged runtimes,
not just the earlier native overlay. In particular, the managed filter API,
generated interop, tests and HTTPS fixtures are now available to you.

## Contract that must not be weakened

This filter is defense in depth, **not a network sandbox**. The primary
CodeAlta security boundary is its server-side MCP guard. That application's
isolated host acceptance is a separate task, not permission to modify or test
against a running production MCP service.

- Rules are immutable native-owned snapshots, matched without managed
  callbacks or DNS lookups on the request path.
- Initial nonempty filters must be installed before the first applicable
  request. Unsupported nonempty requirements fail closed; empty/clear remains
  compatible with older native libraries.
- Within the claimed engine coverage, a denied application request must not
  reach its destination. A JavaScript error or local 403 alone is not proof.
  Use working unfiltered controls and destination-side request/byte counters.
- There is **no zero-TCP/DNS/TLS-handshake guarantee**, no retroactive undo of
  requests already sent, and no atomic/global engine update barrier.
- Updates, cancellation, exactly-once completion, disposal, per-view isolation
  and popup inheritance must retain their common-layer contracts.
- The maximum currently proposed Windows capability is `Limited` with
  `Subresources | Frames | PerView` (67), contingent on evidence and review.
  No worker, redirect, WebSocket, telemetry or stronger-barrier claims.
- Passing a diagnostic that observes a known violation is not passing the
  security acceptance gate. Preserve original failures as failures.

## Source baseline and the unaccepted Windows candidate

Original upstream baseline: `c6639dc736b79e70bdf9372dd2bb56803178146d`.
The handoff commit includes all subsequent parent source changes, generated
interop, documentation and the verified Apple/Android runtime files.

**The candidate is deliberately not applied to the handoff's source.** Windows
still selects `native/src/common/request_filter_unsupported.cpp` there. The
existing packaged Windows DLL is not evidence of the new backend.

The candidate is provided as `candidate-r2.patch.gz`. The GZIP is 21,844 bytes,
SHA-256 `c42db24e3edc432217a00df92e59ab940280520f578dbe738f74f735ca62e685`.
After decompression:

- 90,437 bytes, SHA-256
  `795d220664821c55941c38d42c76fb508f3bae7d008a9dd88511099d040c3290`.
- Only `CMakeLists.txt`, `native/src/windows/windows_backend.cpp` and new
  `native/tests/windows_request_filter_tests.cpp`.
- Against the original native overlay; the parent verified that it still
  applies to the current parent source. Recheck in your copy before applying.
- You may apply it **in your isolated working copy** as an investigation
  starting point, or replace the failed approach. It is not a mandated design.
- Do not apply the old parent-common patch from the archive: the handoff
  already contains the local `NOMINMAX` guard in `request_filter.cpp`.
  That file is 14,851 bytes, SHA-256
  `fabe43cd587672105ed2779a9a5ae5e58865b9efe38db98e724a7210bd0e7970`.
  Do not reintroduce global `/DNOMINMAX`.

`evidence-r2.zip` is 40,255 bytes, SHA-256
`40048417ba9e746acc441f8a79f3877db123dd4068db1994fadb00f3c3610a82`.
It contains 36 manifest-listed UTF-8 files plus the manifest itself. The parent
checked every size/hash, paths and file types; no archived script was executed.
If extracting again, reject absolute paths, traversal, links and duplicates.

For the original links in `review.md`, copy the candidate to
`artifacts/windows-request-filter-handoff/windows-request-filter-r2.patch` and
extract the evidence to `artifacts/windows-request-filter-handoff/r2-review-logs`.
These are local ignored artifacts, not new release inputs.

## Findings already established: do not rediscover them blindly

### Normal behavior and repaired regressions

The final Windows-native run reported:

- Main C-ABI integration: 217 host-counter assertions.
- Simulated `_22` absence: 46 assertions.
- Non-GUI CTest: 8/8, with ABI, ABI-1.7 compatibility, common, matcher,
  completion, script-world, operation and stress tests.
- Refusal-disabled mutation: expected Exit 1 at a denied `/frame_b` request.
- A filtered and an unfiltered view share one environment/profile. The
  unfiltered sibling continues navigating and evaluating in all fault cases.
- An allowed third-origin frame itself runs and attempts fetch/XHR/image/
  POST requests at the denied destination. Allowed controls reach the host.
  Cross-origin POST controls include OPTIONS and decoded bodies.
- Deferred popup opener release now yields `INVALID_STATE` for a late ALLOW,
  with no fallback request. The old code retained the opener and returned OK
  after about one second. The common layer abandons pending decisions while
  their owner view is still alive; no extra opener retain is required.
- The popup context stores the original UTF-8 URI, avoiding an allocating
  conversion in its noexcept completion. Allocation-failure injection has
  not yet tested all setup paths.
- Hidden Windows views can navigate and evaluate. The claim that the harness
  needed `neoastra_window_show` was false; the actual earlier problem was a
  64-entry `fd_set` already occupied by 64 listeners. Do not activate windows.

### The actual blocker

Fresh response construction/assignment failures are covered by a prepared
empty 403 in four tested modes: `create=1`, `assign=1`, `create=all`, `assign=all`.
Those runs show zero destination requests/application bytes.

If neither response can be assigned, the candidate obtains an uncompleted
deferral and leaves the view alive. Before disposal, the destination stays
silent. **Disposal releases GETs for `/b_s.js` and `/b_img.png` to the target.**

- Final cases: `create=all,prepared=all` and
  `assign=all,assign_prepared=all`; see `logs/final/failure.log`.
- An instrumented trace records `/b_s.js` with 587 bytes during `Close()`,
  before the view state is deleted; see `logs/after6/failure.log`.
- Skipping handler removal, skipping Close, and deliberately detaching the
  COM deferrals instead of releasing them did not establish safe teardown.
  Do not assume COM-deferral release alone is the cause.
- Holding without disposal for five seconds stayed silent. This does not
  establish a safe lifetime or resource-cleanup strategy.
- Immediate `Stop()` before teardown still leaked. Stop/about:blank plus
  600 ms message pumping observed zero requests, but no reliable completion
  signal was demonstrated. Those sleeps are diagnostics, not a solution.
- The final candidate does **not** automatically destroy the view. It sends
  `WEB_PROCESS_TERMINATED | RECREATE_VIEW` once and describes it as frozen.
  Recreating/disposal still leaks. The `quarantined` flag only suppresses
  duplicate events, not all subsequent view operations.
- The test executable's 18 assertions include pre-disposal hold checks.
  After disposal it merely prints the known leak and exits 0. A real
  acceptance test must not silently turn this into a pass.

### Additional parent-review findings

1. Main-navigation refusal calls `remember_navigation(..., true)`. The resource
   handler then enters the old `take_refused_navigation`/204 path **before**
   the new refusal helper. Its response-construction/assignment failures are
   ignored. Fix at the shared denial path and add a document-specific
   regression; do not just repair the script/image caller.
2. Failure to prepare the cached 403 is currently only logged, while
   installation succeeds. Decide and test a sound installation/error contract.
3. `defer=all` exists as an injection point but was not run in the final suite.
   The fallback does not provide a proven abort; document-only Stop is not
   proof of subresource cancellation.
4. Failure to store a deferral currently uses `Detach()` and loses ownership.
   Do not turn unbounded leaks into the claimed safety mechanism.
5. Builds contain shared-pointer atomic-helper `STL4029` deprecation warnings.
   They passed, but were not warning-free. Avoid global warning suppression.

A valid investigation may test a signal-based abort or a different supported
mechanism. Do not assume `NavigationCompleted` for `about:blank` proves all
previous held requests are cancelled: establish semantics and test the
adversarial case. Do not invent an atomic barrier or kill the whole shared
environment, browser process or host to hide the failure.

## Existing Windows tools and how to use them

The prior agent installed tools in user-owned directories using official
downloads and hash verification. Recheck versions and paths locally:

```powershell
$tc = "$env:USERPROFILE\neoastra-toolchain"
$env:DOTNET_ROOT = "$env:USERPROFILE\.dotnet\neoastra-sdk-10.0.114"
$env:DOTNET_MULTILEVEL_LOOKUP = '0'
$env:PATH = "$tc\llvm\bin;$tc\cmake\bin;$tc\ninja;$tc\python-3.15.2\tools;$env:DOTNET_ROOT;$env:PATH"
& 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\Tools\Launch-VsDevShell.ps1' -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
```

Recorded setup: LLVM/clang-cl 23.1.6, CMake 4.5.1, Ninja 1.13.2,
Python 3.15.2, .NET SDK 10.0.114, VS18/MSVC 14.51.36231, Windows SDK 10.0.26100,
WebView2 SDK 1.0.4078.44 under
`$env:USERPROFILE\neoastra-toolchain\webview2-sdk-1.0.4078.44`.
The preflight observed WebView2 runtime 158.0.4370.33; record the actual runtime
again in new runs instead of assuming an evergreen installation stayed fixed.

CMake requires clang-cl, not plain MSVC. Keep `global.json` and the pinned
WebView2 SDK unchanged unless a demonstrated task requirement warrants a
reviewed change. Build scripts and presets already exist; inspect them before
creating another wrapper. The archive's `tools/build-native.ps1` documents the
previous successful native configuration.

`ICoreWebView2_22` is documented as introduced in SDK 1.0.2365.46; its release
notes require runtime 122.0.2365.46 or newer. QI on the created view remains
authoritative. The old guessed 132 floor was removed.
Primary references:
`https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2_22`
and
`https://learn.microsoft.com/en-us/microsoft-edge/webview2/release-notes/sdk/1-0-2365-46`.

## Full-source tests now available

- `native/src/common/request_filter.{hpp,cpp}`: normalization, matcher,
  immutable snapshots and Apple compiler.
- `native/src/common/request_filter_operation.cpp`, `native_internal.hpp`,
  `neoastra.cpp`: FIFO, prepare/install, cancellation, owner lifetime.
- `native/include/neoastra.h`: ABI 1.9, filter exports, view options V2.
- `src/NeoAstra.Core/NeoRequestFilter.cs`, `NeoAstra.cs`, `NeoEnvironment.cs`,
  `NeoOptions.cs`, `NeoTypes.cs`, `NeoEvents.cs`: managed integration.
- `Interop/NativeRequestFilter.cs`, `NativeOperation.cs`, `NativeHandles.cs`,
  `Generated/Interop/NativeMethods.Generated.cs`: bindings and lifetime.
  Generate bindings with the existing generator; do not hand-edit generated
  declarations or the native-symbol props file.
- `src/NeoAstra.Tests/RequestFilterTests.cs`, `NativeOperationTests.cs`,
  `AbiLayoutTests.cs`: managed/common contracts.
- `src/NeoAstra.Tests/Fixtures/RequestFilterTlsServer.cs`,
  `RequestFilterTlsServerTests.cs`: local TLS server and counters.
- `src/NeoAstra.Conformance/Program.RequestFilter.cs`: real managed scenarios.
  Its HTTPS path currently skips Windows until platform-local certificate
  setup is implemented. Do not count that skip as TLS coverage.

Usual managed commands from `src` are `dotnet build -c Release` and
`dotnet test -c Release`. From repository root, focused browser conformance is:

```text
dotnet run --project src/NeoAstra.Conformance -c Release -- --run --request-filter-only
```

Ensure managed tests load the DLL you just built; the handoff's old packaged
Windows DLL does not implement the candidate. Inspect the native resolver and
build-copy rules rather than inventing an environment variable. Keep test
runtime placement isolated and record the loaded binary's hash/ABI.

`eng/build_native.py --rid win-x64` is the official build/test/stage path,
**not merely a build command**. Do not use it to stage a candidate with the
known blocker. The existing `windows_callback_tests` activates its window:
do not run it unmodified on the user's interactive desktop. Focused conformance
avoids unrelated media tests; a previous broad macOS run ended with Exit137
after a microphone test and is not a successful full conformance run.

## What the parent has already completed

- Common matcher, native ABI/operation lifecycle and managed API.
- Apple shared implementation; macOS and iOS simulator integration,
  limited basis only. Physical iOS and an iOS managed host are not verified.
- Android real C-ABI/JNI integration on API36/WebView133 and API37/WebView145,
  HTTP and HTTPS with test-local trust. Redirect destinations bypassed the
  interception hook; no redirect/worker/WebSocket claims.
- Parent staged macOS, iOS device/simulator and Android arm64/x64 artifacts.
  Android x64 was cross-built, not runtime-tested.
- Fresh parent pre-handoff regression: Release build with no warnings/errors,
  670 tests passed and 82 skipped. Focused managed HTTPS/frame tests passed
  previously; those browser checks were not repeated for this documentation
  and handoff-only step.
- Linux/GTK and isolated CodeAlta real-browser-to-test-MCP acceptance remain
  open and belong to the parent, not this Windows assignment.

## Expected independent work and final delivery

1. Verify exact handoff commit, toolchain and archive integrity; keep the
   original candidate/evidence immutable. Work on your own session branch.
2. Reproduce the original red cases and the main-document fallback gap before
   fixing. Separate fixture failures from backend failures.
3. Establish a disposal-safe, per-view refusal/error path without fixed delays,
   indefinite leaked objects, global destruction or weakened capability claims.
4. Validate native API, main/frame documents and subresources, positive
   controls, decoded POST bytes, updates/cancel/dispose, popup races and
   independent sibling views. Include response and deferral failure paths.
5. Run managed integration and add Windows HTTPS with test-local certificate
   decisions, not machine-wide trust or disabled certificate verification.
6. Run relevant regressions, repeat the refusal-disabled mutation and review
   your diff. Report loaded runtime/DLL identities and all skips/warnings.
7. Return one consolidated result: changed files, common/managed deltas,
   evidence, residual risks and an acceptance-ready patch or a precisely
   evidenced blocker. No repeated small source handoffs.

If an engine limitation makes the contract impossible, stop with a minimal
reproduction and precise evidence. Only the user/parent may decide to change
the contract; a green diagnostic exit cannot make that decision for them.
