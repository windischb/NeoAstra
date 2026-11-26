#include "native_internal.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <stdexcept>

namespace completion_test_backend {
extern uint64_t filter_flags;
extern uint32_t filter_installs, filter_prepared_destroyed;
extern neoastra_result_t filter_install_result;
extern bool reject_filter_start;
extern std::function<void()> filter_install_hook;
extern std::deque<std::pair<neo_request_filter_prepared_callback_t, void*>> filter_sources;
extern neo_platform_created_callback_t view_callback;
extern void* view_context;
extern std::shared_ptr<const neo_request_filter::filter> initial_filter;
extern uint32_t view_options_version;
void finish_filter(neoastra_result_t result);
}

namespace {
namespace backend = completion_test_backend;
constexpr uint64_t base = NEOASTRA_REQUEST_FILTER_SUBRESOURCES | NEOASTRA_REQUEST_FILTER_FRAMES | NEOASTRA_REQUEST_FILTER_PER_VIEW;

struct request {
    neoastra_request_rule_t rule{};
    neoastra_request_filter_t description{};
    explicit request(int port) {
        rule.size = sizeof(rule); rule.version = 1;
        rule.host_match = NEOASTRA_REQUEST_HOST_ANY; rule.port = port;
        rule.schemes = NEOASTRA_REQUEST_HTTP | NEOASTRA_REQUEST_HTTPS;
        rule.kinds = NEOASTRA_REQUEST_KINDS_ALL;
        description.size = sizeof(description); description.version = 1;
        description.rules = &rule; description.rule_count = 1;
    }
    request(const request&) = delete;
    request& operator=(const request&) = delete;
};

struct answer {
    unsigned calls{};
    neoastra_result_t result{NEOASTRA_ERROR_UNKNOWN}, error_code{NEOASTRA_OK};
    neoastra_view_t* created{};
    std::function<void()> hook;
};

void NEOASTRA_CALL completed(void* context, neoastra_result_t result, const neoastra_error_t* error) {
    auto& state = *static_cast<answer*>(context);
    ++state.calls; state.result = result;
    if (error) state.error_code = error->code;
    if (state.hook) state.hook();
}
void NEOASTRA_CALL created(void* context, neoastra_result_t result, neoastra_view_t* view, const neoastra_error_t* error) {
    static_cast<answer*>(context)->created = view;
    completed(context, result, error);
}

struct fixture {
    neoastra_app_t* app{};
    neoastra_environment_t* environment{};
    neoastra_view_t* view{};
    // Own callback contexts through all cancellation, shutdown and late-source
    // cleanup; none points into a local variable already out of scope.
    std::vector<std::unique_ptr<answer>> answers;
    std::vector<neoastra_operation_t*> operations;

