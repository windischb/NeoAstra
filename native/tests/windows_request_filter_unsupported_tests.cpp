// The Windows request-filter contract WITHOUT ICoreWebView2_22, through the real C ABI, backend and WebView2 runtime: the
// test-hooks library simulates a runtime that lacks the interface (NEOASTRA_TEST_NO_REQUEST_FILTER_22). The backend then
// promises no request coverage: capability NONE, nonempty filters are refused at creation and update, empty filters stay
// compatible no-ops, and views without a filter keep working. The window is created hidden and never shown or activated.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "neoastra.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

namespace {

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); std::fflush(stderr); std::exit(1); } } while (0)

neoastra_string_view_t sv(const std::string& value) { return {reinterpret_cast<const uint8_t*>(value.data()), value.size()}; }

// One loopback listener that answers every complete request head with a small page and counts those requests. A client
// cannot hold it: receiving and sending time out, a head is capped, and an incomplete head gets no answer. Closing the
// listener ends the accept loop; the destructor then waits only for bounded in-flight client I/O.
class loopback {
public:
    loopback() {
        WSADATA data{};
        CHECK(WSAStartup(MAKEWORD(2, 2), &data) == 0);
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        CHECK(listener_ != INVALID_SOCKET);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        CHECK(listen(listener_, 16) == 0);
        int length = sizeof(address);
        CHECK(getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { run(); });
    }
    ~loopback() {
        stopping_ = true;
        closesocket(listener_); // Ends a waiting accept(); a client in progress ends with its bounded receive and send.
        thread_.join();
        WSACleanup();
    }
    int port() const { return port_; }
    int requests() const { return requests_.load(); }

private:
    static constexpr DWORD io_timeout_ms = 2000;
    static constexpr size_t maximum_head = 16 * 1024;

    void run() {
        while (!stopping_) {
            SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) return;
            const DWORD timeout = io_timeout_ms;
            CHECK(setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0);
            CHECK(setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0);
            std::string head;
            char chunk[4096];
            bool complete = false;
            while (!stopping_ && head.size() < maximum_head) {
                const int received = recv(client, chunk, sizeof(chunk), 0);
                if (received <= 0) break; // Closed, failed or timed out.
                head.append(chunk, static_cast<size_t>(received));
                if (head.find("\r\n\r\n") != std::string::npos) { complete = true; break; }
            }
            if (complete) {
                ++requests_;
                static const std::string reply = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 22\r\nConnection: close\r\n\r\n<!doctype html>loaded.";
                send(client, reply.data(), static_cast<int>(reply.size()), 0);
            }
            closesocket(client);
        }
    }
    SOCKET listener_{INVALID_SOCKET};
    std::atomic<bool> stopping_{false};
    int port_{};
    std::atomic<int> requests_{0};
    std::thread thread_;
};

// A user-data folder that this run created itself: CreateDirectoryW fails on an existing name, so an old or foreign folder
// is never reused, and only the folder claimed here is removed at the end.
std::filesystem::path claim_new_temp_directory() {
    wchar_t temp[MAX_PATH + 1]{};
    const DWORD length = GetTempPathW(MAX_PATH + 1, temp);
    CHECK(length > 0 && length <= MAX_PATH); // 0: failure; larger: the buffer was too small.
    const std::filesystem::path base(std::wstring(temp, length));
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        const auto candidate = base / (L"neoastra-rf-unsupported-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                       std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
        if (CreateDirectoryW(candidate.c_str(), nullptr)) return candidate;
        CHECK(GetLastError() == ERROR_ALREADY_EXISTS);
    }
    CHECK(false && "no new temporary directory could be claimed");
    return {};
}

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

struct created { bool done{}; neoastra_result_t result{NEOASTRA_ERROR_UNKNOWN}; void* value{}; };
void NEOASTRA_CALL on_environment(void* p, neoastra_result_t r, neoastra_environment_t* v, const neoastra_error_t*) { *static_cast<created*>(p) = {true, r, v}; }
void NEOASTRA_CALL on_view(void* p, neoastra_result_t r, neoastra_view_t* v, const neoastra_error_t*) { *static_cast<created*>(p) = {true, r, v}; }
struct completion { bool done{}; neoastra_result_t result{NEOASTRA_ERROR_UNKNOWN}; int calls{}; };
void NEOASTRA_CALL on_completion(void* p, neoastra_result_t r, const neoastra_error_t*) { auto* c = static_cast<completion*>(p); c->result = r; c->done = true; ++c->calls; }
std::atomic<int> g_navigations{0};
void NEOASTRA_CALL on_view_event(void*, const neoastra_event_t* event) {
    if (event->header.type == NEOASTRA_EVENT_NAVIGATION_COMPLETED || event->header.type == NEOASTRA_EVENT_NAVIGATION_FAILED) ++g_navigations;
}

neoastra_request_rule_t port_rule(int port) {
    neoastra_request_rule_t rule{};
    rule.size = sizeof(rule); rule.version = 1; rule.host_match = NEOASTRA_REQUEST_HOST_ANY;
    rule.port = port; rule.schemes = 1 | 2; rule.kinds = UINT32_MAX;
    return rule;
}
neoastra_request_filter_t describe(const neoastra_request_rule_t* rules, uint32_t count) {
    neoastra_request_filter_t filter{};
    filter.size = sizeof(filter); filter.version = 1; filter.rules = rules; filter.rule_count = count;
    return filter;
}

