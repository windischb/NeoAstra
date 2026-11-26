// Windows request-filter integration tests through the REAL NeoAstra C ABI, backend and WebView2 runtime.
//
// The loopback fixture below is independent of NeoAstra and of the engine: it counts TCP accepts, raw bytes (partial
// request heads included), HTTP requests per (port, path, method) and decoded POST bodies. A refused request must leave
// NO request and NO bytes on its port; TCP accepts are recorded but never asserted (no zero-TCP guarantee).
// Blocking decisions come from the common matcher (neo_request_filter::filter::refuses), not from the fixture.
//
// Two executables share this source: the default one (real library) and NEOASTRA_FAILURE_ONLY (deterministic failure
// injection, test-hooks library). The contract without ICoreWebView2_22 is windows_request_filter_unsupported_tests.cpp.
//
// NOT covered here: TLS (loopback uses HTTP), external networks, WebSockets and workers (not claimed), redirects
// (diagnostic in the standalone preflight only), managed integration. No window is shown or activated.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// The fixture selects over its listeners and every accepted connection; the default fd_set holds only 64 sockets.
#define FD_SETSIZE 1024
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include "neoastra.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); std::fflush(stderr); std::exit(1); } } while (0)

neoastra_string_view_t sv(const std::string& value) { return {reinterpret_cast<const uint8_t*>(value.data()), value.size()}; }
std::string str(neoastra_string_view_t value) { return value.data ? std::string(reinterpret_cast<const char*>(value.data), value.length) : std::string(); }

// ---------------------------------------------------------------- loopback fixture (host-side counters)
class fixture {
public:
    struct path_stat { uint64_t requests{}, bytes{}, body_bytes{}; };

    fixture() {
        WSADATA data{};
        CHECK(WSAStartup(MAKEWORD(2, 2), &data) == 0);
        for (int i = 0; i < 96; ++i) {
            SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            CHECK(listener != INVALID_SOCKET);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = 0;
            CHECK(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
            CHECK(listen(listener, 16) == 0);
            int length = sizeof(address);
            CHECK(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0);
            u_long nonblocking = 1;
            ioctlsocket(listener, FIONBIO, &nonblocking);
            listeners_.push_back({listener, ntohs(address.sin_port)});
        }
        thread_ = std::thread([this] { run(); });
    }
    ~fixture() {
        stop_ = true;
        thread_.join();
        for (auto& listener : listeners_) closesocket(listener.first);
        WSACleanup();
    }
    int take_port() { std::lock_guard lock(mutex_); CHECK(next_ < listeners_.size()); return listeners_[next_++].second; }
    path_stat stat(int port, const std::string& path) {
        std::lock_guard lock(mutex_);
        const auto found = paths_.find({port, path});
        return found == paths_.end() ? path_stat{} : found->second;
    }
    uint64_t requests(int port, const std::string& path) { return stat(port, path).requests; }
    uint64_t method_count(int port, const std::string& path, const std::string& method) {
        std::lock_guard lock(mutex_);
        const auto found = methods_.find({port, path, method});
        return found == methods_.end() ? 0 : found->second;
    }
    std::string last_body(int port, const std::string& path) {
        std::lock_guard lock(mutex_);
        const auto found = bodies_.find({port, path});
        return found == bodies_.end() ? std::string() : found->second;
    }
    uint64_t raw_bytes(int port) { std::lock_guard lock(mutex_); return raw_bytes_[port]; }
    uint64_t tcp_accepts(int port) { std::lock_guard lock(mutex_); return tcp_[port]; }
    uint64_t partial_bytes(int port) { std::lock_guard lock(mutex_); return partial_[port]; }
    uint64_t port_requests(int port) {
        std::lock_guard lock(mutex_);
        uint64_t total{};
        for (const auto& entry : paths_) if (entry.first.first == port) total += entry.second.requests;
        return total;
    }
    // "path xN method" of every request a port saw, for failure diagnostics.
    std::string describe_port(int port) {
        std::lock_guard lock(mutex_);
        std::string text;
        for (const auto& entry : methods_) if (std::get<0>(entry.first) == port) text += " " + std::get<1>(entry.first) + " x" + std::to_string(entry.second) + " " + std::get<2>(entry.first) + ";";
        return text.empty() ? " (none)" : text;
    }

private:
    struct connection { SOCKET socket; int port; std::string buffer; };

    // Requests of one page, as fetch / XHR / POST, same-origin and cross-origin, with and without CORS preflight.
    static std::string fetch_script(const std::string& prefix, const std::string& m, const std::string& b, const std::string& t) {
        auto at = [](const std::string& port, const std::string& path) { return "http://127.0.0.1:" + port + path; };
        const std::string json = "{headers:{'Content-Type':'application/json','X-Probe':'1'},method:'POST',body:JSON.stringify({probe:'" + prefix + "'})}";
        return "const st=async(k,f)=>{try{const r=await f();window.__r[k]=r.status}catch(e){window.__r[k]='err'}};"
               "const xhr=(u)=>new Promise((a,r)=>{const x=new XMLHttpRequest();x.open('GET',u);x.onload=()=>a({status:x.status});x.onerror=()=>r();x.send()});"
               "await st('" + prefix + "_m',()=>fetch('" + at(m, "/" + prefix + "_m.json") + "'));"
               "await st('" + prefix + "_t',()=>fetch('" + at(t, "/" + prefix + "_t.json") + "'));"
               "await st('" + prefix + "_b',()=>fetch('" + at(b, "/" + prefix + "_b.json") + "'));"
               "await st('" + prefix + "_bx',()=>xhr('" + at(b, "/" + prefix + "_b_xhr.json") + "'));"
               "await st('" + prefix + "_bp',()=>fetch('" + at(b, "/" + prefix + "_b_post") + "',{method:'POST',body:'PAYLOAD-0123456789'}));"
               "await st('" + prefix + "_mpre',()=>fetch('" + at(m, "/" + prefix + "_m_pre") + "'," + json + "));"
               "await st('" + prefix + "_tpre',()=>fetch('" + at(t, "/" + prefix + "_t_pre") + "'," + json + "));"
               "await st('" + prefix + "_bpre',()=>fetch('" + at(b, "/" + prefix + "_b_pre") + "'," + json + "));"
               "new Image().src='" + at(m, "/" + prefix + "_img_m.png") + "';new Image().src='" + at(b, "/" + prefix + "_img_b.png") + "';";
    }
    static std::string page_main(int self, int blocked, int third) {
        const std::string m = std::to_string(self), b = std::to_string(blocked), t = std::to_string(third);
        return "<!doctype html><meta charset=utf-8><title>main</title>"
            "<script src=\"/m_s.js\"></script><img src=\"/m_img.png\">"
            "<script src=\"http://127.0.0.1:" + b + "/b_s.js\"></script><img src=\"http://127.0.0.1:" + b + "/b_img.png\">"
            "<iframe src=\"/frame_m?b=" + b + "&t=" + t + "\"></iframe>"
            "<iframe src=\"http://127.0.0.1:" + b + "/frame_b?m=" + m + "&t=" + t + "\"></iframe>"
            "<iframe src=\"http://127.0.0.1:" + t + "/frame_t?m=" + m + "&b=" + b + "&t=" + t + "\"></iframe>"
            "<script>window.__r={};(async()=>{"
            "const st=async(k,f)=>{try{const r=await f();window.__r[k]=r.status}catch(e){window.__r[k]='err'}};"
            "await st('m_fetch',()=>fetch('/m_fetch.json'));"
            "await st('m_xhr',()=>new Promise((a,r)=>{const x=new XMLHttpRequest();x.open('GET','/m_xhr.json');x.onload=()=>a({status:x.status});x.onerror=()=>r();x.send()}));"
            "await st('m_post',()=>fetch('/m_post',{method:'POST',body:'PAYLOAD-0123456789'}));"
            "await st('b_fetch',()=>fetch('http://127.0.0.1:" + b + "/b_fetch.json'));"
            "await st('b_xhr',()=>new Promise((a,r)=>{const x=new XMLHttpRequest();x.open('GET','http://127.0.0.1:" + b + "/b_xhr.json');x.onload=()=>a({status:x.status});x.onerror=()=>r();x.send()}));"
            "await st('b_post',()=>fetch('http://127.0.0.1:" + b + "/b_post',{method:'POST',body:'PAYLOAD-0123456789'}));"
            "await st('t_pre',()=>fetch('http://127.0.0.1:" + t + "/main_t_pre',{method:'POST',headers:{'Content-Type':'application/json','X-Probe':'1'},body:JSON.stringify({probe:'main'})}));"
            "await st('b_pre',()=>fetch('http://127.0.0.1:" + b + "/main_b_pre',{method:'POST',headers:{'Content-Type':'application/json','X-Probe':'1'},body:JSON.stringify({probe:'main'})}));"
            "window.__done=true;})();</script>";
    }
    // A frame page runs its own requests: the frame on the main origin (fm), on the denied origin (fb) and on a THIRD allowed origin (ft).
    static std::string page_frame(const char* tag, int m, int b, int t) {
        const std::string prefix = tag;
        return "<!doctype html><meta charset=utf-8><title>" + prefix + "</title><script>window.__r={};(async()=>{" +
               fetch_script(prefix, std::to_string(m), std::to_string(b), std::to_string(t)) + "window.__done=true;})();</script>";
    }
    static int query_int(const std::string& target, const char* key) {
        const auto at = target.find(std::string(key) + "=");
        return at == std::string::npos ? 0 : std::atoi(target.c_str() + at + std::strlen(key) + 1);
    }
    static std::string respond(int port, const std::string& method, const std::string& target) {
        const auto path = target.substr(0, target.find('?'));
        const std::string cors = "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: *\r\nAccess-Control-Allow-Methods: *\r\n";
        std::string type = "application/json", body = "{\"ok\":true}";
        static const unsigned char png[] = {0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0,0,0,0x0d,0x49,0x48,0x44,0x52,0,0,0,1,0,0,0,1,8,6,0,0,0,0x1f,0x15,0xc4,0x89,0,0,0,0x0d,0x49,0x44,0x41,0x54,0x78,0x9c,0x63,0x60,0,0,2,0,0,1,0xe2,0x21,0xbc,0x33,0,0,0,0,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82};
        if (method == "OPTIONS") return "HTTP/1.1 204 No Content\r\n" + cors + "Content-Length: 0\r\n\r\n";
        if (path == "/doc") { type = "text/html"; body = page_main(port, query_int(target, "b"), query_int(target, "t")); }
        else if (path == "/frame_m") { type = "text/html"; body = page_frame("fm", port, query_int(target, "b"), query_int(target, "t")); }
        else if (path == "/frame_b") { type = "text/html"; body = page_frame("fb", query_int(target, "m"), port, query_int(target, "t")); }
        else if (path == "/frame_t") { type = "text/html"; body = page_frame("ft", query_int(target, "m"), query_int(target, "b"), port); }
        else if (path == "/popup") {
            type = "text/html";
            const std::string b = std::to_string(query_int(target, "b"));
            body = "<!doctype html><meta charset=utf-8><title>popup</title><script>window.__r={};(async()=>{"
                   "try{await fetch('/pp_m.json')}catch(e){}"
                   "try{await fetch('http://127.0.0.1:" + b + "/pp_b.json')}catch(e){}window.__done=true;})();</script>";
        }
        else if (path == "/storm" || path == "/storm_frame") {
            // Continuous traffic until the view goes away: allowed requests prove that the page is still issuing them,
            // denied ones must never arrive, also not while (or after) the view is disposed.
            type = "text/html";
            const std::string b = std::to_string(query_int(target, "b"));
            const std::string tag = path == "/storm" ? "sm" : "sf";
            body = "<!doctype html><meta charset=utf-8><title>storm</title>" +
                   (path == "/storm" ? "<iframe src=\"/storm_frame?b=" + b + "&open=" + std::to_string(query_int(target, "open")) + "\"></iframe>" : std::string()) +
                   "<script>let n=0;setInterval(()=>{++n;"
                   "fetch('/" + tag + "_m.json?'+n).catch(()=>{});"
                   "fetch('http://127.0.0.1:" + b + "/" + tag + "_b.json?'+n).catch(()=>{});"
                   "fetch('http://127.0.0.1:" + b + "/" + tag + "_bp',{method:'POST',body:'PAYLOAD'}).catch(()=>{});"
                   "fetch('http://127.0.0.1:" + b + "/" + tag + "_bk',{method:'POST',body:'KEEPALIVE',keepalive:true}).catch(()=>{});"
                   "navigator.sendBeacon('http://127.0.0.1:" + b + "/" + tag + "_beacon','BEACON');"
                   "new Image().src='http://127.0.0.1:" + b + "/" + tag + "_b.png?'+n;"
                   + (query_int(target, "open") ? "window.open('http://127.0.0.1:" + b + "/" + tag + "_open?'+n);" : std::string()) +
                   "},4);"
                   "addEventListener('pagehide',()=>{" + (query_int(target, "open") ? "window.open('http://127.0.0.1:" + b + "/" + tag + "_ph_open');" : std::string()) + ""
                   "navigator.sendBeacon('http://127.0.0.1:" + b + "/" + tag + "_ph_beacon','BYE');"
                   "fetch('http://127.0.0.1:" + b + "/" + tag + "_ph_keepalive',{method:'POST',body:'BYE',keepalive:true});});</script>";
        }
        else if (path == "/opener") {
            // Six ticks; each first calls window.open() (target 0: an address on B; 1: the allowed popup page), then GET,
            // POST and an image to B and a fetch to /op_ok.
            type = "text/html";
            const std::string b = "http://127.0.0.1:" + std::to_string(query_int(target, "b"));
            const std::string popup = query_int(target, "target") == 1 ? "'/popup?b=" + std::to_string(query_int(target, "b")) + "&n='+n" : "'" + b + "/op_open?'+n";
            body = "<!doctype html><title>opener</title><script>let n=0;const t=setInterval(()=>{if(++n>6){clearInterval(t);window.__done=true;return;}"
                   "window.open(" + popup + ");"
                   "fetch('" + b + "/op_get?'+n).catch(()=>{});"
                   "fetch('" + b + "/op_post',{method:'POST',body:'PAYLOAD'}).catch(()=>{});"
                   "new Image().src='" + b + "/op_img?'+n;"
                   "fetch('/op_ok?'+n).catch(()=>{});},50);</script>";
        }
        else if (path.rfind("/doc_", 0) == 0) { type = "text/html"; body = "<!doctype html><title>" + path + "</title>ok"; }
        else if (path.size() > 4 && path.compare(path.size() - 4, 4, ".png") == 0) { type = "image/png"; body.assign(reinterpret_cast<const char*>(png), sizeof(png)); }
        else if (path == "/m_s.js") { type = "application/javascript"; body = "window.__script_ran=true;"; }
        else if (path == "/b_s.js") { type = "application/javascript"; body = "window.__b_script_ran=true;"; }
        return "HTTP/1.1 200 OK\r\nContent-Type: " + type + "\r\n" + cors + "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: keep-alive\r\n\r\n" + body;
    }

    void serve(connection& c, bool& open) {
        char chunk[65536];
        const int received = recv(c.socket, chunk, sizeof(chunk), 0);
        if (received == 0 || (received < 0 && WSAGetLastError() != WSAEWOULDBLOCK)) {
            open = false;
            std::lock_guard lock(mutex_);
            partial_[c.port] += c.buffer.size(); // Incomplete request bytes are application bytes too: never discarded.
            return;
        }
        if (received < 0) return;
        {
            std::lock_guard lock(mutex_);
            raw_bytes_[c.port] += static_cast<uint64_t>(received);
        }
        c.buffer.append(chunk, static_cast<size_t>(received));
        for (;;) {
            const auto head_end = c.buffer.find("\r\n\r\n");
            if (head_end == std::string::npos) return;
            const auto head = c.buffer.substr(0, head_end);
            size_t content_length = 0;
            for (size_t at = 0; (at = head.find("\r\n", at)) != std::string::npos;) {
                at += 2;
                if (_strnicmp(head.c_str() + at, "content-length:", 15) == 0) content_length = static_cast<size_t>(std::atoll(head.c_str() + at + 15));
            }
            const size_t total = head_end + 4 + content_length;
            if (c.buffer.size() < total) return;
            const auto first = head.substr(0, head.find("\r\n"));
            const auto sp1 = first.find(' '), sp2 = first.find(' ', sp1 + 1);
            const auto method = first.substr(0, sp1), target = first.substr(sp1 + 1, sp2 - sp1 - 1);
            const auto body = c.buffer.substr(head_end + 4, content_length);
            {
                std::lock_guard lock(mutex_);
                const auto path = target.substr(0, target.find('?'));
                auto& entry = paths_[{c.port, path}];
                ++entry.requests;
                entry.bytes += total;
                entry.body_bytes += content_length;
                ++methods_[{c.port, path, method}];
                if (GetEnvironmentVariableA("NEOASTRA_RF_HOST_HEADERS", nullptr, 0) != 0) std::fprintf(stderr, "[host head] port %d\n%s\n", c.port, head.c_str());
                if (GetEnvironmentVariableA("NEOASTRA_RF_HOST_TIMES", nullptr, 0) != 0) { // arrival times (QPC) for timelines
                    LARGE_INTEGER now{}, rate{}; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&rate); std::fprintf(stderr, "[host %lld us] %s %s port %d\n", now.QuadPart * 1000000 / rate.QuadPart, method.c_str(), target.c_str(), c.port); }
                if (method == "POST") bodies_[{c.port, path}] = body.substr(0, 256);
            }
            c.buffer.erase(0, total);
            const auto reply = respond(c.port, method, target);
            send(c.socket, reply.data(), static_cast<int>(reply.size()), 0);
        }
    }

