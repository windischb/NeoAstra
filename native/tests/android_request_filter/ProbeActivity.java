package io.neoastra.filtertest;

import android.app.Activity;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import com.neoastra.backend.NeoAstraWebHost;
import org.json.JSONObject;
import org.json.JSONTokener;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Collections;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.IntConsumer;

/** The actual C API/JNI/NeoAstraWebHost, not a copy of its WebViewClient. */
public final class ProbeActivity extends Activity {
    static { NeoAstraWebHost.load(); System.loadLibrary("neoastra_android_request_filter_probe"); }
    private static native void init(ProbeActivity activity);
    private static native void create(int token, FrameLayout parent, int port);
    private static native void set(int token, long view, int port, long flags, boolean cancel);
    private static native int navigate(long view, String uri);
    private static native void evaluate(int token, long view, String script);
    private static native WebView webView(long view);
    private static native void destroy(long view);
    private static native int resourceCalls();
    private static native void setPopupMode(int mode);
    private static native int answerPopup();
    private static native void shutdown();

    private static boolean started;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final Map<Integer, CompletableFuture<Result>> operations = new HashMap<>();
    private final Map<Long, CompletableFuture<Void>> loading = new HashMap<>();
    private int token = 1;
    private long a, b;
    private String source, cross, target;
    private FrameLayout parentA, parentB;
    private boolean finished;
    private CompletableFuture<Void> popup;
    private long popupView;
    private String popupUri;

