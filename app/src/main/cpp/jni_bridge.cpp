#include "dbt_engine.h"
#include "linker_hook.h"

#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <string>
#include <memory>
#include <mutex>

#define TAG "EmuxJniBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static std::unique_ptr<emux::DbtEngine> gEngine = nullptr;
static std::mutex gEngineMutex;

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_emux_translator_nativebridge_NativeEngine_nativeInitEngine(
    JNIEnv* /* env */,
    jobject /* thiz */,
    jlong guestRamMb
) {
    std::lock_guard<std::mutex> lock(gEngineMutex);
    LOGI("Inizializzazione Engine Nativo Unicorn DBT con %ld MB di RAM...", (long)guestRamMb);

    gEngine = std::make_unique<emux::DbtEngine>();
    size_t ramBytes = static_cast<size_t>(guestRamMb) * 1024 * 1024;
    bool success = gEngine->initialize(ramBytes);

    if (!success) {
        LOGE("Inizializzazione DbtEngine fallita!");
        gEngine.reset();
        return JNI_FALSE;
    }

    LOGI("DbtEngine inizializzato con successo.");
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_emux_translator_nativebridge_NativeEngine_nativeConfigurePerformance(
    JNIEnv* /* env */,
    jobject /* thiz */,
    jint targetFps,
    jboolean enableFhd,
    jboolean throttleGuard
) {
    std::lock_guard<std::mutex> lock(gEngineMutex);
    if (!gEngine) {
        return JNI_FALSE;
    }
    gEngine->configurePerformance(
        static_cast<uint32_t>(targetFps),
        enableFhd == JNI_TRUE,
        throttleGuard == JNI_TRUE
    );
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_emux_translator_nativebridge_NativeEngine_nativeSetStoragePath(
    JNIEnv* env,
    jobject /* thiz */,
    jstring path
) {
    if (!path) return JNI_FALSE;
    const char* nativePath = env->GetStringUTFChars(path, nullptr);
    if (!nativePath) return JNI_FALSE;

    emux::SyscallDispatcher::setStoragePath(nativePath);
    env->ReleaseStringUTFChars(path, nativePath);
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_emux_translator_nativebridge_NativeEngine_nativeLoadElfLibrary(
    JNIEnv* env,
    jobject /* thiz */,
    jstring libraryPath
) {
    std::lock_guard<std::mutex> lock(gEngineMutex);
    if (!gEngine) {
        LOGE("Engine non inizializzato prima del caricamento ELF");
        return JNI_FALSE;
    }

    const char* nativePath = env->GetStringUTFChars(libraryPath, nullptr);
    if (!nativePath) {
        return JNI_FALSE;
    }

    std::string pathStr(nativePath);
    env->ReleaseStringUTFChars(libraryPath, nativePath);

    LOGI("Caricamento binario ELF 32-bit: %s", pathStr.c_str());
    bool loaded = gEngine->loadLibrary(pathStr);
    return loaded ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_emux_translator_nativebridge_NativeEngine_startEmulationNative(
    JNIEnv* /* env */,
    jobject /* thiz */
) {
    std::lock_guard<std::mutex> lock(gEngineMutex);
    if (!gEngine) {
        LOGE("Engine non inizializzato prima di startEmulationNative");
        return JNI_FALSE;
    }

    LOGI("Invocazione startEmulationNative su Unicorn Engine...");
    bool started = gEngine->startEmulation();
    return started ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_emux_translator_nativebridge_NativeEngine_nativeExecuteEntryPoint(
    JNIEnv* env,
    jobject thiz
) {
    return Java_com_emux_translator_nativebridge_NativeEngine_startEmulationNative(env, thiz);
}

JNIEXPORT void JNICALL
Java_com_emux_translator_nativebridge_NativeEngine_nativeSetSurface(
    JNIEnv* env,
    jobject /* thiz */,
    jobject surface
) {
    std::lock_guard<std::mutex> lock(gEngineMutex);
    if (!gEngine) return;

    if (surface) {
        ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
        gEngine->setNativeSurface(window);
    } else {
        gEngine->setNativeSurface(nullptr);
    }
}

JNIEXPORT void JNICALL
Java_com_emux_translator_nativebridge_NativeEngine_nativeReleaseEngine(
    JNIEnv* /* env */,
    jobject /* thiz */
) {
    std::lock_guard<std::mutex> lock(gEngineMutex);
    if (gEngine) {
        LOGI("Rilascio DbtEngine...");
        gEngine.reset();
    }
}

} // extern "C"
