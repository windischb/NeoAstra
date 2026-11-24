#pragma once

#include "native_internal.hpp"
#import <Foundation/Foundation.h>
#import <WebKit/WebKit.h>

#include <cstdlib>
#include <filesystem>

// Shared by Cocoa and UIKit. The backend view supplies its WKWebView and the
// owning store/entry fields; operation ordering and lifetime stay in common C++.
namespace neo_apple_filter {

struct store {
    std::string folder;
    bool created{};
    WKContentRuleListStore* value{};
    ~store() {
        @autoreleasepool {
            value = nil;
            if (created) {
                try {
                    std::error_code ignored;
                    std::filesystem::remove_all(folder, ignored);
                } catch (...) { /* Cleanup must not escape a native destructor. */ }
            }
        }
    }
};

inline std::shared_ptr<store> make_store() {
    auto owner = std::make_shared<store>();
    NSString* name = [NSString stringWithFormat:@"neoastra-request-filter-%d-XXXXXX", NSProcessInfo.processInfo.processIdentifier];
    owner->folder = [NSTemporaryDirectory() stringByAppendingPathComponent:name].UTF8String;
    // mkdtemp creates a private directory atomically, never a caller-owned path.
    if (!mkdtemp(owner->folder.data())) throw std::runtime_error("Could not create a temporary content-rule store");
    owner->created = true;
    owner->value = [WKContentRuleListStore storeWithURL:
        [NSURL fileURLWithPath:@(owner->folder.c_str()) isDirectory:YES]];
    if (!owner->value) throw std::runtime_error("Could not open a content-rule store");
    return owner;
}

inline void remove_entry(std::shared_ptr<store> owner, NSString* identifier) noexcept {
    @autoreleasepool {
        @try {
            // Keep the directory alive through removal, including after disposal
            // during compilation or while a popup shares the immutable entry.
            [owner->value removeContentRuleListForIdentifier:identifier completionHandler:^(NSError*) { (void)owner; }];
        } @catch (NSException*) { /* The last owner removes its private directory. */ }
    }
}

struct entry {
    std::shared_ptr<store> owner;
    WKContentRuleList* list;
    entry(std::shared_ptr<store> owner, WKContentRuleList* list) : owner(std::move(owner)), list(list) { }
    ~entry() { if (list) remove_entry(owner, list.identifier); }
};

template<class ViewState>
struct prepared final : neo_prepared_request_filter {
    std::shared_ptr<entry> value;
    explicit prepared(std::shared_ptr<entry> value) : value(std::move(value)) { }
    neoastra_result_t install(neoastra_view_t* view, neoastra_error_t** error) noexcept override {
        @autoreleasepool {
            auto* state = static_cast<ViewState*>(view->platform);
            if (!state || !state->webview) return neo_fail(error, NEOASTRA_ERROR_DISPOSED, "The web view is unavailable");
            @try {
                auto* controller = state->webview.configuration.userContentController;
                if (value) [controller addContentRuleList:value->list];
                if (state->filter_entry) [controller removeContentRuleList:state->filter_entry->list];
                state->filter_entry = value;
                return NEOASTRA_OK;
            } @catch (NSException* exception) {
                const char* reason = exception.reason.UTF8String;
                return neo_fail(error, NEOASTRA_ERROR_NATIVE_FAILURE, reason ? reason : "Could not install request filter", 0, "wkwebview");
            }
        }
    }
};

template<class ViewState>
neoastra_result_t prepare(neoastra_view_t* view, const neo_request_filter::filter& rules,
    neo_request_filter_prepared_callback_t callback, void* context, neoastra_error_t** error) noexcept {
    @autoreleasepool {
        @try {
            try {
                auto* state = static_cast<ViewState*>(view->platform);
                if (!state || !state->webview) return neo_fail(error, NEOASTRA_ERROR_DISPOSED, "The web view is unavailable");
                if (rules.empty()) {
                    callback(context, std::make_unique<prepared<ViewState>>(nullptr), NEOASTRA_OK, nullptr);
                    return NEOASTRA_OK;
                }
                const auto json = rules.webkit_json();
                if (!state->filter_store) state->filter_store = make_store();
                const auto owner = state->filter_store;
                NSString* identifier = NSUUID.UUID.UUIDString;
                [owner->value compileContentRuleListForIdentifier:identifier encodedContentRuleList:@(json.c_str())
                    completionHandler:^(WKContentRuleList* list, NSError* failure) {
                        @autoreleasepool {
                            neoastra_error_t* error{};
                            if (failure || !list) {
                                remove_entry(owner, identifier);
                                neo_fail(&error, NEOASTRA_ERROR_NATIVE_FAILURE, "Could not compile request filter", failure.code, "wkwebview");
                                callback(context, nullptr, NEOASTRA_ERROR_NATIVE_FAILURE, error);
                                return;
                            }
                            std::shared_ptr<entry> compiled;
                            try {
                                compiled = std::make_shared<entry>(owner, list);
                                callback(context, std::make_unique<prepared<ViewState>>(compiled), NEOASTRA_OK, nullptr);
                            } catch (...) {
                                if (!compiled) remove_entry(owner, identifier);
                                neo_fail(&error, NEOASTRA_ERROR_NATIVE_FAILURE, "Could not retain compiled request filter", 0, "wkwebview");
                                callback(context, nullptr, NEOASTRA_ERROR_NATIVE_FAILURE, error);
                            }
                        }
                    }];
                return NEOASTRA_OK;
            } catch (const std::invalid_argument& exception) {
                return neo_fail(error, NEOASTRA_ERROR_NOT_SUPPORTED, exception.what(), 0, "wkwebview");
            } catch (const std::exception& exception) {
                return neo_fail(error, NEOASTRA_ERROR_NATIVE_FAILURE, exception.what(), 0, "wkwebview");
            }
        } @catch (NSException* exception) {
            const char* reason = exception.reason.UTF8String;
            return neo_fail(error, NEOASTRA_ERROR_NATIVE_FAILURE, reason ? reason : "Could not prepare request filter", 0, "wkwebview");
        }
    }
}

} // namespace neo_apple_filter
