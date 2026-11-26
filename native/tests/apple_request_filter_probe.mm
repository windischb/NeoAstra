// Engine probes and public request-filter ABI tests. Diagnostic --gates samples
// more classes than the backend currently advertises; it never enables flags.
// Uses real NeoAstra views and public WKContentRuleList APIs. Counts accepted TCP
// connections separately from request bytes: a JavaScript rejection is not proof
// that a request was stopped before reaching the server.
#include "neoastra.h"
#include "request_filter.hpp"

#import <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#else
#import <Cocoa/Cocoa.h>
#endif
#import <WebKit/WebKit.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <set>
#include <thread>
#include <vector>

namespace {

#if TARGET_OS_IPHONE
UIView* probe_root;
#endif

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "Probe failure: %s\n", message);
        std::abort();
    }
}

void pump() {
    [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
}

template<typename Predicate>
void wait_for(Predicate predicate, const char* what) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!predicate()) {
        check(std::chrono::steady_clock::now() < deadline, what);
        pump();
    }
}

struct counts {
    unsigned connections{};
    unsigned requests{};
    unsigned upgrades{};
    unsigned long long bytes{};
};

bool no_request(counts value) {
    // These listeners speak plaintext HTTP: every received byte is application
    // data. A TCP accept alone is diagnostic, not a failure of the request contract.
    return value.bytes == 0 && value.requests == 0 && value.upgrades == 0;
}

counts since(counts before, counts after) {
    return {after.connections - before.connections, after.requests - before.requests,
        after.upgrades - before.upgrades, after.bytes - before.bytes};
}

// Unlike the managed navigation fixture, this probe must also see a preconnect
// which sends no HTTP headers. Nonblocking clients keep it from hiding later requests.
class loopback_server final {
public:
    explicit loopback_server(std::string redirect = {}) : _redirect(std::move(redirect)) {
        _listener = socket(AF_INET, SOCK_STREAM, 0);
        check(_listener >= 0, "socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(bind(_listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind loopback");
        socklen_t size = sizeof(address);
        check(getsockname(_listener, reinterpret_cast<sockaddr*>(&address), &size) == 0, "getsockname");
        _port = ntohs(address.sin_port);
        check(listen(_listener, 16) == 0, "listen");
        check(fcntl(_listener, F_SETFL, O_NONBLOCK) == 0, "nonblocking listener");
        _thread = std::thread([this] { serve(); });
    }

    ~loopback_server() {
        _stopping.store(true);
        _thread.join();
        close(_listener);
    }
    loopback_server(const loopback_server&) = delete;
    loopback_server& operator=(const loopback_server&) = delete;

    uint16_t port() const { return _port; }
    std::string origin(const char* host = "127.0.0.1") const {
        return "http://" + std::string(host) + ":" + std::to_string(_port);
    }
    counts snapshot() {
        // Drain sockets already pending at the server before inspecting a negative
        // result. This is a server-observation barrier, not a WebKit update barrier.
        const auto sample = ++_samples;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (_sampled.load() < sample) {
            check(std::chrono::steady_clock::now() < deadline, "server observation");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return {_connections.load(), _requests.load(), _upgrades.load(), _bytes.load()};
    }

private:
    struct client { int fd; std::string data; };
    int _listener{-1};
    uint16_t _port{};
    const std::string _redirect;
    std::atomic<bool> _stopping{};
    std::atomic<unsigned> _connections{}, _requests{}, _upgrades{};
    std::atomic<unsigned long long> _bytes{};
    std::atomic<unsigned> _samples{}, _sampled{};
    std::thread _thread;

    void serve() {
        std::vector<client> clients;
        while (!_stopping.load()) {
            const auto sample = _samples.load();
            pollfd listener{_listener, POLLIN, 0};
            const int ready = poll(&listener, 1, 10);
            check(ready >= 0 || errno == EINTR, "poll listener");
            while (ready > 0) {
                const int fd = accept(_listener, nullptr, nullptr);
                if (fd >= 0) {
                    ++_connections;
                    int yes = 1;
                    check(setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) == 0, "SO_NOSIGPIPE");
                    timeval timeout{1, 0};
                    check(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0, "send timeout");
                    clients.push_back({fd, {}});
                } else {
                    check(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR, "accept");
                    break;
                }
            }
            for (auto it = clients.begin(); it != clients.end();) {
                char buffer[8192];
                const auto length = recv(it->fd, buffer, sizeof(buffer), MSG_DONTWAIT);
                if (length > 0) {
                    _bytes.fetch_add(static_cast<unsigned long long>(length));
                    it->data.append(buffer, static_cast<size_t>(length));
                }
                const bool complete = it->data.find("\r\n\r\n") != std::string::npos;
                if (complete) {
                    ++_requests;
                    auto lower = it->data;
                    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
                        return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : static_cast<char>(c);
                    });
                    const bool upgrade = lower.find("\r\nupgrade: websocket\r\n") != std::string::npos;
                    if (upgrade) ++_upgrades;
                    const bool redirect = !_redirect.empty() && it->data.starts_with("GET /redirect");
                    const bool frame = !_redirect.empty() && it->data.starts_with("GET /frame");
                    const std::string body = "<!doctype html><meta charset=utf-8><title>Request filter probe</title><body>Probe</body>" +
                        (frame ? "<script>fetch('" + _redirect + "/frame-fetch').then(()=>{window.probeFetchResult='load';parent.postMessage('load','*')},()=>{window.probeFetchResult='error';parent.postMessage('error','*')});</script>" : "");
                    const std::string response =
                        (redirect ? "HTTP/1.1 302 Found\r\nLocation: " + _redirect + "/target\r\n" :
                            upgrade ? "HTTP/1.1 400 Bad Request\r\n" : "HTTP/1.1 200 OK\r\n") +
                        std::string("Content-Type: text/html\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: ") +
                        std::to_string(body.size()) + "\r\n\r\n" + body;
                    size_t sent = 0;
                    while (sent < response.size()) {
                        const auto n = send(it->fd, response.data() + sent, response.size() - sent, 0);
                        if (n <= 0) break; // The engine may cancel an image/script whose response type is unsuitable.
                        sent += static_cast<size_t>(n);
                    }
                }
                if (complete || length == 0 || it->data.size() > 32768 ||
                    (length < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    close(it->fd);
                    it = clients.erase(it);
                } else ++it;
            }
            _sampled.store(sample);
        }
        for (const auto& client : clients) close(client.fd);
    }
};

