// Real GTK/WebKitGTK probes. --engine tests the engine independently of the
// product capability; --api exercises the public ABI. Neither mode grants flags.
#include "neoastra.h"
#include "request_filter.hpp"
#include "request_filter_loopback_server.hpp"

#include <gtk/gtk.h>
#include <webkit/webkit.h>
#include <sys/stat.h>
#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {
// UI-thread-only, test-executable state. Zero leaves every compiler call intact.
unsigned corrupt_compile_countdown{};
unsigned corrupted_compiles{};
}

extern "C" __attribute__((visibility("default")))
void webkit_user_content_filter_store_save(WebKitUserContentFilterStore* store, const gchar* identifier,
    GBytes* source, GCancellable* cancellable, GAsyncReadyCallback callback, gpointer user_data) {
    static const auto real_save = reinterpret_cast<decltype(&webkit_user_content_filter_store_save)>(
        dlsym(RTLD_NEXT, "webkit_user_content_filter_store_save"));
    if (!real_save) { std::fputs("Missing real WebKit compiler\n", stderr); std::abort(); }
    if (corrupt_compile_countdown && --corrupt_compile_countdown == 0) {
        ++corrupted_compiles;
        auto* invalid = g_bytes_new_static("[", 1);
        real_save(store, identifier, invalid, cancellable, callback, user_data);
        g_bytes_unref(invalid);
    } else real_save(store, identifier, source, cancellable, callback, user_data);
}

namespace {
using namespace neo_request_filter_probe;

void check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "GTK filter failure: %s\n", message); std::abort(); }
}

template<typename Predicate>
void wait_for(Predicate predicate, const char* message) {
    const auto deadline = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;
    while (!predicate()) {
        check(g_get_monotonic_time() < deadline, message);
        if (!g_main_context_iteration(nullptr, FALSE)) g_usleep(1000);
    }
}

neoastra_string_view_t text(const std::string& value) {
    return {reinterpret_cast<const uint8_t*>(value.data()), value.size()};
}

struct completion {
    bool done{};
    neoastra_result_t result{NEOASTRA_ERROR_UNKNOWN};
    void* value{};
    unsigned calls{};
    int64_t native_error{};
    std::string error_domain;
};
void NEOASTRA_CALL environment_created(void* p, neoastra_result_t r, neoastra_environment_t* v, const neoastra_error_t*) {
    *static_cast<completion*>(p) = {true, r, v};
}
void NEOASTRA_CALL profile_created(void* p, neoastra_result_t r, neoastra_profile_t* v, const neoastra_error_t*) {
    *static_cast<completion*>(p) = {true, r, v};
}
void NEOASTRA_CALL view_created(void* p, neoastra_result_t r, neoastra_view_t* v, const neoastra_error_t*) {
    *static_cast<completion*>(p) = {true, r, v};
}
void NEOASTRA_CALL operation_completed(void* p, neoastra_result_t r, const neoastra_error_t* error) {
    auto& state = *static_cast<completion*>(p);
    state.done = true;
    state.result = r;
    ++state.calls;
    if (error) {
        check(neoastra_error_get_code(error) == r, "error result matches callback");
        state.native_error = neoastra_error_get_native_code(error);
        const auto domain = neoastra_error_get_domain(error);
        state.error_domain.assign(reinterpret_cast<const char*>(domain.data), domain.length);
    }
}

class probe_view final {
public:
    probe_view(neoastra_app_t* app, neoastra_environment_t* env, const neoastra_request_filter_t* initial = nullptr,
        neoastra_decision_t* popup = nullptr, neoastra_profile_t* profile = nullptr) {
        neoastra_window_options_t window_options{};
        window_options.size = sizeof(window_options);
        window_options.version = 1;
        window_options.bounds = {0, 0, 640, 480};
        check(neoastra_app_create_window(app, &window_options, &_window, nullptr) == NEOASTRA_OK, "create window");
        neoastra_view_options_t options{};
        options.size = sizeof(options);
        options.version = initial ? 2 : 1;
        options.request_filter = initial;
        options.popup_request = popup;
        options.profile = profile;
        options.window = _window;
        options.fill_parent = 1;
        options.bridge_policy = NEOASTRA_BRIDGE_DISABLED;
        completion created;
        check(neoastra_environment_create_view_async(env, &options, view_created, &created, nullptr, nullptr) == NEOASTRA_OK, "create view start");
        if (popup) check(created.done, "popup creation completes synchronously");
        wait_for([&] { return created.done; }, "create view completion");
        check(created.result == NEOASTRA_OK && created.value, "create view result");
        value = static_cast<neoastra_view_t*>(created.value);
        neoastra_native_handle_t handle{};
        handle.size = sizeof(handle);
        handle.version = 1;
        check(neoastra_view_get_native_handle(value, NEOASTRA_NATIVE_HANDLE_WEBKITGTK_WEBVIEW, &handle) == NEOASTRA_OK, "native webview");
        web = WEBKIT_WEB_VIEW(handle.value);
        neoastra_view_set_event_callback(value, [](void* p, const neoastra_event_t* event) {
            auto& self = *static_cast<probe_view*>(p);
            // GTK also emits load-finished after load-failed, possibly in the
            // same main-context iteration. Completion must not erase failure.
            if (event->header.type == NEOASTRA_EVENT_NAVIGATION_COMPLETED) { self._done = true; self._loaded = !self._failed; }
            if (event->header.type == NEOASTRA_EVENT_NAVIGATION_FAILED) { self._done = self._failed = true; self._loaded = false; }
            if (event->header.type == NEOASTRA_EVENT_NEW_WINDOW_REQUESTED && self.on_popup) self.on_popup(event->decision);
            check(event->header.type != NEOASTRA_EVENT_WEB_PROCESS_TERMINATED, "web process terminated");
        }, this);
    }
    ~probe_view() {
        neoastra_view_set_event_callback(value, nullptr, nullptr);
        neoastra_view_release(value);
        neoastra_window_release(_window);
    }
    probe_view(const probe_view&) = delete;
    probe_view& operator=(const probe_view&) = delete;
    neoastra_view_t* value{};
    WebKitWebView* web{};
    std::function<void(neoastra_decision_t*)> on_popup;
    bool navigation_done() const { return _done; }
    neoastra_window_t* window() const { return _window; }

    bool navigate(const std::string& uri) {
        _done = _loaded = _failed = false;
        check(neoastra_view_navigate(value, text(uri), nullptr) == NEOASTRA_OK, "navigation start");
        wait_for([&] { return _done; }, "navigation completion");
        return _loaded;
    }

    bool reload() {
        _done = _loaded = _failed = false;
        check(neoastra_view_reload(value, 1) == NEOASTRA_OK, "reload start");
        wait_for([&] { return _done; }, "reload completion");
        return _loaded;
    }

    std::string run(const std::string& body) {
        struct script_result { bool done{}; std::string text; } state;
        webkit_web_view_call_async_javascript_function(web, body.c_str(), -1, nullptr, nullptr, nullptr, nullptr,
            [](GObject* source, GAsyncResult* result, void* p) {
                auto& output = *static_cast<script_result*>(p);
                GError* error{};
                auto* value = webkit_web_view_call_async_javascript_function_finish(WEBKIT_WEB_VIEW(source), result, &error);
                if (error) std::fprintf(stderr, "JavaScript: %s\n", error->message);
                check(!error && value, "JavaScript completion");
                auto* string = jsc_value_to_string(value);
                output.text = string ? string : "";
                g_free(string);
                g_object_unref(value);
                output.done = true;
            }, &state);
        wait_for([&] { return state.done; }, "JavaScript timeout");
        return state.text;
    }

    neoastra_result_t set_filter(const neoastra_request_filter_t& filter, bool cancel = false) {
        completion state;
        neoastra_operation_t* operation{};
        const auto result = neoastra_view_set_request_filter_async(value, &filter, operation_completed, &state, &operation, nullptr);
        if (result != NEOASTRA_OK) return result;
        if (cancel) neoastra_operation_cancel(operation);
        wait_for([&] { return state.done; }, "set filter completion");
        check(state.calls == 1, "one filter completion");
        neoastra_operation_release(operation);
        return state.result;
    }
private:
    neoastra_window_t* _window{};
    bool _done{}, _loaded{}, _failed{};
};

void observe_silence(loopback_server& server, counts before, const char* name) {
    // A bounded observation, not a promise about future requests or propagation.
    const auto until = g_get_monotonic_time() + G_USEC_PER_SEC;
    wait_for([&] { return g_get_monotonic_time() >= until; }, "observation");
    const auto result = since(before, server.snapshot());
    std::printf("%s: accepts=%u requests=%u bytes=%llu\n", name, result.connections, result.requests, result.bytes);
    check(no_request(result), name);
}

void fetch(probe_view& view, loopback_server& target, bool blocked) {
    const auto before = target.snapshot();
    const auto outcome = view.run("try { await fetch('" + target.origin() +
        "/fetch', {cache:'no-store'}); return 'load'; } catch (_) { return 'error'; }");
    const auto observed = since(before, target.snapshot());
    std::printf("fetch view=%p expected=%s actual=%s requests=%u bytes=%llu\n",
        static_cast<void*>(view.web), blocked ? "error" : "load", outcome.c_str(), observed.requests, observed.bytes);
    check(outcome == (blocked ? "error" : "load"), "fetch result");
    if (blocked) observe_silence(target, before, "filtered fetch");
    else check(since(before, target.snapshot()).requests > 0, "unfiltered fetch reached target");
}