    void run() {
        std::vector<connection> connections;
        while (!stop_) {
            fd_set read_set;
            FD_ZERO(&read_set);
            for (auto& listener : listeners_) FD_SET(listener.first, &read_set);
            for (auto& c : connections) FD_SET(c.socket, &read_set);
            timeval timeout{0, 20000};
            if (select(0, &read_set, nullptr, nullptr, &timeout) <= 0) continue;
            for (auto& listener : listeners_) {
                if (!FD_ISSET(listener.first, &read_set)) continue;
                SOCKET accepted = accept(listener.first, nullptr, nullptr);
                if (accepted == INVALID_SOCKET) continue;
                u_long nonblocking = 1;
                ioctlsocket(accepted, FIONBIO, &nonblocking);
                connections.push_back({accepted, listener.second, {}});
                std::lock_guard lock(mutex_);
                ++tcp_[listener.second];
            }
            for (size_t i = 0; i < connections.size();) {
                bool open = true;
                if (FD_ISSET(connections[i].socket, &read_set)) serve(connections[i], open);
                if (!open) { closesocket(connections[i].socket); connections.erase(connections.begin() + static_cast<std::ptrdiff_t>(i)); }
                else ++i;
            }
        }
        for (auto& c : connections) closesocket(c.socket);
    }