struct completion {
    bool done{};
    neoastra_result_t result{NEOASTRA_ERROR_UNKNOWN};
    void* value{};
};

void NEOASTRA_CALL environment_created(void* context, neoastra_result_t result, neoastra_environment_t* value, const neoastra_error_t*) {
    *static_cast<completion*>(context) = {true, result, value};
}

void NEOASTRA_CALL view_created(void* context, neoastra_result_t result, neoastra_view_t* value, const neoastra_error_t*) {
    *static_cast<completion*>(context) = {true, result, value};
}

void NEOASTRA_CALL operation_completed(void* context, neoastra_result_t result, const neoastra_error_t*) {
    *static_cast<completion*>(context) = {true, result, nullptr};
}

struct navigation {
    bool finished{}, failed{}, refuse{}, refused{};
    unsigned completed{};
    std::vector<std::string> requested;
    std::function<void(neoastra_decision_t*)> popup;
};

void NEOASTRA_CALL view_event(void* context, const neoastra_event_t* event) {
    auto& state = *static_cast<navigation*>(context);
    if (event->header.type == NEOASTRA_EVENT_NAVIGATION_REQUESTED && event->uri.data)
        state.requested.emplace_back(reinterpret_cast<const char*>(event->uri.data), static_cast<size_t>(event->uri.length));
    if (event->header.type == NEOASTRA_EVENT_NAVIGATION_COMPLETED) { state.finished = true; ++state.completed; }
    if (event->header.type == NEOASTRA_EVENT_NAVIGATION_FAILED) state.failed = true;
    if (event->header.type == NEOASTRA_EVENT_NAVIGATION_REQUESTED && state.refuse) {
        neoastra_decision_response_t response{};
        response.size = sizeof(response);
        response.version = 1;
        response.action = NEOASTRA_DECISION_CANCEL;
        check(neoastra_decision_complete(event->decision, &response, nullptr) == NEOASTRA_OK, "cancel navigation");
        state.refused = true;
    }
    if (event->header.type == NEOASTRA_EVENT_NEW_WINDOW_REQUESTED && state.popup) state.popup(event->decision);
    check(event->header.type != NEOASTRA_EVENT_WEB_PROCESS_TERMINATED, "WebContent process terminated");
}

class probe_view final {
public:
    explicit probe_view(neoastra_environment_t* environment, const neoastra_request_filter_t* initial = nullptr,
        neoastra_decision_t* popup = nullptr) {
#if TARGET_OS_IPHONE
        _parent = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 640, 480)];
        [probe_root addSubview:_parent];
#else
        _parent = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 640, 480)];
#endif
        neoastra_view_options_t options{};
        options.size = sizeof(options);
        options.version = initial ? 2 : 1;
        options.request_filter = initial;
        options.popup_request = popup;
        options.parent.size = sizeof(options.parent);
        options.parent.version = 1;
#if TARGET_OS_IPHONE
        options.parent.kind = NEOASTRA_NATIVE_PARENT_UIKIT_UIVIEW;
#else
        options.parent.kind = NEOASTRA_NATIVE_PARENT_COCOA_NSVIEW;