void resources(probe_view& view, loopback_server& target, bool blocked) {
    const auto before = target.snapshot();
    check(view.run("await Promise.all(['img','script','link'].map(tag => new Promise(resolve => {"
        "const e=document.createElement(tag); e.onload=e.onerror=resolve;"
        "if(tag==='link'){e.rel='stylesheet';e.href='" + target.origin() + "/style';}"
        "else e.src='" + target.origin() + "/'+tag; document.head.appendChild(e);"
        "}))); return 'attempted';") == "attempted", "resource attempts completed");
    if (blocked) observe_silence(target, before, "filtered image/script/style");
    else check(since(before, target.snapshot()).requests >= 3, "unfiltered resource requests reached target");
}

void frame_fetch(probe_view& view, loopback_server& source, loopback_server& target, bool blocked) {
    const auto before = target.snapshot();
    const auto result = view.run("return await new Promise(resolve => {"
        "window.addEventListener('message',e=>resolve(e.data),{once:true});"
        "const f=document.createElement('iframe'); f.src='" + source.origin() + "/frame';document.body.appendChild(f);});");
    check(result == (blocked ? "error" : "load"), "frame fetch settled");
    if (blocked) observe_silence(target, before, "filtered frame fetch");
    else check(since(before, target.snapshot()).requests > 0, "unfiltered frame fetch reached target");
}

void frame_document(probe_view& view, loopback_server& target, bool blocked) {
    const auto uri = target.origin() + "/frame-document";
    struct attempt { const std::string& uri; bool observed{}; } state{uri};
    const auto signal = g_signal_connect(view.web, "resource-load-started",
        G_CALLBACK(+[](WebKitWebView*, WebKitWebResource*, WebKitURIRequest* request, void* data) {
            auto& value = *static_cast<attempt*>(data);
            if (value.uri == webkit_uri_request_get_uri(request)) value.observed = true;
        }), &state);
    const auto before = target.snapshot();
    check(view.run("const f=document.createElement('iframe');f.src='" + uri +
        "';document.body.appendChild(f);return f.isConnected&&f.contentWindow&&f.src==='" +
        uri + "'?'attempted':'missing';") == "attempted", "frame document scheduled");
    // A blocked frame on this GTK engine never emits resource-load-started.
    // The acknowledged DOM navigation and positive controls before filtering
    // and after removal are required; an absent engine signal is not evidence.
    if (blocked) {
        observe_silence(target, before, "filtered frame document");
        std::printf("Filtered frame resource-load-started=%d (diagnostic)\n", state.observed);
    } else {
        wait_for([&] { return state.observed; }, "native unfiltered frame document attempt");
        wait_for([&] { return since(before, target.snapshot()).requests > 0; }, "unfiltered frame document");
    }
    g_signal_handler_disconnect(view.web, signal);
}

void engine_probe(neoastra_app_t* app, neoastra_environment_t* env, const std::filesystem::path& root) {
    loopback_server target, source(target.origin());
    probe_view control(app, env), filtered(app, env);
    check(control.navigate(source.origin()), "control document");
    fetch(control, target, false);
    resources(control, target, false);
    frame_fetch(control, source, target, false);
    frame_document(control, target, false);

    const neo_request_filter::rule rule{neo_request_filter::host_match::loopback, "", target.port()};
    const auto json = neo_request_filter::filter({&rule, 1}).webkit_json();
    auto* store = webkit_user_content_filter_store_new((root / "engine-store").c_str());
    check(store, "create engine store");
    auto* bytes = g_bytes_new(json.data(), json.size());
    struct compile_result { bool done{}; WebKitUserContentFilter* filter{}; } compiled;
    webkit_user_content_filter_store_save(store, "baseline", bytes, nullptr,
        [](GObject* source, GAsyncResult* result, void* p) {
            auto& state = *static_cast<compile_result*>(p);
            GError* error{};
            state.filter = webkit_user_content_filter_store_save_finish(WEBKIT_USER_CONTENT_FILTER_STORE(source), result, &error);
            if (error) std::fprintf(stderr, "Compile: %s\n", error->message);
            check(!error && state.filter, "compile rules");
            state.done = true;
        }, &compiled);
    g_bytes_unref(bytes);
    wait_for([&] { return compiled.done; }, "compile completion");
    auto* manager = webkit_web_view_get_user_content_manager(filtered.web);
    webkit_user_content_manager_add_filter(manager, compiled.filter);
    const auto before = target.snapshot();
    const auto loaded = filtered.navigate(target.origin());
    std::printf("Initial document reported loaded=%d\n", loaded);
    observe_silence(target, before, "initial document");
    check(!loaded, "initial document refused");
    check(filtered.navigate(source.origin()), "filtered source document");
    fetch(filtered, target, true);
    resources(filtered, target, true);
    frame_fetch(filtered, source, target, true);
    frame_document(filtered, target, true);
    fetch(control, target, false); // Same environment/network session, separate manager.
    webkit_user_content_manager_remove_filter(manager, compiled.filter);
    fetch(filtered, target, false);
    frame_document(filtered, target, false);
    webkit_user_content_filter_unref(compiled.filter);
    g_object_unref(store);
}