    std::mutex mutex_;
    std::vector<std::pair<SOCKET, int>> listeners_;
    size_t next_{};
    std::map<std::pair<int, std::string>, path_stat> paths_;
    std::map<std::tuple<int, std::string, std::string>, uint64_t> methods_;
    std::map<std::pair<int, std::string>, std::string> bodies_;
    std::map<int, uint64_t> raw_bytes_, tcp_, partial_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

fixture* g_host{};

// ---------------------------------------------------------------- UI-thread helpers
template <class F> bool pump_until(F&& condition, int milliseconds = 30000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}
void settle(int milliseconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    pump_until([&] { return std::chrono::steady_clock::now() >= end; }, milliseconds + 200);
}

struct created { bool done{}; neoastra_result_t result{NEOASTRA_ERROR_UNKNOWN}; void* value{}; };
void NEOASTRA_CALL on_environment(void* p, neoastra_result_t r, neoastra_environment_t* v, const neoastra_error_t*) { *static_cast<created*>(p) = {true, r, v}; }
void NEOASTRA_CALL on_profile(void* p, neoastra_result_t r, neoastra_profile_t* v, const neoastra_error_t*) { *static_cast<created*>(p) = {true, r, v}; }
void NEOASTRA_CALL on_view(void* p, neoastra_result_t r, neoastra_view_t* v, const neoastra_error_t*) { *static_cast<created*>(p) = {true, r, v}; }

// What the test host does with a window request: defer it (and decide later), cancel it at once, leave it unhandled (the
// default CANCEL), allow it without a target view (fallback navigation of the opener), or serve it as a tracked popup.
enum class popup_policy { defer, cancel, unhandled, fallback, track };
struct view_state {
    neoastra_view_t* view{};
    popup_policy policy{popup_policy::defer};
    std::atomic<int> navigation_finished{};
    std::atomic<int> quarantined{};             // NEOASTRA_EVENT_WEB_PROCESS_TERMINATED with RECREATE_VIEW
    int new_window_events{};
    std::vector<neoastra_decision_t*> popups;   // retained, deferred
    std::vector<std::string> popup_uris;
};
void NEOASTRA_CALL on_view_event(void* p, const neoastra_event_t* event) {
    auto* state = static_cast<view_state*>(p);
    static const bool trace = GetEnvironmentVariableA("NEOASTRA_RF_TRACE", nullptr, 0) != 0;
    if (trace) std::fprintf(stderr, "  event type=%d uri=%s value=%llu\n", static_cast<int>(event->header.type), str(event->uri).c_str(), static_cast<unsigned long long>(event->value));
    if (event->header.type == NEOASTRA_EVENT_NAVIGATION_COMPLETED || event->header.type == NEOASTRA_EVENT_NAVIGATION_FAILED) ++state->navigation_finished;
    else if (event->header.type == NEOASTRA_EVENT_WEB_PROCESS_TERMINATED && (event->value & NEOASTRA_PROCESS_FAILURE_RECREATE_VIEW) != 0) ++state->quarantined;
    else if (event->header.type == NEOASTRA_EVENT_NEW_WINDOW_REQUESTED && event->decision) {
        ++state->new_window_events;
        if (state->policy == popup_policy::unhandled) return;
        if (state->policy == popup_policy::cancel || state->policy == popup_policy::fallback) {
            neoastra_decision_response_t response{};
            response.size = sizeof(response); response.version = 1;
            response.action = state->policy == popup_policy::cancel ? NEOASTRA_DECISION_CANCEL : NEOASTRA_DECISION_ALLOW;
            CHECK(neoastra_decision_complete(event->decision, &response, nullptr) == NEOASTRA_OK);
            return;
        }
        neoastra_decision_retain(event->decision);
        CHECK(neoastra_decision_defer(event->decision) == NEOASTRA_OK);
        state->popups.push_back(event->decision);
        state->popup_uris.push_back(str(event->uri));
    }
}

struct string_result { bool done{}; neoastra_result_t result{}; std::string value; };
void NEOASTRA_CALL on_string(void* p, neoastra_result_t r, neoastra_string_view_t v, const neoastra_error_t*) {
    auto* s = static_cast<std::shared_ptr<string_result>*>(p);
    (*s)->result = r; (*s)->value = str(v); (*s)->done = true;
    delete s;
}
neoastra_result_t eval_rc(neoastra_view_t* view, const std::string& script, std::string* output = nullptr, int milliseconds = 10000) {
    auto state = std::make_shared<string_result>();
    auto* holder = new std::shared_ptr<string_result>(state);
    neoastra_error_t* error{};
    const auto rc = neoastra_view_evaluate_script_async(view, sv(script), on_string, holder, nullptr, &error);
    if (error) neoastra_error_release(error);
    if (rc != NEOASTRA_OK) { delete holder; return rc; }
    if (!pump_until([&] { return state->done; }, milliseconds)) return NEOASTRA_ERROR_TIMED_OUT;
    if (output) *output = state->value;
    return state->result;
}
std::string eval(neoastra_view_t* view, const std::string& script) {
    std::string value;
    const auto rc = eval_rc(view, script, &value);
    if (rc == NEOASTRA_ERROR_TIMED_OUT) return "<timeout>";
    return rc == NEOASTRA_OK ? value : "<rejected>";
}
bool wait_done(neoastra_view_t* view, int milliseconds = 15000) {
    return pump_until([&] { return eval(view, "String(window.__done)").find("true") != std::string::npos; }, milliseconds);
}
// Page-initiated request from the already loaded document; the result is polled (ExecuteScript does not await promises).
std::string page_fetch(neoastra_view_t* view, const std::string& url, const std::string& method = "GET", const std::string& body = "") {
    const std::string options = method == "POST" ? ",{method:'POST',body:'" + body + "'}" : "";
    eval(view, "window.__x=null;fetch('" + url + "'" + options + ").then(r=>window.__x=r.status).catch(e=>window.__x='err');0");
    std::string last;
    pump_until([&] { last = eval(view, "String(window.__x)"); return last != "\"null\"" && last != "<timeout>"; }, 8000);
    return last;
}
std::string url(int port, const std::string& path) { return "http://127.0.0.1:" + std::to_string(port) + path; }
void navigate_and_wait(view_state& state, const std::string& address) {
    const int before = state.navigation_finished.load();
    neoastra_error_t* error{};
    CHECK(neoastra_view_navigate(state.view, sv(address), &error) == NEOASTRA_OK);
    if (!pump_until([&] { return state.navigation_finished.load() > before; }, 20000)) {
        const auto colon = address.find(':', 7);
        const int port = colon == std::string::npos ? 0 : std::atoi(address.c_str() + colon + 1);
        std::fprintf(stderr, "navigation to %s did not finish: host requests on port=%llu raw=%llu, location=%s\n", address.c_str(),
                     static_cast<unsigned long long>(g_host->port_requests(port)), static_cast<unsigned long long>(g_host->raw_bytes(port)),
                     eval(state.view, "location.href").c_str());
        CHECK(false);
    }
}

struct completion { bool done{}; neoastra_result_t result{NEOASTRA_ERROR_UNKNOWN}; int calls{}; };
void NEOASTRA_CALL on_completion(void* p, neoastra_result_t r, const neoastra_error_t*) { auto* c = static_cast<completion*>(p); c->result = r; c->done = true; ++c->calls; }

std::vector<neoastra_request_rule_t> loopback_rules(const std::vector<int>& ports) {
    std::vector<neoastra_request_rule_t> rules;
    for (const int port : ports) {
        neoastra_request_rule_t rule{};
        rule.size = sizeof(rule); rule.version = 1; rule.host_match = NEOASTRA_REQUEST_HOST_LOOPBACK;
        rule.port = port; rule.schemes = 1 /* http */; rule.kinds = UINT32_MAX;
        rules.push_back(rule);
    }
    return rules;
}
neoastra_request_filter_t describe(const std::vector<neoastra_request_rule_t>& rules, uint64_t required = 0) {
    neoastra_request_filter_t filter{};
    filter.size = sizeof(filter); filter.version = 1;
    filter.rules = rules.empty() ? nullptr : rules.data();
    filter.rule_count = static_cast<uint32_t>(rules.size());
    filter.required_flags = required;
    return filter;
}
// Returns the synchronous rejection, or the asynchronous completion result.
neoastra_result_t set_filter(neoastra_view_t* view, const std::vector<int>& blocked) {
    const auto rules = loopback_rules(blocked);
    const auto filter = describe(rules);
    completion done;
    neoastra_error_t* error{};
    const auto accepted = neoastra_view_set_request_filter_async(view, &filter, on_completion, &done, nullptr, &error);
    if (error) neoastra_error_release(error);
    if (accepted != NEOASTRA_OK) return accepted;
    CHECK(pump_until([&] { return done.done; }, 15000));
    CHECK(done.calls == 1);
    return done.result;
}

// ---------------------------------------------------------------- environment fixture
struct provider_state { std::atomic<int> calls{}; };
neoastra_result_t NEOASTRA_CALL custom_scheme_provider(void* context, const neoastra_resource_request_t* request, neoastra_resource_response_t* response) {
    ++static_cast<provider_state*>(context)->calls;
    static thread_local std::string page;
    const auto address = str(request->uri);
    const auto read = [&](const char* key) { const auto at = address.find(std::string(key) + "="); return at == std::string::npos ? 0 : std::atoi(address.c_str() + at + std::strlen(key) + 1); };
    page = "<!doctype html><meta charset=utf-8><title>app</title><script>window.__r={};(async()=>{"
           "try{await fetch('http://127.0.0.1:" + std::to_string(read("m")) + "/cs_m.json')}catch(e){}"
           "try{await fetch('http://127.0.0.1:" + std::to_string(read("b")) + "/cs_b.json')}catch(e){}"
           "window.__done=true;})();</script>";
    response->status_code = 200;
    response->body_kind = NEOASTRA_RESOURCE_BODY_BYTES;
    static const std::string mime = "text/html";
    response->mime_type = sv(mime);
    response->bytes = reinterpret_cast<const uint8_t*>(page.data());
    response->byte_length = page.size();
    response->content_length = page.size();
    return NEOASTRA_OK;
}

struct world {
    neoastra_app_t* app{};
    neoastra_window_t* window{};
    neoastra_environment_t* environment{};
    neoastra_profile_t* profile{};
    std::shared_ptr<provider_state> provider{std::make_shared<provider_state>()};
    std::filesystem::path root;
};

// The request-filter messages of the library (refusal fallbacks, degraded popups) go into the test log as evidence.
std::atomic<int> g_degraded{0}, g_fail_closed{0}, g_popup_hook_errors{0}, g_creation_timeouts{0}, g_app_sink_limit{0};
void NEOASTRA_CALL on_log(void*, neoastra_log_level_t level, neoastra_string_view_t category, neoastra_string_view_t message, uint64_t, uint64_t, int64_t native_code, uint64_t) {
    if (str(category) != "request_filter") return;
    const auto text = str(message);
    if (text.rfind("Request filter degraded", 0) == 0) ++g_degraded;
    if (text.rfind("Request filter fail-closed", 0) == 0) ++g_fail_closed;
    if (text.find("could not be registered after its assignment") != std::string::npos) ++g_popup_hook_errors;
    if (text.find("was not created in time") != std::string::npos) ++g_creation_timeouts;
    if (text.find("limit of popup sinks of the application") != std::string::npos) ++g_app_sink_limit;
    std::fprintf(stderr, "  [library log %d request_filter] %s (0x%08llx)\n", static_cast<int>(level), str(message).c_str(), static_cast<unsigned long long>(native_code) & 0xffffffffull);
}
world make_world(const char* label, bool custom_scheme) {
    world w;
    neoastra_app_options_t app_options{};
    app_options.size = sizeof(app_options); app_options.version = 1; app_options.shutdown_mode = NEOASTRA_APP_SHUTDOWN_EXPLICIT;
    app_options.log_callback = on_log;
    CHECK(neoastra_app_attach(&app_options, &w.app, nullptr) == NEOASTRA_OK);
    // The window is created and never shown or activated: the harness comparison against the pristine library proved that
    // navigation and script evaluation work in a hidden window, and the tests must not take the focus of the user.
    neoastra_window_options_t window_options{};
    window_options.size = sizeof(window_options); window_options.version = 1; window_options.bounds = {100, 100, 640, 480}; window_options.flags = 3;
    CHECK(neoastra_app_create_window(w.app, &window_options, &w.window, nullptr) == NEOASTRA_OK);
    wchar_t temp[MAX_PATH]{};
    CHECK(GetTempPathW(MAX_PATH, temp) != 0);
    w.root = std::filesystem::path(temp) / (std::string("neoastra-rf-") + label + "-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    const auto root_text = w.root.u8string();
    static const std::string scheme_name = "neoapp";
    neoastra_custom_scheme_t scheme{};
    scheme.size = sizeof(scheme); scheme.version = 1; scheme.name = sv(scheme_name);
    scheme.flags = NEOASTRA_CUSTOM_SCHEME_HAS_AUTHORITY | NEOASTRA_CUSTOM_SCHEME_SECURE;
    scheme.resource_provider = custom_scheme_provider; scheme.resource_provider_context = w.provider.get();
    neoastra_environment_options_t options{};
    options.size = sizeof(options); options.version = 1;
    options.user_data_root = {reinterpret_cast<const uint8_t*>(root_text.data()), root_text.size()};
    if (custom_scheme) { options.custom_scheme_count = 1; options.custom_schemes = &scheme; options.custom_scheme_stride = sizeof(scheme); }
    created environment;
    CHECK(neoastra_environment_create_async(w.app, &options, on_environment, &environment, nullptr, nullptr) == NEOASTRA_OK);
    CHECK(pump_until([&] { return environment.done; }));
    CHECK(environment.result == NEOASTRA_OK);
    w.environment = static_cast<neoastra_environment_t*>(environment.value);
    neoastra_profile_options_t profile_options{};
    profile_options.size = sizeof(profile_options); profile_options.version = 1;
    static const std::string profile_name = "rf-shared";
    profile_options.name = sv(profile_name);
    created profile;
    CHECK(neoastra_environment_create_profile_async(w.environment, &profile_options, on_profile, &profile, nullptr, nullptr) == NEOASTRA_OK);
    CHECK(pump_until([&] { return profile.done; }));
    CHECK(profile.result == NEOASTRA_OK);
    w.profile = static_cast<neoastra_profile_t*>(profile.value);
    return w;
}
void destroy_world(world& w) {
    neoastra_profile_release(w.profile);
    neoastra_window_release(w.window);
    neoastra_environment_release(w.environment);
    CHECK(neoastra_app_detach(w.app, nullptr) == NEOASTRA_OK);
    neoastra_app_release(w.app);
    std::error_code ignored;
    std::filesystem::remove_all(w.root, ignored); // Only the directory this test created.
}

// Creates a view in the shared environment/profile. `initial` (when set) is installed before the first navigation.
neoastra_result_t create_view(world& w, view_state& state, const std::vector<int>* initial, neoastra_decision_t* popup = nullptr,
                              neoastra_window_t* window = nullptr) {
    neoastra_view_options_t options{};
    options.size = sizeof(options);
    options.version = 2;
    options.profile = w.profile;
    options.window = window ? window : w.window;
    options.fill_parent = 1;
    options.popup_request = popup;
    std::vector<neoastra_request_rule_t> rules;
    neoastra_request_filter_t filter{};
    if (initial) {
        rules = loopback_rules(*initial);
        filter = describe(rules);
        options.request_filter = &filter;
    }
    created result;
    neoastra_error_t* error{};
    const auto accepted = neoastra_environment_create_view_async(w.environment, &options, on_view, &result, nullptr, &error);
    if (error) neoastra_error_release(error);
    if (accepted != NEOASTRA_OK) return accepted;
    CHECK(pump_until([&] { return result.done; }));
    if (result.result != NEOASTRA_OK) return result.result;
    state.view = static_cast<neoastra_view_t*>(result.value);
    CHECK(neoastra_view_set_event_callback(state.view, on_view_event, &state) == NEOASTRA_OK);
    return NEOASTRA_OK;
}

int g_checks = 0;
void report(const char* name) { std::printf("[PASS] %s\n", name); std::fflush(stdout); }
void expect_reached(int port, const char* path) { ++g_checks; if (g_host->requests(port, path) < 1) { std::fprintf(stderr, "expected host request %d%s\n", port, path); CHECK(false); } }
void expect_zero(int port, const char* path) { ++g_checks; if (g_host->requests(port, path) != 0) { std::fprintf(stderr, "unexpected host request %d%s x%llu\n", port, path, static_cast<unsigned long long>(g_host->requests(port, path))); CHECK(false); } }
// A refused port saw no HTTP request and not one application byte (partial heads included). TCP accepts are only logged.
void expect_port_silent(int port, const char* label) {
    ++g_checks;
    std::printf("  %s: port %d tcp_accepts=%llu raw_bytes=%llu http_requests=%llu partial_bytes=%llu\n", label, port,
                static_cast<unsigned long long>(g_host->tcp_accepts(port)), static_cast<unsigned long long>(g_host->raw_bytes(port)),
                static_cast<unsigned long long>(g_host->port_requests(port)), static_cast<unsigned long long>(g_host->partial_bytes(port)));
    CHECK(g_host->port_requests(port) == 0);
    CHECK(g_host->raw_bytes(port) == 0);
    CHECK(g_host->partial_bytes(port) == 0);
}

// The layout: M = main origin (allowed), B = denied origin, T = third allowed origin. A frame document runs its own requests.
struct ports { int m, b, t; };
struct res { char origin; const char* path; };
int port_of(const ports& p, char origin) { return origin == 'm' ? p.m : origin == 'b' ? p.b : p.t; }
// What an unfiltered view fetches: every document and subresource of the three origins.
const res kReachedWhenOpen[] = {
    {'m', "/doc"}, {'m', "/m_s.js"}, {'m', "/m_img.png"}, {'m', "/frame_m"}, {'b', "/frame_b"}, {'t', "/frame_t"},
    {'m', "/m_fetch.json"}, {'m', "/m_xhr.json"}, {'m', "/m_post"}, {'b', "/b_s.js"}, {'b', "/b_img.png"}, {'b', "/b_fetch.json"}, {'b', "/b_xhr.json"}, {'b', "/b_post"},
    {'t', "/main_t_pre"}, {'b', "/main_b_pre"},
    // frame on the main origin, on the denied origin and on the third origin: own fetch / XHR / POST / preflight / image requests
    {'m', "/fm_m.json"}, {'t', "/fm_t.json"}, {'b', "/fm_b.json"}, {'b', "/fm_b_xhr.json"}, {'b', "/fm_b_post"}, {'m', "/fm_m_pre"}, {'t', "/fm_t_pre"}, {'b', "/fm_b_pre"}, {'m', "/fm_img_m.png"}, {'b', "/fm_img_b.png"},
    {'m', "/fb_m.json"}, {'t', "/fb_t.json"}, {'b', "/fb_b.json"}, {'b', "/fb_b_xhr.json"}, {'b', "/fb_b_post"}, {'m', "/fb_m_pre"}, {'t', "/fb_t_pre"}, {'b', "/fb_b_pre"}, {'m', "/fb_img_m.png"}, {'b', "/fb_img_b.png"},
    {'m', "/ft_m.json"}, {'t', "/ft_t.json"}, {'b', "/ft_b.json"}, {'b', "/ft_b_xhr.json"}, {'b', "/ft_b_post"}, {'m', "/ft_m_pre"}, {'t', "/ft_t_pre"}, {'b', "/ft_b_pre"}, {'m', "/ft_img_m.png"}, {'b', "/ft_img_b.png"},
};
void load_layout(view_state& state, const ports& p) {
    navigate_and_wait(state, url(p.m, "/doc?b=" + std::to_string(p.b) + "&t=" + std::to_string(p.t)));
    CHECK(wait_done(state.view));
    settle(3500); // images and frames, each running its own requests
}
bool denied_origin(const res& r) { return r.origin == 'b'; }
// Everything the denied origin serves stayed unreachable; everything else (positive controls) arrived. Frame documents on the
// denied origin are themselves refused, so the requests THAT frame would have made are absent too.
void expect_filtered_layout(const ports& p) {
    for (const auto& r : kReachedWhenOpen) {
        const int port = port_of(p, r.origin);
        if (denied_origin(r)) expect_zero(port, r.path);
        else if (std::strncmp(r.path, "/fb_", 4) == 0) expect_zero(port, r.path); // issued only by the refused denied-origin frame
        else expect_reached(port, r.path);
    }
    // Cross-origin POST with preflight: allowed destination arrived decoded, denied destination left neither POST nor OPTIONS.
    ++g_checks;
    CHECK(g_host->method_count(p.t, "/main_t_pre", "OPTIONS") >= 1 && g_host->method_count(p.t, "/main_t_pre", "POST") >= 1);
    CHECK(g_host->last_body(p.t, "/main_t_pre") == "{\"probe\":\"main\"}");
    CHECK(g_host->method_count(p.m, "/ft_m_pre", "OPTIONS") >= 1 && g_host->last_body(p.m, "/ft_m_pre") == "{\"probe\":\"ft\"}");
    CHECK(g_host->method_count(p.b, "/main_b_pre", "OPTIONS") == 0 && g_host->method_count(p.b, "/ft_b_pre", "OPTIONS") == 0 && g_host->method_count(p.b, "/ft_b_pre", "POST") == 0);
    CHECK(g_host->stat(p.m, "/m_post").body_bytes >= 18);   // positive POST control: the body reached the host
    expect_port_silent(p.b, "denied origin");
}
void expect_open_layout(const ports& p) {
    for (const auto& r : kReachedWhenOpen) expect_reached(port_of(p, r.origin), r.path);
    CHECK(g_host->last_body(p.t, "/main_t_pre") == "{\"probe\":\"main\"}");
    CHECK(g_host->last_body(p.b, "/main_b_pre") == "{\"probe\":\"main\"}" && g_host->method_count(p.b, "/main_b_pre", "OPTIONS") >= 1);
    CHECK(g_host->last_body(p.b, "/ft_b_pre") == "{\"probe\":\"ft\"}" && g_host->last_body(p.b, "/ft_b_post") == "PAYLOAD-0123456789");
    CHECK(g_host->stat(p.m, "/m_post").body_bytes >= 18);
    CHECK(g_host->stat(p.b, "/b_post").body_bytes >= 18);
}
ports take_ports() { return {g_host->take_port(), g_host->take_port(), g_host->take_port()}; }

// Disposal while the page and a frame keep requesting the denied origin (fetch, keepalive POST, beacon, image, more of them
// from pagehide; with `open`, window.open() in every task and in pagehide too). WebView2 sends requests whose
// WebResourceRequested event is pending, or answered but not yet applied, when a controller closes (observed on runtime
// 158): the backend unloads a filtered view's document before it closes (see windows_retiring_view). Used by the acceptance
// suite and by the InFlightClose diagnostic. Odd rounds put the view in a window of its own and destroy that window right
// after the view. Returns the number of rounds in which the denied origin received anything.
#ifndef NEOASTRA_FAILURE_ONLY
int dispose_during_traffic(world& w, const char* label, int rounds, bool open) {
    int leaking = 0;
    for (int round = 0; round < rounds; ++round) {
        view_state a, sibling;
        const int m = g_host->take_port(), b = g_host->take_port();
        const std::vector<int> initial{b};
        neoastra_window_t* own_window{};
        if (round % 2 == 1) {
            neoastra_window_options_t window_options{};
            window_options.size = sizeof(window_options); window_options.version = 1; window_options.bounds = {120, 120, 640, 480}; window_options.flags = 3;
            CHECK(neoastra_app_create_window(w.app, &window_options, &own_window, nullptr) == NEOASTRA_OK); // never shown
        }
        CHECK(create_view(w, sibling, nullptr) == NEOASTRA_OK);
        CHECK(create_view(w, a, &initial, nullptr, own_window) == NEOASTRA_OK);
        navigate_and_wait(a, url(m, std::string(open ? "/storm?open=1&b=" : "/storm?b=") + std::to_string(b)));
        // Hidden pages run timers slowly: a few allowed requests of the page and of its frame prove that both issue requests.
        CHECK(pump_until([&] { return g_host->requests(m, "/sm_m.json") >= 3 && g_host->requests(m, "/sf_m.json") >= 3; }, 30000));
        const auto live = g_host->port_requests(b);
        neoastra_view_release(a.view);                       // the page is still issuing requests
        if (own_window) neoastra_window_release(own_window);
        settle(4000);                                        // observation window after the disposal: past the 3 s at which
                                                             // the backend closes a filtered view at the latest
        const auto after = g_host->port_requests(b) - live;
        std::printf("  DIAGNOSTIC [%s%s] round %d%s: denied origin while alive=%llu, after disposal=%llu bytes=%llu:%s\n", label, open ? ", window.open" : "", round,
                    own_window ? " (own window)" : "", static_cast<unsigned long long>(live), static_cast<unsigned long long>(after),
                    static_cast<unsigned long long>(g_host->raw_bytes(b)), g_host->describe_port(b).c_str());
        if (live || after || g_host->raw_bytes(b)) ++leaking;
        const int m2 = g_host->take_port();
        navigate_and_wait(sibling, url(m2, "/doc_after_storm"));
        expect_reached(m2, "/doc_after_storm");                // the sibling keeps working
        neoastra_view_release(sibling.view);
    }
    return leaking;
}
#endif

// ---------------------------------------------------------------- popups of filtered views (window.open bypass regressions)
// Standalone finding on runtime 158 (windows-r3 reproducer, Test 1 pass 1): after a NewWindowRequested event that is handled
// without a new window, WebView2 stopped offering the opener's following requests (18 of 18 reached the refused port); with
// put_NewWindow and the filter registered after the assignment, all 18 were offered and refused. A filtered opener therefore
// answers every window request with a real WebView: the host's tracked popup, or a hidden popup sink of the backend.

// Visible top-level windows of this process and of its WebView2 processes (children and grandchildren): a popup that is
// refused or contained must not become a visible window, and none of these tests shows one.
int visible_windows_of_test() {
    std::vector<DWORD> family{GetCurrentProcessId()};
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        for (int depth = 0; depth < 2; ++depth) {
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
                const bool parent_known = std::find(family.begin(), family.end(), entry.th32ParentProcessID) != family.end();
                const bool known = std::find(family.begin(), family.end(), entry.th32ProcessID) != family.end();
                if (parent_known && !known) family.push_back(entry.th32ProcessID);
            }
        }
        CloseHandle(snapshot);
    }
    struct context { std::vector<DWORD>* family; int count; } c{&family, 0};
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto* c = reinterpret_cast<context*>(parameter);
        DWORD process{};
        GetWindowThreadProcessId(window, &process);
        if (IsWindowVisible(window) && std::find(c->family->begin(), c->family->end(), process) != c->family->end()) ++c->count;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&c));
    return c.count;
}