#endif
        options.parent.handle = (__bridge void*)_parent;
        options.bounds = {0, 0, 640, 480};
        options.bridge_policy = NEOASTRA_BRIDGE_DISABLED;
        completion result;
        check(neoastra_environment_create_view_async(environment, &options, view_created, &result, nullptr, nullptr) == NEOASTRA_OK, "create view");
        if (popup) check(result.done, "popup creation must finish synchronously");
        wait_for([&] { return result.done; }, "create view completion");
        check(result.result == NEOASTRA_OK && result.value, "view creation result");
        _view = static_cast<neoastra_view_t*>(result.value);
        neoastra_native_handle_t handle{};
        handle.size = sizeof(handle);
        handle.version = 1;
        check(neoastra_view_get_native_handle(_view, NEOASTRA_NATIVE_HANDLE_WKWEBVIEW, &handle) == NEOASTRA_OK, "WKWebView handle");
        web = (__bridge WKWebView*)handle.value;
        check(neoastra_view_set_event_callback(_view, view_event, &_navigation) == NEOASTRA_OK, "event callback");
    }
    ~probe_view() {
        neoastra_view_set_event_callback(_view, nullptr, nullptr);
        neoastra_view_release(_view);
        [_parent removeFromSuperview];
    }
    probe_view(const probe_view&) = delete;
    probe_view& operator=(const probe_view&) = delete;

    WKWebView* web{};
    neoastra_view_t* handle() const { return _view; }
    unsigned completed_navigations() const { return _navigation.completed; }
    void on_popup(std::function<void(neoastra_decision_t*)> callback) { _navigation.popup = std::move(callback); }

    neoastra_result_t set_filter(const neoastra_request_filter_t& filter, bool cancel = false) {
        completion state;
        neoastra_operation_t* operation{};
        neoastra_error_t* error{};
        const auto started = neoastra_view_set_request_filter_async(_view, &filter, operation_completed, &state, &operation, &error);
        if (error) neoastra_error_release(error);
        if (started != NEOASTRA_OK) { check(operation == nullptr, "rejected filter has no operation"); return started; }
        if (cancel) neoastra_operation_cancel(operation);
        wait_for([&] { return state.done; }, "request filter completion");
        neoastra_operation_release(operation);
        return state.result;
    }

    bool was_requested(const std::string& uri) const {
        return std::find(_navigation.requested.begin(), _navigation.requested.end(), uri) != _navigation.requested.end();
    }

    bool navigate(const std::string& uri, bool refuse = false) {
        _navigation = {};
        _navigation.refuse = refuse;
        neoastra_string_view_t raw{reinterpret_cast<const uint8_t*>(uri.data()), uri.size()};
        check(neoastra_view_navigate(_view, raw, nullptr) == NEOASTRA_OK, "navigate");
        const auto message = "navigation completion: " + uri;
        wait_for([&] { return _navigation.finished || _navigation.failed || _navigation.refused; }, message.c_str());
        if (refuse) check(_navigation.refused, "native navigation guard was invoked");
        return _navigation.finished;
    }

    bool navigate_from_page(const std::string& uri, bool refuse) {
        _navigation = {};
        _navigation.refuse = refuse;
        run(@"window.location.assign(url); return 'started';", @{@"url": @(uri.c_str())});
        wait_for([&] { return _navigation.finished || _navigation.failed || _navigation.refused; }, "page navigation completion");
        if (refuse) check(_navigation.refused, "native page-navigation guard was invoked");
        return _navigation.finished;
    }

    NSString* run(NSString* script, NSDictionary<NSString*, id>* arguments = @{}) {
        __block bool finished = false;
        __block id value = nil;
        __block NSError* failure = nil;
        [web callAsyncJavaScript:script arguments:arguments inFrame:nil inContentWorld:WKContentWorld.pageWorld completionHandler:^(id result, NSError* error) {
            value = result;
            failure = error;
            finished = true;
        }];
        wait_for(^bool { return finished; }, "JavaScript completion");
        if (failure) std::fprintf(stderr, "JavaScript: %s\n", failure.description.UTF8String);
        check(failure == nil, "JavaScript execution");
        return [value isKindOfClass:NSString.class] ? value : [value description];
    }

private:
#if TARGET_OS_IPHONE
    UIView* _parent{};
#else
    NSView* _parent{};
#endif
    neoastra_view_t* _view{};
    navigation _navigation;
};

WKContentRuleList* compile(WKContentRuleListStore* store, const neo_request_filter::rule& rule) {
    const auto json = neo_request_filter::filter(std::span<const neo_request_filter::rule>(&rule, 1)).webkit_json();
    NSString* source = [[NSString alloc] initWithBytes:json.data() length:json.size() encoding:NSUTF8StringEncoding];
    __block bool finished = false;
    __block WKContentRuleList* compiled = nil;
    __block NSError* failure = nil;
    [store compileContentRuleListForIdentifier:NSUUID.UUID.UUIDString
        encodedContentRuleList:source
        completionHandler:^(WKContentRuleList* result, NSError* error) {
            compiled = result;
            failure = error;
            finished = true;
        }];
    wait_for(^bool { return finished; }, "compile content rules");
    if (failure) std::fprintf(stderr, "Rules: %s\n", failure.description.UTF8String);
    check(compiled != nil && failure == nil, "compile rules result");
    return compiled;
}