void api_probe(neoastra_app_t* app, neoastra_environment_t* env) {
    const auto stores = [] {
        std::set<std::string> paths;
        const auto prefix = "neoastra-request-filter-" + std::to_string(getpid()) + "-";
        for (const auto& path : std::filesystem::directory_iterator(g_get_tmp_dir())) {
            if (path.path().filename().string().starts_with(prefix)) {
                paths.insert(path.path().string());
                const auto permissions = path.status().permissions();
                check((permissions & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) ==
                    std::filesystem::perms::none, "private filter directory");
            }
        }
        return paths;
    };
    const auto before_stores = stores();
    loopback_server target, source(target.origin());
    neoastra_capability_info_t capability{};
    capability.size = sizeof(capability);
    capability.version = 1;
    check(neoastra_environment_get_capability(env, NEOASTRA_CAPABILITY_REQUEST_FILTER, &capability) == NEOASTRA_OK, "query filter capability");
    constexpr auto base = NEOASTRA_REQUEST_FILTER_SUBRESOURCES | NEOASTRA_REQUEST_FILTER_FRAMES | NEOASTRA_REQUEST_FILTER_PER_VIEW;
    check(capability.support == NEOASTRA_SUPPORT_LIMITED && capability.flags == base, "verified baseline capability");
    neoastra_request_rule_t rule{};
    rule.size = sizeof(rule); rule.version = 1; rule.port = target.port();
    rule.schemes = NEOASTRA_REQUEST_HTTP | NEOASTRA_REQUEST_HTTPS;
    rule.kinds = NEOASTRA_REQUEST_KINDS_ALL;
    neoastra_request_filter_t filter{};
    filter.size = sizeof(filter); filter.version = 1; filter.rules = &rule; filter.rule_count = 1;
    neoastra_request_filter_t empty{};
    empty.size = sizeof(empty); empty.version = 1;
    {
        probe_view control(app, env);
        check(control.navigate(source.origin()), "API control document");
        fetch(control, target, false);
        resources(control, target, false);
        frame_document(control, target, false);
        frame_fetch(control, source, target, false);
        auto filtered = std::make_unique<probe_view>(app, env, &filter);
        check(stores().size() == before_stores.size() + 1, "initial private store exists");
        auto before = target.snapshot();
        check(!filtered->navigate(target.origin()), "API initial document refused");
        observe_silence(target, before, "API initial document");
        check(filtered->navigate(source.origin()), "API source document");
        fetch(*filtered, target, true);
        resources(*filtered, target, true);
        frame_document(*filtered, target, true);
        frame_fetch(*filtered, source, target, true);
        fetch(control, target, false);

        // Fail private-store creation using only this process's own temporary
        // parent. Do not change /tmp or any user/engine-wide trust setting.
        const auto temporary = std::filesystem::path(g_get_tmp_dir());
        check(temporary.filename() == "tmp" && temporary.parent_path().filename().string().starts_with("neoastra-gtk-filter-probe-"),
            "fault injection is confined to the probe");
        check(geteuid() != 0, "permission failure injection requires an unprivileged test user");
        check(chmod(temporary.c_str(), 0500) == 0, "make private temporary parent unwritable");
        rule.port = source.port();
        const auto failed_update = filtered->set_filter(filter);
        completion failed_create;
        neoastra_view_options_t failed_options{};
        failed_options.size = sizeof(failed_options); failed_options.version = 2;
        failed_options.window = control.window(); failed_options.fill_parent = 1;
        failed_options.bridge_policy = NEOASTRA_BRIDGE_DISABLED;
        failed_options.request_filter = &filter;
        check(neoastra_environment_create_view_async(env, &failed_options, view_created, &failed_create, nullptr, nullptr) ==
            NEOASTRA_OK, "start failed initial filter");
        wait_for([&] { return failed_create.done; }, "failed initial filter completion");
        check(chmod(temporary.c_str(), 0700) == 0, "restore private temporary parent");
        check(failed_update == NEOASTRA_ERROR_NATIVE_FAILURE, "store failure is reported");
        check(failed_create.result == NEOASTRA_ERROR_NATIVE_FAILURE && !failed_create.value, "failed initial filter publishes no view");
        rule.port = target.port();
        fetch(*filtered, target, true);
        fetch(*filtered, source, false);

        filter.required_flags = NEOASTRA_REQUEST_FILTER_WEBSOCKETS;
        check(filtered->set_filter(filter) == NEOASTRA_ERROR_NOT_SUPPORTED, "unsupported coverage rejected");
        filter.required_flags = 0;
        rule.port = source.port();
        check(filtered->set_filter(filter, true) == NEOASTRA_ERROR_CANCELED, "cancel before installation");
        fetch(*filtered, target, true);
        fetch(*filtered, source, false);
        check(filtered->set_filter(filter) == NEOASTRA_OK, "replace filter");
        fetch(*filtered, source, true);
        fetch(*filtered, target, false);
        rule.port = target.port();
        check(filtered->set_filter(filter) == NEOASTRA_OK, "restore target rule");

        // All operations use copied input. Callback delivery and the installed
        // result follow FIFO; mutating the caller's descriptor cannot change it.
        struct ordered_completion { completion result; int id; std::vector<int>* order; };
        std::vector<int> order;
        ordered_completion states[3]{{{}, 0, &order}, {{}, 1, &order}, {{}, 2, &order}};
        neoastra_operation_t* operations[3]{};
        for (int index = 0; index < 3; ++index) {
            const auto& description = index == 1 ? empty : filter;
            check(neoastra_view_set_request_filter_async(filtered->value, &description,
                [](void* p, neoastra_result_t result, const neoastra_error_t* error) {
                    auto& state = *static_cast<ordered_completion*>(p);
                    operation_completed(&state.result, result, error);
                    state.order->push_back(state.id);
                }, &states[index], &operations[index], nullptr) == NEOASTRA_OK, "queue update");
        }
        rule.port = source.port();
        wait_for([&] { return order.size() == 3; }, "FIFO completion");
        check(order == std::vector<int>({0, 1, 2}), "FIFO order");
        for (int index = 0; index < 3; ++index) {
            check(states[index].result.result == NEOASTRA_OK && states[index].result.calls == 1, "FIFO result");
            neoastra_operation_release(operations[index]);
        }
        fetch(*filtered, target, true);
        fetch(*filtered, source, false);
        rule.port = target.port();

        const auto reject_popup = [&](probe_view& opener, neoastra_decision_t* decision) {
            neoastra_view_options_t options{};
            options.size = sizeof(options); options.version = 1;
            options.window = opener.window(); options.fill_parent = 1;
            options.bridge_policy = NEOASTRA_BRIDGE_DISABLED;
            options.popup_request = decision;
            completion created;
            check(neoastra_environment_create_view_async(env, &options, view_created, &created, nullptr, nullptr) ==
                NEOASTRA_OK, "rejected popup start");
            check(created.done && created.result == NEOASTRA_ERROR_NOT_SUPPORTED && !created.value,
                "unsafe popup rejected synchronously");
            neoastra_decision_response_t response{};
            response.size = sizeof(response); response.version = 1;
            response.action = NEOASTRA_DECISION_CANCEL;
            check(neoastra_decision_complete(decision, &response, nullptr) == NEOASTRA_OK, "cancel rejected popup");
        };
        bool refused{};
        filtered->on_popup = [&](neoastra_decision_t* decision) {
            reject_popup(*filtered, decision);
            refused = true;
        };
        webkit_settings_set_javascript_can_open_windows_automatically(webkit_web_view_get_settings(filtered->web), TRUE);
        before = target.snapshot();
        filtered->run("window.open('" + source.origin() + "/frame');return 'requested';");
        wait_for([&] { return refused; }, "filtered popup refusal");
        observe_silence(target, before, "no unsafe popup or replacement navigation");
        struct popup_case { std::string script; bool network_navigation; };
        const popup_case popup_variants[]{
            {"window.open('about:blank');return 'requested';", false},
            {"const a=document.createElement('a');a.target='_blank';a.href='" + source.origin() +
                "/frame';document.body.append(a);a.click();a.remove();return 'requested';", true}
        };
        for (const auto& item : popup_variants) {
            refused = false;
            before = target.snapshot();
            check(filtered->run(item.script) == "requested", "popup variant requested");
            wait_for([&] { return refused; }, "filtered about:blank/target=_blank popup refusal");
            observe_silence(target, before, "no popup variant request escape");
        }
        fetch(*filtered, target, true);
        check(filtered->set_filter(empty) == NEOASTRA_OK, "clear opener");
        fetch(*filtered, target, false);

        // Existing unfiltered tracked popups remain supported. Neither side may
        // subsequently install a filter into their engine-shared controller.
        std::unique_ptr<probe_view> popup;
        filtered->on_popup = [&](neoastra_decision_t* decision) {
            popup = std::make_unique<probe_view>(app, env, nullptr, decision);
            neoastra_decision_response_t response{};
            response.size = sizeof(response); response.version = 1;
            response.action = NEOASTRA_DECISION_ALLOW; response.target_view = popup->value;
            check(neoastra_decision_complete(decision, &response, nullptr) == NEOASTRA_OK, "accept unfiltered popup");
        };
        filtered->run("window.open('" + source.origin() + "/frame');return 'requested';");
        wait_for([&] { return popup && popup->navigation_done(); }, "unfiltered popup document");
        fetch(*popup, target, false);
        check(filtered->set_filter(filter) == NEOASTRA_ERROR_NOT_SUPPORTED, "shared opener filter rejected");
        check(popup->set_filter(filter) == NEOASTRA_ERROR_NOT_SUPPORTED, "shared popup filter rejected");
        fetch(*filtered, target, false);
        fetch(*popup, target, false);
        check(popup->set_filter(empty) == NEOASTRA_OK, "empty remains supported on popup");
        popup.reset();
        // The same creation paths must work without a filter. In particular,
        // an about:blank popup can issue a later fetch in this positive control.
        for (const auto& item : popup_variants) {
            check(filtered->run(item.script) == "requested", "unfiltered popup variant requested");
            wait_for([&] { return popup != nullptr; }, "unfiltered popup variant created");
            if (item.network_navigation)
                wait_for([&] { return popup->navigation_done(); }, "unfiltered popup variant document");
            fetch(*popup, target, false);
            popup.reset();
        }
        check(filtered->set_filter(filter) == NEOASTRA_ERROR_NOT_SUPPORTED, "shared controller stays restricted after popup disposal");
        filtered.reset();

        // Creating a popup must not race a nonempty compile whose old installed
        // snapshot is still empty. Begin the compile in the synchronous event.
        {
            probe_view pending(app, env);
            check(pending.navigate(source.origin()), "pending filter source");
            webkit_settings_set_javascript_can_open_windows_automatically(webkit_web_view_get_settings(pending.web), TRUE);
            completion installed;
            neoastra_operation_t* operation{};
            bool rejected{};
            pending.on_popup = [&](neoastra_decision_t* decision) {
                check(neoastra_view_set_request_filter_async(pending.value, &filter, operation_completed, &installed,
                    &operation, nullptr) == NEOASTRA_OK, "start compile inside popup event");
                reject_popup(pending, decision);
                rejected = true;
            };
            pending.run("window.open('" + source.origin() + "/frame');return 'requested';");
            wait_for([&] { return rejected && installed.done; }, "pending filter and rejected popup");
            check(installed.result == NEOASTRA_OK && installed.calls == 1, "compile survives rejected popup");
            neoastra_operation_release(operation);
            fetch(pending, target, true);
        }

        auto disposable = std::make_unique<probe_view>(app, env, &filter);
        completion disposed;
        neoastra_operation_t* operation{};
        rule.port = source.port();
        check(neoastra_view_set_request_filter_async(disposable->value, &filter, operation_completed, &disposed,
            &operation, nullptr) == NEOASTRA_OK, "start compile before disposal");
        disposable.reset();
        wait_for([&] { return disposed.done; }, "dispose completion");
        check(disposed.result == NEOASTRA_ERROR_DISPOSED && disposed.calls == 1, "dispose completes once");
        neoastra_operation_release(operation);
        wait_for([&] { return stores() == before_stores; }, "all late compiles release their stores");
        check(disposed.calls == 1, "late compile does not complete twice");
    }
    wait_for([&] { return stores() == before_stores; }, "no leaked private stores");
}

void disposal_traffic(neoastra_app_t* app, neoastra_environment_t* env) {
    const auto start = [](probe_view& view, loopback_server& target, const std::string& popup_uri) {
        webkit_settings_set_javascript_can_open_windows_automatically(webkit_web_view_get_settings(view.web), TRUE);
        // Use an allowed popup URL so this really exercises the host's create
        // handler, rather than a popup already refused by the content list.
        view.run("window.__ticks=0;const send=()=>{window.open('" + popup_uri + "');fetch('" + target.origin() +
            "/get').catch(()=>{});navigator.sendBeacon('" + target.origin() +
            "/beacon','probe');const i=new Image();i.src='" + target.origin() +
            "/image?'+window.__ticks;};window.addEventListener('pagehide',()=>navigator.sendBeacon('" + target.origin() +
            "/pagehide','probe'));window.__timer=setInterval(()=>{++window.__ticks;send();},10);return 'started';");
        wait_for([&] { return view.run("return window.__ticks>=10?'yes':'no';") == "yes"; }, "traffic ticks");
    };
    loopback_server control_target, source;
    {
        probe_view control(app, env);
        check(control.navigate(source.origin()), "traffic positive source");
        unsigned popups{};
        control.on_popup = [&](neoastra_decision_t*) { ++popups; }; // Native default denies, without a new view.
        start(control, control_target, source.origin() + "/popup");
        check(popups > 0 && control_target.snapshot().requests > 0, "traffic and window.open positive control");
    }
    for (int round = 0; round < 3; ++round) {
        loopback_server target;
        neoastra_request_rule_t rule{};
        rule.size = sizeof(rule); rule.version = 1; rule.port = target.port();
        rule.schemes = NEOASTRA_REQUEST_HTTP | NEOASTRA_REQUEST_HTTPS; rule.kinds = NEOASTRA_REQUEST_KINDS_ALL;
        neoastra_request_filter_t filter{};
        filter.size = sizeof(filter); filter.version = 1; filter.rules = &rule; filter.rule_count = 1;
        auto view = std::make_unique<probe_view>(app, env, &filter);
        check(view->navigate(source.origin()), "filtered traffic source");
        unsigned popups{};
        view->on_popup = [&](neoastra_decision_t*) { ++popups; };
        start(*view, target, source.origin() + "/popup");
        const auto live = target.snapshot();
        std::printf("traffic round=%d popups=%u requests=%u bytes=%llu\n", round, popups, live.requests, live.bytes);
        check(popups > 0, "filtered page really requested popups");
        check(no_request(live), "live window.open traffic remains filtered");
        view.reset();
        observe_silence(target, {}, "no request escape on disposal under traffic");
    }
}