// Serves window requests of `opener` (policy track) as tracked popups: each gets its own view, created from the decision
// (inheriting the opener's snapshot), and is allowed into it. Runs for `milliseconds`.
void serve_tracked_popups(world& w, view_state& opener, std::vector<std::unique_ptr<view_state>>& popups, int milliseconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    size_t handled = 0;
    while (std::chrono::steady_clock::now() < end) {
        const auto left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count());
        pump_until([&] { return opener.popups.size() > handled; }, std::max(left, 1));
        while (handled < opener.popups.size()) {
            auto* decision = opener.popups[handled++];
            auto popup = std::make_unique<view_state>();
            neoastra_decision_response_t response{};
            response.size = sizeof(response); response.version = 1;
            if (create_view(w, *popup, nullptr, decision) == NEOASTRA_OK) { response.action = NEOASTRA_DECISION_ALLOW; response.target_view = popup->view; }
            else response.action = NEOASTRA_DECISION_CANCEL;
            CHECK(neoastra_decision_complete(decision, &response, nullptr) == NEOASTRA_OK);
            neoastra_decision_release(decision);
            if (popup->view) popups.push_back(std::move(popup));
        }
    }
}

// One opener page (/opener): six ticks, each first calls window.open(), then GET, POST and an image to the refused port B and
// a fetch to /op_ok. `target` 0: the popup address is on B (refused by the snapshot); 1: an allowed popup page (/popup).
struct popup_case_result { uint64_t opener_b{}, popup_b{}, popup_page{}, popup_ok{}, ok{}, port_b{}, bytes_b{}; int visible{}; std::string detail; };
popup_case_result run_popup_case(world& w, const char* name, popup_policy policy, int target, bool filtered) {
    view_state opener;
    opener.policy = policy;
    const int m = g_host->take_port(), b = g_host->take_port();
    const std::vector<int> initial{b};
    CHECK(create_view(w, opener, filtered ? &initial : nullptr) == NEOASTRA_OK);
    std::vector<std::unique_ptr<view_state>> popups;
    navigate_and_wait(opener, url(m, "/opener?b=" + std::to_string(b) + "&target=" + std::to_string(target)));
    if (policy == popup_policy::track) serve_tracked_popups(w, opener, popups, 9000);
    else settle(9000);                                        // six hidden-page timer ticks, then the popups' own requests
    popup_case_result result;
    result.opener_b = g_host->requests(b, "/op_get") + g_host->requests(b, "/op_post") + g_host->requests(b, "/op_img") + g_host->requests(b, "/op_open");
    result.popup_b = g_host->requests(b, "/pp_b.json");
    result.popup_page = g_host->requests(m, "/popup");
    result.popup_ok = g_host->requests(m, "/pp_m.json");
    result.ok = g_host->requests(m, "/op_ok");
    result.port_b = g_host->port_requests(b);                 // every path on B, not only the expected ones
    result.bytes_b = g_host->raw_bytes(b) + g_host->partial_bytes(b);
    result.visible = visible_windows_of_test();
    result.detail = g_host->describe_port(b);
    std::printf("  [%s] window requests=%d tracked popups=%zu; B opener=%llu popup=%llu; /op_ok=%llu /popup=%llu /pp_m.json=%llu; visible windows=%d; B:%s\n",
                name, opener.new_window_events, popups.size(), static_cast<unsigned long long>(result.opener_b), static_cast<unsigned long long>(result.popup_b),
                static_cast<unsigned long long>(result.ok), static_cast<unsigned long long>(result.popup_page), static_cast<unsigned long long>(result.popup_ok),
                result.visible, result.detail.c_str());
    for (auto& popup : popups) neoastra_view_release(popup->view);
    for (auto* decision : opener.popups) if (opener.policy != popup_policy::track) neoastra_decision_release(decision);
    neoastra_view_release(opener.view);
    settle(500);
    return result;
}

#if !defined(NEOASTRA_FAILURE_ONLY)
// ---------------------------------------------------------------- tests of the real library
void test_capability(world& w) {
    neoastra_capability_info_t info{};
    info.size = sizeof(info); info.version = 1;
    CHECK(neoastra_environment_get_capability(w.environment, NEOASTRA_CAPABILITY_REQUEST_FILTER, &info) == NEOASTRA_OK);
    const uint64_t base = NEOASTRA_REQUEST_FILTER_SUBRESOURCES | NEOASTRA_REQUEST_FILTER_FRAMES | NEOASTRA_REQUEST_FILTER_PER_VIEW;
    std::printf("  request-filter capability: support=%u flags=%llu\n", static_cast<unsigned>(info.support), static_cast<unsigned long long>(info.flags));
    CHECK(info.support == NEOASTRA_SUPPORT_LIMITED);
    CHECK(info.flags == base); // exactly the baseline: no worker, redirect, websocket or kind claims
    report("capability: LIMITED, exactly Subresources|Frames|PerView");
}

void test_initial_filter_and_isolation(world& w, view_state& a, view_state& b) {
    const auto pa = take_ports(), pb = take_ports();
    const std::vector<int> initial{pa.b};
    CHECK(create_view(w, a, &initial) == NEOASTRA_OK);        // A: initial nonempty filter, before its first navigation
    CHECK(create_view(w, b, nullptr) == NEOASTRA_OK);         // B: unfiltered, same environment and profile
    load_layout(a, pa);
    expect_filtered_layout(pa);
    load_layout(b, pb);
    expect_open_layout(pb);
    report("initial filter; allowed third-origin frame attempts denied requests; A filtered / B unfiltered in one environment and profile");
}

