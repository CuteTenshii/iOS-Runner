#include <jni.h>

#include <cstring>
#include <string>

#include "engine.h"
#include "localization.h"

namespace {

JavaVM* g_vm = nullptr;
jclass g_activity_class = nullptr;
jmethodID g_on_message = nullptr;

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

void send_message(const char* kind, const char* payload) {
    JNIEnv* env = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        // The guest thread lives for the whole process, so it stays attached.
        g_vm->AttachCurrentThread(&env, nullptr);
    }
    jstring java_kind = env->NewStringUTF(kind);
    jstring java_payload = to_java_string(env, payload);
    env->CallStaticVoidMethod(g_activity_class, g_on_message, java_kind, java_payload);
    env->DeleteLocalRef(java_kind);
    env->DeleteLocalRef(java_payload);
}

}  // namespace

extern "C" JNIEXPORT jint JNICALL Java_moe_tenshii_iosrunner_GuestActivity_nativeRun(
    JNIEnv* env, jclass activity_class, jstring bundle_dir, jstring executable, jstring log_path, jstring language) {
    env->GetJavaVM(&g_vm);
    g_activity_class = static_cast<jclass>(env->NewGlobalRef(activity_class));
    g_on_message = env->GetStaticMethodID(activity_class, "onEngineMessage", "(Ljava/lang/String;Ljava/lang/String;)V");
    engine_set_message_sink(send_message);
    localization_set_language(to_string(env, language));
    return run_app_bundle(to_string(env, bundle_dir), to_string(env, executable), to_string(env, log_path));
}

extern "C" JNIEXPORT void JNICALL Java_moe_tenshii_iosrunner_GuestActivity_nativeAction(JNIEnv*, jclass, jint action) {
    engine_post_action(action);
}