void compile_errors(neoastra_app_t* app, neoastra_environment_t* env) {
    loopback_server target, source;
    neoastra_request_rule_t rule{};
    rule.size = sizeof(rule); rule.version = 1; rule.port = target.port();
    rule.schemes = NEOASTRA_REQUEST_HTTP | NEOASTRA_REQUEST_HTTPS; rule.kinds = NEOASTRA_REQUEST_KINDS_ALL;
    neoastra_request_filter_t filter{};
    filter.size = sizeof(filter); filter.version = 1; filter.rules = &rule; filter.rule_count = 1;
    const auto expect_compile_error = [](const completion& state) {
        check(state.done && state.calls == 1 && state.result == NEOASTRA_ERROR_NATIVE_FAILURE &&
            state.error_domain == "webkitgtk" && state.native_error == WEBKIT_USER_CONTENT_FILTER_ERROR_INVALID_SOURCE,
            "real compile GError mapped once to native failure");
    };
    const auto store_count = [] {
        size_t count{};
        const auto prefix = "neoastra-request-filter-" + std::to_string(getpid()) + "-";
        for (const auto& path : std::filesystem::directory_iterator(g_get_tmp_dir()))
            if (path.path().filename().string().starts_with(prefix)) ++count;
        return count;
    };
    check(store_count() == 0, "compile-error process starts without stores");
    {
        probe_view control(app, env);
        check(control.navigate(source.origin()), "compile-error positive source");
        fetch(control, target, false);
        completion created;
        neoastra_view_options_t options{};
        options.size = sizeof(options); options.version = 2; options.window = control.window();
        options.fill_parent = 1; options.bridge_policy = NEOASTRA_BRIDGE_DISABLED; options.request_filter = &filter;
        corrupt_compile_countdown = 1;
        check(neoastra_environment_create_view_async(env, &options,
            [](void* p, neoastra_result_t result, neoastra_view_t* view, const neoastra_error_t* error) {
                operation_completed(p, result, error);
                static_cast<completion*>(p)->value = view;
            }, &created, nullptr, nullptr) == NEOASTRA_OK, "start initial compile-error");
        wait_for([&] { return created.done; }, "initial compile-error completion");
        expect_compile_error(created);
        check(!created.value && corrupted_compiles == 1, "initial compiler failure publishes no fallback view");
        wait_for([&] { return store_count() == 0; }, "initial failed store removed");

        probe_view filtered(app, env, &filter);
        check(filtered.navigate(source.origin()), "compile-error filtered source");
        fetch(filtered, target, true);
        rule.port = source.port();
        completion failed;
        neoastra_operation_t* operation{};
        corrupt_compile_countdown = 1;
        check(neoastra_view_set_request_filter_async(filtered.value, &filter, operation_completed, &failed,
            &operation, nullptr) == NEOASTRA_OK, "start update compile-error");
        wait_for([&] { return failed.done; }, "update compile-error completion");
        expect_compile_error(failed);
        neoastra_operation_release(operation);
        wait_for([&] { return store_count() == 1; }, "failed update retains only installed store");
        fetch(filtered, target, true);
        fetch(filtered, source, false);
        fetch(control, target, false);

        struct ordered { completion result; int id; std::vector<int>* order; };
        std::vector<int> order;
        ordered states[3]{{{}, 0, &order}, {{}, 1, &order}, {{}, 2, &order}};
        neoastra_operation_t* operations[3]{};
        corrupt_compile_countdown = 2; // A succeeds, B compiles invalid JSON, C succeeds.
        for (int index = 0; index < 3; ++index) {
            rule.port = index == 1 ? target.port() : source.port();
            check(neoastra_view_set_request_filter_async(filtered.value, &filter,
                [](void* p, neoastra_result_t result, const neoastra_error_t* error) {
                    auto& state = *static_cast<ordered*>(p);
                    operation_completed(&state.result, result, error);
                    state.order->push_back(state.id);
                }, &states[index], &operations[index], nullptr) == NEOASTRA_OK, "queue compiler-error FIFO");
        }
        wait_for([&] { return order.size() == 3; }, "compiler-error FIFO completes");
        check(order == std::vector<int>({0, 1, 2}), "compiler failure preserves FIFO order");
        expect_compile_error(states[1].result);
        for (int index : {0, 2})
            check(states[index].result.calls == 1 && states[index].result.result == NEOASTRA_OK, "FIFO neighbors succeed");
        for (auto* queued : operations) neoastra_operation_release(queued);
        check(corrupted_compiles == 3 && corrupt_compile_countdown == 0, "all three real compiler faults consumed");
        fetch(filtered, source, true);
        fetch(filtered, target, false);
    }
    wait_for([&] { return store_count() == 0; }, "all compiler-error stores removed");
    std::puts("PASS real compile GError: initial/update/FIFO, retained filter, isolation, cleanup");
}

void request_matrix(neoastra_app_t* app, neoastra_environment_t* env) {
    loopback_server target, source(target.origin()), cross(target.origin());
    neoastra_request_rule_t rule{};
    rule.size = sizeof(rule); rule.version = 1; rule.port = target.port();
    rule.schemes = NEOASTRA_REQUEST_HTTP | NEOASTRA_REQUEST_HTTPS; rule.kinds = NEOASTRA_REQUEST_KINDS_ALL;
    neoastra_request_filter_t filter{};
    filter.size = sizeof(filter); filter.version = 1; filter.rules = &rule; filter.rule_count = 1;
    neoastra_request_filter_t empty{};
    empty.size = sizeof(empty); empty.version = 1;
    probe_view control(app, env), filtered(app, env, &filter);
    check(control.navigate(source.origin()) && filtered.navigate(source.origin()), "matrix source documents");

    // Every case has a real host-observed positive control before the negative
    // case and again after clear. Invalid media/font payloads are intentional:
    // these test request delivery, not decoding, playback, or EventSource parsing.
    struct resource_case { const char* name; const char* script; };
    const resource_case cases[]{
        {"xhr", "await new Promise(resolve=>{const x=new XMLHttpRequest();x.onload=x.onerror=resolve;x.open('GET',url);x.send();});"},
        {"fetch-post", "try{await fetch(url,{method:'POST',body:'probe',cache:'no-store'});}catch(_){}"},
        {"eventsource", "await new Promise(resolve=>{const e=new EventSource(url);e.onopen=e.onerror=()=>{e.close();resolve();};});"},
        {"font", "try{await new FontFace('probe','url('+url+')').load();}catch(_){}"},
        {"audio", "await new Promise(resolve=>{const e=document.createElement('audio');e.preload='auto';e.onloadedmetadata=e.onerror=()=>{e.remove();resolve();};document.body.append(e);e.src=url;e.load();});"},
        {"video", "await new Promise(resolve=>{const e=document.createElement('video');e.preload='auto';e.onloadedmetadata=e.onerror=()=>{e.remove();resolve();};document.body.append(e);e.src=url;e.load();});"},
        {"preload", "await new Promise(resolve=>{const e=document.createElement('link');e.rel='preload';e.as='fetch';e.crossOrigin='anonymous';e.onload=e.onerror=()=>{e.remove();resolve();};e.href=url;document.head.append(e);});"},
        {"beacon", "navigator.sendBeacon(url,'probe');"},
        {"form-get", "const f=document.createElement('iframe');f.name='get-probe';document.body.append(f);const form=document.createElement('form');form.method='GET';form.action=url;form.target=f.name;document.body.append(form);form.submit();form.remove();"},
        {"form-post", "const f=document.createElement('iframe');f.name='post-probe';document.body.append(f);const form=document.createElement('form');form.method='POST';form.action=url;form.target=f.name;document.body.append(form);form.submit();form.remove();"}
    };
    const auto attempt = [&](probe_view& view, const resource_case& item, bool blocked) {
        const auto before = target.snapshot();
        const auto script = "const url='" + target.origin() + "/" + item.name + "';" + item.script + "return 'attempted';";
        check(view.run(script) == "attempted", "matrix request initiated");
        if (blocked) observe_silence(target, before, item.name);
        else wait_for([&] { return since(before, target.snapshot()).requests > 0; }, item.name);
        const auto observed = since(before, target.snapshot());
        std::printf("matrix %s blocked=%d requests=%u bytes=%llu\n", item.name, blocked, observed.requests, observed.bytes);
    };
    for (const auto& item : cases) {
        std::printf("matrix begin %s\n", item.name);
        attempt(control, item, false);
        attempt(filtered, item, true);
    }
    check(filtered.set_filter(empty) == NEOASTRA_OK, "matrix clear");
    for (const auto& item : cases) attempt(filtered, item, false);
    check(filtered.set_filter(filter) == NEOASTRA_OK, "matrix restore");

    for (auto* frame_source : {&source, &cross}) {
        const auto nested = [&](probe_view& view, bool blocked) {
            const auto before = target.snapshot();
            const auto result = view.run(
                "return await new Promise(resolve=>{const f=document.createElement('iframe');"
                "const receive=e=>{if(e.source===f.contentWindow){removeEventListener('message',receive);f.remove();resolve(e.data);}};"
                "addEventListener('message',receive);f.srcdoc=`<script>addEventListener('message',e=>parent.postMessage(e.data,'*'));</script>"
                "<iframe src=\"" + frame_source->origin() + "/frame\"></iframe>`;document.body.append(f);});");
            check(result == (blocked ? "error" : "load"), "nested frame fetch settles");
            if (blocked) observe_silence(target, before, "nested frame fetch");
            else check(since(before, target.snapshot()).requests > 0, "nested frame positive control");
        };
        nested(control, false);
        nested(filtered, true);
    }

    // Public C-ABI reload uses the existing loaded document URL. Its first
    // positive request is observed before installing the rule on this view.
    probe_view reloader(app, env);
    auto before = target.snapshot();
    check(reloader.navigate(target.origin()) && reloader.reload(), "unfiltered document and reload");
    check(since(before, target.snapshot()).requests >= 2, "reload positive requests");
    check(reloader.set_filter(filter) == NEOASTRA_OK, "install reload rule");
    before = target.snapshot();
    check(!reloader.reload(), "filtered reload refused");
    observe_silence(target, before, "filtered reload");
    check(reloader.set_filter(empty) == NEOASTRA_OK && reloader.navigate(target.origin()) && reloader.reload(),
        "clear restores document and reload");

    // The listener is IPv4-only. Each alias must actually reach it unfiltered;
    // IPv6 and public-origin/DNS cases are not inferred from these results.
    rule.host_match = NEOASTRA_REQUEST_HOST_LOOPBACK;
    check(filtered.set_filter(filter) == NEOASTRA_OK, "install loopback alias rule");
    for (const char* host : {"localhost", "localhost.", "127.0.0.1", "127.1", "2130706433", "0x7f000001", "0177.0.0.1", "127%2e0%2e0%2e1"}) {
        const auto uri = target.origin(host) + "/alias";
        const auto send = [&](probe_view& view, bool blocked) {
            const auto start = target.snapshot();
            const auto result = view.run("try{await fetch('" + uri + "',{cache:'no-store'});return 'load';}catch(_){return 'error';}");
            std::printf("alias %s blocked=%d result=%s\n", host, blocked, result.c_str());
            check(result == (blocked ? "error" : "load"), "alias fetch result");
            if (blocked) observe_silence(target, start, "loopback alias");
            else check(since(start, target.snapshot()).requests > 0, "alias positive request");
        };
        send(control, false);
        send(filtered, true);
    }
    rule.host_match = NEOASTRA_REQUEST_HOST_EXACT;
    const std::string exact = "127.0.0.1";
    rule.host = text(exact);
    check(filtered.set_filter(filter) == NEOASTRA_OK, "install exact-host rule");
    fetch(filtered, target, true);
    before = target.snapshot();
    check(filtered.run("await fetch('" + target.origin("localhost") + "/exact-control');return 'load';") == "load" &&
        since(before, target.snapshot()).requests > 0, "exact host does not resolve DNS aliases");

    // Unsupported classes remain explicit rejections, not newly inferred flags.
    for (const auto flag : {NEOASTRA_REQUEST_FILTER_WEBSOCKETS, NEOASTRA_REQUEST_FILTER_REDIRECTS,
        NEOASTRA_REQUEST_FILTER_SERVICE_WORKERS, NEOASTRA_REQUEST_FILTER_SHARED_WORKERS,
        NEOASTRA_REQUEST_FILTER_DEDICATED_WORKERS, NEOASTRA_REQUEST_FILTER_RESOURCE_KINDS,
        NEOASTRA_REQUEST_FILTER_REPORTS_REFUSALS}) {
        filter.required_flags = flag;
        check(filtered.set_filter(filter) == NEOASTRA_ERROR_NOT_SUPPORTED, "unsupported matrix coverage refused");
    }
    fetch(filtered, target, true); // Rejected requirements never clear the old rule.
    std::puts("PASS HTTP resource/form/reload/nested-frame/IPv4-alias matrix; optional flags remain unsupported");
}