void test_main_document(world& w) {
    view_state a;
    const int blocked = g_host->take_port(), allowed = g_host->take_port();
    const std::vector<int> initial{blocked};
    CHECK(create_view(w, a, &initial) == NEOASTRA_OK);
    navigate_and_wait(a, url(blocked, "/doc_blocked"));
    expect_zero(blocked, "/doc_blocked");
    expect_port_silent(blocked, "refused main document");
    navigate_and_wait(a, url(allowed, "/doc_ok"));
    expect_reached(allowed, "/doc_ok");
    neoastra_view_release(a.view);
    report("main document: refused origin never reaches the host; positive control loads");
}

void test_concurrent_isolation(view_state& a, view_state& b) {
    const auto pa = take_ports(), pb = take_ports();
    CHECK(set_filter(a.view, {pa.b}) == NEOASTRA_OK);
    const int fa = a.navigation_finished.load(), fb = b.navigation_finished.load();
    neoastra_error_t* error{};
    CHECK(neoastra_view_navigate(a.view, sv(url(pa.m, "/doc?b=" + std::to_string(pa.b) + "&t=" + std::to_string(pa.t))), &error) == NEOASTRA_OK);
    CHECK(neoastra_view_navigate(b.view, sv(url(pb.m, "/doc?b=" + std::to_string(pb.b) + "&t=" + std::to_string(pb.t))), &error) == NEOASTRA_OK);
    CHECK(pump_until([&] { return a.navigation_finished.load() > fa && b.navigation_finished.load() > fb; }, 20000));
    CHECK(wait_done(a.view)); CHECK(wait_done(b.view));
    settle(3500);
    expect_filtered_layout(pa);
    expect_open_layout(pb);
    report("concurrent navigation: filtered view refuses, sibling view in the same profile is unaffected");
}

void test_replace_and_clear(view_state& a, view_state& b) {
    const int m = g_host->take_port(), first = g_host->take_port(), second = g_host->take_port();
    CHECK(set_filter(a.view, {first}) == NEOASTRA_OK);
    navigate_and_wait(a, url(m, "/doc_rt"));
    page_fetch(a.view, url(first, "/rt1.json"));
    expect_zero(first, "/rt1.json");
    CHECK(set_filter(a.view, {second}) == NEOASTRA_OK);               // replace: first passes, second refused
    page_fetch(a.view, url(first, "/rt2_first.json"));
    page_fetch(a.view, url(second, "/rt2_second.json"));
    expect_reached(first, "/rt2_first.json");
    expect_zero(second, "/rt2_second.json");
    CHECK(set_filter(a.view, {}) == NEOASTRA_OK);                     // clear
    page_fetch(a.view, url(second, "/rt3_after_clear.json"));
    expect_reached(second, "/rt3_after_clear.json");
    CHECK(set_filter(a.view, {second}) == NEOASTRA_OK);               // install again after clear
    page_fetch(a.view, url(second, "/rt4_again.json"));
    expect_zero(second, "/rt4_again.json");
    // The unfiltered sibling never sees any of it.
    navigate_and_wait(b, url(m, "/doc_rt_b"));
    page_fetch(b.view, url(second, "/rt5_sibling.json"));
    expect_reached(second, "/rt5_sibling.json");
    report("runtime replace / clear / re-install on a loaded page; stable engine registration, sibling unaffected");
}

void test_unsupported_retains_snapshot(view_state& a) {
    const int m = g_host->take_port(), blocked = g_host->take_port();
    CHECK(set_filter(a.view, {blocked}) == NEOASTRA_OK);
    navigate_and_wait(a, url(m, "/doc_unsupported"));
    const auto rules = loopback_rules({blocked});
    for (const uint64_t flag : {static_cast<uint64_t>(NEOASTRA_REQUEST_FILTER_WEBSOCKETS), static_cast<uint64_t>(NEOASTRA_REQUEST_FILTER_REDIRECTS),
                                static_cast<uint64_t>(NEOASTRA_REQUEST_FILTER_SERVICE_WORKERS), static_cast<uint64_t>(NEOASTRA_REQUEST_FILTER_SHARED_WORKERS),
                                static_cast<uint64_t>(NEOASTRA_REQUEST_FILTER_DEDICATED_WORKERS), static_cast<uint64_t>(NEOASTRA_REQUEST_FILTER_RESOURCE_KINDS)}) {
        const auto filter = describe(rules, flag);
        completion done;
        neoastra_error_t* error{};
        const auto result = neoastra_view_set_request_filter_async(a.view, &filter, on_completion, &done, nullptr, &error);
        if (error) neoastra_error_release(error);
        CHECK(result == NEOASTRA_ERROR_NOT_SUPPORTED);
        CHECK(!done.done && done.calls == 0);
    }
    auto websocket_rules = loopback_rules({blocked});
    websocket_rules[0].schemes = 4; // ws
    { const auto filter = describe(websocket_rules); completion done; neoastra_error_t* error{};
      const auto result = neoastra_view_set_request_filter_async(a.view, &filter, on_completion, &done, nullptr, &error);
      if (error) neoastra_error_release(error); CHECK(result == NEOASTRA_ERROR_NOT_SUPPORTED); }
    auto kind_rules = loopback_rules({blocked});
    kind_rules[0].kinds = 1u << NEOASTRA_RESOURCE_IMAGE;
    { const auto filter = describe(kind_rules); completion done; neoastra_error_t* error{};
      const auto result = neoastra_view_set_request_filter_async(a.view, &filter, on_completion, &done, nullptr, &error);
      if (error) neoastra_error_release(error); CHECK(result == NEOASTRA_ERROR_NOT_SUPPORTED); }
    // The old snapshot is retained after every rejection.
    page_fetch(a.view, url(blocked, "/unsupported_after.json"));
    expect_zero(blocked, "/unsupported_after.json");
    expect_port_silent(blocked, "after rejected updates");
    report("unsupported requirements are rejected without callback and retain the previous snapshot");
}

void test_cancel_and_dispose(world& w, view_state& a) {
    const int m = g_host->take_port(), first = g_host->take_port(), second = g_host->take_port();
    CHECK(set_filter(a.view, {first}) == NEOASTRA_OK);
    navigate_and_wait(a, url(m, "/doc_cancel"));
    {
        const auto rules = loopback_rules({second});
        const auto filter = describe(rules);
        completion done;
        neoastra_operation_t* operation{};
        neoastra_error_t* error{};
        CHECK(neoastra_view_set_request_filter_async(a.view, &filter, on_completion, &done, &operation, &error) == NEOASTRA_OK);
        if (error) neoastra_error_release(error);
        neoastra_operation_cancel(operation);
        CHECK(pump_until([&] { return done.done; }, 15000));
        CHECK(done.calls == 1);
        CHECK(done.result == NEOASTRA_OK || done.result == NEOASTRA_ERROR_CANCELED);
        std::printf("  cancel raced with installation: result=%d\n", static_cast<int>(done.result));
        page_fetch(a.view, url(first, "/cancel_first.json"));
        page_fetch(a.view, url(second, "/cancel_second.json"));
        if (done.result == NEOASTRA_ERROR_CANCELED) { expect_zero(first, "/cancel_first.json"); expect_reached(second, "/cancel_second.json"); }
        else { expect_reached(first, "/cancel_first.json"); expect_zero(second, "/cancel_second.json"); }
        neoastra_operation_release(operation);
    }
    {
        view_state doomed;
        const std::vector<int> initial{first};
        CHECK(create_view(w, doomed, &initial) == NEOASTRA_OK);
        const auto rules = loopback_rules({second});
        const auto filter = describe(rules);
        completion done;
        neoastra_error_t* error{};
        CHECK(neoastra_view_set_request_filter_async(doomed.view, &filter, on_completion, &done, nullptr, &error) == NEOASTRA_OK);
        if (error) neoastra_error_release(error);
        neoastra_view_release(doomed.view); // dispose with the update still queued
        CHECK(pump_until([&] { return done.done; }, 15000));
        CHECK(done.calls == 1);
        CHECK(done.result == NEOASTRA_OK || done.result == NEOASTRA_ERROR_DISPOSED || done.result == NEOASTRA_ERROR_CANCELED);
        std::printf("  dispose with queued update: result=%d\n", static_cast<int>(done.result));
    }
    report("cancel and dispose: exactly one completion, local snapshot consistent with the result");
}

void test_custom_scheme(world& w) {
    view_state a;
    const int m = g_host->take_port(), blocked = g_host->take_port();
    const std::vector<int> initial{blocked};
    CHECK(create_view(w, a, &initial) == NEOASTRA_OK);
    navigate_and_wait(a, "neoapp://app/index?m=" + std::to_string(m) + "&b=" + std::to_string(blocked));
    CHECK(wait_done(a.view));
    settle(800);
    CHECK(w.provider->calls.load() >= 1);                       // the custom scheme was served by its provider
    expect_reached(m, "/cs_m.json");                           // positive control from the custom-scheme page
    expect_zero(blocked, "/cs_b.json");
    expect_port_silent(blocked, "custom-scheme page");
    CHECK(eval(a.view, "location.protocol").find("neoapp") != std::string::npos);
    neoastra_view_release(a.view);
    report("custom scheme coexists with an active filter (provider served, http target refused)");
}

void open_popup(view_state& opener, const std::string& address, size_t expected_events) {
    eval(opener.view, "window.open('" + address + "');0");
    CHECK(pump_until([&] { return opener.popups.size() >= expected_events; }, 15000));
}

void test_popup_inheritance(world& w) {
    view_state opener;
    const int m = g_host->take_port(), blocked = g_host->take_port();
    const std::vector<int> initial{blocked};
    CHECK(create_view(w, opener, &initial) == NEOASTRA_OK);
    navigate_and_wait(opener, url(m, "/doc_opener"));
    open_popup(opener, url(m, "/popup?b=" + std::to_string(blocked)), 1);
    view_state popup;
    CHECK(create_view(w, popup, nullptr, opener.popups[0]) == NEOASTRA_OK);   // inherits the opener's snapshot
    neoastra_decision_response_t response{};
    response.size = sizeof(response); response.version = 1; response.action = NEOASTRA_DECISION_ALLOW; response.target_view = popup.view;
    CHECK(neoastra_decision_complete(opener.popups[0], &response, nullptr) == NEOASTRA_OK);
    neoastra_decision_release(opener.popups[0]);
    CHECK(wait_done(popup.view));
    settle(1500);
    expect_reached(m, "/popup");
    expect_reached(m, "/pp_m.json");        // positive control inside the popup
    expect_zero(blocked, "/pp_b.json");     // inherited snapshot, effective from the popup's first request
    expect_port_silent(blocked, "popup inherited");
    // Independence: clearing the popup does not clear the opener, and vice versa.
    CHECK(set_filter(popup.view, {}) == NEOASTRA_OK);
    page_fetch(popup.view, url(blocked, "/pp_after_clear.json"));
    expect_reached(blocked, "/pp_after_clear.json");
    page_fetch(opener.view, url(blocked, "/op_still_blocked.json"));
    expect_zero(blocked, "/op_still_blocked.json");
    const int other = g_host->take_port();
    CHECK(set_filter(popup.view, {other}) == NEOASTRA_OK);
    page_fetch(popup.view, url(other, "/pp_own_filter.json"));
    expect_zero(other, "/pp_own_filter.json");
    page_fetch(opener.view, url(other, "/op_other_passes.json"));
    expect_reached(other, "/op_other_passes.json");
    neoastra_view_release(popup.view);
    neoastra_view_release(opener.view);
    report("tracked popup inherits the snapshot before its first request, then updates independently");
}

void test_deferred_fallback(world& w) {
    view_state a;
    const int m = g_host->take_port(), unrelated = g_host->take_port(), later_refused = g_host->take_port();
    const std::vector<int> initial{unrelated};
    CHECK(create_view(w, a, &initial) == NEOASTRA_OK);
    navigate_and_wait(a, url(m, "/doc_df"));
    neoastra_decision_response_t allow{};
    allow.size = sizeof(allow); allow.version = 1; allow.action = NEOASTRA_DECISION_ALLOW;
    // Control: a deferred fallback navigation to an allowed address loads.
    open_popup(a, url(m, "/doc_df_ok"), 1);
    CHECK(neoastra_decision_complete(a.popups[0], &allow, nullptr) == NEOASTRA_OK);
    neoastra_decision_release(a.popups[0]);
    CHECK(pump_until([&] { return g_host->requests(m, "/doc_df_ok") >= 1; }, 15000));
    // Deferred: the snapshot changes while the decision is pending; the fallback must use the CURRENT snapshot.
    navigate_and_wait(a, url(m, "/doc_df2"));
    open_popup(a, url(later_refused, "/doc_df_late"), 2);
    CHECK(set_filter(a.view, {later_refused}) == NEOASTRA_OK);
    CHECK(neoastra_decision_complete(a.popups[1], &allow, nullptr) == NEOASTRA_OK);
    neoastra_decision_release(a.popups[1]);
    settle(2000);
    expect_zero(later_refused, "/doc_df_late");
    expect_port_silent(later_refused, "deferred fallback");
    // A new-window address that the snapshot already refuses never reaches the host.
    const int events = a.new_window_events;
    eval(a.view, "window.open('" + url(later_refused, "/doc_df_direct") + "');0");
    settle(1500);
    CHECK(a.new_window_events == events);
    expect_zero(later_refused, "/doc_df_direct");
    neoastra_view_release(a.view);
    report("deferred new-window fallback and direct popup request use the current snapshot");
}

