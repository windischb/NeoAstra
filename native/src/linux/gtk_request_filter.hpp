#pragma once

#include "../common/native_internal.hpp"
#include <webkit/webkit.h>
#include <unistd.h>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace neo_gtk_filter {

// One private store per compilation. The immutable entry and compile context
// keep it alive until compilation and use finish, without an asynchronous
// delete queue. There is no caller-owned path or process-wide rule cache.
struct store {
    gchar* folder{};
    gchar* identifier{};
    WebKitUserContentFilterStore* value{};
    ~store() {
        if (value) g_object_unref(value);
        if (folder) {
            try {
                std::error_code ignored;
                std::filesystem::remove_all(folder, ignored);
            } catch (...) { /* Native destruction must not throw. */ }
            g_free(folder);
        }
        g_free(identifier);
    }
};

inline std::shared_ptr<store> make_store() {
    auto owner = std::make_shared<store>();
    const auto pattern = "neoastra-request-filter-" + std::to_string(getpid()) + "-XXXXXX";
    owner->folder = g_dir_make_tmp(pattern.c_str(), nullptr); // Atomic creation, mode 0700.
    if (!owner->folder) throw std::runtime_error("Could not create a private content-filter store");
    owner->identifier = g_uuid_string_random();
    owner->value = webkit_user_content_filter_store_new(owner->folder);
    if (!owner->value) throw std::runtime_error("Could not open a content-filter store");
    return owner;
}

struct filter_deleter {
    void operator()(WebKitUserContentFilter* value) const noexcept {
        if (value) webkit_user_content_filter_unref(value);
    }
};
using filter_ptr = std::unique_ptr<WebKitUserContentFilter, filter_deleter>;

struct entry {
    std::shared_ptr<store> owner;
    filter_ptr value;
    entry(std::shared_ptr<store> owner, filter_ptr value) : owner(std::move(owner)), value(std::move(value)) { }
};

template<class ViewState>
struct prepared final : neo_prepared_request_filter {
    std::shared_ptr<entry> value;
    explicit prepared(std::shared_ptr<entry> value) : value(std::move(value)) { }
    neoastra_result_t install(neoastra_view_t* view, neoastra_error_t** error) noexcept override {
        auto* state = static_cast<ViewState*>(view->platform);
        if (!state || !state->widget || !state->content)
            return neo_fail(error, NEOASTRA_ERROR_DISPOSED, "The web view is unavailable");
        if (value && state->shares_content_controller)
            return neo_fail(error, NEOASTRA_ERROR_NOT_SUPPORTED, "Tracked popup views cannot have independent request filters");
        // These public calls are void. Completion records local installation,
        // not acknowledgment from every WebKit process. Unique identifiers let
        // the new list coexist with the old one until the old list is removed.
        if (value) webkit_user_content_manager_add_filter(state->content, value->value.get());
        if (state->filter_entry)
            webkit_user_content_manager_remove_filter(state->content, state->filter_entry->value.get());
        state->filter_entry = value;
        return NEOASTRA_OK;
    }
};

struct compile_context {
    std::shared_ptr<store> owner;
    GBytes* bytes{};
    neo_request_filter_prepared_callback_t callback{};
    void* context{};
    ~compile_context() { if (bytes) g_bytes_unref(bytes); }
};

template<class ViewState>
neoastra_result_t prepare(neoastra_view_t* view, const neo_request_filter::filter& rules,
    neo_request_filter_prepared_callback_t callback, void* context, neoastra_error_t** error) noexcept {
    try {
        auto* state = static_cast<ViewState*>(view->platform);
        if (!state || !state->widget || !state->content)
            return neo_fail(error, NEOASTRA_ERROR_DISPOSED, "The web view is unavailable");
        if (rules.empty()) {
            callback(context, std::make_unique<prepared<ViewState>>(nullptr), NEOASTRA_OK, nullptr);
            return NEOASTRA_OK;
        }
        if (state->shares_content_controller)
            return neo_fail(error, NEOASTRA_ERROR_NOT_SUPPORTED, "Tracked popup views cannot have independent request filters");
        const auto json = rules.webkit_json();
        auto pending = std::make_unique<compile_context>();
        pending->owner = make_store();
        pending->bytes = g_bytes_new(json.data(), json.size());
        pending->callback = callback;
        pending->context = context;
        auto* source = pending.release();
        // Cancellation belongs to the common operation queue: a canceled
        // compile may finish, but its entry is discarded and never installed.
        webkit_user_content_filter_store_save(source->owner->value, source->owner->identifier, source->bytes, nullptr,
            [](GObject* object, GAsyncResult* result, void* pointer) noexcept {
                std::unique_ptr<compile_context> pending(static_cast<compile_context*>(pointer));
                GError* failure{};
                filter_ptr compiled(webkit_user_content_filter_store_save_finish(
                    WEBKIT_USER_CONTENT_FILTER_STORE(object), result, &failure));
                neoastra_error_t* error{};
                if (failure || !compiled) {
                    neo_fail(&error, NEOASTRA_ERROR_NATIVE_FAILURE, "Could not compile request filter",
                        failure ? failure->code : 0, "webkitgtk");
                    if (failure) g_error_free(failure);
                    pending->callback(pending->context, nullptr, NEOASTRA_ERROR_NATIVE_FAILURE, error);
                    return;
                }
                try {
                    auto value = std::make_shared<entry>(pending->owner, std::move(compiled));
                    auto installation = std::make_unique<prepared<ViewState>>(std::move(value));
                    pending->callback(pending->context, std::move(installation), NEOASTRA_OK, nullptr);
                } catch (...) {
                    neo_fail(&error, NEOASTRA_ERROR_NATIVE_FAILURE, "Could not retain compiled request filter", 0, "webkitgtk");
                    pending->callback(pending->context, nullptr, NEOASTRA_ERROR_NATIVE_FAILURE, error);
                }
            }, source);
        return NEOASTRA_OK;
    } catch (const std::invalid_argument& exception) {
        return neo_fail(error, NEOASTRA_ERROR_NOT_SUPPORTED, exception.what(), 0, "webkitgtk");
    } catch (const std::exception& exception) {
        return neo_fail(error, NEOASTRA_ERROR_NATIVE_FAILURE, exception.what(), 0, "webkitgtk");
    }
}

} // namespace neo_gtk_filter