WKContentRuleList* compile(WKContentRuleListStore* store, uint16_t port) {
    neo_request_filter::rule rule;
    rule.port = port;
    rule.schemes = neo_request_filter::all_schemes;
    return compile(store, rule);
}

// Completion means load/error or expiry of the observation window, not that the
// network was blocked. A frame with no completion needs separate native evidence
// of its navigation attempt and a positive control, not just a JavaScript timeout.
NSString* attempt(probe_view& view, const std::string& uri, NSString* kind) {
    return view.run(@R"JS(
        const pending = (async () => {
            if (kind === 'fetch' || kind === 'redirect') {
                try { await fetch(url, {cache:'no-store'}); return 'load'; } catch { return 'error'; }
            }
            if (kind === 'xhr') return await new Promise(resolve => {
                const x = new XMLHttpRequest(); x.onload = () => resolve('load');
                x.onerror = () => resolve('error'); x.open('GET', url); x.send();
            });
            if (kind === 'websocket') return await new Promise(resolve => {
                const s = new WebSocket(url.replace(/^http/, 'ws'));
                s.onopen = () => { s.close(); resolve('open'); }; s.onerror = () => resolve('error');
            });
            if (kind === 'worker') return await new Promise(resolve => {
                const source = 'fetch(' + JSON.stringify(url) + ").then(()=>postMessage('load'),()=>postMessage('error'));";
                const blob = URL.createObjectURL(new Blob([source], {type:'text/javascript'}));
                const worker = new Worker(blob);
                worker.onmessage = event => { worker.terminate(); URL.revokeObjectURL(blob); resolve(event.data); };
                worker.onerror = () => { worker.terminate(); URL.revokeObjectURL(blob); resolve('worker-error'); };
            });
            if (kind === 'same-frame-fetch' || kind === 'cross-frame-fetch') return await new Promise(resolve => {
                const frame = document.createElement('iframe');
                const listener = event => {
                    if (event.source !== frame.contentWindow) return;
                    window.removeEventListener('message', listener); resolve(event.data);
                };
                window.addEventListener('message', listener); frame.src = url; document.body.append(frame);
            });
            return await new Promise(resolve => {
                const e = document.createElement(kind);
                e.onload = () => resolve('load'); e.onerror = () => resolve('error');
                e.src = url; document.body.append(e);
            });
        })();
        return await Promise.race([pending, new Promise(resolve => setTimeout(() => resolve('timeout'), 5000))]);
    )JS", @{@"url": @(uri.c_str()), @"kind": kind});
}

void report(const char* name, NSString* outcome, counts value) {
    std::printf("%s outcome=%s connections=%u requests=%u upgrades=%u bytes=%llu\n",
        name, outcome.UTF8String, value.connections, value.requests, value.upgrades, value.bytes);
    std::fflush(stdout);
}

bool request_case(neoastra_environment_t* environment, WKContentRuleListStore* store, NSString* kind, bool filtered) {
    loopback_server target;
    loopback_server source(target.origin());
    probe_view view(environment);
    if (filtered) [view.web.configuration.userContentController addContentRuleList:compile(store, target.port())];
    if ([kind isEqualToString:@"document"] || [kind isEqualToString:@"guarded-document"]) {
        NSString* outcome = view.navigate(target.origin() + "/first-document", filtered && [kind isEqualToString:@"guarded-document"]) ? @"load" : @"error";
        const auto value = target.snapshot();
        report((std::string(filtered ? "filtered-" : "control-") + kind.UTF8String).c_str(), outcome, value);
        if (!filtered) check(value.connections > 0 && value.requests > 0, "positive initial navigation control");
        return no_request(value);
    }
    check(view.navigate(source.origin() + "/page"), "source navigation");
    if ([kind isEqualToString:@"page-document"] || [kind isEqualToString:@"guarded-page-document"]) {
        NSString* outcome = view.navigate_from_page(target.origin() + "/page-navigation", filtered && [kind isEqualToString:@"guarded-page-document"]) ? @"load" : @"error";
        const auto value = target.snapshot();
        report((std::string(filtered ? "filtered-" : "control-") + kind.UTF8String).c_str(), outcome, value);
        if (!filtered) check(value.connections > 0 && value.requests > 0, "positive page navigation control");
        return no_request(value);
    }
    const auto uri = [kind isEqualToString:@"redirect"] ? source.origin() + "/redirect" :
        [kind isEqualToString:@"same-frame-fetch"] ? source.origin() + "/frame" :
        [kind isEqualToString:@"cross-frame-fetch"] ? source.origin("localhost") + "/frame" : target.origin() + "/target";
    NSString* outcome = attempt(view, uri, kind);
    const auto value = target.snapshot();
    report((std::string(filtered ? "filtered-" : "control-") + kind.UTF8String).c_str(), outcome, value);
    if ([outcome isEqualToString:@"timeout"]) {
        check(filtered, "positive request control timed out");
        if ([kind isEqualToString:@"iframe"] && view.was_requested(uri) && no_request(value)) {
            std::puts("Frame navigation observed natively; no application bytes during the five-second observation window.");
            return true;
        }
        std::printf("INCONCLUSIVE: filtered %s emitted no load/error; zero counters alone do not pass this gate.\n", kind.UTF8String);
        return false;
    }
    if (!filtered) check(value.connections > 0 && value.requests > 0 && value.bytes > 0, "positive network control");
    check(![outcome isEqualToString:@"worker-error"], "worker bootstrap failed");
    if (!filtered && [kind isEqualToString:@"websocket"]) check(value.upgrades > 0, "positive WebSocket upgrade control");
    return no_request(value);
}