// The decision no longer keeps its opener alive: releasing the last client handle disposes the view at once, which
// abandons its pending decisions with their default action. A late ALLOW then finds a settled decision.
void test_opener_release_with_deferred_popup(world& w) {
    view_state opener;
    const int m = g_host->take_port(), unrelated = g_host->take_port();
    const std::vector<int> initial{unrelated};
    CHECK(create_view(w, opener, &initial) == NEOASTRA_OK);
    navigate_and_wait(opener, url(m, "/doc_late"));
    open_popup(opener, url(m, "/doc_late_target"), 1);
    auto* decision = opener.popups[0];
    const auto released_at = std::chrono::steady_clock::now();
    neoastra_view_release(opener.view);       // last client handle; the decision is still deferred
    opener.view = nullptr;
    settle(1000);
    neoastra_decision_response_t allow{};
    allow.size = sizeof(allow); allow.version = 1; allow.action = NEOASTRA_DECISION_ALLOW;
    const auto late = neoastra_decision_complete(decision, &allow, nullptr);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - released_at).count();
    std::printf("  late ALLOW after releasing the opener: result=%d after %lld ms\n", static_cast<int>(late), static_cast<long long>(elapsed_ms));
    CHECK(late != NEOASTRA_OK);               // already terminal (abandoned with its default action), promptly and not at the decision timeout
    settle(1500);
    expect_zero(m, "/doc_late_target");       // no fallback navigation, no resurrection of the view
    neoastra_decision_release(decision);
    report("opener released with a deferred popup outstanding: prompt disposal, late ALLOW finds a terminal decision, no fallback request");
}

// Window requests of filtered views never become visible windows and never let the opener's requests through: refused
// addresses and host-refused or unhandled requests are contained, tracked popups are filtered from their assignment on.
void test_popups_of_filtered_views(world& w) {
    const auto silent = [](const popup_case_result& r) { ++g_checks; CHECK(r.port_b == 0 && r.bytes_b == 0 && r.visible == 0); };
    auto r = run_popup_case(w, "popup address refused by the filter", popup_policy::defer, 0, true);
    silent(r); CHECK(r.ok >= 6);
    r = run_popup_case(w, "host cancels the popups", popup_policy::cancel, 1, true);
    silent(r); CHECK(r.ok >= 6); CHECK(r.popup_page == 0); // a host-refused popup does not navigate
    r = run_popup_case(w, "host leaves the popups unhandled", popup_policy::unhandled, 1, true);
    silent(r); CHECK(r.ok >= 6); CHECK(r.popup_page == 0);
    r = run_popup_case(w, "tracked popups", popup_policy::track, 1, true);
    silent(r); CHECK(r.ok >= 6); CHECK(r.popup_page >= 1 && r.popup_ok >= 1); // the popups load and run, filtered
    r = run_popup_case(w, "allowed without a target view (fallback navigation)", popup_policy::fallback, 1, true);
    silent(r); CHECK(r.popup_page >= 1);                    // the opener itself navigated to the popup page
    // Control: an unfiltered opener keeps its behavior; its requests reach B and a cancelled popup does not navigate.
    r = run_popup_case(w, "unfiltered opener, host cancels (control)", popup_policy::cancel, 1, false);
    ++g_checks; CHECK(r.opener_b == 18 && r.popup_page == 0 && r.visible == 0);
    report("popups of filtered views: refused, cancelled, unhandled, tracked and fallback requests keep the opener filtered; no visible window");
}

// The standalone bypass pattern inside NeoAstra: page and frame call window.open() to the denied origin in every timer task
// while they request it with fetch, keepalive POST, beacon and images. Asserted while the view lives and after its disposal,
// past the 3 s at which the backend closes a filtered view at the latest.
void test_window_open_traffic(world& w) {
    view_state a;
    const int m = g_host->take_port(), b = g_host->take_port();
    const std::vector<int> initial{b};
    CHECK(create_view(w, a, &initial) == NEOASTRA_OK);
    navigate_and_wait(a, url(m, "/storm?open=1&b=" + std::to_string(b)));
    CHECK(pump_until([&] { return g_host->requests(m, "/sm_m.json") >= 5 && g_host->requests(m, "/sf_m.json") >= 5; }, 30000));
    std::printf("  window.open traffic while alive: allowed page=%llu frame=%llu; denied origin:%s; visible windows=%d\n",
                static_cast<unsigned long long>(g_host->requests(m, "/sm_m.json")), static_cast<unsigned long long>(g_host->requests(m, "/sf_m.json")),
                g_host->describe_port(b).c_str(), visible_windows_of_test());
    expect_port_silent(b, "denied origin with window.open traffic, view alive");
    ++g_checks; CHECK(visible_windows_of_test() == 0);
    neoastra_view_release(a.view);
    settle(4000);
    expect_port_silent(b, "denied origin with window.open traffic, after disposal");
    ++g_checks; CHECK(visible_windows_of_test() == 0);
    report("window.open in every task of page and frame: the denied origin stays silent while the view lives and after its disposal");
}

// A filtered view disposed while its page and a frame keep requesting the denied origin (fetch, keepalive POST, beacon, image,
// more of them from pagehide), half of the rounds with its own window destroyed right after it, and six more rounds whose
// pages also call window.open() in every task and in pagehide: its document is unloaded before its controller closes, so
// nothing may reach the denied origin, also not during or after the disposal.
void test_disposal_during_traffic(world& w) {
    const int leaking = dispose_during_traffic(w, "disposal during traffic", 6, false) +
                        dispose_during_traffic(w, "disposal during traffic", 6, true);
    ++g_checks;
    CHECK(leaking == 0);
    report("disposal during traffic: the denied origin stays silent during and after the disposal");
}

void test_lifetime(world& w) {
    for (int round = 0; round < 3; ++round) {
        view_state a;
        const int m = g_host->take_port(), b = g_host->take_port();
        const std::vector<int> initial{b};
        CHECK(create_view(w, a, &initial) == NEOASTRA_OK);
        navigate_and_wait(a, url(m, "/doc_life"));
        CHECK(set_filter(a.view, {b, m}) == NEOASTRA_OK);
        neoastra_view_release(a.view);
    }
    view_state plain;
    const int m = g_host->take_port();
    CHECK(create_view(w, plain, nullptr) == NEOASTRA_OK);
    navigate_and_wait(plain, url(m, "/doc_life_after"));
    expect_reached(m, "/doc_life_after");
    neoastra_view_release(plain.view);
    report("lifetime: repeated filtered view create/update/dispose; a later plain view in the same profile still loads");
}
#endif

#ifdef NEOASTRA_FAILURE_ONLY
// ---------------------------------------------------------------- failure injection while refusing (test-hooks library)
// NEOASTRA_TEST_RF_FAIL="point=count,..." fails the first `count` (or `all`) attempts at one point of the backend:
//   create / assign            the fresh empty response of a refusal
//   prepared / assign_prepared the response prepared when the filter was installed
//   neutralize                 rewriting the request itself to an address that fails inside the engine
//   prepare                    preparing that response while a filter is installed (installation must fail, not degrade)
//   new_window                 setting up the decision of a new-window request (allocation failure)
// Every refusal case asserts the silence of the denied origin while the view lives AND after its disposal.
void inject(const char* spec) { CHECK(SetEnvironmentVariableA("NEOASTRA_TEST_RF_FAIL", spec)); }

// `--case <text>` runs only the cases whose name contains the text (to reproduce one case on its own).
const char* g_case_filter{};
bool selected(const char* name) { return !g_case_filter || std::strstr(name, g_case_filter) != nullptr; }

void expect_sibling_works(view_state& sibling) {
    const int m = g_host->take_port();
    navigate_and_wait(sibling, url(m, "/doc_healthy"));
    expect_reached(m, "/doc_healthy");
    CHECK(eval(sibling.view, "1") == "1");
}

// Disposes the view, then observes the denied origin for a bounded window: a refusal must survive the teardown.
void dispose_and_expect_silent(view_state& state, int port, const char* name) {
    neoastra_view_release(state.view);
    state.view = nullptr;
    settle(4000);                                             // past the 3 s at which a filtered view closes at the latest
    std::printf("    after disposal, denied-origin requests:%s\n", g_host->describe_port(port).c_str());
    expect_port_silent(port, name);
}

// Documents, frames, subresources and POSTs of the full layout while the injected failures are active.
void run_layout_case(world& w, const char* name, const char* spec) {
    if (!selected(name)) return;
    const auto p = take_ports();
    view_state healthy;                                       // sibling in the same environment and profile, never filtered
    CHECK(create_view(w, healthy, nullptr) == NEOASTRA_OK);
    view_state a;
    const std::vector<int> initial{p.b};
    CHECK(create_view(w, a, &initial) == NEOASTRA_OK);
    inject(spec);
    neoastra_error_t* navigate_error{};
    CHECK(neoastra_view_navigate(a.view, sv(url(p.m, "/doc?b=" + std::to_string(p.b) + "&t=" + std::to_string(p.t))), &navigate_error) == NEOASTRA_OK);
    const bool finished = wait_done(a.view, 20000);           // a refused request fails; nothing is held back
    settle(3500);
    inject("");
    std::printf("  [%s] spec='%s': page finished=%d, denied-origin requests:%s\n", name, spec, finished ? 1 : 0, g_host->describe_port(p.b).c_str());
    expect_filtered_layout(p);                                // every positive control arrived, the denied origin stayed silent
    CHECK(finished);
    CHECK(a.quarantined.load() == 0);                         // no recreate request: the view stays usable
    expect_sibling_works(healthy);
    dispose_and_expect_silent(a, p.b, name);
    neoastra_view_release(healthy.view);
    report(name);
}

// Refused main-frame navigations: host-initiated, script-initiated and a form POST. Their requests take the path of a
// refused navigation in the resource handler, which must use the same refusal (and the same fallbacks) as the filter.
void run_document_case(world& w, const char* name, const char* spec) {
    if (!selected(name)) return;
    view_state healthy;
    CHECK(create_view(w, healthy, nullptr) == NEOASTRA_OK);
    view_state a;
    const int allowed = g_host->take_port(), blocked = g_host->take_port();
    const std::vector<int> initial{blocked};
    CHECK(create_view(w, a, &initial) == NEOASTRA_OK);
    navigate_and_wait(a, url(allowed, "/doc_start"));
    inject(spec);
    navigate_and_wait(a, url(blocked, "/doc_blocked"));
    eval(a.view, "location.href='" + url(blocked, "/doc_blocked_script") + "';0");
    settle(1500);
    navigate_and_wait(a, url(allowed, "/doc_form"));
    eval(a.view, "const f=document.createElement('form');f.method='POST';f.action='" + url(blocked, "/doc_blocked_post") +
                 "';const i=document.createElement('input');i.name='payload';i.value='PAYLOAD';f.appendChild(i);document.body.appendChild(f);f.submit();0");
    settle(1500);
    inject("");
    std::printf("  [%s] spec='%s': denied-origin requests:%s\n", name, spec, g_host->describe_port(blocked).c_str());
    expect_zero(blocked, "/doc_blocked");
    expect_zero(blocked, "/doc_blocked_script");
    expect_zero(blocked, "/doc_blocked_post");
    expect_port_silent(blocked, name);
    CHECK(a.quarantined.load() == 0);
    navigate_and_wait(a, url(allowed, "/doc_after"));          // the view still navigates
    expect_reached(allowed, "/doc_after");
    expect_sibling_works(healthy);
    dispose_and_expect_silent(a, blocked, name);
    neoastra_view_release(healthy.view);
    report(name);
}

