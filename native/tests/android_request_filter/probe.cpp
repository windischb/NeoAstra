// JNI test driver using the public C ABI and the real NeoAstra Android backend.
#include "neoastra.h"
#include <jni.h>
#include <atomic>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>

namespace {
JavaVM* vm{};
jobject activity{};
jmethodID result_method{}, loaded_method{}, popup_method{};
neoastra_app_t* app{};
neoastra_environment_t* environment{};
std::map<neoastra_view_t*, jobject> parents;
std::atomic<int> resource_calls{};
int popup_mode{};
neoastra_decision_t* deferred_popup{};
struct pending { int token; jobject parent{}; };

JNIEnv* jni() {
    JNIEnv* value{};
    if (vm->GetEnv(reinterpret_cast<void**>(&value), JNI_VERSION_1_6) != JNI_OK) std::abort();
    return value;
}
neoastra_string_view_t text(const std::string& value) {
    return {reinterpret_cast<const uint8_t*>(value.data()), value.size()};
}
std::string from_java(JNIEnv* env, jstring value) {
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (!chars) throw std::bad_alloc();
    std::string result(chars); // All probe fixtures and URLs are ASCII.
    env->ReleaseStringUTFChars(value, chars);
    return result;
}
void report(int token, neoastra_result_t code, jlong value = 0, neoastra_string_view_t message = {}) {
    JNIEnv* env = jni();
    std::string copy(message.data ? reinterpret_cast<const char*>(message.data) : "", static_cast<size_t>(message.length));
    jstring content = env->NewStringUTF(copy.c_str());
    env->CallVoidMethod(activity, result_method, token, static_cast<jint>(code), value, content);
    env->DeleteLocalRef(content);
}
void failure(int token, neoastra_result_t code, const neoastra_error_t* error) {
    report(token, code, 0, error ? neoastra_error_get_message(error) : neoastra_string_view_t{});
}
void NEOASTRA_CALL completed(void* context, neoastra_result_t code, const neoastra_error_t* error) {
    std::unique_ptr<pending> state(static_cast<pending*>(context));
    failure(state->token, code, error);
}
void NEOASTRA_CALL evaluated(void* context, neoastra_result_t code, neoastra_string_view_t json, const neoastra_error_t* error) {
    std::unique_ptr<pending> state(static_cast<pending*>(context));
    if (code == NEOASTRA_OK) report(state->token, code, 0, json);
    else failure(state->token, code, error);
}
void NEOASTRA_CALL event(void* context, const neoastra_event_t* value) {
    if (value->header.type != NEOASTRA_EVENT_NAVIGATION_COMPLETED && value->header.type != NEOASTRA_EVENT_NAVIGATION_FAILED
        && value->header.type != NEOASTRA_EVENT_NEW_WINDOW_REQUESTED) return;
    JNIEnv* env = jni();
    const std::string uri(value->uri.data ? reinterpret_cast<const char*>(value->uri.data) : "", static_cast<size_t>(value->uri.length));
    jstring url = env->NewStringUTF(uri.c_str());
    if (value->header.type == NEOASTRA_EVENT_NEW_WINDOW_REQUESTED) {
        neoastra_result_t code = NEOASTRA_OK;
        if (popup_mode == 2 && !deferred_popup) {
            code = neoastra_decision_defer(value->decision);
            if (code == NEOASTRA_OK) {
                deferred_popup = value->decision;
                neoastra_decision_retain(deferred_popup);
            }
        } else {
            neoastra_decision_response_t answer{};
            answer.size = sizeof(answer); answer.version = 1;
            answer.action = popup_mode == 1 ? NEOASTRA_DECISION_ALLOW : NEOASTRA_DECISION_CANCEL;
            code = neoastra_decision_complete(value->decision, &answer, nullptr);
        }
        env->CallVoidMethod(activity, popup_method, reinterpret_cast<jlong>(context), static_cast<jint>(code), url);
    } else {
        env->CallVoidMethod(activity, loaded_method, reinterpret_cast<jlong>(context),
            value->header.type == NEOASTRA_EVENT_NAVIGATION_COMPLETED ? JNI_TRUE : JNI_FALSE, url);
    }
    env->DeleteLocalRef(url);
}
void NEOASTRA_CALL created(void* context, neoastra_result_t code, neoastra_view_t* view, const neoastra_error_t* error) {
    std::unique_ptr<pending> state(static_cast<pending*>(context));
    if (code == NEOASTRA_OK) {
        parents.emplace(view, state->parent);
        neoastra_view_set_event_callback(view, event, view);
        report(state->token, code, reinterpret_cast<jlong>(view));
    } else {
        jni()->DeleteGlobalRef(state->parent);
        failure(state->token, code, error);
    }
}
neoastra_result_t NEOASTRA_CALL resource(void*, const neoastra_resource_request_t*, neoastra_resource_response_t* response) {
    static const std::string html = "<!doctype html><body>projected-ok</body>";
    static const std::string mime = "text/html";
    ++resource_calls;
    response->status_code = 200;
    response->body_kind = NEOASTRA_RESOURCE_BODY_BYTES;
    response->mime_type = text(mime);
    response->bytes = reinterpret_cast<const uint8_t*>(html.data());
    response->byte_length = response->content_length = html.size();
    return NEOASTRA_OK;
}
void NEOASTRA_CALL environment_created(void*, neoastra_result_t code, neoastra_environment_t* value, const neoastra_error_t* error) {
    if (code != NEOASTRA_OK) { failure(0, code, error); return; }
    environment = value;
    neoastra_capability_info_t info{};
    info.size = sizeof(info); info.version = 1;
    code = neoastra_environment_get_capability(environment, NEOASTRA_CAPABILITY_REQUEST_FILTER, &info);
    if (code == NEOASTRA_OK && (info.support != NEOASTRA_SUPPORT_LIMITED || info.flags != 67))
        code = NEOASTRA_ERROR_NATIVE_FAILURE;
    report(0, code, static_cast<jlong>(info.flags));
}
struct description {
    neoastra_request_rule_t rule{};
    neoastra_request_filter_t filter{};
    description(int port, uint64_t flags = 0) {
        rule.size = sizeof(rule); rule.version = 1;
        rule.port = port; rule.schemes = NEOASTRA_REQUEST_HTTP | NEOASTRA_REQUEST_HTTPS;
        rule.kinds = NEOASTRA_REQUEST_KINDS_ALL;
        filter.size = sizeof(filter); filter.version = 1;
        filter.rules = port > 0 ? &rule : nullptr; filter.rule_count = port > 0 ? 1 : 0;
        filter.required_flags = flags;
    }
};
}

