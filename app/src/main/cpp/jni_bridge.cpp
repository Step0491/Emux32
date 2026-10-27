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

static std::shared_ptr<emux::DbtEngine> gEngine = nullptr;
static ANativeWindow* gSavedWindow = nullptr;
static std::mutex gEngineMutex;

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_emux_translator_nativebridge_NativeEngine_nativeInitEngine(
    JNIEnv* /* env */,
    jobject /* thiz */,
    jlong guestRamMb
) {
    std::shared_ptr<emux::DbtEngine> oldEngine;
    {
        std::lock_guard<std::mutex> lock(gEngineMutex);
        oldEngine = gEngine;
    }
    if (oldEngine) {
        oldEngine->requestStop();
    }

    std::lock_guard<std::mutex> lock(gEngineMutex);
    LOGI("Inizializzazione Engine Nativo Unicorn DBT con %ld MB di RAM...", (long)guestRamMb);

    gEngine = std::make_shared<emux::DbtEngine>();
    if (gSavedWindow) {
        gEngine->setNativeSurface(gSavedWindow);
    }

    size_t ramBytes = static_cast<size_t>(guestRamMb) * 1024 * 1024;
    if (!gEngine->initialize(ramBytes)) {
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
    std::shared_ptr<emux::DbtEngine> engine;
    {
        std::lock_guard<std::mutex> lock(gEngineMutex);
        engine = gEngine;
    }
    if (!engine) return JNI_FALSE;
    engine->configurePerformance(
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

JNIEXPORT void JNICALL
Java_com_emux_translator_nativebridge_NativeEngine_nativeSetGameContext(
    JNIEnv* env,
    jobject /* thiz */,
    jstring packageName,
    jstring apkPath,
    jstring obbDir,
    jstring nativeLibDir,
    jstring filesDir
) {
    auto jstrToStd = [&](jstring js) -> std::string {
        if (!js) return "";
        const char* cs = env->GetStringUTFChars(js, nullptr);
        if (!cs) return "";
        std::string res(cs);
        env->ReleaseStringUTFChars(js, cs);
        return res;
    };

    emux::GuestRuntimeEnvironment::getInstance().setGameContext(
        jstrToStd(packageName),
        jstrToStd(apkPath),
        jstrToStd(obbDir),
        jstrToStd(nativeLibDir),
        jstrToStd(filesDir)
    );
}

JNIEXPORT jboolean JNICALL
Java_com_emux_translator_nativebridge_NativeEngine_nativeLoadElfLibrary(
    JNIEnv* env,
    jobject /* thiz */,
    jstring libraryPath
) {
    std::shared_ptr<emux::DbtEngine> engine;
    {
        std::lock_guard<std::mutex> lock(gEngineMutex);
        engine = gEngine;
    }
    if (!engine) {
        LOGE("Engine non inizializzato prima del caricamento ELF");
        return JNI_FALSE;
    }

    const char* nativePath = env->GetStringUTFChars(libraryPath, nullptr);
    if (!nativePath) return JNI_FALSE;
    std::string pathStr(nativePath);
    env->ReleaseStringUTFChars(libraryPath, nativePath);

    LOGI("Caricamento binario ELF 32-bit: %s", pathStr.c_str());
    return engine->loadLibrary(pathStr) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_emux_translator_nativebridge_NativeEngine_startEmulationNative(
    JNIEnv* /* env */,
    jobject /* thiz */
) {
    std::shared_ptr<emux::DbtEngine> engine;
    {
        std::lock_guard<std::mutex> lock(gEngineMutex);
        engine = gEngine;
    }
    if (!engine) {
        LOGE("Engine non inizializzato prima di startEmulationNative");
        return JNI_FALSE;
    }

    // NON mantenere gEngineMutex bloccato durante il loop a 60 FPS per evitare deadlock con UI/Touch/Surface
    LOGI("Invocazione startEmulationNative su Worker Thread isolato...");
    bool started = engine->startEmulation();
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
Java_com_emux_translator_nativebridge_NativeEngine_nativeStopEmulation(
    JNIEnv* /* env */,
    jobject /* thiz */
) {
    std::shared_ptr<emux::DbtEngine> engine;
    {
        std::lock_guard<std::mutex> lock(gEngineMutex);
        engine = gEngine;
    }
    if (engine) {
        engine->requestStop();
    }
}

JNIEXPORT void JNICALL
Java_com_emux_translator_nativebridge_NativeEngine_nativeSetSurface(
    JNIEnv* env,
    jobject /* thiz */,
    jobject surface
) {
    std::shared_ptr<emux::DbtEngine> engine;
    ANativeWindow* win = nullptr;
    {
        std::lock_guard<std::mutex> lock(gEngineMutex);
        if (gSavedWindow) {
            ANativeWindow_release(gSavedWindow);
            gSavedWindow = nullptr;
        }
        if (surface) {
            gSavedWindow = ANativeWindow_fromSurface(env, surface);
        }
        win = gSavedWindow;
        engine = gEngine;
    }
    if (engine) {
        engine->setNativeSurface(win);
    }
}

JNIEXPORT jboolean JNICALL
Java_com_emux_translator_nativebridge_NativeEngine_nativeSendTouchEvent(
    JNIEnv* /* env */,
    jobject /* thiz */,
    jint action,
    jfloat x,
    jfloat y,
    jint pointerId
) {
    std::shared_ptr<emux::DbtEngine> engine;
    {
        std::lock_guard<std::mutex> lock(gEngineMutex);
        engine = gEngine;
    }
    if (!engine) return JNI_FALSE;
    return engine->injectTouchEvent(action, x, y, pointerId) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_emux_translator_nativebridge_NativeEngine_nativeReleaseEngine(
    JNIEnv* /* env */,
    jobject /* thiz */
) {
    std::shared_ptr<emux::DbtEngine> engine;
    {
        std::lock_guard<std::mutex> lock(gEngineMutex);
        engine = std::move(gEngine);
        if (gSavedWindow) {
            ANativeWindow_release(gSavedWindow);
            gSavedWindow = nullptr;
        }
    }
    if (engine) {
        LOGI("Rilascio DbtEngine...");
        engine->requestStop();
        engine.reset();
    }
}

} // extern "C"