void addressing_matrix(neoastra_app_t* app, neoastra_environment_t* env) {
    loopback_server ipv6({}, true), ipv4, source;
    neoastra_request_rule_t rule{};
    rule.size = sizeof(rule); rule.version = 1; rule.host_match = NEOASTRA_REQUEST_HOST_LOOPBACK;
    rule.schemes = NEOASTRA_REQUEST_HTTP | NEOASTRA_REQUEST_HTTPS; rule.kinds = NEOASTRA_REQUEST_KINDS_ALL;
    neoastra_request_filter_t filter{};
    filter.size = sizeof(filter); filter.version = 1; filter.rules = &rule; filter.rule_count = 1;
    neoastra_request_filter_t empty{};
    empty.size = sizeof(empty); empty.version = 1;
    probe_view control(app, env), filtered(app, env);
    check(control.navigate(source.origin()) && filtered.navigate(source.origin()), "IPv6 matrix source");
    struct address_case { loopback_server* server; const char* host; };
    const address_case cases[]{
        {&ipv6, "[::1]"}, {&ipv6, "[0:0:0:0:0:0:0:1]"},
        {&ipv6, "[0000:0000:0000:0000:0000:0000:0000:0001]"},
        {&ipv4, "[::ffff:127.0.0.1]"}, {&ipv4, "[::ffff:7f00:1]"},
        {&ipv4, "[0:0:0:0:0:ffff:7f00:1]"}
    };
    for (const auto& item : cases) {
        rule.port = item.server->port();
        check(filtered.set_filter(filter) == NEOASTRA_OK, "install IPv6 loopback rule");
        const auto attempt = [&](probe_view& view, bool blocked) {
            const auto before = item.server->snapshot();
            const auto result = view.run("try{await fetch('" + item.server->origin(item.host) +
                "/ipv6',{cache:'no-store'});return 'load';}catch(_){return 'error';}");
            std::printf("IPv6 alias %s blocked=%d result=%s\n", item.host, blocked, result.c_str());
            check(result == (blocked ? "error" : "load"), "IPv6 fetch result");
            if (blocked) observe_silence(*item.server, before, "IPv6 loopback refusal");
            else check(since(before, item.server->snapshot()).requests > 0, "IPv6 positive request");
        };
        attempt(control, false);
        attempt(filtered, true);
        check(filtered.set_filter(empty) == NEOASTRA_OK, "clear IPv6 rule");
        attempt(filtered, false);
    }
    std::puts("PASS IPv6 and IPv4-mapped loopback spellings with positive/clear controls");
}

void link_probes(neoastra_app_t* app, neoastra_environment_t* env) {
    loopback_server target, source;
    neoastra_request_rule_t rule{};
    rule.size = sizeof(rule); rule.version = 1; rule.port = target.port();
    rule.schemes = NEOASTRA_REQUEST_HTTP | NEOASTRA_REQUEST_HTTPS; rule.kinds = NEOASTRA_REQUEST_KINDS_ALL;
    neoastra_request_filter_t filter{};
    filter.size = sizeof(filter); filter.version = 1; filter.rules = &rule; filter.rule_count = 1;
    neoastra_request_filter_t empty{};
    empty.size = sizeof(empty); empty.version = 1;
    probe_view control(app, env), filtered(app, env, &filter);
    check(control.navigate(source.origin()) && filtered.navigate(source.origin()), "link probe source");
    const std::string prefetch = "const e=document.createElement('link');e.rel='prefetch';e.href='" +
        target.origin() + "/prefetch';document.head.append(e);return 'attempted';";
    const std::string ping = "const f=document.createElement('iframe');f.name='ping-destination';document.body.append(f);"
        "const a=document.createElement('a');a.target=f.name;a.href='" + source.origin() + "/ping-destination';"
        "a.ping='" + target.origin() + "/ping';document.body.append(a);a.click();a.remove();return 'attempted';";
    for (const auto& item : {std::pair{"prefetch", prefetch}, std::pair{"ping", ping}}) {
        const auto attempt = [&](probe_view& view) {
            const auto before = target.snapshot();
            check(view.run(item.second) == "attempted", "link attempt acknowledged");
            const auto until = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
            wait_for([&] { return g_get_monotonic_time() >= until; }, "bounded link observation");
            return since(before, target.snapshot());
        };
        const auto positive = attempt(control);
        std::printf("link %s positive requests=%u bytes=%llu\n", item.first, positive.requests, positive.bytes);
        if (!positive.requests) {
            std::printf("NOT_EXERCISED %s: no positive request in this engine/configuration; no filter verdict\n", item.first);
            continue;
        }
        const auto negative = attempt(filtered);
        std::printf("link %s filtered requests=%u bytes=%llu\n", item.first, negative.requests, negative.bytes);
        check(no_request(negative), "link request escaped HTTP base filter");
        check(filtered.set_filter(empty) == NEOASTRA_OK, "clear link rule");
        check(attempt(filtered).requests > 0, "link clear positive request");
        check(filtered.set_filter(filter) == NEOASTRA_OK, "restore link rule");
        std::printf("PASS %s positive/refused/clear\n", item.first);
    }
    // This manual probe is not a CTest acceptance gate: absent positive
    // controls are explicitly NOT_EXERCISED, never counted as filter passes.
    std::puts("Link observations complete; inspect NOT_EXERCISED before claiming coverage");
}