bool observe_effect(probe_view& view, loopback_server& target, bool blocked) {
    // Observe propagation in the test only. There is no production sleep, probe,
    // or claim that one successful sample acknowledges every engine process.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    unsigned attempts = 0;
    do {
        const auto before = target.snapshot();
        NSString* outcome = attempt(view, target.origin() + (blocked ? "/after-add-" : "/after-remove-") + std::to_string(++attempts), @"fetch");
        const auto value = since(before, target.snapshot());
        report(blocked ? "after-add-sample" : "after-remove-sample", outcome, value);
        check(![outcome isEqualToString:@"timeout"], "update probe timed out");
        if (blocked ? ([outcome isEqualToString:@"error"] && no_request(value))
                    : ([outcome isEqualToString:@"load"] && value.requests > 0)) {
            std::printf("Observed %s after %u sample(s); this is not an update-barrier proof.\n", blocked ? "blocking" : "removal", attempts);
            return true;
        }
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

bool update_case(neoastra_environment_t* environment, WKContentRuleListStore* store) {
    loopback_server target;
    loopback_server source;
    probe_view view(environment);
    check(view.navigate(source.origin() + "/page"), "update source navigation");
    WKContentRuleList* rules = compile(store, target.port());
    [view.web.configuration.userContentController addContentRuleList:rules];
    const bool blocked = observe_effect(view, target, true);
    [view.web.configuration.userContentController removeContentRuleList:rules];
    const bool removed = observe_effect(view, target, false);
    return blocked && removed;
}

std::set<std::string> filter_folders() {
    std::set<std::string> result;
    NSError* error = nil;
    NSArray<NSString*>* names = [NSFileManager.defaultManager contentsOfDirectoryAtPath:NSTemporaryDirectory() error:&error];
    check(names != nil && error == nil, "read temporary filter stores");
    NSString* prefix = [NSString stringWithFormat:@"neoastra-request-filter-%d-", NSProcessInfo.processInfo.processIdentifier];
    for (NSString* name in names) if ([name hasPrefix:prefix]) result.emplace(name.UTF8String);
    return result;
}

void api_cases(neoastra_environment_t* environment) {
    const auto before = filter_folders();
    neoastra_capability_info_t capability{};
    capability.size = sizeof(capability); capability.version = 1;
    check(neoastra_environment_get_capability(environment, NEOASTRA_CAPABILITY_REQUEST_FILTER, &capability) == NEOASTRA_OK, "filter capability");
    const uint64_t base = NEOASTRA_REQUEST_FILTER_SUBRESOURCES | NEOASTRA_REQUEST_FILTER_FRAMES | NEOASTRA_REQUEST_FILTER_PER_VIEW;
    check(capability.support == NEOASTRA_SUPPORT_LIMITED && capability.flags == base, "honest base coverage");
    @autoreleasepool {
        loopback_server target;
        loopback_server source(target.origin());
        neoastra_request_rule_t rule{};
        rule.size = sizeof(rule); rule.version = 1; rule.port = target.port();
        rule.schemes = NEOASTRA_REQUEST_HTTP | NEOASTRA_REQUEST_HTTPS;
        rule.kinds = NEOASTRA_REQUEST_KINDS_ALL;
        neoastra_request_filter_t filter{};
        filter.size = sizeof(filter); filter.version = 1; filter.rules = &rule; filter.rule_count = 1;
        {
            std::unique_ptr<probe_view> popup;
            probe_view filtered(environment, &filter), unfiltered(environment);
            check(!filtered.navigate(target.origin() + "/api-first"), "initial filter holds before first document");
            report("api-initial", @"refused", target.snapshot());
            check(no_request(target.snapshot()), "initial filter sends no application bytes");
            check(filtered.navigate(source.origin() + "/page"), "API source navigation");
            check(unfiltered.navigate(source.origin() + "/page"), "API unfiltered source");

            filter.required_flags = NEOASTRA_REQUEST_FILTER_WEBSOCKETS;
            check(filtered.set_filter(filter) == NEOASTRA_ERROR_NOT_SUPPORTED, "explicit unsupported coverage is rejected");
            filter.required_flags = 0;
            rule.port = source.port();
            check(filtered.set_filter(filter, true) == NEOASTRA_ERROR_CANCELED, "cancellation before installation");
            check(observe_effect(filtered, target, true), "failed/canceled updates preserve previous filter");
            check([attempt(filtered, source.origin() + "/still-allowed", @"fetch") isEqualToString:@"load"], "canceled rule was never installed");

#if !TARGET_OS_IPHONE
            filtered.on_popup([&](neoastra_decision_t* decision) {
                popup = std::make_unique<probe_view>(environment, nullptr, decision);
                neoastra_decision_response_t answer{};
                answer.size = sizeof(answer); answer.version = 1;
                answer.action = NEOASTRA_DECISION_ALLOW; answer.target_view = popup->handle();
                check(neoastra_decision_complete(decision, &answer, nullptr) == NEOASTRA_OK, "accept tracked popup");
            });
            // Its allowed first document fetches the blocked port immediately,
            // before the host has an opportunity to install a filter afterwards.
            filtered.run(@"window.open(url); return 'requested';", @{@"url": @((source.origin() + "/frame").c_str())});
            wait_for([&] { return popup != nullptr; }, "tracked popup");
            check(popup->web.configuration.userContentController != filtered.web.configuration.userContentController, "independent popup controller");
            wait_for([&] { return [popup->run(@"return window.probeFetchResult || '';") isEqualToString:@"error"]; }, "popup's initial fetch refused");
            check(no_request(target.snapshot()), "popup inherits filter before its first script request");
#else
            // UIKit does not offer tracked popups. Its existing allow action
            // navigates the opener instead; blocked targets must not take that path.
            bool allowed = false;
            const auto allow_popup = [&](neoastra_decision_t* decision) {
                neoastra_decision_response_t answer{};
                answer.size = sizeof(answer); answer.version = 1;
                answer.action = NEOASTRA_DECISION_ALLOW;
                check(neoastra_decision_complete(decision, &answer, nullptr) == NEOASTRA_OK, "allow UIKit new-window decision");
                allowed = true;
            };
            const auto popup_uri = target.origin() + "/blocked-popup";
            const auto completed = unfiltered.completed_navigations();
            unfiltered.on_popup(allow_popup);
            unfiltered.run(@"window.open(url); return 'requested';", @{@"url": @(popup_uri.c_str())});
            wait_for([&] { return allowed && unfiltered.was_requested(popup_uri); }, "UIKit fallback positive control");
            wait_for([&] { return target.snapshot().requests > 0; }, "UIKit fallback reaches unfiltered target");
            wait_for([&] { return unfiltered.completed_navigations() > completed; }, "UIKit fallback document completed");
            check(unfiltered.navigate(source.origin() + "/page"), "restore UIKit control page");
            const auto before_popup = target.snapshot();
            allowed = false;
            filtered.on_popup(allow_popup);
            filtered.run(@"window.open(url); return 'requested';", @{@"url": @(popup_uri.c_str())});
            // An installed content rule can refuse before WKUIDelegate is
            // called. The successful unfiltered control proves the attempt path.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (std::chrono::steady_clock::now() < deadline) {
                pump();
                check(!filtered.was_requested(popup_uri), "blocked UIKit popup must not become an opener navigation");
            }
            check(target.snapshot().bytes == before_popup.bytes, "UIKit popup sends no new application bytes");

            // Also exercise a real host decision already outstanding when the
            // filter changes; WebKit did not block that earlier window.open.
            {
                probe_view pending_popup(environment);
                check(pending_popup.navigate(source.origin() + "/pending-popup"), "deferred popup source");
                neoastra_decision_t* delayed{};
                pending_popup.on_popup([&](neoastra_decision_t* decision) {
                    neoastra_decision_retain(decision);
                    check(neoastra_decision_defer(decision) == NEOASTRA_OK, "defer UIKit popup");
                    delayed = decision;
                });
                pending_popup.run(@"window.open(url); return 'requested';", @{@"url": @(popup_uri.c_str())});
                wait_for([&] { return delayed != nullptr; }, "deferred UIKit popup decision");
                rule.port = target.port();
                check(pending_popup.set_filter(filter) == NEOASTRA_OK, "install filter before deferred popup answer");
                const auto before_answer = target.snapshot();
                allow_popup(delayed);
                neoastra_decision_release(delayed);
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                while (std::chrono::steady_clock::now() < until) {
                    pump();
                    check(!pending_popup.was_requested(popup_uri), "new filter must prevent deferred popup fallback navigation");
                }
                check(target.snapshot().bytes == before_answer.bytes, "deferred popup sends no new application bytes");
            }
            std::puts("UIKit blocked-popup fallback case passed (tracked popups unsupported)");
#endif

            check([attempt(unfiltered, target.origin() + "/other-view", @"fetch") isEqualToString:@"load"], "second view remains unfiltered");
            check(unfiltered.navigate(source.origin() + "/frame"), "popup script's unfiltered document control");
            wait_for([&] { return [unfiltered.run(@"return window.probeFetchResult || '';") isEqualToString:@"load"]; }, "popup script's positive fetch control");
            neoastra_request_filter_t empty{};
            empty.size = sizeof(empty); empty.version = 1;
            if (popup) {
                check(popup->set_filter(empty) == NEOASTRA_OK, "clear popup filter");
                check(observe_effect(*popup, target, false), "popup filter is independently replaceable");
                check(observe_effect(filtered, target, true), "popup update leaves opener's filter intact");
            }
            loopback_server replacement_target;
            rule.port = replacement_target.port();
            check(filtered.set_filter(filter) == NEOASTRA_OK, "replace installed rules through the public API");
            check(observe_effect(filtered, target, false), "replacement eventually removes old target rule");
            check(observe_effect(filtered, replacement_target, true), "replacement eventually blocks new target");
            check(filtered.set_filter(empty) == NEOASTRA_OK, "clear installed filter");
            check(observe_effect(filtered, replacement_target, false), "clear eventually permits requests");
        }

        // Disposing the last view handle settles the accepted operation before a
        // late WebKit compile can touch its deleted platform state.
        completion result;
        neoastra_operation_t* operation{};
        auto pending = std::make_unique<probe_view>(environment);
        check(neoastra_view_set_request_filter_async(pending->handle(), &filter, operation_completed, &result, &operation, nullptr) == NEOASTRA_OK,
            "start disposal probe");
        pending.reset();
        wait_for([&] { return result.done; }, "disposed filter operation");
        check(result.result == NEOASTRA_ERROR_DISPOSED, "view disposal completes pending filter");
        neoastra_operation_release(operation);
    }
    wait_for([&] {
        const auto now = filter_folders();
        return std::all_of(now.begin(), now.end(), [&](const auto& name) { return before.contains(name); });
    }, "temporary rule stores released after late compiles");
    std::puts("Public request-filter ABI cases passed");
}

bool host_rule_cases(neoastra_environment_t* environment, WKContentRuleListStore* store) {
    using neo_request_filter::host_match;
    struct example { const char* spelling; host_match match; const char* host; };
    const example examples[] = {
        {"2130706433", host_match::loopback, ""},
        {"0x7f.1", host_match::loopback, ""},
        {"0177.0.0.1", host_match::loopback, ""},
        {"%31%32%37.0.0.1", host_match::loopback, ""},
        {"localhost", host_match::exact, "localhost"},
        {"127.0.0.1", host_match::exact, "127.0.0.1"},
        {"localhost", host_match::suffix, "localhost"},
    };
    bool passed = true;
    for (const auto& example : examples) for (const bool filtered : {false, true}) { @autoreleasepool {
        loopback_server target;
        loopback_server source;
        probe_view view(environment);
        neo_request_filter::rule rule;
        rule.match = example.match;
        rule.host = example.host;
        rule.port = target.port();
        const auto uri = target.origin(example.spelling) + "/host-rule";
        const neo_request_filter::filter matcher(std::span<const neo_request_filter::rule>(&rule, 1));
        check(matcher.refuses(uri, NEOASTRA_RESOURCE_FETCH), "common matcher agrees with host-rule vector");
        if (filtered) [view.web.configuration.userContentController addContentRuleList:compile(store, rule)];
        check(view.navigate(source.origin() + "/page"), "host-rule source navigation");
        NSString* outcome = attempt(view, uri, @"fetch");
        const auto value = target.snapshot();
        report((std::string(filtered ? "host-filtered-" : "host-control-") + example.spelling).c_str(), outcome, value);
        if (!filtered) check(value.requests > 0 && [outcome isEqualToString:@"load"], "host spelling positive control");
        else passed = ([outcome isEqualToString:@"error"] && no_request(value)) && passed;
    } }
    return passed;
}

void isolation_case(neoastra_environment_t* environment, WKContentRuleListStore* store) {
    loopback_server target;
    loopback_server source;
    probe_view filtered(environment), unfiltered(environment);
    check(filtered.web.configuration.websiteDataStore == unfiltered.web.configuration.websiteDataStore, "same environment data store");
    [filtered.web.configuration.userContentController addContentRuleList:compile(store, target.port())];
    check(filtered.navigate(source.origin() + "/page"), "filtered source navigation");
    check(unfiltered.navigate(source.origin() + "/page"), "unfiltered source navigation");
    NSString* outcome = attempt(filtered, target.origin() + "/filtered", @"fetch");
    report("same-store-filtered", outcome, target.snapshot());
    check([outcome isEqualToString:@"error"] && no_request(target.snapshot()), "filtered isolation control");
    outcome = attempt(unfiltered, target.origin() + "/unfiltered", @"fetch");
    report("same-store-unfiltered", outcome, target.snapshot());
    check([outcome isEqualToString:@"load"] && target.snapshot().requests > 0, "unfiltered isolation control");
}

} // namespace

