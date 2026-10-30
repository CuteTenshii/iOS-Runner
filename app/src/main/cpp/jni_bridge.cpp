#include <dlfcn.h>
#include <jni.h>

#include <cstring>
#include <string>

#include "engine.h"
#include "foundation_shim.h"
#include "localization.h"
#include "swiftui_shim.h"

namespace {

JavaVM* g_vm = nullptr;
jclass g_activity_class = nullptr;
jmethodID g_on_message = nullptr;
jmethodID g_http = nullptr;
jmethodID g_cookies = nullptr;

std::string to_string(JNIEnv* env, jstring value) {
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string copy(chars);
    env->ReleaseStringUTFChars(value, chars);
    return copy;
}

// Passes text by bytes: NewStringUTF rejects the 4-byte UTF-8 that emoji in guest strings use.
jstring to_java_string(JNIEnv* env, const char* text) {
    const size_t length = strlen(text);
    jbyteArray bytes = env->NewByteArray(static_cast<jsize>(length));
    env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(length), reinterpret_cast<const jbyte*>(text));
    jclass string_class = env->FindClass("java/lang/String");
    jmethodID constructor = env->GetMethodID(string_class, "<init>", "([BLjava/lang/String;)V");
    jstring charset = env->NewStringUTF("UTF-8");
    auto result = static_cast<jstring>(env->NewObject(string_class, constructor, bytes, charset));
    env->DeleteLocalRef(charset);
    env->DeleteLocalRef(bytes);
    env->DeleteLocalRef(string_class);
    return result;
}

// A pending exception would abort at the next JNI call, so report it in the guest's log and carry on.
void report_exception(JNIEnv* env, const char* where) {
    if (!env->ExceptionCheck()) return;
    jthrowable thrown = env->ExceptionOccurred();
    env->ExceptionClear();
    jclass throwable = env->GetObjectClass(thrown);
    auto text = static_cast<jstring>(env->CallObjectMethod(thrown, env->GetMethodID(throwable, "toString", "()Ljava/lang/String;")));
    engine_log("HOST ERROR in %s: %s", where, text != nullptr ? to_string(env, text).c_str() : "unknown");
    env->DeleteLocalRef(throwable);
    env->DeleteLocalRef(thrown);
}

void send_message(const char* kind, const char* payload) {
    JNIEnv* env = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        // The guest thread lives for the whole process, so it stays attached.
        g_vm->AttachCurrentThread(&env, nullptr);
    }
    jstring java_kind = env->NewStringUTF(kind);
    jstring java_payload = to_java_string(env, payload);
    env->CallStaticVoidMethod(g_activity_class, g_on_message, java_kind, java_payload);
    report_exception(env, kind);
    env->DeleteLocalRef(java_kind);
    env->DeleteLocalRef(java_payload);
}

// liblog's structure, from API 30; a fatal VM error otherwise reaches only logcat, which some phones turn off.
struct LogMessage {
    size_t struct_size;
    int32_t buffer_id;
    int32_t priority;
    const char* tag;
    const char* file;
    uint32_t line;
    const char* message;
};

using LogHandler = void (*)(const LogMessage*);
LogHandler g_default_logger = nullptr;

constexpr int32_t kLogPriorityError = 6;

void copy_errors_to_engine_log(const LogMessage* message) {
    if (message->priority >= kLogPriorityError) engine_log("VM: %s: %s", message->tag != nullptr ? message->tag : "", message->message);
    if (g_default_logger != nullptr) g_default_logger(message);
}

void capture_vm_errors() {
    const auto set_logger = reinterpret_cast<void (*)(LogHandler)>(dlsym(RTLD_DEFAULT, "__android_log_set_logger"));
    g_default_logger = reinterpret_cast<LogHandler>(dlsym(RTLD_DEFAULT, "__android_log_logd_logger"));
    if (set_logger != nullptr) set_logger(copy_errors_to_engine_log);
}

JNIEnv* guest_env() {
    JNIEnv* env = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) g_vm->AttachCurrentThread(&env, nullptr);
    return env;
}