void scope_diagnostics(neoastra_app_t* app, neoastra_environment_t* env) {
    loopback_server target, source(target.origin()), first(source.origin());
    neoastra_request_rule_t rule{};
    rule.size = sizeof(rule); rule.version = 1; rule.port = target.port();
    rule.schemes = NEOASTRA_REQUEST_HTTP | NEOASTRA_REQUEST_HTTPS;
    rule.kinds = NEOASTRA_REQUEST_KINDS_ALL;
    neoastra_request_filter_t filter{};
    filter.size = sizeof(filter); filter.version = 1; filter.rules = &rule; filter.rule_count = 1;
    neoastra_request_filter_t empty{};
    empty.size = sizeof(empty); empty.version = 1;
    probe_view control(app, env), filtered(app, env, &filter);
    check(control.navigate(source.origin()) && filtered.navigate(source.origin()), "scope diagnostic source");
    const auto diagnose = [&](const std::string& name, const std::string& script, loopback_server& observed) {
        const auto attempt = [&](probe_view& view) {
            const auto before = observed.snapshot();
            const auto result = view.run(script);
            const auto until = g_get_monotonic_time() + G_USEC_PER_SEC;
            wait_for([&] { return g_get_monotonic_time() >= until; }, "scope observation");
            const auto value = since(before, observed.snapshot());
            std::printf("scope %s view=%s result=%s requests=%u upgrades=%u bytes=%llu\n",
                name.c_str(), &view == &control ? "control" : "filtered-or-cleared", result.c_str(),
                value.requests, value.upgrades, value.bytes);
            return value;
        };
        const auto positive = attempt(control);
        check(positive.requests > 0, "scope diagnostic positive request");
        const auto negative = attempt(filtered);
        std::printf("DIAGNOSTIC %s: %s (no capability granted)\n", name.c_str(),
            no_request(negative) ? "NO_REQUEST_OBSERVED" : "REACHES_TARGET");
        check(filtered.set_filter(empty) == NEOASTRA_OK, "clear diagnostic rule");
        const auto cleared = attempt(filtered);
        check(cleared.requests > 0, "scope diagnostic clear control");
        check(filtered.set_filter(filter) == NEOASTRA_OK, "restore diagnostic rule");
    };
    for (const char* code : {"301", "302", "303", "307", "308"}) {
        for (const bool chain : {false, true}) {
            const auto uri = (chain ? first.origin() + "/chain/" : source.origin() + "/redirect/") + code;
            diagnose(std::string(chain ? "two-hop-" : "redirect-") + code,
                "try{await fetch('" + uri + "',{cache:'no-store'});return 'load';}catch(_){return 'error';}", target);
        }
    }
    // The C ABI rejects WS schemes even when RequiredFlags is empty. Do not
    // bypass that guard or mistake an HTTP-only filter for a WS experiment.
    rule.schemes |= NEOASTRA_REQUEST_WS | NEOASTRA_REQUEST_WSS;
    check(filtered.set_filter(filter) == NEOASTRA_ERROR_NOT_SUPPORTED, "WS/WSS schemes rejected by product contract");
    rule.schemes = NEOASTRA_REQUEST_HTTP | NEOASTRA_REQUEST_HTTPS;
    const auto before_upgrade = target.snapshot();
    control.run("return await new Promise(resolve=>{const w=new WebSocket('ws://127.0.0.1:" +
        std::to_string(target.port()) + "/upgrade');w.onopen=()=>{w.close();resolve('open');};w.onerror=()=>resolve('error');});");
    const auto upgrade = since(before_upgrade, target.snapshot());
    check(upgrade.upgrades > 0, "unfiltered WebSocket handshake reaches host");
    std::printf("NOT_SUPPORTED WS/WSS rules rejected; positive WS upgrades=%u; no filtered WS/WSS verdict\n", upgrade.upgrades);
    const auto worker = "return await new Promise(resolve=>{const w=new Worker('" + source.origin() +
        "/dedicated-worker.js');w.onmessage=e=>{w.terminate();resolve(e.data);};"
        "w.onerror=()=>{w.terminate();resolve('bootstrap-error');};w.postMessage('fetch');});";
    diagnose("dedicated-worker-fetch", worker, target);
    rule.port = source.port();
    check(filtered.set_filter(filter) == NEOASTRA_OK, "install worker bootstrap diagnostic rule");
    diagnose("dedicated-worker-bootstrap", worker, source);
    std::puts("Scope diagnostics complete; NOT filter acceptance for redirects/WebSockets/workers");
}

std::string connect_worker(probe_view& view, const std::string& kind, const std::string& origin,
    const std::string& token, bool attach) {
    // Fresh script queries/scopes distinguish actual bootstraps from reusing an
    // existing registration. Replies travel over their own MessageChannel.
    return view.run("const kind='" + kind + "',origin='" + origin + "',token='" + token +
        "',attach=" + (attach ? "true;" : "false;") + R"JS(
        window.__rfEndpoint=null;window.__rfRegistration=null;window.__rfShared=null;
        window.__rfReady=false;window.__rfWorkerInfo='';window.__rfBootstrapFailure='';
        window.__rfRequest=(cmd,label='')=>new Promise(resolve=>{
            const channel=new MessageChannel();let timer;
            const finish=value=>{clearTimeout(timer);channel.port1.close();channel.port2.close();resolve(value);};
            timer=setTimeout(()=>finish('no-reply:'+window.__rfBootstrapFailure),5000);
            channel.port1.onmessage=e=>finish(String(e.data));
            try{window.__rfEndpoint.postMessage({cmd,label},[channel.port2]);}
            catch(e){finish('message-error:'+e.name);}
        });
        window.__rfClose=async()=>{
            if(kind==='shared'){
                if(window.__rfReady)await window.__rfRequest('stop');
                if(window.__rfShared)window.__rfShared.port.close();
                return 'closed';
            }
            if(window.__rfRegistration)
                return await window.__rfRegistration.unregister()?'unregistered':'already-unregistered';
            return 'no-registration';
        };
        let deadline;
        try{return await Promise.race([(async()=>{
            const script=origin+'/'+kind+'-worker.js?probe='+token;
            if(kind==='shared'){
                if(typeof SharedWorker!=='function')return 'api-unavailable';
                window.__rfShared=new SharedWorker(script,token);
                window.__rfShared.onerror=e=>{window.__rfBootstrapFailure=e.message||'worker-error';};
                window.__rfEndpoint=window.__rfShared.port;
                window.__rfShared.port.start();
                window.__rfWorkerInfo=script+'|'+token;
            }else{
                if(!navigator.serviceWorker)return 'api-unavailable';
                const scope=origin+'/worker-scope-'+token+'/';
                const registration=attach?await navigator.serviceWorker.getRegistration(scope):
                    await navigator.serviceWorker.register(script,{scope,updateViaCache:'none'});
                if(!registration)return 'registration-unavailable';
                window.__rfRegistration=registration;
                const worker=registration.active||registration.installing||registration.waiting;
                if(!worker)return 'worker-unavailable';
                if(worker.state!=='activated')await new Promise((resolve,reject)=>{
                    const changed=()=>{
                        if(worker.state==='activated'){worker.removeEventListener('statechange',changed);resolve();}
                        else if(worker.state==='redundant'){worker.removeEventListener('statechange',changed);reject(new Error('redundant'));}
                    };
                    worker.addEventListener('statechange',changed);changed();
                });
                window.__rfEndpoint=registration.active;
                window.__rfWorkerInfo=registration.scope+'|'+registration.active.scriptURL;
            }
            const answer=await window.__rfRequest('hello');
            window.__rfReady=answer.startsWith('ready:');
            return answer;
        })(),new Promise(resolve=>{deadline=setTimeout(()=>resolve('setup-timeout'),8000);})]);}
        catch(e){return 'bootstrap-error:'+e.name+':'+e.message;}
        finally{clearTimeout(deadline);}
    )JS");
}