// Preparing the refusal response is part of installing a filter: a failure there fails the installation and keeps the
// previous state, instead of reporting success with a missing fallback.
void test_prepare_failure(world& w) {
    const int m = g_host->take_port(), b = g_host->take_port(), b2 = g_host->take_port();
    inject("prepare=all");
    view_state refused;
    const std::vector<int> initial{b};
    CHECK(create_view(w, refused, &initial) != NEOASTRA_OK);  // fail closed: no unfiltered replacement view
    CHECK(refused.view == nullptr);
    view_state a;
    CHECK(create_view(w, a, nullptr) == NEOASTRA_OK);
    CHECK(set_filter(a.view, {b}) != NEOASTRA_OK);
    inject("");
    navigate_and_wait(a, url(m, "/doc_prepare"));
    page_fetch(a.view, url(b, "/prepare_previous.json"));
    expect_reached(b, "/prepare_previous.json");              // the previous (empty) snapshot is still the installed one
    CHECK(set_filter(a.view, {b2}) == NEOASTRA_OK);           // a later installation succeeds and refuses
    page_fetch(a.view, url(b2, "/prepare_after.json"));
    expect_zero(b2, "/prepare_after.json");
    dispose_and_expect_silent(a, b2, "prepare failure");
    // A tracked popup inherits its opener's snapshot and prepares during creation: a failure there fails the popup.
    view_state opener;
    const int pm = g_host->take_port(), pb = g_host->take_port();
    const std::vector<int> opener_filter{pb};
    CHECK(create_view(w, opener, &opener_filter) == NEOASTRA_OK);
    navigate_and_wait(opener, url(pm, "/doc_opener_prepare"));
    eval(opener.view, "window.open('" + url(pm, "/popup?b=" + std::to_string(pb)) + "');0");
    CHECK(pump_until([&] { return !opener.popups.empty(); }, 15000));
    inject("prepare=all");
    view_state popup;
    CHECK(create_view(w, popup, nullptr, opener.popups[0]) != NEOASTRA_OK);
    inject("");
    CHECK(popup.view == nullptr);
    neoastra_decision_response_t cancel{};
    cancel.size = sizeof(cancel); cancel.version = 1; cancel.action = NEOASTRA_DECISION_CANCEL;
    (void)neoastra_decision_complete(opener.popups[0], &cancel, nullptr);
    neoastra_decision_release(opener.popups[0]);
    settle(1500);
    expect_zero(pm, "/popup");                                // neither a popup nor a fallback navigation
    dispose_and_expect_silent(opener, pb, "popup preparation failure");
    report("preparation failure: initial filter, update and inherited popup fail closed; the previous state stays usable");
}

// A window request of a filtered view that cannot be contained (no popup sink could be created, or the limit of sinks of
// the view or of the application is reached) leaves the opener fail-closed: it is cut off from the network before the
// request is answered, and once that is confirmed the library logs "Request filter fail-closed". Not one request or byte may
// reach the refused port, and no window may become visible. `cap_variable` names the limit that `cap` overrides.
void run_uncontained_popup_case(world& w, const char* name, const char* spec, const char* cap_variable, const char* cap) {
    if (!selected(name)) return;
    inject(spec);
    if (cap) CHECK(SetEnvironmentVariableA(cap_variable, cap));
    const int before = g_fail_closed.load(), degraded_before = g_degraded.load();
    const auto r = run_popup_case(w, name, popup_policy::defer, 0, true);
    inject("");
    if (cap) CHECK(SetEnvironmentVariableA(cap_variable, nullptr));
    const int notified = g_fail_closed.load() - before;
    std::printf("  [%s] fail-closed notifications=%d\n", name, notified);
    g_checks += 4;
    CHECK(notified >= 1);                                     // never silent
    CHECK(g_degraded.load() == degraded_before);
    CHECK(r.port_b == 0 && r.bytes_b == 0);
    CHECK(r.visible == 0);
    report(name);
}

// LIMIT, shown rather than hidden: no popup sink, and the opener cannot be cut off from the network either, because the
// offline switch cannot be sent ("cut_off") or its completion reports a failure ("cut_off_async"). The request is answered
// without a window, which must be notified ("Request filter degraded", never "fail-closed"); what then reaches the refused
// port is printed as the observed limit and not asserted either way.
void run_degraded_popup_case(world& w, const char* name, const char* spec) {
    if (!selected(name)) return;
    inject(spec);
    const int before = g_degraded.load(), fail_closed_before = g_fail_closed.load();
    const auto r = run_popup_case(w, name, popup_policy::defer, 0, true);
    inject("");
    const int notified = g_degraded.load() - before;
    std::printf("  LIMIT [%s]: degraded notifications=%d fail-closed=%d; refused port received opener=%llu popup=%llu (observed, not asserted)\n",
                name, notified, g_fail_closed.load() - fail_closed_before, static_cast<unsigned long long>(r.opener_b), static_cast<unsigned long long>(r.popup_b));
    g_checks += 3;
    CHECK(notified >= 1);                                     // never silent
    CHECK(g_fail_closed.load() == fail_closed_before);        // never reported as cut off
    CHECK(r.visible == 0);
    report(name);
}

void hold_popup_sinks(const char* cap, const char* lifetime_ms) {
    CHECK(SetEnvironmentVariableA("NEOASTRA_TEST_SINK_HOLD", "1"));
    CHECK(SetEnvironmentVariableA("NEOASTRA_TEST_SINK_CAP", cap));
    CHECK(SetEnvironmentVariableA("NEOASTRA_TEST_SINK_LIFETIME_MS", lifetime_ms));
}
void release_popup_sinks() {
    CHECK(SetEnvironmentVariableA("NEOASTRA_TEST_SINK_HOLD", nullptr));
    CHECK(SetEnvironmentVariableA("NEOASTRA_TEST_SINK_CAP", nullptr));
    CHECK(SetEnvironmentVariableA("NEOASTRA_TEST_SINK_LIFETIME_MS", nullptr));
}
// The address must be allowed by the opener's filter: a refused one is answered by the backend without reaching the host.
void open_window(view_state& opener, const std::string& address) {
    const int before = opener.new_window_events;
    eval(opener.view, "window.open('" + address + "');0");
    CHECK(pump_until([&] { return opener.new_window_events > before; }, 15000));
}

// The limit of popup sinks holds per view: a filtered view that exhausts its own fails closed (and is offline from then on),
// while a filtered sibling in the same profile still gets its window requests contained and stays online. Sinks are held
// past their first navigation, so that they still count.
void test_sink_limit_per_view(world& w) {
    hold_popup_sinks("2", "20000");
    view_state a, sibling;
    a.policy = sibling.policy = popup_policy::cancel;
    const int m = g_host->take_port(), b = g_host->take_port(), m2 = g_host->take_port();
    const std::vector<int> initial{b};
    CHECK(create_view(w, a, &initial) == NEOASTRA_OK);
    CHECK(create_view(w, sibling, &initial) == NEOASTRA_OK);
    navigate_and_wait(a, url(m, "/doc_limit_a"));
    navigate_and_wait(sibling, url(m, "/doc_limit_sibling"));
    const int before = g_fail_closed.load();
    for (int i = 0; i < 3; ++i) { open_window(a, url(m, "/limit_a")); settle(300); }
    settle(1000);
    const int after_a = g_fail_closed.load() - before;
    for (int i = 0; i < 2; ++i) { open_window(sibling, url(m, "/limit_sibling")); settle(300); }
    settle(1000);
    const int after_sibling = g_fail_closed.load() - before;
    std::printf("  [sink limit per view] fail-closed after the view's third request=%d, after the sibling's two=%d\n", after_a, after_sibling);
    g_checks += 2;
    CHECK(after_a == 1);
    CHECK(after_sibling == 1);
    navigate_and_wait(sibling, url(m2, "/doc_limit_sibling_online"));
    expect_reached(m2, "/doc_limit_sibling_online");           // the sibling stays online
    navigate_and_wait(a, url(m2, "/doc_limit_a_offline"));
    expect_zero(m2, "/doc_limit_a_offline");                   // the view that failed closed loads nothing any more
    expect_zero(m, "/limit_a");                                // the sinks cancelled the popups' navigations
    expect_zero(m, "/limit_sibling");
    expect_port_silent(b, "sink limit per view");
    neoastra_view_release(a.view);
    neoastra_view_release(sibling.view);
    release_popup_sinks();
    settle(500);
    report("sink limit per view: the exhausted view fails closed and goes offline; a filtered sibling keeps its sinks and stays online");
}

// The limit of popup sinks of the application counts the sinks of every view: once two views hold it together, the next
// window request fails its opener closed (and offline), while the view that holds sinks stays online.
void test_sink_limit_per_app(world& w) {
    hold_popup_sinks("16", "20000");
    CHECK(SetEnvironmentVariableA("NEOASTRA_TEST_SINK_APP_CAP", "2"));
    view_state a, sibling;
    a.policy = sibling.policy = popup_policy::cancel;
    const int m = g_host->take_port(), b = g_host->take_port(), m2 = g_host->take_port();
    const std::vector<int> initial{b};
    CHECK(create_view(w, a, &initial) == NEOASTRA_OK);
    CHECK(create_view(w, sibling, &initial) == NEOASTRA_OK);
    navigate_and_wait(a, url(m, "/doc_app_limit_a"));
    navigate_and_wait(sibling, url(m, "/doc_app_limit_sibling"));
    const int before = g_app_sink_limit.load(), fail_closed_before = g_fail_closed.load();
    for (int i = 0; i < 2; ++i) { open_window(a, url(m, "/app_limit_a")); settle(300); }
    settle(1000);
    const int after_a = g_app_sink_limit.load() - before;
    open_window(sibling, url(m, "/app_limit_sibling"));
    settle(1000);
    const int after_sibling = g_app_sink_limit.load() - before;
    std::printf("  [sink limit per application] limit reached after the view's two requests=%d, after the sibling's one=%d; fail-closed=%d\n",
                after_a, after_sibling, g_fail_closed.load() - fail_closed_before);
    g_checks += 3;
    CHECK(after_a == 0);
    CHECK(after_sibling == 1);
    CHECK(g_fail_closed.load() - fail_closed_before == 1);
    navigate_and_wait(a, url(m2, "/doc_app_limit_a_online"));
    expect_reached(m2, "/doc_app_limit_a_online");             // the view that holds the sinks stays online
    navigate_and_wait(sibling, url(m2, "/doc_app_limit_sibling_offline"));
    expect_zero(m2, "/doc_app_limit_sibling_offline");         // the view that hit the limit failed closed
    expect_zero(m, "/app_limit_a");
    expect_zero(m, "/app_limit_sibling");
    expect_port_silent(b, "sink limit per application");
    neoastra_view_release(a.view);
    neoastra_view_release(sibling.view);
    CHECK(SetEnvironmentVariableA("NEOASTRA_TEST_SINK_APP_CAP", nullptr));
    release_popup_sinks();
    settle(500);
    report("sink limit per application: counted over all views; the view that reaches it fails closed, the others stay online");
}

// A popup sink whose controller is never created (injected) fails its opener closed when its creation deadline (300 ms here)
// expires. The popup's address never loads, no window becomes visible, the refused port stays silent. A deadline that a
// real creation outruns cannot be shown: WM_TIMER comes only when no other message waits.
void test_sink_creation_deadline(world& w) {
    CHECK(SetEnvironmentVariableA("NEOASTRA_TEST_SINK_CREATION_MS", "300"));
    view_state a;
    a.policy = popup_policy::cancel;
    const int m = g_host->take_port(), b = g_host->take_port();
    const std::vector<int> initial{b};
    CHECK(create_view(w, a, &initial) == NEOASTRA_OK);
    navigate_and_wait(a, url(m, "/doc_creation_deadline"));
    const int timeouts = g_creation_timeouts.load(), fail_closed = g_fail_closed.load();
    inject("sink_never=1");
    open_window(a, url(m, "/creation_deadline"));
    settle(2000);
    inject("");
    std::printf("  [sink creation deadline] timeouts=%d fail-closed=%d /creation_deadline=%llu visible windows=%d\n",
                g_creation_timeouts.load() - timeouts, g_fail_closed.load() - fail_closed,
                static_cast<unsigned long long>(g_host->requests(m, "/creation_deadline")), visible_windows_of_test());
    g_checks += 3;
    CHECK(g_creation_timeouts.load() - timeouts == 1);
    CHECK(g_fail_closed.load() - fail_closed == 1);
    CHECK(visible_windows_of_test() == 0);
    CHECK(eval(a.view, "1") == "1");                           // no crash, the view still answers
    expect_zero(m, "/creation_deadline");
    expect_port_silent(b, "sink creation deadline");
    neoastra_view_release(a.view);
    CHECK(SetEnvironmentVariableA("NEOASTRA_TEST_SINK_CREATION_MS", nullptr));
    settle(500);
    report("sink creation deadline: a sink whose controller never comes fails its opener closed");
}