// The host returns the status as four big-endian bytes before the body, or null on failure.
HttpResult perform_http(const std::string& url, const std::string& method, const std::string& headers, const std::string& body) {
    JNIEnv* env = guest_env();
    jstring java_url = to_java_string(env, url.c_str());
    jstring java_method = to_java_string(env, method.c_str());
    jstring java_headers = to_java_string(env, headers.c_str());
    jbyteArray java_body = env->NewByteArray(static_cast<jsize>(body.size()));
    env->SetByteArrayRegion(java_body, 0, static_cast<jsize>(body.size()), reinterpret_cast<const jbyte*>(body.data()));
    auto result = static_cast<jbyteArray>(env->CallStaticObjectMethod(g_activity_class, g_http, java_url, java_method, java_headers, java_body));
    report_exception(env, "http");
    env->DeleteLocalRef(java_url);
    env->DeleteLocalRef(java_method);
    env->DeleteLocalRef(java_headers);
    env->DeleteLocalRef(java_body);
    HttpResult out;
    if (result == nullptr) return out;
    const jsize length = env->GetArrayLength(result);
    std::string bytes(static_cast<size_t>(length), '\0');
    env->GetByteArrayRegion(result, 0, length, reinterpret_cast<jbyte*>(bytes.data()));
    env->DeleteLocalRef(result);
    constexpr size_t kStatusBytes = 4;
    int status = 0;
    for (size_t i = 0; i < kStatusBytes; ++i) status = (status << 8) | static_cast<uint8_t>(bytes[i]);
    out.status = status;
    out.body = bytes.substr(kStatusBytes);
    return out;
}

std::string read_cookies() {
    JNIEnv* env = guest_env();
    auto cookies = static_cast<jstring>(env->CallStaticObjectMethod(g_activity_class, g_cookies));
    report_exception(env, "cookies");
    if (cookies == nullptr) return {};
    std::string text = to_string(env, cookies);
    env->DeleteLocalRef(cookies);
    return text;
}

}  // namespace

extern "C" JNIEXPORT jint JNICALL Java_moe_tenshii_iosrunner_GuestActivity_nativeRun(
    JNIEnv* env, jclass activity_class, jstring bundle_dir, jstring executable, jstring log_path, jstring language) {
    env->GetJavaVM(&g_vm);
    g_activity_class = static_cast<jclass>(env->NewGlobalRef(activity_class));
    g_on_message = env->GetStaticMethodID(activity_class, "onEngineMessage", "(Ljava/lang/String;Ljava/lang/String;)V");
    g_http = env->GetStaticMethodID(activity_class, "httpFetch", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[B)[B");
    capture_vm_errors();
    engine_set_message_sink(send_message);
    engine_set_http_handler(perform_http);
    g_cookies = env->GetStaticMethodID(activity_class, "readCookies", "()Ljava/lang/String;");
    engine_set_cookie_handler(read_cookies);
    localization_set_language(to_string(env, language));
    return run_app_bundle(to_string(env, bundle_dir), to_string(env, executable), to_string(env, log_path));
}

extern "C" JNIEXPORT void JNICALL Java_moe_tenshii_iosrunner_GuestActivity_nativeAction(JNIEnv*, jclass, jint action) {
    engine_post_action(action);
}

extern "C" JNIEXPORT void JNICALL Java_moe_tenshii_iosrunner_GuestActivity_nativeTextInput(JNIEnv* env, jclass, jint action, jstring text) {
    engine_post_event([action, value = to_string(env, text)] { swiftui_set_text(action, value); });
}

extern "C" JNIEXPORT void JNICALL Java_moe_tenshii_iosrunner_GuestActivity_nativeScriptFinished(JNIEnv* env, jclass, jint call, jstring json, jboolean failed) {
    foundation_js_finished(call, to_string(env, json), failed);
}

extern "C" JNIEXPORT void JNICALL Java_moe_tenshii_iosrunner_GuestActivity_nativeUrlChanged(JNIEnv* env, jclass, jint web_view, jstring url) {
    foundation_web_url_changed(web_view, to_string(env, url));
}

extern "C" JNIEXPORT void JNICALL Java_moe_tenshii_iosrunner_GuestActivity_nativeCookiesChanged(JNIEnv*, jclass) { foundation_cookies_changed(); }
