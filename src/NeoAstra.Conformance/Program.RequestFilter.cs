using System.Text.Json;
using NeoAstra;
using NeoAstra.Tests;

internal static partial class Program
{
    private sealed partial class ConformanceSuite
    {
        // Lifecycle cases use unused ports. The separate HTTPS fixture below
        // measures actual request bytes as well as the managed UI-thread contract.
        private async ValueTask RunRequestFilterScenariosAsync(NeoEnvironment environment)
        {
            await using var window = CreateHiddenWindow("NeoAstra request-filter conformance");
            var capability = environment.GetCapability(NeoCapability.RequestFilter);
            var initial = new NeoRequestFilter
            {
                Rules = [new NeoRequestRule { HostMatch = NeoHostMatch.Exact, Host = "LOCALHOST.", Port = 43127 }],
            };
            if (capability.SupportLevel == NeoSupportLevel.None)
            {
                await RunCaseAsync("an unsupported request filter fails closed; clearing stays compatible", async () =>
                {
                    await using var view = await environment.CreateWebViewAsync(NeoAstraHost.FillWindow(window));
                    await view.SetRequestFilterAsync(NeoRequestFilter.Empty);
                    var rejected = false;
                    try { await view.SetRequestFilterAsync(initial); }
                    catch (NotSupportedException) { rejected = true; }
                    Require(rejected && view.RequestFilter.Rules.Count == 0, "Unsupported rules were accepted or changed the snapshot.");
                    rejected = false;
                    try
                    {
                        await using var unexpected = await environment.CreateWebViewAsync(
                            NeoAstraHost.FillWindow(window), new NeoAstraOptions { RequestFilter = initial });
                    }
                    catch (NotSupportedException) { rejected = true; }
                    Require(rejected, "Creation silently ignored an unsupported initial filter.");
                });
                return;
            }

            await RunCaseAsync("request-filter snapshots, UI dispatch, FIFO and confirmed cancellation", async () =>
            {
                var createOptions = new NeoAstraOptions { RequestFilter = initial };
                var creating = environment.CreateWebViewAsync(NeoAstraHost.FillWindow(window), createOptions);
                createOptions.RequestFilter = NeoRequestFilter.Empty;
                await using var view = await creating;
                Require(view.RequestFilter.Rules is [{ Host: "localhost", Port: 43127 }], "Creation did not retain its normalized snapshot.");
                await using var sibling = await environment.CreateWebViewAsync(NeoAstraHost.FillWindow(window));
                Require(sibling.RequestFilter.Rules.Count == 0, "A sibling inherited an unrelated filter.");

                var first = new NeoRequestFilter { Rules = [new NeoRequestRule { Port = 43128 }] };
                var second = new NeoRequestFilter { Rules = [new NeoRequestRule { Port = 43129 }] };
                var a = view.SetRequestFilterAsync(first);
                var b = view.SetRequestFilterAsync(second);
                await Task.WhenAll(a, b);
                Require(view.RequestFilter.Rules is [{ Port: 43129 }], "The later setter was overwritten by an older update.");
                await Task.Run(() => view.SetRequestFilterAsync(initial));
                Require(view.RequestFilter.Rules is [{ Host: "localhost", Port: 43127 }], "A setter from another thread failed.");

                using var cancellation = new CancellationTokenSource();
                var changing = view.SetRequestFilterAsync(second, cancellation.Token);
                await application.Dispatcher.InvokeAsync(cancellation.Cancel);
                try
                {
                    await changing;
                    Require(view.RequestFilter.Rules is [{ Port: 43129 }], "A successful installation did not publish its snapshot.");
                }
                catch (OperationCanceledException)
                {
                    Require(view.RequestFilter.Rules is [{ Port: 43127 }], "A canceled installation changed the snapshot.");
                }

                var missing = new[] { NeoRequestFilterFlags.WebSockets, NeoRequestFilterFlags.ResourceKinds, NeoRequestFilterFlags.DedicatedWorkers }
                    .FirstOrDefault(flag => ((NeoRequestFilterFlags)capability.Flags & flag) == 0);
                if (missing != 0)
                {
                    var before = view.RequestFilter;
                    var rejected = false;
                    try { await view.SetRequestFilterAsync(new NeoRequestFilter { Rules = initial.Rules, RequiredFlags = missing }); }
                    catch (NotSupportedException) { rejected = true; }
                    Require(rejected && ReferenceEquals(before, view.RequestFilter), "A failed requirement changed the installed snapshot.");
                }

                await view.SetRequestFilterAsync(NeoRequestFilter.Empty);
                Require(view.RequestFilter.Rules.Count == 0, "Clearing did not publish the empty snapshot.");
                var pending = view.SetRequestFilterAsync(first);
                await view.DisposeAsync(); // Before the posted setter starts.
                var disposed = false;
                try { await pending; }
                catch (ObjectDisposedException) { disposed = true; }
                Require(disposed, "A queued setter ran after disposal.");
            });

            if (OperatingSystem.IsLinux())
            {
                await RunCaseAsync("GTK rejects a tracked popup that cannot have an independent filter", async () =>
                {
                    await using var opener = await environment.CreateWebViewAsync(
                        NeoAstraHost.FillWindow(window), new NeoAstraOptions { RequestFilter = initial });
                    await NavigateAndWaitAsync(opener, IndexUri, options.Timeout);
                    EnableGtkProbePopups(opener);
                    var before = opener.RequestFilter;
                    var refused = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
                    opener.NewWindowRequested = async request =>
                    {
                        try
                        {
                            await using var unexpected = await request.CreateViewAsync(NeoAstraHost.FillWindow(window));
                            refused.TrySetResult(false);
                        }
                        catch (NotSupportedException) { refused.TrySetResult(true); }
                        catch (Exception error) { refused.TrySetException(error); }
                        return NeoNewWindowDecision.Cancel;
                    };
                    _ = await opener.EvaluateScriptAsync("window.open('about:blank'); true");
                    Require(await refused.Task.WaitAsync(options.Timeout), "GTK published an unsafe filtered popup.");
                    Require(ReferenceEquals(before, opener.RequestFilter), "Popup rejection changed the opener's snapshot.");
                    await opener.SetRequestFilterAsync(NeoRequestFilter.Empty);
                });
            }
            else if (IsSupported(environment, NeoCapability.TrackedPopups))
            {
                await RunCaseAsync("request-filter inheritance in a tracked managed popup", async () =>
                {
                    await using var opener = await environment.CreateWebViewAsync(
                        NeoAstraHost.FillWindow(window), new NeoAstraOptions { RequestFilter = initial });
                    await NavigateAndWaitAsync(opener, IndexUri, options.Timeout);
                    var opened = new TaskCompletionSource<NeoAstra.NeoAstra>(TaskCreationOptions.RunContinuationsAsynchronously);
                    opener.NewWindowRequested = async request =>
                    {
                        try
                        {
                            var popup = await request.CreateViewAsync(NeoAstraHost.FillWindow(window));
                            opened.TrySetResult(popup);
                            return NeoNewWindowDecision.UseView(popup);
                        }
                        catch (Exception error)
                        {
                            opened.TrySetException(error);
                            return NeoNewWindowDecision.Cancel;
                        }
                    };
                    _ = await opener.EvaluateScriptAsync("window.open('about:blank'); true");
                    await using var popup = await opened.Task.WaitAsync(options.Timeout);
                    Require(ReferenceEquals(opener.RequestFilter, popup.RequestFilter), "Popup lost the opener's immutable snapshot.");
                    await popup.SetRequestFilterAsync(NeoRequestFilter.Empty);
                    Require(opener.RequestFilter.Rules.Count == 1 && popup.RequestFilter.Rules.Count == 0,
                        "Replacing a popup filter changed its opener.");
                });
            }

            if (OperatingSystem.IsLinux() || IsSupported(environment, NeoCapability.TlsErrorDecisions))
                await RunRequestFilterTlsScenarioAsync(environment);
            else
                Skip("managed request-filter HTTPS traffic", "This backend needs its platform-specific test-local certificate setup.");
        }