int run_probe(int argc, char** argv) {
    const bool gates = argc == 2 && std::strcmp(argv[1], "--gates") == 0;
    const bool api = argc == 2 && std::strcmp(argv[1], "--api") == 0;
    if (!gates && !api && !(argc == 2 && std::strcmp(argv[1], "--baseline") == 0)) {
        std::fprintf(stderr, "Usage: %s --baseline|--gates|--api\n", argv[0]);
        return 64;
    }
    @autoreleasepool {
        std::puts("Contract: block application requests; TCP accepts are diagnostic. No global update barrier.");
        std::printf("OS=%s WebKit=%s\n", NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String,
            [[[NSBundle bundleForClass:WKWebView.class] objectForInfoDictionaryKey:@"CFBundleVersion"] description].UTF8String);
        neoastra_app_options_t options{};
        options.size = sizeof(options);
        options.version = 1;
        options.shutdown_mode = NEOASTRA_APP_SHUTDOWN_EXPLICIT;
        neoastra_app_t* app{};
        check(neoastra_app_attach(&options, &app, nullptr) == NEOASTRA_OK, "attach app");
        neoastra_environment_options_t environment_options{};
        environment_options.size = sizeof(environment_options);
        environment_options.version = 1;
        environment_options.private_mode = 1;
        completion result;
        check(neoastra_environment_create_async(app, &environment_options, environment_created, &result, nullptr, nullptr) == NEOASTRA_OK, "create environment");
        wait_for([&] { return result.done; }, "environment completion");
        check(result.result == NEOASTRA_OK && result.value, "environment result");
        auto* environment = static_cast<neoastra_environment_t*>(result.value);
        NSURL* directory = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString] isDirectory:YES];
        check([NSFileManager.defaultManager createDirectoryAtURL:directory withIntermediateDirectories:NO attributes:nil error:nil], "temporary rule store");
        WKContentRuleListStore* store = [WKContentRuleListStore storeWithURL:directory];
        bool passed = true;
        @autoreleasepool {
            if (api) api_cases(environment);
            NSArray<NSString*>* kinds = gates ? @[@"document", @"guarded-document", @"page-document", @"guarded-page-document", @"fetch", @"xhr", @"img", @"script", @"iframe", @"same-frame-fetch", @"cross-frame-fetch", @"redirect", @"websocket", @"worker"] : @[@"fetch"];
            for (NSString* kind in kinds) {
                // Bound WebKit's autoreleased configurations/views per case,
                // rather than retaining every case until the whole suite ends.
                @autoreleasepool { request_case(environment, store, kind, false); }
                @autoreleasepool { passed = request_case(environment, store, kind, true) && passed; }
            }
            isolation_case(environment, store);
            if (gates) {
                passed = update_case(environment, store) && passed;
                passed = host_rule_cases(environment, store) && passed;
            }
        }
        neoastra_environment_release(environment);
        check(neoastra_app_detach(app, nullptr) == NEOASTRA_OK, "detach app");
        neoastra_app_release(app);
        // No view holds a filter and no compile remains outstanding.
        store = nil;
        check([NSFileManager.defaultManager removeItemAtURL:directory error:nil], "remove temporary rule store");
        std::printf("Observed request cases: %s. Not a capability or an update-barrier proof.\n", passed ? "PASS" : "FAIL");
        return passed ? 0 : 1;
    }
}