void worker_diagnostics(neoastra_app_t* app, neoastra_environment_t* env, bool persistent) {
    unsigned missing_controls{}, affected_controls{};
    for (const std::string kind : {"shared", "service"}) {
        loopback_server target, source(target.origin());
        neoastra_request_rule_t rule{};
        rule.size = sizeof(rule); rule.version = 1; rule.port = target.port();
        rule.schemes = NEOASTRA_REQUEST_HTTP | NEOASTRA_REQUEST_HTTPS; rule.kinds = NEOASTRA_REQUEST_KINDS_ALL;
        neoastra_request_filter_t filter{};
        filter.size = sizeof(filter); filter.version = 1; filter.rules = &rule; filter.rule_count = 1;
        neoastra_request_filter_t empty{};
        empty.size = sizeof(empty); empty.version = 1;
        std::printf("WORKER ENV kind=%s persistent=%d origin=%s\n", kind.c_str(), persistent, source.origin().c_str());
        const auto cleanup = [&](probe_view& view) {
            std::printf("worker cleanup kind=%s result=%s\n", kind.c_str(), view.run("return await window.__rfClose();").c_str());
        };
        const auto measure = [&](probe_view& view, const char* stage, bool control) {
            const auto before = target.snapshot();
            const auto result = view.run("return await window.__rfRequest('fetch','" + std::string(stage) + "');");
            const auto until = g_get_monotonic_time() + G_USEC_PER_SEC;
            wait_for([&] { return g_get_monotonic_time() >= until; }, "worker traffic observation");
            const auto value = since(before, target.snapshot());
            const bool positive = result == "load" && value.requests > 0 && value.bytes > 0;
            std::printf("WORKER kind=%s stage=%s result=%s requests=%u bytes=%llu verdict=%s\n",
                kind.c_str(), stage, result.c_str(), value.requests, value.bytes,
                !no_request(value) ? "REACHES_TARGET" : result == "error" ? "NO_REQUEST_OBSERVED" : "INCONCLUSIVE");
            if (control && !positive) {
                ++affected_controls;
                std::printf("CONTROL_FAILED kind=%s stage=%s; not an isolation pass\n", kind.c_str(), stage);
            }
            return positive;
        };
        bool baseline_available{};
        for (const bool initially_filtered : {false, true}) {
            if (initially_filtered && !baseline_available) break;
            const auto token = initially_filtered ? "created-filtered" : "created-unfiltered";
            auto creator = std::make_unique<probe_view>(app, env, initially_filtered ? &filter : nullptr);
            probe_view peer(app, env);
            auto* session = webkit_web_view_get_network_session(creator->web);
            check(session == webkit_web_view_get_network_session(peer.web) &&
                (webkit_network_session_is_ephemeral(session) == !persistent), "worker views share the intended session");
            check(creator->navigate(source.origin()) && peer.navigate(source.origin()), "worker source documents");
            std::printf("worker APIs kind=%s %s\n", kind.c_str(), creator->run(
                "return 'secure='+isSecureContext+' shared='+typeof SharedWorker+' service='+typeof navigator.serviceWorker;").c_str());
            const auto creator_id = connect_worker(*creator, kind, source.origin(), token, false);
            std::printf("worker creator kind=%s token=%s reply=%s scripts=%u\n",
                kind.c_str(), token, creator_id.c_str(), source.worker_scripts());
            if (!creator_id.starts_with("ready:")) {
                ++missing_controls;
                std::printf("NOT_EXERCISED kind=%s token=%s: creator bootstrap unavailable\n", kind.c_str(), token);
                cleanup(*creator);
                continue;
            }
            if (!initially_filtered) {
                baseline_available = measure(*creator, "baseline-creator", true);
                if (!baseline_available) {
                    ++missing_controls;
                    std::printf("NOT_EXERCISED kind=%s: no positive worker fetch\n", kind.c_str());
                    cleanup(*creator);
                    continue;
                }
            }
            const auto peer_id = connect_worker(peer, kind, source.origin(), token, true);
            std::printf("worker peer kind=%s token=%s reply=%s same-instance=%d\n",
                kind.c_str(), token, peer_id.c_str(), peer_id == creator_id);
            const bool same_identity = peer.run("return window.__rfWorkerInfo;") ==
                creator->run("return window.__rfWorkerInfo;");
            if (!peer_id.starts_with("ready:") || !same_identity || (kind == "shared" && peer_id != creator_id)) {
                ++missing_controls;
                std::printf("NOT_EXERCISED kind=%s: common worker identity not established\n", kind.c_str());
                cleanup(peer);
                cleanup(*creator);
                continue;
            }
            // Service workers may restart between messages; registration identity,
            // not an execution-instance identifier, establishes their shared scope.
            measure(peer, initially_filtered ? "unfiltered-peer-of-filtered-creator" : "baseline-peer", true);
            auto& modified = initially_filtered ? *creator : peer;
            check(modified.set_filter(filter) == NEOASTRA_OK, "install worker diagnostic rule");
            measure(modified, initially_filtered ? "new-filtered-worker-fetch" : "existing-worker-after-filter", false);
            if (!initially_filtered) measure(*creator, "unfiltered-creator-after-peer-filter", true);
            // Run these after the worker observations: a page request must not
            // silently stand in for a worker control or perturb its first sample.
            std::printf("PAGE_CONTROL kind=%s stage=modified-view expected=blocked\n", kind.c_str());
            fetch(modified, target, true);
            std::printf("PAGE_CONTROL kind=%s stage=unfiltered-other-view expected=allowed\n", kind.c_str());
            fetch(initially_filtered ? peer : *creator, target, false);
            check(modified.set_filter(empty) == NEOASTRA_OK, "clear worker diagnostic rule");
            measure(modified, "clear-before-creator-disposal", true);
            check(modified.set_filter(filter) == NEOASTRA_OK, "restore worker diagnostic rule");
            creator.reset(); // Release the creating NeoAstra view and window handles.
            measure(peer, initially_filtered ? "unfiltered-peer-after-creator-disposal" : "filtered-peer-after-creator-disposal",
                initially_filtered);
            std::printf("PAGE_CONTROL kind=%s stage=surviving-peer expected=%s\n",
                kind.c_str(), initially_filtered ? "allowed" : "blocked");
            fetch(peer, target, !initially_filtered);
            check(peer.set_filter(empty) == NEOASTRA_OK, "clear surviving peer");
            measure(peer, "surviving-peer-clear", true);
            cleanup(peer);
        }
        if (baseline_available) {
            // Fresh query/name/scope for each bootstrap prevents an already
            // running worker from disguising a missing network request.
            for (const int phase : {0, 1, 2}) {
                probe_view view(app, env);
                check(view.navigate(source.origin()), "bootstrap diagnostic source");
                rule.port = source.port();
                if (phase != 0) check(view.set_filter(filter) == NEOASTRA_OK, "block worker script endpoint");
                if (phase == 2) check(view.set_filter(empty) == NEOASTRA_OK, "clear script endpoint");
                const auto before = source.snapshot();
                const auto scripts = source.worker_scripts();
                const auto result = connect_worker(view, kind, source.origin(), "bootstrap-" + std::to_string(phase), false);
                const auto value = since(before, source.snapshot());
                const auto script_requests = source.worker_scripts() - scripts;
                std::printf("WORKER_BOOTSTRAP kind=%s phase=%d reply=%s scripts=%u requests=%u bytes=%llu\n",
                    kind.c_str(), phase, result.c_str(), script_requests, value.requests, value.bytes);
                if (phase != 1 && (!result.starts_with("ready:") || script_requests == 0)) {
                    ++missing_controls;
                    std::printf("NOT_EXERCISED kind=%s bootstrap phase=%d: missing fresh-script control\n", kind.c_str(), phase);
                }
                cleanup(view);
            }
        }
        if (kind == "service") {
            // Only this probe's fresh loopback origin and isolated session.
            probe_view cleanup_view(app, env);
            check(cleanup_view.navigate(source.origin()), "registration cleanup source");
            const auto result = cleanup_view.run(
                "if(!navigator.serviceWorker)return 'api-unavailable';"
                "for(const r of await navigator.serviceWorker.getRegistrations())await r.unregister();"
                "return String((await navigator.serviceWorker.getRegistrations()).length);");
            check(result == "0" || result == "api-unavailable", "no retained test service-worker registrations");
            std::printf("service registration cleanup=%s\n", result.c_str());
        }
    }
    std::printf("Worker diagnostics complete: missing-controls=%u affected-controls=%u; no capability granted\n",
        missing_controls, affected_controls);
}
// Explicit opt-in profiles: each ephemeral profile owns its own web context and
// network session, including cookies and workers. Keep the shared-session
// diagnostics above as the negative comparison.
void isolated_worker_profiles(neoastra_app_t* app, neoastra_environment_t* env, bool separate_environments) {
    unsigned failures{}, missing{}, checks{};
    std::unique_ptr<neoastra_environment_t, decltype(&neoastra_environment_release)> other_environment(nullptr, neoastra_environment_release);
    if (separate_environments) {
        neoastra_environment_options_t options{};
        options.size = sizeof(options); options.version = 1; options.private_mode = 1;
        completion created;
        check(neoastra_environment_create_async(app, &options, environment_created, &created, nullptr, nullptr) == NEOASTRA_OK,
            "second private environment start");
        wait_for([&] { return created.done; }, "second private environment completion");
        check(created.result == NEOASTRA_OK && created.value, "second private environment result");
        other_environment.reset(static_cast<neoastra_environment_t*>(created.value));
    }
    auto* peer_environment = other_environment ? other_environment.get() : env;
    const auto make_profile = [&](neoastra_environment_t* owner) {
        neoastra_profile_options_t options{};
        options.size = sizeof(options); options.version = 1; options.ephemeral = 1;
        completion created;
        check(neoastra_environment_create_profile_async(owner, &options, profile_created, &created, nullptr, nullptr) == NEOASTRA_OK,
            "isolated profile start");
        wait_for([&] { return created.done; }, "isolated profile completion");
        check(created.result == NEOASTRA_OK && created.value, "isolated profile result");
        return std::unique_ptr<neoastra_profile_t, decltype(&neoastra_profile_release)>(
            static_cast<neoastra_profile_t*>(created.value), neoastra_profile_release);
    };
    // Views of one ephemeral profile share that profile's context and session; a
    // non-ephemeral profile keeps the environment's, so ordinary profile sharing
    // is unchanged.
    {
        auto shared = make_profile(env);
        probe_view first(app, env, nullptr, nullptr, shared.get());
        probe_view second(app, env, nullptr, nullptr, shared.get());
        check(webkit_web_view_get_context(first.web) == webkit_web_view_get_context(second.web) &&
            webkit_web_view_get_network_session(first.web) == webkit_web_view_get_network_session(second.web),
            "views of one ephemeral profile share its context and session");
        neoastra_profile_options_t persistent_options{};
        persistent_options.size = sizeof(persistent_options); persistent_options.version = 1; persistent_options.ephemeral = 0;
        completion created;
        check(neoastra_environment_create_profile_async(env, &persistent_options, profile_created, &created, nullptr, nullptr) == NEOASTRA_OK,
            "persistent profile start");
        wait_for([&] { return created.done; }, "persistent profile completion");
        check(created.result == NEOASTRA_OK && created.value, "persistent profile result");
        std::unique_ptr<neoastra_profile_t, decltype(&neoastra_profile_release)> persistent(
            static_cast<neoastra_profile_t*>(created.value), neoastra_profile_release);
        probe_view plain(app, env);
        probe_view with_persistent(app, env, nullptr, nullptr, persistent.get());
        check(webkit_web_view_get_context(with_persistent.web) == webkit_web_view_get_context(plain.web) &&
            webkit_web_view_get_network_session(with_persistent.web) == webkit_web_view_get_network_session(plain.web),
            "non-ephemeral profile keeps the environment context and session");
        check(webkit_web_view_get_context(first.web) != webkit_web_view_get_context(plain.web),
            "ephemeral profile context differs from the environment context");
    }
    for (const std::string kind : {"shared", "service"}) {
        for (const bool initially_filtered : {false, true}) {
            loopback_server target, source(target.origin());
            neoastra_request_rule_t rule{};
            rule.size = sizeof(rule); rule.version = 1; rule.port = target.port();
            rule.schemes = NEOASTRA_REQUEST_HTTP | NEOASTRA_REQUEST_HTTPS; rule.kinds = NEOASTRA_REQUEST_KINDS_ALL;
            neoastra_request_filter_t filter{};
            filter.size = sizeof(filter); filter.version = 1; filter.rules = &rule; filter.rule_count = 1;
            neoastra_request_filter_t empty{};
            empty.size = sizeof(empty); empty.version = 1;
            auto a = make_profile(env), b = make_profile(peer_environment);
            auto filtered = std::make_unique<probe_view>(app, env, initially_filtered ? &filter : nullptr, nullptr, a.get());
            probe_view other(app, peer_environment, nullptr, nullptr, b.get());
            auto* first_session = webkit_web_view_get_network_session(filtered->web);
            auto* second_session = webkit_web_view_get_network_session(other.web);
            check(first_session != second_session && webkit_network_session_is_ephemeral(first_session) &&
                webkit_network_session_is_ephemeral(second_session), "distinct ephemeral sessions");
            check(webkit_web_view_get_context(filtered->web) != webkit_web_view_get_context(other.web),
                "distinct web contexts for distinct ephemeral profiles");
            check(filtered->navigate(source.origin()) && other.navigate(source.origin()), "isolated profile source pages");
            check(filtered->run("document.cookie='rf_profile=first;path=/';return document.cookie;") == "rf_profile=first",
                "first profile cookie positive control");
            check(other.run("return document.cookie;").empty(), "second profile does not share cookie");
            std::printf("ISOLATED_PROFILE kind=%s initial=%d distinct-sessions=1 cookies-separated=1 distinct-contexts=%d\n",
                kind.c_str(), initially_filtered,
                webkit_web_view_get_context(filtered->web) != webkit_web_view_get_context(other.web) ? 1 : 0);

            const auto token = initially_filtered ? "isolated-initial" : "isolated-update";
            const auto first = connect_worker(*filtered, kind, source.origin(), token, false);
            if (kind == "service") {
                const auto lookup = other.run("return (await navigator.serviceWorker.getRegistration('" +
                    source.origin() + "/worker-scope-" + token + "/'))?'shared-registration':'absent';");
                check(lookup == "absent", "second profile has no first-profile service registration");
            }
            // Identical script URL/name/scope, but independently registered in
            // the other session. This must not attach to the first worker.
            const auto second = connect_worker(other, kind, source.origin(), token, false);
            std::printf("ISOLATED_WORKER kind=%s initial=%d first=%s second=%s\n",
                kind.c_str(), initially_filtered, first.c_str(), second.c_str());
            if (!first.starts_with("ready:") || !second.starts_with("ready:")) {
                ++missing;
                std::printf("NOT_EXERCISED kind=%s initial=%d missing worker bootstrap\n", kind.c_str(), initially_filtered);
                filtered->run("return await window.__rfClose();");
                other.run("return await window.__rfClose();");
                continue;
            }
            if (kind == "shared") check(first != second, "distinct shared-worker instances across profiles");
            const auto measure = [&](probe_view& view, const char* stage, bool blocked) {
                const auto before = target.snapshot();
                const auto result = view.run("return await window.__rfRequest('fetch','" + std::string(stage) + "');");
                const auto until = g_get_monotonic_time() + G_USEC_PER_SEC;
                wait_for([&] { return g_get_monotonic_time() >= until; }, "isolated worker observation");
                const auto value = since(before, target.snapshot());
                const bool pass = blocked ? result == "error" && no_request(value) :
                    result == "load" && value.requests > 0 && value.bytes > 0;
                ++checks;
                if (!pass) ++failures;
                std::printf("ISOLATED_FETCH kind=%s initial=%d stage=%s expected=%s result=%s requests=%u bytes=%llu verdict=%s\n",
                    kind.c_str(), initially_filtered, stage, blocked ? "blocked" : "allowed", result.c_str(),
                    value.requests, value.bytes, pass ? "PASS" : "FAIL");
            };
            measure(other, "other-before", false);
            if (!initially_filtered) measure(*filtered, "owner-before", false);
            check(filtered->set_filter(filter) == NEOASTRA_OK, "isolated filter install/update");
            measure(*filtered, "owner-filtered", true);
            measure(other, "other-after-owner-filter", false);
            fetch(*filtered, target, true);
            fetch(other, target, false);
            check(filtered->set_filter(empty) == NEOASTRA_OK, "isolated clear");
            measure(*filtered, "owner-cleared", false);
            measure(other, "other-after-owner-clear", false);
            check(filtered->set_filter(filter) == NEOASTRA_OK, "isolated restore");
            measure(*filtered, "owner-restored", true);
            // Close/unregister only this profile's worker, then dispose its
            // view. A same-named worker in the other profile must keep working.
            std::printf("ISOLATED_CLEANUP owner=%s\n", filtered->run("return await window.__rfClose();").c_str());
            filtered.reset();
            measure(other, "other-after-owner-disposal", false);
            fetch(other, target, false);
            std::printf("ISOLATED_CLEANUP other=%s\n", other.run("return await window.__rfClose();").c_str());
        }
    }
    std::printf("ISOLATED_SUMMARY checks=%u failures=%u missing=%u; explicit-profile HTTP RPC probes with per-profile web contexts\n",
        checks, failures, missing);
    check(failures == 0 && missing == 0, "isolated worker profile checks complete and passing");
}
} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    check(argc == 2, "mode argument");
    const std::string mode(argv[1]);
    check(mode == "--engine" || mode == "--api" || mode == "--compile-errors" || mode == "--matrix" ||
        mode == "--addressing" || mode == "--links" || mode == "--scope-diagnostics" ||
        mode == "--workers-private" || mode == "--workers-persistent" || mode == "--workers-isolated-profiles" ||
        mode == "--workers-isolated-environments", "mode");
    std::string directory = "/tmp/neoastra-gtk-filter-probe-XXXXXX";
    check(mkdtemp(directory.data()), "temporary directory");
    const std::filesystem::path root(directory);
    std::filesystem::create_directory(root / "tmp");
    g_setenv("TMPDIR", (root / "tmp").c_str(), TRUE);
    check(std::filesystem::path(g_get_tmp_dir()) == root / "tmp", "private GLib temporary directory");
    g_setenv("XDG_DATA_HOME", (root / "data").c_str(), TRUE);
    g_setenv("XDG_CACHE_HOME", (root / "cache").c_str(), TRUE);
    std::printf("GTK %u.%u.%u; WebKitGTK %u.%u.%u\n", gtk_get_major_version(), gtk_get_minor_version(),
        gtk_get_micro_version(), webkit_get_major_version(), webkit_get_minor_version(), webkit_get_micro_version());
    std::fflush(stdout);
    neoastra_app_options_t options{};
    options.size = sizeof(options);
    options.version = 1;
    options.shutdown_mode = NEOASTRA_APP_SHUTDOWN_EXPLICIT;
    neoastra_app_t* app{};
    check(neoastra_app_attach(&options, &app, nullptr) == NEOASTRA_OK, "attach app");
    neoastra_environment_options_t settings{};
    settings.size = sizeof(settings);
    settings.version = 1;
    settings.private_mode = mode == "--workers-persistent" ? 0 : 1;
    const auto worker_root = (root / "worker-profile").string();
    if (!settings.private_mode) settings.user_data_root = text(worker_root); // Never use a default/user profile.
    completion created;
    check(neoastra_environment_create_async(app, &settings, environment_created, &created, nullptr, nullptr) == NEOASTRA_OK, "environment start");
    wait_for([&] { return created.done; }, "environment completion");
    check(created.result == NEOASTRA_OK && created.value, "environment result");
    auto* env = static_cast<neoastra_environment_t*>(created.value);
    if (mode == "--engine") engine_probe(app, env, root);
    else if (mode == "--compile-errors") compile_errors(app, env);
    else if (mode == "--matrix") request_matrix(app, env);
    else if (mode == "--addressing") addressing_matrix(app, env);
    else if (mode == "--links") link_probes(app, env);
    else if (mode == "--scope-diagnostics") scope_diagnostics(app, env);
    else if (mode == "--workers-private" || mode == "--workers-persistent")
        worker_diagnostics(app, env, mode == "--workers-persistent");
    else if (mode == "--workers-isolated-profiles" || mode == "--workers-isolated-environments")
        isolated_worker_profiles(app, env, mode == "--workers-isolated-environments");
    else {
        api_probe(app, env);
        disposal_traffic(app, env);
    }
    neoastra_environment_release(env);
    check(neoastra_app_detach(app, nullptr) == NEOASTRA_OK, "detach app");
    neoastra_app_release(app);
    std::error_code error;
    std::filesystem::remove_all(root, error);
    check(!error, "remove probe data");
    std::puts(mode == "--scope-diagnostics" || mode == "--links" ||
        mode == "--workers-private" || mode == "--workers-persistent" ?
        "GTK request-filter diagnostic run complete (not full acceptance)" : "GTK request-filter probe PASS");
}