        private async ValueTask RunRequestFilterTlsScenarioAsync(NeoEnvironment environment)
        {
            await RunCaseAsync("HTTPS request bytes, per-view filtering and removal", async () =>
            {
                await using var target = new RequestFilterTlsServer();
                var frameTarget = JsonSerializer.Serialize(new Uri(target.Origin, "frame-fetch").AbsoluteUri);
                var frame = "<!doctype html><script>fetch(" + frameTarget
                    + ",{cache:'no-store'}).then(r=>parent.postMessage({tlsFrame:r.status},'*'),"
                    + "e=>parent.postMessage({tlsFrame:0},'*'));</script>";
                await using var source = new RequestFilterTlsServer(frame, target.Certificate);
                await using var cross = new RequestFilterTlsServer(frame, target.Certificate);
                await using var window = CreateHiddenWindow("NeoAstra isolated HTTPS filter probe");
                var filter = new NeoRequestFilter { Rules = [new NeoRequestRule { Port = target.Origin.Port }] };
                await using var filtered = await environment.CreateWebViewAsync(
                    NeoAstraHost.FillWindow(window), new NeoAstraOptions { RequestFilter = filter });
                await using var control = await environment.CreateWebViewAsync(NeoAstraHost.FillWindow(window));
                if (OperatingSystem.IsLinux())
                {
                    // GTK has no managed TLS decision here. Pin only the fixture
                    // certificate/host in this disposable private network session.
                    AllowGtkProbeCertificate(filtered, target);
                    AllowGtkProbeCertificate(control, target);
                }
                else
                {
                    // Only these disposable views load our self-signed loopback pages.
                    // Some backends omit the challenge URI: this is not a policy for
                    // arbitrary/untrusted content. No global trust setting changes.
                    filtered.TlsErrorRequested = _ => ValueTask.FromResult(NeoTlsErrorDecision.Allow);
                    control.TlsErrorRequested = _ => ValueTask.FromResult(NeoTlsErrorDecision.Allow);
                }
                await NavigateAndWaitAsync(filtered, source.Origin, options.Timeout);
                await NavigateAndWaitAsync(control, source.Origin, options.Timeout);
                Require((await control.EvaluateScriptAsync("document.body.textContent"))?.Contains("neoastra-tls-ready", StringComparison.Ordinal) == true,
                    "The HTTPS source control did not actually load.");

                async ValueTask FetchAsync(NeoAstra.NeoAstra view, bool allowed, string label)
                {
                    var uri = JsonSerializer.Serialize(new Uri(target.Origin, label).AbsoluteUri);
                    _ = await view.EvaluateScriptAsync(
                        $"window.__tlsFilter=undefined;fetch({uri},{{cache:'no-store'}}).then(r=>window.__tlsFilter=r.status,e=>window.__tlsFilter=0);true");
                    await WaitUntilScriptAsync(view, "window.__tlsFilter!==undefined", "HTTPS fetch did not settle.", options.Timeout);
                    using var result = await EvaluateJsonAsync(view, "window.__tlsFilter");
                    Require(allowed ? result.RootElement.GetInt32() == 200 : result.RootElement.GetInt32() is 0 or 403,
                        $"Unexpected HTTPS fetch result for {label}: {result.RootElement}.");
                }

                async ValueTask FrameAsync(NeoAstra.NeoAstra view, Uri origin, bool allowed)
                {
                    var uri = JsonSerializer.Serialize(new Uri(origin, "frame").AbsoluteUri);
                    _ = await view.EvaluateScriptAsync(
                        "window.__tlsFrame=undefined;(()=>{const f=document.createElement('iframe');"
                        + "const receive=e=>{if(e.source===f.contentWindow&&e.data&&typeof e.data.tlsFrame==='number'){"
                        + "window.__tlsFrame=e.data.tlsFrame;removeEventListener('message',receive);f.remove();}};"
                        + $"addEventListener('message',receive);f.src={uri};document.body.append(f);}})();true");
                    await WaitUntilScriptAsync(view, "window.__tlsFrame!==undefined", "HTTPS frame did not report its fetch.", options.Timeout);
                    using var result = await EvaluateJsonAsync(view, "window.__tlsFrame");
                    Require(allowed ? result.RootElement.GetInt32() == 200 : result.RootElement.GetInt32() is 0 or 403,
                        $"Unexpected HTTPS frame fetch result from {origin}: {result.RootElement}.");
                }

                await FetchAsync(control, true, "control");
                Require(target.Requests == 1 && target.Bytes > 0, "The positive HTTPS control sent no application bytes.");
                var before = target.Bytes;
                await FetchAsync(filtered, false, "blocked");
                // Bounded test observation only, not a production propagation barrier.
                await Task.Delay(1000);
                Require(target.Bytes == before, "The filtered HTTPS request reached the target.");
                foreach (var origin in new[] { source.Origin, cross.Origin })
                {
                    await FrameAsync(control, origin, true);
                    before = target.Bytes;
                    await FrameAsync(filtered, origin, false);
                    await Task.Delay(1000);
                    Require(target.Bytes == before, $"The HTTPS frame fetch from {origin} reached the target.");
                }
                await filtered.SetRequestFilterAsync(NeoRequestFilter.Empty);
                await FetchAsync(filtered, true, "cleared");
                Require(target.Requests == 4 && target.Bytes > before, "Clearing left HTTPS requests blocked.");
                Console.WriteLine($"HTTPS target: TCP={target.Connections}, TLS={target.Handshakes}, HTTP={target.Requests}, bytes={target.Bytes}");
            });
        }
    }
}
