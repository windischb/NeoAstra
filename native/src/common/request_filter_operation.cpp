#include "operation_completion.hpp"

// The queue owns one reference, the completion registry one, and accepted
// platform preparation another. The view is weak: disposing its last client
// handle must not be delayed by a compile. destroy_ui aborts and unlinks the queue.
struct neo_request_filter_completion final : neo_operation_completion {
    neoastra_view_t* view;
    const std::shared_ptr<const neo_request_filter::filter> rules;
    neoastra_completion_callback_t callback;
    void* context;
    std::unique_ptr<neo_prepared_request_filter> prepared;
    neoastra_error_t* error{};
    neoastra_result_t requested{NEOASTRA_ERROR_CANCELED};
    bool preparing{}, terminal{}, no_op{};

    neo_request_filter_completion(neoastra_view_t* view, std::shared_ptr<const neo_request_filter::filter> rules,
        neoastra_completion_callback_t callback, void* context, neoastra_operation_t* operation)
        : neo_operation_completion(view->environment->app), view(view), rules(std::move(rules)),
          callback(callback), context(context) { accept(operation); }

    ~neo_request_filter_completion() override { if (error) error->release(); }

    static void ready(void* context, std::unique_ptr<neo_prepared_request_filter> value,
        neoastra_result_t result, neoastra_error_t* failure) noexcept {
        auto* self = static_cast<neo_request_filter_completion*>(context);
        if (self->view && !self->terminal) {
            self->prepared = std::move(value);
            self->requested = result;
            self->error = failure;
            self->submit();
        } else if (failure) failure->release();
        self->release(); // Platform-source reference.
    }

    static void start_next(neoastra_view_t* view) noexcept {
        if (view->ui_destroyed.load(std::memory_order_acquire) || view->request_filter_queue.empty()) return;
        auto* self = view->request_filter_queue.front();
        if (self->preparing) return;
        self->preparing = true;
        self->retain(); // Even an inline platform callback owns its source.
        if (self->operation->state.load(std::memory_order_acquire) == neo_operation_state::cancel_requested) {
            ready(self, nullptr, NEOASTRA_ERROR_CANCELED, nullptr);
            return;
        }
        const auto previous = std::atomic_load(&view->request_filter);
        if (self->rules->empty() && (!previous || previous->empty()) && neo_platform_request_filter_flags(view->environment) == 0) {
            self->no_op = true;
            ready(self, nullptr, NEOASTRA_OK, nullptr);
            return;
        }
        neoastra_error_t* error{};
        const auto result = neo_platform_prepare_request_filter(view, *self->rules, ready, self, &error);
        if (result != NEOASTRA_OK) ready(self, nullptr, result, error);
    }

    void invoke(neoastra_operation_t* pending, bool shutdown) override {
        auto* current_view = view;
        neo_retain_guard owner(current_view);
        auto desired = shutdown ? NEOASTRA_ERROR_CANCELED
            : (!owner || current_view->ui_destroyed.load(std::memory_order_acquire)) ? NEOASTRA_ERROR_DISPOSED : requested;
        if (desired == NEOASTRA_OK && !prepared && !no_op) desired = NEOASTRA_ERROR_NATIVE_FAILURE;
        if (desired == NEOASTRA_OK && pending->try_commit()) {
            neoastra_error_t* install_error{};
            desired = prepared ? prepared->install(current_view, &install_error) : NEOASTRA_OK;
            if (install_error) {
                if (error) error->release();
                error = install_error;
            }
            if (desired != NEOASTRA_OK) current_view->destroy_ui_once();
            else if (current_view->ui_destroyed.load(std::memory_order_acquire)) desired = NEOASTRA_ERROR_DISPOSED;
            else std::atomic_store(&current_view->request_filter, rules);
        }
        // A failed try_commit leaves cancel_requested for try_complete to resolve.
        terminal = true;
        view = nullptr;
        if (current_view) {
            auto& queue = current_view->request_filter_queue;
            const auto found = std::find(queue.begin(), queue.end(), this);
            if (found != queue.end()) {
                queue.erase(found);
                release(); // Queue reference; delivery still owns the registry reference.
            }
        }
        neoastra_result_t actual{};
        auto notify = std::exchange(callback, nullptr);
        auto data = std::exchange(context, nullptr);
        if (pending->try_complete(desired, actual)) {
            try { notify(data, actual, !shutdown && actual == desired ? error : nullptr); }
            catch (...) { /* No caller exception may stall the rest of the queue. */ }
        }
        if (!shutdown && owner) start_next(current_view);
    }
};

neoastra_result_t neo_start_request_filter(neoastra_view_t* view, std::shared_ptr<const neo_request_filter::filter> rules,
    neoastra_completion_callback_t callback, void* context, neoastra_operation_t* operation, neoastra_error_t** error) noexcept {
    const auto phase = view->environment->app->state.load(std::memory_order_acquire);
    if (view->ui_destroyed.load(std::memory_order_acquire) || (phase != neo_app_state::created && phase != neo_app_state::running))
        return neo_fail(error, NEOASTRA_ERROR_DISPOSED, "The view or application is shutting down");
    neo_request_filter_completion* state{};
    try {
        state = new neo_request_filter_completion(view, std::move(rules), callback, context, operation);
        view->request_filter_queue.push_back(state);
    } catch (...) {
        if (state) { state->discard_start(); state->release(); }
        return neo_fail(error, NEOASTRA_ERROR_NATIVE_FAILURE, "Could not queue request filter");
    }
    neo_request_filter_completion::start_next(view);
    return NEOASTRA_OK;
}

void neo_abort_request_filters(neoastra_view_t* view) noexcept {
    while (!view->request_filter_queue.empty()) {
        auto* state = view->request_filter_queue.front();
        view->request_filter_queue.pop_front();
        state->view = nullptr;
        state->requested = NEOASTRA_ERROR_DISPOSED;
        // A currently delivering installation keeps its own failure details;
        // queued completions instead report disposal, not an old prepare error.
        if (state->operation && state->error) { state->error->release(); state->error = nullptr; }
        state->submit();
        state->release(); // Queue reference. A late preparation only releases its own data.
    }
}