#if TARGET_OS_IPHONE
@interface NeoRequestFilterProbeDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation NeoRequestFilterProbeDelegate
@end

@interface NeoRequestFilterProbeScene : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow* window;
@property(nonatomic, assign) BOOL started;
@end

@implementation NeoRequestFilterProbeScene
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
    (void)session; (void)options;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
    self.window.rootViewController = [UIViewController new];
    [self.window makeKeyAndVisible];
    probe_root = self.window.rootViewController.view;
}
- (void)sceneDidBecomeActive:(UIScene*)scene {
    (void)scene;
    if (self.started) return;
    self.started = YES;
    // Start from the run loop, not inside a main-queue block: the synchronous
    // probe pumps that loop and must let native main-queue completions run.
    [self performSelector:@selector(runProbe) withObject:nil afterDelay:0];
}
- (void)runProbe {
    @autoreleasepool {
        std::setbuf(stdout, nullptr);
        NSArray<NSString*>* arguments = NSProcessInfo.processInfo.arguments;
        std::string mode = arguments.count > 1 ? arguments[1].UTF8String : "--baseline";
        char name[] = "neoastra_ios_request_filter_probe";
        char* argv[] = {name, mode.data()};
        const int result = run_probe(2, argv);
        // The host checks this fresh result, not simctl launch's process-start status.
        NSURL* documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
        NSString* summary = [NSString stringWithFormat:@"%s:%d\n", mode.c_str(), result];
        [summary writeToURL:[documents URLByAppendingPathComponent:@"request-filter-result.txt"]
            atomically:YES encoding:NSUTF8StringEncoding error:nil];
        std::fflush(stdout);
        std::fflush(stderr);
        std::exit(result);
    }
}
@end

int main(int argc, char** argv) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(NeoRequestFilterProbeDelegate.class)); }
}
#else
int main(int argc, char** argv) { return run_probe(argc, argv); }
#endif