extern "C" JNIEXPORT void JNICALL Java_io_neoastra_filtertest_ProbeActivity_init(JNIEnv* env, jclass, jobject owner) {
    env->GetJavaVM(&vm);
    activity = env->NewGlobalRef(owner);
    jclass type = env->GetObjectClass(owner);
    result_method = env->GetMethodID(type, "onNativeResult", "(IIJLjava/lang/String;)V");
    loaded_method = env->GetMethodID(type, "onLoaded", "(JZLjava/lang/String;)V");
    popup_method = env->GetMethodID(type, "onPopup", "(JILjava/lang/String;)V");
    neoastra_app_options_t options{};
    options.size = sizeof(options); options.version = 1; options.shutdown_mode = NEOASTRA_APP_SHUTDOWN_EXPLICIT;
    auto code = neoastra_app_attach(&options, &app, nullptr);
    if (code != NEOASTRA_OK) { report(0, code); return; }
    static const std::string scheme_name = "probe";
    neoastra_custom_scheme_t scheme{};
    scheme.size = sizeof(scheme); scheme.version = 1; scheme.name = text(scheme_name);
    scheme.resource_provider = resource;
    neoastra_environment_options_t settings{};
    settings.size = sizeof(settings); settings.version = 1;
    settings.custom_schemes = &scheme; settings.custom_scheme_count = 1; settings.custom_scheme_stride = sizeof(scheme);
    code = neoastra_environment_create_async(app, &settings, environment_created, nullptr, nullptr, nullptr);
    if (code != NEOASTRA_OK) report(0, code);
}

extern "C" JNIEXPORT void JNICALL Java_io_neoastra_filtertest_ProbeActivity_create(JNIEnv* env, jclass, jint token, jobject parent, jint port) {
    description filter(port);
    neoastra_view_options_t options{};
    options.size = sizeof(options); options.version = 2; options.fill_parent = 1;
    auto* state = new pending{token, env->NewGlobalRef(parent)};
    options.parent.size = sizeof(options.parent); options.parent.version = 1;
    options.parent.kind = NEOASTRA_NATIVE_PARENT_ANDROID_VIEW_GROUP; options.parent.handle = state->parent;
    options.request_filter = &filter.filter;
    neoastra_error_t* error{};
    auto code = neoastra_environment_create_view_async(environment, &options, created, state, nullptr, &error);
    if (code != NEOASTRA_OK) { created(state, code, nullptr, error); }
    if (error) neoastra_error_release(error);
}

