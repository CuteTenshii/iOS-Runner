#include <jni.h>

#include <string>

#include "engine.h"

namespace {

std::string to_string(JNIEnv* env, jstring value) {
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string copy(chars);
    env->ReleaseStringUTFChars(value, chars);
    return copy;
}

}  // namespace

extern "C" JNIEXPORT jint JNICALL Java_moe_tenshii_iosrunner_EngineService_nativeRun(
    JNIEnv* env, jclass, jstring bundle_dir, jstring executable, jstring log_path) {
    return run_app_bundle(to_string(env, bundle_dir), to_string(env, executable), to_string(env, log_path));
}