// Popup sinks closed by their timer (held past their first navigation, lifetime 50 ms) while another window request is being
// decided: a timer of a closed sink closes nothing else and expires no decision (a stale sink timer once crashed).
void test_sink_timers(world& w) {
    hold_popup_sinks("16", "50");
    view_state a, deciding;
    a.policy = popup_policy::cancel;
    const int m = g_host->take_port(), b = g_host->take_port();
    const std::vector<int> initial{b};
    CHECK(create_view(w, a, &initial) == NEOASTRA_OK);
    CHECK(create_view(w, deciding, &initial) == NEOASTRA_OK);
    navigate_and_wait(a, url(m, "/doc_timers_a"));
    navigate_and_wait(deciding, url(m, "/doc_timers_deciding"));
    open_window(deciding, url(m, "/timers_deciding"));         // deferred: its decision waits with a timer of its own
    for (int i = 0; i < 6; ++i) { open_window(a, url(m, "/timers_a")); settle(150); }
    settle(1500);
    neoastra_decision_response_t cancel{};
    cancel.size = sizeof(cancel); cancel.version = 1; cancel.action = NEOASTRA_DECISION_CANCEL;
    (void)neoastra_decision_complete(deciding.popups[0], &cancel, nullptr);
    neoastra_decision_release(deciding.popups[0]);
    settle(500);
    CHECK(eval(a.view, "1") == "1");
    CHECK(eval(deciding.view, "1") == "1");
    expect_zero(m, "/timers_a");
    expect_zero(m, "/timers_deciding");
    expect_port_silent(b, "sink timers");
    neoastra_view_release(a.view);
    neoastra_view_release(deciding.view);
    release_popup_sinks();
    settle(500);
    report("sink timers: sinks closed by their timer leave other sinks and pending decisions alone");
}

// The opener is disposed right after its window request, while its popup sink is being created or was just assigned: a sink
// that still holds the request is closed with it (completing that request later once crashed WebView2), an assigned one
// closes by itself. The popup's address never loads, no window becomes visible, and the refused port stays silent.
void test_opener_disposed_while_sink_created(world& w) {
    view_state a;
    a.policy = popup_policy::cancel;
    const int m = g_host->take_port(), b = g_host->take_port();
    const std::vector<int> initial{b};
    CHECK(create_view(w, a, &initial) == NEOASTRA_OK);
    navigate_and_wait(a, url(m, "/doc_disposed_opener"));
    open_window(a, url(m, "/disposed_opener"));
    neoastra_view_release(a.view);
    a.view = nullptr;
    settle(2500);
    std::printf("  [opener disposed while its sink is created] /disposed_opener=%llu visible windows=%d\n",
                static_cast<unsigned long long>(g_host->requests(m, "/disposed_opener")), visible_windows_of_test());
    expect_zero(m, "/disposed_opener");                        // the request was answered without a window
    ++g_checks;
    CHECK(visible_windows_of_test() == 0);
    expect_port_silent(b, "opener disposed while its sink is created");
    report("opener disposed while its popup sink is created: no crash, the refused port stays silent");
}

// A tracked popup whose filter cannot be registered after its assignment is disposed before it loads: it neither loads its
// page nor requests the refused port. The opener's requests are printed as an observation.
void test_tracked_popup_hook_failure(world& w) {
    inject("popup_hook=all");
    const int before = g_popup_hook_errors.load();
    const auto r = run_popup_case(w, "tracked popup filter cannot be registered", popup_policy::track, 1, true);
    inject("");
    std::printf("  [tracked popup filter cannot be registered] errors=%d; opener requests at the refused port=%llu (observed)\n",
                g_popup_hook_errors.load() - before, static_cast<unsigned long long>(r.opener_b));
    ++g_checks;
    CHECK(g_popup_hook_errors.load() - before >= 1);
    CHECK(r.popup_page == 0 && r.popup_b == 0 && r.visible == 0);
    report("tracked popup filter cannot be registered: the popup is disposed before it loads, with an error");
}

// An allocation failure while setting up a new-window decision cancels the request: no popup, no fallback navigation.
void test_new_window_setup_failure(world& w) {
    view_state opener;
    const int m = g_host->take_port(), b = g_host->take_port();
    const std::vector<int> initial{b};
    CHECK(create_view(w, opener, &initial) == NEOASTRA_OK);
    navigate_and_wait(opener, url(m, "/doc_nw_opener"));
    inject("new_window=1");
    eval(opener.view, "window.open('" + url(m, "/doc_nw_target") + "');0");
    settle(2000);
    inject("");
    CHECK(opener.new_window_events == 0);
    expect_zero(m, "/doc_nw_target");
    CHECK(eval(opener.view, "1") == "1");                     // the opener stays usable
    eval(opener.view, "window.open('" + url(m, "/doc_nw_control") + "');0");
    CHECK(pump_until([&] { return opener.new_window_events == 1; }, 15000)); // control: the next request is offered normally
    neoastra_decision_response_t cancel{};
    cancel.size = sizeof(cancel); cancel.version = 1; cancel.action = NEOASTRA_DECISION_CANCEL;
    (void)neoastra_decision_complete(opener.popups[0], &cancel, nullptr);
    neoastra_decision_release(opener.popups[0]);
    dispose_and_expect_silent(opener, b, "new-window setup failure");
    report("new-window setup failure: cancelled without popup or fallback request; the next request is offered");
}

void test_failures() {
    auto w = make_world("failure", false);
    run_layout_case(w, "fresh response construction fails once", "create=1");
    run_layout_case(w, "fresh response assignment fails once", "assign=1");
    run_layout_case(w, "fresh construction fails every time (prepared response)", "create=all");
    run_layout_case(w, "fresh assignment fails every time (prepared response)", "assign=all");
    run_layout_case(w, "construction and prepared response fail every time (neutralized request)", "create=all,prepared=all");
    run_layout_case(w, "both assignments fail every time (neutralized request)", "assign=all,assign_prepared=all");
    run_document_case(w, "refused main navigations, construction and prepared response fail every time", "create=all,prepared=all");
    run_document_case(w, "refused main navigations, both assignments fail every time", "assign=all,assign_prepared=all");
    run_document_case(w, "refused main navigations, fresh construction fails every time", "create=all");
    run_uncontained_popup_case(w, "popup sink cannot be created: the opener fails closed", "sink=all", nullptr, nullptr);
    run_uncontained_popup_case(w, "popup sink limit of the view reached: the opener fails closed", "", "NEOASTRA_TEST_SINK_CAP", "0");
    run_uncontained_popup_case(w, "popup sink limit of the application reached: the opener fails closed", "", "NEOASTRA_TEST_SINK_APP_CAP", "0");
    run_degraded_popup_case(w, "popup sink cannot be created and the opener cannot be cut off (LIMIT)", "sink=all,cut_off=all");
    run_degraded_popup_case(w, "popup sink cannot be created and cutting the opener off fails afterwards (LIMIT)", "sink=all,cut_off_async=all");
    // First among the sink cases: the per-view case holds sinks that outlive their views by up to 20 s and count against
    // the application's limit.
    if (selected("sink limit per application")) { settle(1500); test_sink_limit_per_app(w); }
    if (selected("sink limit per view")) test_sink_limit_per_view(w);
    if (selected("sink creation deadline")) test_sink_creation_deadline(w);
    if (selected("sink timers")) test_sink_timers(w);
    if (selected("opener disposed while its popup sink is created")) test_opener_disposed_while_sink_created(w);
    if (selected("tracked popup filter cannot be registered")) test_tracked_popup_hook_failure(w);
    if (selected("prepare failure")) test_prepare_failure(w);
    if (selected("new-window setup failure")) test_new_window_setup_failure(w);
    destroy_world(w);
}

// DIAGNOSTIC, not an acceptance case: every refusal mechanism of the engine fails (both responses and the request rewrite).
// Nothing is left that could stop the request; this run measures what reaches the denied origin. Exit 3 when anything did.
int diagnose_all_mechanisms_failing() {
    auto w = make_world("all-mechanisms", false);
    const auto p = take_ports();
    view_state a;
    const std::vector<int> initial{p.b};
    CHECK(create_view(w, a, &initial) == NEOASTRA_OK);
    inject("create=all,prepared=all,neutralize=all");
    neoastra_error_t* navigate_error{};
    CHECK(neoastra_view_navigate(a.view, sv(url(p.m, "/doc?b=" + std::to_string(p.b) + "&t=" + std::to_string(p.t))), &navigate_error) == NEOASTRA_OK);
    (void)wait_done(a.view, 20000);
    settle(3500);
    inject("");
    std::printf("  DIAGNOSTIC all mechanisms failing, view alive: denied-origin requests=%llu:%s\n",
                static_cast<unsigned long long>(g_host->port_requests(p.b)), g_host->describe_port(p.b).c_str());
    neoastra_view_release(a.view);
    settle(2500);
    std::printf("  DIAGNOSTIC after disposal: denied-origin requests=%llu raw_bytes=%llu\n",
                static_cast<unsigned long long>(g_host->port_requests(p.b)), static_cast<unsigned long long>(g_host->raw_bytes(p.b)));
    const bool reached = g_host->port_requests(p.b) != 0 || g_host->raw_bytes(p.b) != 0;
    destroy_world(w);
    return reached ? 3 : 0;
}
#endif

} // namespace

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_ARM64) || defined(__aarch64__)
// Test-only crash report: no debugger is installed on the test machine, so an unhandled exception prints a symbolized
// stack (dbghelp, PDBs in the build directory) before the process ends with its usual exit code.
LONG WINAPI print_crash_stack(EXCEPTION_POINTERS* info) noexcept {
    std::fprintf(stderr, "CRASH code=0x%08lx address=%p\n", static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode),
                 info->ExceptionRecord->ExceptionAddress);
    const auto process = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    if (SymInitialize(process, nullptr, TRUE)) {
        CONTEXT context = *info->ContextRecord;
        STACKFRAME64 frame{};
#if defined(_M_ARM64) || defined(__aarch64__)
        constexpr DWORD machine = IMAGE_FILE_MACHINE_ARM64;
        frame.AddrPC.Offset = context.Pc; frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Fp; frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Sp; frame.AddrStack.Mode = AddrModeFlat;
#else
        constexpr DWORD machine = IMAGE_FILE_MACHINE_AMD64;
        frame.AddrPC.Offset = context.Rip; frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp; frame.AddrStack.Mode = AddrModeFlat;
#endif
        for (int depth = 0; depth < 48; ++depth) {
            if (!StackWalk64(machine, process, GetCurrentThread(), &frame, &context, nullptr,
                             SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
            const auto address = frame.AddrPC.Offset;
            IMAGEHLP_MODULE64 module{};
            module.SizeOfStruct = sizeof(module);
            const bool known_module = SymGetModuleInfo64(process, address, &module) != FALSE;
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512]{};
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 512;
            DWORD64 displacement{};
            std::fprintf(stderr, "  #%02d %s+0x%llx", depth, known_module ? module.ModuleName : "?",
                         static_cast<unsigned long long>(known_module ? address - module.BaseOfImage : address));
            if (SymFromAddr(process, address, &displacement, symbol))
                std::fprintf(stderr, " %s+0x%llx", symbol->Name, static_cast<unsigned long long>(displacement));
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD line_displacement{};
            if (SymGetLineFromAddr64(process, address, &line_displacement, &line))
                std::fprintf(stderr, " (%s:%lu)", line.FileName, line.LineNumber);
            std::fprintf(stderr, "\n");
        }
        std::fflush(stderr);
    } else {
        std::fprintf(stderr, "CRASH no dbghelp symbols: %lu\n", static_cast<unsigned long>(GetLastError()));
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_ARM64) || defined(__aarch64__)
    SetUnhandledExceptionFilter(print_crash_stack);
#endif
    fixture host;
    g_host = &host;
    std::printf("windows request-filter integration tests (real C ABI + WebView2 runtime, in-process loopback fixture)\n");
#if defined(NEOASTRA_FAILURE_ONLY)
    if (argc > 1 && std::strcmp(argv[1], "--all-mechanisms-diagnostic") == 0) return diagnose_all_mechanisms_failing();
    if (argc > 2 && std::strcmp(argv[1], "--case") == 0) g_case_filter = argv[2];
    test_failures();
#else
    if (argc > 1 && std::strcmp(argv[1], "--in-flight-close-diagnostic") == 0) {
        // Exit 3 when a disposal under traffic let anything reach the denied origin: the known Close gap, reported.
        auto w = make_world("in-flight-close", true);
        const int leaking = dispose_during_traffic(w, "in-flight close", argc > 2 ? std::atoi(argv[2]) : 6, argc > 3 && std::strcmp(argv[3], "open") == 0);
        destroy_world(w);
        std::printf("DIAGNOSTIC in-flight close: %d round(s) with requests at the denied origin\n", leaking);
        return leaking ? 3 : 0;
    }
    {
        auto w = make_world("main", true);
        test_capability(w);
        view_state a, b;
        test_initial_filter_and_isolation(w, a, b);
        test_main_document(w);
        test_concurrent_isolation(a, b);
        test_replace_and_clear(a, b);
        test_unsupported_retains_snapshot(a);
        test_cancel_and_dispose(w, a);
        test_custom_scheme(w);
        test_popup_inheritance(w);
        test_deferred_fallback(w);
        test_opener_release_with_deferred_popup(w);
        neoastra_view_release(a.view);
        neoastra_view_release(b.view);
        test_popups_of_filtered_views(w);
        test_window_open_traffic(w);
        test_disposal_during_traffic(w);
        test_lifetime(w);
        destroy_world(w);
    }
#endif
    std::printf("ALL PASSED (%d host-counter assertions)\n", g_checks);
    return 0;
}