// Creates a view; `version` 1 omits the filter field, `filter` is used with version 2.
neoastra_result_t create_view(neoastra_environment_t* environment, neoastra_window_t* window, uint32_t version,
                              const neoastra_request_filter_t* filter, neoastra_view_t** output) {
    neoastra_view_options_t options{};
    options.size = version == 1 ? static_cast<uint32_t>(offsetof(neoastra_view_options_t, request_filter)) : static_cast<uint32_t>(sizeof(options));
    options.version = version;
    options.window = window;
    options.fill_parent = 1;
    if (version == 2) options.request_filter = filter;
    created result;
    neoastra_error_t* error{};
    const auto accepted = neoastra_environment_create_view_async(environment, &options, on_view, &result, nullptr, &error);
    if (error) neoastra_error_release(error);
    if (accepted != NEOASTRA_OK) return accepted;
    CHECK(pump_until([&] { return result.done; }));
    *output = static_cast<neoastra_view_t*>(result.value);
    return result.result;
}

} // namespace

int main() {
    CHECK(SetEnvironmentVariableA("NEOASTRA_TEST_NO_REQUEST_FILTER_22", "1")); // read by the test-hooks library only
    loopback host;
    neoastra_app_t* app{};
    neoastra_app_options_t app_options{};
    app_options.size = sizeof(app_options); app_options.version = 1; app_options.shutdown_mode = NEOASTRA_APP_SHUTDOWN_EXPLICIT;
    CHECK(neoastra_app_attach(&app_options, &app, nullptr) == NEOASTRA_OK);
    neoastra_window_t* window{};
    neoastra_window_options_t window_options{};
    window_options.size = sizeof(window_options); window_options.version = 1; window_options.bounds = {100, 100, 640, 480}; window_options.flags = 3;
    CHECK(neoastra_app_create_window(app, &window_options, &window, nullptr) == NEOASTRA_OK); // never shown
    const auto root = claim_new_temp_directory();
    const auto root_text = root.u8string();
    neoastra_environment_options_t environment_options{};
    environment_options.size = sizeof(environment_options); environment_options.version = 1;
    environment_options.user_data_root = {reinterpret_cast<const uint8_t*>(root_text.data()), root_text.size()};
    created environment_result;
    CHECK(neoastra_environment_create_async(app, &environment_options, on_environment, &environment_result, nullptr, nullptr) == NEOASTRA_OK);
    CHECK(pump_until([&] { return environment_result.done; }));
    CHECK(environment_result.result == NEOASTRA_OK);
    auto* environment = static_cast<neoastra_environment_t*>(environment_result.value);

    // Capability: no promise of request coverage.
    neoastra_capability_info_t info{};
    info.size = sizeof(info); info.version = 1;
    CHECK(neoastra_environment_get_capability(environment, NEOASTRA_CAPABILITY_REQUEST_FILTER, &info) == NEOASTRA_OK);
    std::printf("request-filter capability: support=%u flags=%llu\n", static_cast<unsigned>(info.support), static_cast<unsigned long long>(info.flags));
    CHECK(info.support == NEOASTRA_SUPPORT_NONE && info.flags == 0);

    // A nonempty initial filter fails closed: no view, not an unfiltered one.
    const auto rule = port_rule(host.port());
    const auto nonempty = describe(&rule, 1);
    neoastra_view_t* refused{};
    CHECK(create_view(environment, window, 2, &nonempty, &refused) == NEOASTRA_ERROR_NOT_SUPPORTED);
    CHECK(refused == nullptr);

    // An empty initial filter and version-1 options create ordinary views.
    const auto empty = describe(nullptr, 0);
    neoastra_view_t* view{};
    CHECK(create_view(environment, window, 2, &empty, &view) == NEOASTRA_OK && view);
    neoastra_view_t* old_layout{};
    CHECK(create_view(environment, window, 1, nullptr, &old_layout) == NEOASTRA_OK && old_layout);
    neoastra_view_release(old_layout);
    CHECK(neoastra_view_set_event_callback(view, on_view_event, nullptr) == NEOASTRA_OK);

    // A nonempty update is refused synchronously, without a completion; clearing completes exactly once with success.
    completion update;
    neoastra_error_t* error{};
    CHECK(neoastra_view_set_request_filter_async(view, &nonempty, on_completion, &update, nullptr, &error) == NEOASTRA_ERROR_NOT_SUPPORTED);
    if (error) neoastra_error_release(error);
    completion cleared;
    CHECK(neoastra_view_set_request_filter_async(view, &empty, on_completion, &cleared, nullptr, nullptr) == NEOASTRA_OK);
    CHECK(pump_until([&] { return cleared.done; }));
    pump_until([] { return false; }, 200);
    CHECK(!update.done && update.calls == 0);
    CHECK(cleared.result == NEOASTRA_OK && cleared.calls == 1);

    // Positive control: the view without a filter still loads from the loopback host.
    const int before = g_navigations.load();
    const std::string address = "http://127.0.0.1:" + std::to_string(host.port()) + "/unsupported_control";
    CHECK(neoastra_view_navigate(view, sv(address), nullptr) == NEOASTRA_OK);
    CHECK(pump_until([&] { return g_navigations.load() > before && host.requests() >= 1; }));
    std::printf("positive control: %d request(s) reached the loopback host\n", host.requests());

    neoastra_view_release(view);
    neoastra_window_release(window);
    neoastra_environment_release(environment);
    CHECK(neoastra_app_detach(app, nullptr) == NEOASTRA_OK);
    neoastra_app_release(app);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored); // Only the directory this test created.
    std::printf("ALL PASSED: Windows request filter is unsupported and fail-closed\n");
    return 0;
}