    private static final class Result {
        final int code; final long handle; final String json;
        Result(int code, long handle, String json) { this.code = code; this.handle = handle; this.json = json; }
        Result expect(int expected) {
            check(code == expected, "native result " + code + " != " + expected + ": " + json);
            return this;
        }
    }
    private static void check(boolean condition, String message) { if (!condition) throw new AssertionError(message); }
    private static String quote(String text) { return JSONObject.quote(text); }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        if (started) { Log.e("NAFILTER", "FAIL activity recreated"); return; }
        started = true;
        source = getIntent().getStringExtra("source");
        cross = getIntent().getStringExtra("cross");
        target = getIntent().getStringExtra("target");
        Log.i("NAFILTER", "ORIGINS source=" + source + " cross=" + cross + " target=" + target);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        parentA = new FrameLayout(this); parentB = new FrameLayout(this);
        layout.addView(parentA, new LinearLayout.LayoutParams(-1, 0, 1));
        layout.addView(parentB, new LinearLayout.LayoutParams(-1, 0, 1));
        setContentView(layout);
        CompletableFuture<Result> ready = new CompletableFuture<>();
        operations.put(0, ready);
        main.postDelayed(() -> finishProbe(new AssertionError("probe deadline")), 90000);
        ready.thenCompose(r -> {
            r.expect(0);
            check(r.handle == 67, "only base capability flags");
            Log.i("NAFILTER", "ENGINE " + WebView.getCurrentWebViewPackage());
            return call(id -> create(id, parentA, Uri.parse(target).getPort()));
        }).thenCompose(r -> {
            a = r.expect(0).handle;
            return call(id -> create(id, parentB, 0));
        }).thenCompose(r -> {
            b = r.expect(0).handle;
            check(!NeoAstraWebHost.nativeRefusesRequest(a, source + "/page", true), "filtered view permits source");
            check(!NeoAstraWebHost.nativeRefusesRequest(b, target + "/page", true), "unfiltered view permits target");
            return load(b, source + "/page?v=B&case=ready", false);
        }).thenCompose(v -> eval(b, "document.body.textContent"))
          .thenCompose(r -> {
            check(r.expect(0).json.contains("neoastra-probe-ready"), "unfiltered source fixture unavailable: " + r.json);
            check(navigate(a, target + "/page?v=A&case=hostInitial") == -8, "host navigation is refused");
            // Direct native WebView navigation also exercises the real request hook.
            return load(a, target + "/page?v=A&case=initial", true);
        }).thenCompose(v -> load(b, target + "/page?v=B&case=initial", true))
          .thenCompose(v -> load(a, source + "/page?v=A", false))
          .thenCompose(v -> load(b, source + "/page?v=B", false))
          .thenCompose(v -> eval(b, "JSON.stringify({url:location.href,body:document.body.innerText})"))
          .thenCompose(r -> {
              Log.i("NAFILTER", "CONTROL_PAGE " + r.expect(0).json);
              check(r.json.contains("neoastra-probe-ready"), "control source page must actually be loaded");
              return CompletableFuture.completedFuture(null);
          })
          .thenCompose(v -> network(a, "A"))
          .thenCompose(v -> network(b, "B"))
          .thenCompose(v -> popupCases())
          .thenCompose(v -> mainNavigationCases())
          .thenCompose(v -> call(id -> set(id, a, Uri.parse(source).getPort(), 0, true)))
          .thenCompose(r -> { r.expect(-8); return fetch(a, source, "A", "cancelAllowed", true); })
          .thenCompose(v -> fetch(a, target, "A", "cancelStillBlocked", false))
          .thenCompose(v -> fetch(b, target, "B", "cancelStillBlocked", true))
          .thenCompose(v -> call(id -> set(id, a, Uri.parse(target).getPort(), 4, false)))
          .thenCompose(r -> { r.expect(-4); return fetch(a, target, "A", "unsupportedStillBlocked", false); })
          .thenCompose(v -> fetch(b, target, "B", "unsupportedStillBlocked", true))
          .thenCompose(v -> call(id -> set(id, a, Uri.parse(cross).getPort(), 0, false)))
          .thenCompose(r -> { r.expect(0); return fetch(a, target, "A", "replacementOldAllowed", true); })
          .thenCompose(v -> fetch(a, cross, "A", "replacementBlocked", false))
          .thenCompose(v -> fetch(b, cross, "B", "replacementBlocked", true))
          .thenCompose(v -> call(id -> set(id, a, 0, 0, false)))
          .thenCompose(r -> { r.expect(0); return fetch(a, cross, "A", "clearAllowed", true); })
          .thenCompose(v -> call(id -> set(id, a, 443, 0, false)))
          .thenCompose(r -> { r.expect(0); return load(a, "probe://local/projected", false); })
          .thenCompose(v -> eval(a, "document.body.textContent"))
          .thenCompose(r -> {
              check(r.expect(0).json.contains("projected-ok") && resourceCalls() > 0, "custom scheme not blocked as projected HTTPS");
              return call(id -> set(id, a, Uri.parse(target).getPort(), 0, false));
          }).thenCompose(r -> { r.expect(0); return disposalRace(); })
          .whenComplete((v, error) -> finishProbe(error));
        init(this);
    }

    private CompletableFuture<Result> call(IntConsumer start) {
        int id = token++;
        CompletableFuture<Result> result = new CompletableFuture<>();
        operations.put(id, result);
        start.accept(id);
        return result;
    }
    public void onNativeResult(int id, int code, long handle, String json) {
        CompletableFuture<Result> result = operations.remove(id);
        if (result == null) { finishProbe(new AssertionError("duplicate native completion " + id)); return; }
        result.complete(new Result(code, handle, json));
    }
    public void onLoaded(long view, boolean success, String uri) {
        Log.i("NAFILTER", "LOADED " + view + " " + success + " " + uri);
        CompletableFuture<Void> result = loading.remove(view);
        if (result != null) {
            if (success) result.complete(null);
            else result.completeExceptionally(new AssertionError("navigation failed: " + uri));
        }
    }
    public void onPopup(long view, int code, String uri) {
        Log.i("NAFILTER", "POPUP " + view + " " + code + " " + uri);
        if (popup == null || view != popupView || !uri.equals(popupUri) || code != 0) {
            finishProbe(new AssertionError("unexpected popup callback: " + uri + " code=" + code));
            return;
        }
        CompletableFuture<Void> pending = popup;
        popup = null;
        pending.complete(null);
    }

    private CompletableFuture<Void> openPopup(long view, String uri, int mode) {
        popup = new CompletableFuture<>();
        CompletableFuture<Void> pending = popup;
        popupView = view; popupUri = uri;
        setPopupMode(mode);
        // Test-local setting: exercise the real engine popup path without taking
        // the user's input device. Production popup/user-gesture settings stay unchanged.
        webView(view).getSettings().setJavaScriptCanOpenWindowsAutomatically(true);
        webView(view).evaluateJavascript("window.open(" + quote(uri) + ");true", null);
        main.postDelayed(() -> {
            if (!pending.isDone()) pending.completeExceptionally(new AssertionError("popup callback deadline"));
        }, 10000);
        return pending;
    }

    private CompletableFuture<Void> observeNoPopupNavigation(long view, String name) {
        CompletableFuture<Void> observed = new CompletableFuture<>();
        // Only a bounded test observation, not an engine propagation barrier.
        main.postDelayed(() -> eval(view, "location.href").whenComplete((r, error) -> {
            if (error != null) observed.completeExceptionally(error);
            else {
                try {
                    check(r.expect(0).json.contains(source), name + " unexpectedly navigated: " + r.json);
                    Log.i("NAFILTER", "CHECK " + name + " PASS");
                    observed.complete(null);
                } catch (Throwable failure) { observed.completeExceptionally(failure); }
            }
        }), 1000);
        return observed;
    }

    private CompletableFuture<Void> popupCases() {
        CompletableFuture<Void> loaded = new CompletableFuture<>();
        loading.put(b, loaded);
        return openPopup(b, target + "/page?v=B&case=popup", 1)
            .thenCompose(v -> loaded)
            .thenCompose(v -> load(b, source + "/page?v=B", false))
            .thenCompose(v -> openPopup(a, target + "/page?v=A&case=popup", 1))
            .thenCompose(v -> observeNoPopupNavigation(a, "filtered-popup"))
            .thenCompose(v -> call(id -> set(id, a, 0, 0, false)))
            .thenCompose(r -> { r.expect(0); return openPopup(a, target + "/page?v=A&case=deferredPopup", 2); })
            .thenCompose(v -> call(id -> set(id, a, Uri.parse(target).getPort(), 0, false)))
            .thenCompose(r -> {
                r.expect(0); check(answerPopup() == 0, "answer deferred popup after filter install");
                return observeNoPopupNavigation(a, "deferred-popup");
            }).thenCompose(v -> {
                CompletableFuture<Void> controlLoaded = new CompletableFuture<>();
                loading.put(b, controlLoaded);
                return openPopup(b, target + "/page?v=B&case=deferredPopup", 2)
                    .thenCompose(ignored -> {
                        check(answerPopup() == 0, "deferred popup control allowed");
                        return controlLoaded;
                    });
            }).thenCompose(v -> load(b, source + "/page?v=B", false));
    }

    private CompletableFuture<Void> boundedObservation(String name) {
        CompletableFuture<Void> observed = new CompletableFuture<>();
        main.postDelayed(() -> {
            Log.i("NAFILTER", "OBSERVED " + name);
            observed.complete(null);
        }, 1000);
        return observed;
    }

    private CompletableFuture<Void> mainForm(long view, String label, String method) {
        String name = "mainForm" + method;
        String script = "const f=document.createElement('form');f.method=" + quote(method)
            + ";f.action=" + quote(target + "/page?v=" + label + "&case=" + name)
            + ";for(const [key,value] of [['v'," + quote(label) + "],['case'," + quote(name)
            + "]]){const i=document.createElement('input');i.name=key;i.value=value;f.append(i);}document.body.append(f);f.submit();";
        // Cross-origin main-frame POST is already refused by the backend's
        // navigation policy. Use a loaded same-origin document, then install the
        // filter, so its positive control genuinely exercises the network path.
        boolean post = method.equals("Post");
        CompletableFuture<Result> ready = post && label.equals("A")
            ? call(id -> set(id, view, 0, 0, false))
            : CompletableFuture.completedFuture(new Result(0, 0, ""));
        return ready.thenCompose(r -> {
            r.expect(0);
            return load(view, (post ? target : source) + "/page?v=" + label + "&case=formSetup", false);
        }).thenCompose(v -> post && label.equals("A")
            ? call(id -> set(id, view, Uri.parse(target).getPort(), 0, false))
            : CompletableFuture.completedFuture(new Result(0, 0, ""))).thenCompose(r -> {
            r.expect(0);
            CompletableFuture<Void> loaded = new CompletableFuture<>();
            if (label.equals("B")) loading.put(view, loaded);
            Log.i("NAFILTER", "ATTEMPT " + label + " " + name);
            webView(view).evaluateJavascript(script, null);
            // A refused GET may leave the current document intact; a POST can
            // finish with a local error response. Server bytes decide, not load/error.
            return label.equals("B") ? loaded : boundedObservation(name);
        });
    }

    private CompletableFuture<Void> reloadCase(long view, String label) {
        return load(view, target + "/page?v=" + label + "&case=reloadSetup", false)
            .thenCompose(v -> eval(view, "history.replaceState(null,''," + quote(target + "/page?v=" + label + "&case=reload") + ");true"))
            .thenCompose(r -> {
                r.expect(0);
                return label.equals("A") ? call(id -> set(id, view, Uri.parse(target).getPort(), 0, false))
                    : CompletableFuture.completedFuture(new Result(0, 0, ""));
            }).thenCompose(r -> {
                r.expect(0);
                CompletableFuture<Void> loaded = new CompletableFuture<>();
                if (label.equals("B")) loading.put(view, loaded);
                webView(view).reload();
                return label.equals("B") ? loaded : boundedObservation("reload");
            });
    }

    private CompletableFuture<Void> mainNavigationCases() {
        return mainForm(a, "A", "Get")
            .thenCompose(v -> mainForm(b, "B", "Get"))
            .thenCompose(v -> mainForm(a, "A", "Post"))
            .thenCompose(v -> mainForm(b, "B", "Post"))
            .thenCompose(v -> call(id -> set(id, a, 0, 0, false)))
            .thenCompose(r -> { r.expect(0); return reloadCase(a, "A"); })
            .thenCompose(v -> reloadCase(b, "B"))
            .thenCompose(v -> load(a, source + "/page?v=A", false))
            .thenCompose(v -> load(b, source + "/page?v=B", false));
    }
    private CompletableFuture<Void> load(long view, String uri, boolean direct) {
        CompletableFuture<Void> result = new CompletableFuture<>();
        loading.put(view, result);
        if (direct) webView(view).loadUrl(uri);
        else {
            int code = navigate(view, uri);
            if (code != 0) { loading.remove(view); result.completeExceptionally(new AssertionError("navigate " + code)); }
        }
        return result;
    }
    private CompletableFuture<Result> eval(long view, String script) { return call(id -> evaluate(id, view, script)); }

    private CompletableFuture<JSONObject> script(long view, String body) {
        return eval(view, "window.__probe=undefined;(async()=>{" + body
            + "})().then(r=>window.__probe=r,e=>window.__probe={failure:String(e)});true")
            .thenCompose(r -> {
                r.expect(0);
                check("true".equals(r.json), "test script did not start: " + r.json);
                return waitScript(view, android.os.SystemClock.elapsedRealtime() + 15000);
            });
    }
    private CompletableFuture<JSONObject> waitScript(long view, long deadline) {
        return eval(view, "window.__probe===undefined?null:JSON.stringify(window.__probe)").thenCompose(r -> {
            try {
                Object value = new JSONTokener(r.expect(0).json).nextValue();
                if (value != JSONObject.NULL) {
                    JSONObject result = new JSONObject((String)value);
                    check(!result.has("failure"), result.toString());
                    return CompletableFuture.completedFuture(result);
                }
                check(android.os.SystemClock.elapsedRealtime() < deadline, "JavaScript test deadline");
                CompletableFuture<JSONObject> pending = new CompletableFuture<>();
                main.postDelayed(() -> waitScript(view, deadline).whenComplete((v, e) -> {
                    if (e != null) pending.completeExceptionally(e); else pending.complete(v);
                }), 25);
                return pending;
            } catch (Exception error) {
                CompletableFuture<JSONObject> failed = new CompletableFuture<>();
                failed.completeExceptionally(error);
                return failed;
            }
        });
    }
    private CompletableFuture<Void> fetch(long view, String origin, String label, String name, boolean allowed) {
        String uri = origin + "/item?v=" + label + "&case=" + name;
        return script(view, "try{const r=await fetch(" + quote(uri) + ",{cache:'no-store'});return {status:r.status};}"
            + "catch(e){return {status:0};}").thenAccept(r -> {
                int status = r.optInt("status", -1);
                check(allowed ? status == 200 : status == 403 || status == 0, name + ": " + r);
                Log.i("NAFILTER", "CHECK " + name + " " + label + " " + r);
            });
    }
    private CompletableFuture<Void> network(long view, String label) {
        String body = "const T=" + quote(target) + ",S=" + quote(source) + ",X=" + quote(cross) + ",V=" + quote(label) + ";"
            + "const u=(b,p,c)=>b+p+'?v='+V+'&case='+c;"
            + "const f=async(url)=>{try{return (await fetch(url,{cache:'no-store'})).status;}catch(e){return 0;}};"
            + "const el=(tag,url)=>new Promise(resolve=>{const e=document.createElement(tag);e.onload=()=>resolve('load');e.onerror=()=>resolve('error');e.src=url;document.body.append(e);});"
            + "const fr=(base,name)=>new Promise(resolve=>{const listener=e=>{if(e.data&&e.data.probeFrame===name){removeEventListener('message',listener);resolve(e.data);}};"
            + "addEventListener('message',listener);const e=document.createElement('iframe');e.src=u(base,'/frame',name)+'&target='+encodeURIComponent(T);document.body.append(e);});"
            + "const xhr=()=>new Promise(resolve=>{const x=new XMLHttpRequest();x.onload=()=>resolve(x.status);x.onerror=()=>resolve(0);x.open('GET',u(T,'/item','xhr'));x.send();});"
            + "const style=()=>new Promise(resolve=>{const e=document.createElement('link');e.rel='stylesheet';e.onload=()=>resolve('load');e.onerror=()=>resolve('error');e.href=u(T,'/style','style');document.head.append(e);});"
            + "const events=()=>new Promise(resolve=>{const e=new EventSource(u(T,'/event','eventSource'));e.onmessage=()=>{e.close();resolve('message');};e.onerror=()=>{e.close();resolve('error');};});"
            + "const audio=()=>new Promise(resolve=>{const a=new Audio();a.onloadedmetadata=()=>resolve('metadata');a.onerror=()=>resolve('error');a.preload='metadata';a.src=u(T,'/audio','audio');a.load();});"
            + "const form=method=>new Promise(resolve=>{const name='form'+method;const frame=document.createElement('iframe');frame.name=name;document.body.append(frame);"
            + "const f=document.createElement('form');f.method=method;f.target=name;f.action=u(T,'/page',name);"
            + "for(const [key,value] of [['v',V],['case',name]]){const i=document.createElement('input');i.name=key;i.value=value;f.append(i);}document.body.append(f);"
            + "frame.onload=()=>resolve('load');frame.onerror=()=>resolve('error');f.submit();});"
            + "return {fetch:await f(u(T,'/item','fetch')),allowed:await f(u(S,'/item','allowedSource')),"
            + "image:await el('img',u(T,'/image','image')),script:await el('script',u(T,'/script','script')),"
            + "xhr:await xhr(),style:await style(),eventSource:await events(),audio:await audio(),"
            + "beacon:navigator.sendBeacon(u(T,'/item','beacon'),'probe'),"
            + "noCorsPost:await fetch(u(T,'/item','noCorsPost'),{method:'POST',mode:'no-cors',body:'probe'}).then(()=>true,()=>false),"
            + "formGet:await form('Get'),formPost:await form('Post'),"
            + "frameDocument:await el('iframe',u(T,'/page','frameDocument')),"
            + "frameSame:await fr(S,'frameSame'),frameCross:await fr(X,'frameCross')};";
        return script(view, body).thenAccept(r -> {
            check(r.optInt("allowed") == 200, "allowed source control");
            check(label.equals("B") ? r.optInt("fetch") == 200 : r.optInt("fetch") != 200, "per-view fetch " + r);
            if (label.equals("B")) {
                check(r.optInt("xhr") == 200 && "load".equals(r.optString("style"))
                    && "message".equals(r.optString("eventSource")) && "metadata".equals(r.optString("audio"))
                    && r.optBoolean("beacon") && r.optBoolean("noCorsPost"), "additional positive controls " + r);
            }
            Log.i("NAFILTER", "NETWORK " + label + " " + r);
        });
    }

    private CompletableFuture<Void> disposalRace() {
        WebView oldWeb = webView(a);
        WebViewClient oldClient = oldWeb.getWebViewClient();
        WebResourceRequest request = new WebResourceRequest() {
            public Uri getUrl() { return Uri.parse(target + "/item?v=A&case=dispose"); }
            public boolean isForMainFrame() { return false; }
            public boolean isRedirect() { return false; }
            public boolean hasGesture() { return false; }
            public String getMethod() { return "GET"; }
            public Map<String, String> getRequestHeaders() { return Collections.emptyMap(); }
        };
        AtomicReference<Throwable> failure = new AtomicReference<>();
        CountDownLatch entered = new CountDownLatch(1);
        CompletableFuture<Void> finishedRequests = new CompletableFuture<>();
        Thread worker = new Thread(() -> {
            try {
                for (int i = 0; i < 10000; i++) {
                    WebResourceResponse response = oldClient.shouldInterceptRequest(oldWeb, request);
                    check(response != null && response.getStatusCode() == 403, "race request must stay refused");
                    if (i == 0) entered.countDown();
                }
            } catch (Throwable error) { failure.set(error); }
            entered.countDown();
            main.post(() -> {
                if (failure.get() == null) finishedRequests.complete(null);
                else finishedRequests.completeExceptionally(failure.get());
            });
        });
        worker.start();
        try { check(entered.await(5, TimeUnit.SECONDS) && failure.get() == null, "request worker entered the real JNI path"); }
        catch (InterruptedException error) { Thread.currentThread().interrupt(); throw new AssertionError(error); }
        CompletableFuture<Result> pending = call(id -> set(id, a, Uri.parse(cross).getPort(), 0, false));
        destroy(a); a = 0;
        return pending.thenCompose(r -> { r.expect(-13); return finishedRequests; })
            .thenCompose(v -> call(id -> create(id, parentA, 0)))
            .thenAccept(r -> {
                a = r.expect(0).handle;
                check(oldClient.shouldInterceptRequest(oldWeb, request).getStatusCode() == 403,
                    "stale Java client never targets a new unfiltered native view");
                Log.i("NAFILTER", "CHECK disposal-race PASS");
            });
    }
    private void finishProbe(Throwable error) {
        if (finished) return;
        finished = true;
        main.removeCallbacksAndMessages(null);
        try {
            shutdown();
            String result = error == null ? "PASS" : "FAIL";
            if (error != null) Log.e("NAFILTER", "FAIL", error);
            Files.write(new java.io.File(getFilesDir(), "result.txt").toPath(), result.getBytes(StandardCharsets.UTF_8));
            Log.i("NAFILTER", result);
        } catch (Throwable cleanup) { Log.e("NAFILTER", "FAIL cleanup", cleanup); }
        finish();
    }
}