extern "C" JNIEXPORT void JNICALL Java_io_neoastra_filtertest_ProbeActivity_set(JNIEnv*, jclass, jint token, jlong value, jint port, jlong flags, jboolean cancel) {
    description filter(port, static_cast<uint64_t>(flags));
    auto* state = new pending{token};
    neoastra_operation_t* operation{};
    neoastra_error_t* error{};
    auto code = neoastra_view_set_request_filter_async(reinterpret_cast<neoastra_view_t*>(value), &filter.filter, completed, state, &operation, &error);
    if (code != NEOASTRA_OK) completed(state, code, error);
    if (operation) {
        if (cancel) neoastra_operation_cancel(operation);
        neoastra_operation_release(operation);
    }
    if (error) neoastra_error_release(error);
}

extern "C" JNIEXPORT jint JNICALL Java_io_neoastra_filtertest_ProbeActivity_navigate(JNIEnv* env, jclass, jlong value, jstring url) {
    const auto uri = from_java(env, url);
    return neoastra_view_navigate(reinterpret_cast<neoastra_view_t*>(value), text(uri), nullptr);
}
extern "C" JNIEXPORT void JNICALL Java_io_neoastra_filtertest_ProbeActivity_evaluate(JNIEnv* env, jclass, jint token, jlong value, jstring script) {
    const auto source = from_java(env, script);
    auto* state = new pending{token};
    neoastra_error_t* error{};
    auto code = neoastra_view_evaluate_script_async(reinterpret_cast<neoastra_view_t*>(value), text(source), evaluated, state, nullptr, &error);
    if (code != NEOASTRA_OK) completed(state, code, error);
    if (error) neoastra_error_release(error);
}
extern "C" JNIEXPORT jobject JNICALL Java_io_neoastra_filtertest_ProbeActivity_webView(JNIEnv* env, jclass, jlong value) {
    neoastra_native_handle_t handle{};
    handle.size = sizeof(handle); handle.version = 1;
    if (neoastra_view_get_native_handle(reinterpret_cast<neoastra_view_t*>(value), NEOASTRA_NATIVE_HANDLE_ANDROID_WEBVIEW, &handle) != NEOASTRA_OK) return nullptr;
    return env->NewLocalRef(static_cast<jobject>(handle.value));
}
extern "C" JNIEXPORT void JNICALL Java_io_neoastra_filtertest_ProbeActivity_destroy(JNIEnv* env, jclass, jlong value) {
    auto* view = reinterpret_cast<neoastra_view_t*>(value);
    const auto found = parents.find(view);
    if (found == parents.end()) return;
    neoastra_view_set_event_callback(view, nullptr, nullptr);
    neoastra_view_release(view);
    env->DeleteGlobalRef(found->second);
    parents.erase(found);
}
extern "C" JNIEXPORT jint JNICALL Java_io_neoastra_filtertest_ProbeActivity_resourceCalls(JNIEnv*, jclass) { return resource_calls.load(); }
extern "C" JNIEXPORT void JNICALL Java_io_neoastra_filtertest_ProbeActivity_setPopupMode(JNIEnv*, jclass, jint mode) {
    popup_mode = mode;
}
extern "C" JNIEXPORT jint JNICALL Java_io_neoastra_filtertest_ProbeActivity_answerPopup(JNIEnv*, jclass) {
    if (!deferred_popup) return NEOASTRA_ERROR_INVALID_STATE;
    neoastra_decision_response_t answer{};
    answer.size = sizeof(answer); answer.version = 1; answer.action = NEOASTRA_DECISION_ALLOW;
    const auto code = neoastra_decision_complete(deferred_popup, &answer, nullptr);
    neoastra_decision_release(deferred_popup); deferred_popup = nullptr;
    return code;
}
extern "C" JNIEXPORT void JNICALL Java_io_neoastra_filtertest_ProbeActivity_shutdown(JNIEnv* env, jclass) {
    while (!parents.empty()) Java_io_neoastra_filtertest_ProbeActivity_destroy(env, nullptr, reinterpret_cast<jlong>(parents.begin()->first));
    if (deferred_popup) { neoastra_decision_release(deferred_popup); deferred_popup = nullptr; }
    if (environment) { neoastra_environment_release(environment); environment = nullptr; }
    if (app) { neoastra_app_detach(app, nullptr); neoastra_app_release(app); app = nullptr; }
    env->DeleteGlobalRef(activity); activity = nullptr;
}