    fixture() {
        assert(backend::filter_sources.empty());
        backend::filter_flags = base;
        backend::filter_installs = backend::filter_prepared_destroyed = 0;
        backend::filter_install_result = NEOASTRA_OK;
        backend::reject_filter_start = false;
        backend::filter_install_hook = {};
        backend::view_callback = nullptr; backend::view_context = nullptr;
        backend::initial_filter.reset();
        neoastra_app_options_t options{};
        options.size = sizeof(options); options.version = 1;
        options.shutdown_mode = NEOASTRA_APP_SHUTDOWN_EXPLICIT;
        options.maximum_pending_dispatches = 64;
        assert(neoastra_app_attach(&options, &app, nullptr) == NEOASTRA_OK);
        environment = new neoastra_environment(app);
        view = new neoastra_view(environment);
    }
    ~fixture() {
        backend::filter_install_hook = {};
        if (view) neoastra_view_release(view);
        for (auto& value : answers) if (value->created) { neoastra_view_release(value->created); value->created = nullptr; }
        if (!app->stopped.load()) assert(neoastra_app_detach(app, nullptr) == NEOASTRA_OK);
        finish_creation();
        while (!backend::filter_sources.empty()) backend::finish_filter(NEOASTRA_OK);
        for (auto* operation : operations) neoastra_operation_release(operation);
        neoastra_environment_release(environment);
        neoastra_app_release(app);
    }
    answer& record() { answers.push_back(std::make_unique<answer>()); return *answers.back(); }
    void drain() { neo_drain_dispatch(app); }
    static void finish_creation() {
        if (auto callback = std::exchange(backend::view_callback, nullptr))
            callback(std::exchange(backend::view_context, nullptr), nullptr);
    }
    neoastra_result_t set(request& value, answer& result, neoastra_operation_t** handle = nullptr) {
        neoastra_operation_t* operation{};
        neoastra_error_t* error{};
        auto status = neoastra_view_set_request_filter_async(view, &value.description, completed, &result, &operation, &error);
        if (error) { result.error_code = error->code; neoastra_error_release(error); }
        if (operation) operations.push_back(operation);
        if (handle) *handle = operation;
        else if (status != NEOASTRA_OK) assert(operation == nullptr);
        return status;
    }
};

void validation_and_old_abi() {
    fixture f;
    request data(2582);
    auto& rejected = f.record();
    data.rule.port = 0;
    assert(f.set(data, rejected) == NEOASTRA_ERROR_INVALID_ARGUMENT);
    data.rule.port = 2582;
    data.description.required_flags = uint64_t{1} << 63;
    assert(f.set(data, rejected) == NEOASTRA_ERROR_INVALID_ARGUMENT);
    data.description.required_flags = NEOASTRA_REQUEST_FILTER_WEBSOCKETS;
    assert(f.set(data, rejected) == NEOASTRA_ERROR_NOT_SUPPORTED);
    data.description.required_flags = 0;
    data.rule.schemes |= NEOASTRA_REQUEST_WS;
    assert(f.set(data, rejected) == NEOASTRA_ERROR_NOT_SUPPORTED);
    data.rule.schemes = NEOASTRA_REQUEST_HTTP;
    data.rule.kinds = 1u << NEOASTRA_RESOURCE_IMAGE;
    assert(f.set(data, rejected) == NEOASTRA_ERROR_NOT_SUPPORTED);
    data.rule.kinds = NEOASTRA_REQUEST_KINDS_ALL;
    backend::filter_flags = 0;
    assert(f.set(data, rejected) == NEOASTRA_ERROR_NOT_SUPPORTED);
    data.description.rules = nullptr; data.description.rule_count = 0;
    auto& cleared = f.record();
    assert(f.set(data, cleared) == NEOASTRA_OK);
    assert(cleared.calls == 0);
    f.drain();
    assert(cleared.calls == 1 && cleared.result == NEOASTRA_OK && backend::filter_installs == 0);
    assert(rejected.calls == 0);

    // A V1 caller has no filter field, including when bytes after the old prefix
    // happen to contain an invalid pointer. The independent frozen consumer also
    // exercises a real original-size allocation against the rebuilt library.
    neoastra_view_options_t options{};
    options.size = offsetof(neoastra_view_options_t, request_filter); options.version = 1;
    options.request_filter = reinterpret_cast<const neoastra_request_filter_t*>(uintptr_t{1});
    auto& old = f.record();
    assert(neoastra_environment_create_view_async(f.environment, &options, created, &old, nullptr, nullptr) == NEOASTRA_OK);
    assert(!backend::initial_filter && backend::view_options_version == 2);
    f.finish_creation(); f.drain();
    assert(old.calls == 1 && old.result == NEOASTRA_OK);
    options.version = 2;
    assert(neoastra_environment_create_view_async(f.environment, &options, created, &old, nullptr, nullptr) == NEOASTRA_ERROR_INVALID_ARGUMENT);
    request malformed(0);
    options.size = sizeof(options);
    options.request_filter = &malformed.description;
    assert(neoastra_environment_create_view_async(reinterpret_cast<neoastra_environment_t*>(uintptr_t{1}),
        &options, created, &old, nullptr, nullptr) == NEOASTRA_ERROR_INVALID_ARGUMENT);
}

void rejected_preparation_keeps_the_queue_usable() {
    fixture f;
    request data(2582);
    auto& rejected = f.record();
    backend::reject_filter_start = true;
    assert(f.set(data, rejected) == NEOASTRA_OK); // Accepted by the queue; backend rejection is a completion.
    f.drain();
    assert(rejected.calls == 1 && rejected.result == NEOASTRA_ERROR_NOT_SUPPORTED);
    assert(!f.view->request_filter && !f.view->ui_destroyed.load());
    backend::reject_filter_start = false;
    auto& next = f.record();
    assert(f.set(data, next) == NEOASTRA_OK);
    backend::finish_filter(NEOASTRA_OK); f.drain();
    assert(next.calls == 1 && next.result == NEOASTRA_OK);
}

void ordered_owned_snapshots() {
    fixture f;
    request first(2582), second(2583);
    std::string host = "example.com";
    first.rule.host_match = NEOASTRA_REQUEST_HOST_EXACT;
    first.rule.host = neo_string_view(host);
    auto& a = f.record(); auto& b = f.record();
    assert(f.set(first, a) == NEOASTRA_OK);
    assert(f.set(second, b) == NEOASTRA_OK);
    first.rule.port = 1234; host.assign("changed.example");
    assert(backend::filter_sources.size() == 1); // Only the FIFO head prepares.
    assert(a.calls == 0 && b.calls == 0);
    backend::finish_filter(NEOASTRA_OK); f.drain();
    assert(a.calls == 1 && a.result == NEOASTRA_OK && b.calls == 0);
    assert(f.view->request_filter->rules()[0].port == 2582);
    assert(f.view->request_filter->rules()[0].host == "example.com");
    assert(backend::filter_sources.size() == 1);
    backend::finish_filter(NEOASTRA_ERROR_NATIVE_FAILURE); f.drain();
    assert(b.calls == 1 && b.result == NEOASTRA_ERROR_NATIVE_FAILURE);
    assert(f.view->request_filter->rules()[0].port == 2582 && backend::filter_installs == 1);
}

void cancellation_and_disposal() {
    fixture f;
    request data(2582);
    auto& a = f.record(); auto& b = f.record();
    neoastra_operation_t* cancel{};
    assert(f.set(data, a, &cancel) == NEOASTRA_OK);
    assert(f.set(data, b) == NEOASTRA_OK);
    neoastra_operation_cancel(cancel);
    backend::finish_filter(NEOASTRA_OK); f.drain();
    assert(a.calls == 1 && a.result == NEOASTRA_ERROR_CANCELED && backend::filter_installs == 0);
    assert(backend::filter_sources.size() == 1);
    neoastra_view_release(std::exchange(f.view, nullptr));
    f.drain();
    assert(b.calls == 1 && b.result == NEOASTRA_ERROR_DISPOSED);
    backend::finish_filter(NEOASTRA_OK); f.drain();
    assert(b.calls == 1 && backend::filter_installs == 0 && backend::filter_prepared_destroyed == 2);
}

void commit_wins_and_callback_reentrancy() {
    fixture f;
    request data(2582);
    auto& a = f.record(); auto& b = f.record();
    neoastra_operation_t* operation{};
    assert(f.set(data, a, &operation) == NEOASTRA_OK);
    assert(f.set(data, b) == NEOASTRA_OK);
    backend::filter_install_hook = [operation] { neoastra_operation_cancel(operation); };
    a.hook = [] { throw std::runtime_error("caller callback"); };
    backend::finish_filter(NEOASTRA_OK); f.drain();
    assert(a.calls == 1 && a.result == NEOASTRA_OK);
    assert(backend::filter_sources.size() == 1);
    backend::filter_install_hook = {};
    backend::finish_filter(NEOASTRA_OK); f.drain();
    assert(b.calls == 1 && b.result == NEOASTRA_OK && backend::filter_installs == 2);
}

void initial_filter_waits_and_shutdown_settles() {
    fixture f;
    request data(2582);
    neoastra_view_options_t options{};
    options.size = sizeof(options); options.version = 2; options.request_filter = &data.description;
    auto& view = f.record();
    assert(neoastra_environment_create_view_async(f.environment, &options, created, &view, nullptr, nullptr) == NEOASTRA_OK);
    assert(backend::initial_filter->rules()[0].port == 2582);
    f.finish_creation(); f.drain();
    assert(view.calls == 0 && backend::filter_sources.size() == 1);
    backend::finish_filter(NEOASTRA_OK); f.drain();
    assert(view.calls == 1 && view.created && backend::filter_installs == 1);

    auto& update = f.record();
    assert(f.set(data, update) == NEOASTRA_OK);
    assert(neoastra_app_detach(f.app, nullptr) == NEOASTRA_OK);
    assert(update.calls == 1 && update.result != NEOASTRA_OK);
    backend::finish_filter(NEOASTRA_OK);
    assert(update.calls == 1 && backend::filter_installs == 1);
}

void late_creation_after_shutdown() {
    fixture f;
    request data(2582);
    neoastra_view_options_t options{};
    options.size = sizeof(options); options.version = 2; options.request_filter = &data.description;
    auto& result = f.record();
    assert(neoastra_environment_create_view_async(f.environment, &options, created, &result, nullptr, nullptr) == NEOASTRA_OK);
    assert(neoastra_app_detach(f.app, nullptr) == NEOASTRA_OK);
    assert(result.calls == 1 && result.result == NEOASTRA_ERROR_CANCELED);
    f.finish_creation();
    assert(backend::filter_sources.empty() && result.calls == 1);
}

void installation_failure_closes_view() {
    fixture f;
    request data(2582);
    auto& a = f.record(); auto& b = f.record();
    assert(f.set(data, a) == NEOASTRA_OK);
    assert(f.set(data, b) == NEOASTRA_OK);
    backend::filter_install_result = NEOASTRA_ERROR_NATIVE_FAILURE;
    backend::finish_filter(NEOASTRA_OK); f.drain();
    assert(a.calls == 1 && a.result == NEOASTRA_ERROR_NATIVE_FAILURE);
    assert(a.error_code == NEOASTRA_ERROR_NATIVE_FAILURE);
    assert(b.calls == 1 && b.result == NEOASTRA_ERROR_DISPOSED);
    assert(f.view->ui_destroyed.load() && backend::filter_sources.empty());
}

} // namespace

int main() {
    validation_and_old_abi();
    rejected_preparation_keeps_the_queue_usable();
    ordered_owned_snapshots();
    cancellation_and_disposal();
    commit_wins_and_callback_reentrancy();
    initial_filter_waits_and_shutdown_settles();
    late_creation_after_shutdown();
    installation_failure_closes_view();
    std::puts("Request filter ABI/operation tests passed");
}
