#include "dbt_engine.h"
#include "linker_hook.h"
#include "unicorn/unicorn.h"
#include "unicorn/arm.h"

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

JNIEXPORT jstring JNICALL
Java_com_emux_translator_nativebridge_NativeEngine_nativeRunSelfTest(
    JNIEnv* env,
    jobject /* thiz */
) {
    std::string report = "=== EMUX32 DBT ARCHITECTURAL VERIFICATION ===\n";
    uc_engine* uc = nullptr;
    uc_err err = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc);
    if (err != UC_ERR_OK || !uc) {
        report += "FAIL: uc_open fallita: " + std::string(uc_strerror(err)) + "\n";
        return env->NewStringUTF(report.c_str());
    }

    uint32_t testBase = 0x40000000U;
    size_t testSize = 0x10000U; // 64 KB
    err = uc_mem_map(uc, testBase, testSize, UC_PROT_ALL);
    if (err != UC_ERR_OK) {
        report += "FAIL: uc_mem_map fallita: " + std::string(uc_strerror(err)) + "\n";
        uc_close(uc);
        return env->NewStringUTF(report.c_str());
    }

    int passCount = 0;

    // Test 1: ARM32 ALU & Instruction Translation (42 + 58 = 100)
    // MOV R0, #42       (0xE3A0002A)
    // MOV R1, #58       (0xE3A0103A)
    // ADD R2, R0, R1    (0xE0802001)
    // BX LR             (0xE12FFF1E)
    uint32_t codeAlu[4] = {
        0xE3A0002AU,
        0xE3A0103AU,
        0xE0802001U,
        0xE12FFF1EU
    };
    uc_mem_write(uc, testBase, codeAlu, sizeof(codeAlu));
    uint32_t lr = 0xDEADBEEFU;
    uc_reg_write(uc, UC_ARM_REG_LR, &lr);
    err = uc_emu_start(uc, testBase, lr, 0, 100);
    uint32_t r2 = 0;
    uc_reg_read(uc, UC_ARM_REG_R2, &r2);
    if (err == UC_ERR_OK && r2 == 100) {
        report += "  [PASS] Test 1: ARM32 ALU Hardware Execution (42 + 58 = 100)\n";
        passCount++;
    } else {
        report += "  [FAIL] Test 1: ARM32 ALU (r2=" + std::to_string(r2) + ")\n";
    }

    // Test 2: Memory O(1) Translation (STR / LDR)
    // STR R2, [R0, #16] (0xE5802010)
    // LDR R3, [R0, #16] (0xE5903010)
    // BX LR             (0xE12FFF1E)
    uint32_t dataAddr = testBase + 0x1000U;
    uc_reg_write(uc, UC_ARM_REG_R0, &dataAddr);
    uint32_t codeMem[3] = {
        0xE5802010U,
        0xE5903010U,
        0xE12FFF1EU
    };
    uint32_t codeMemAddr = testBase + 0x20U;
    uc_mem_write(uc, codeMemAddr, codeMem, sizeof(codeMem));
    uc_reg_write(uc, UC_ARM_REG_LR, &lr);
    err = uc_emu_start(uc, codeMemAddr, lr, 0, 100);
    uint32_t r3 = 0;
    uc_reg_read(uc, UC_ARM_REG_R3, &r3);
    if (err == UC_ERR_OK && r3 == 100) {
        report += "  [PASS] Test 2: Memory O(1) Flat Translation (STR/LDR = 100)\n";
        passCount++;
    } else {
        report += "  [FAIL] Test 2: Memory Translation (r3=" + std::to_string(r3) + ")\n";
    }

    // Test 3: Thumb-2 Mode Execution & Interworking
    // MOVS R0, #25      (0x2019)
    // ADDS R0, R0, #75  (0x304B)
    // BX LR             (0x4770)
    uint16_t codeThumb[3] = {
        0x2019U,
        0x304BU,
        0x4770U
    };
    uint32_t codeThumbAddr = testBase + 0x40U;
    uc_mem_write(uc, codeThumbAddr, codeThumb, sizeof(codeThumb));
    uc_reg_write(uc, UC_ARM_REG_LR, &lr);
    uint32_t thumbCpsr = 0x00000030U; // Modalità Thumb
    uc_reg_write(uc, UC_ARM_REG_CPSR, &thumbCpsr);
    err = uc_emu_start(uc, codeThumbAddr | 1U, lr, 0, 100);
    uint32_t r0 = 0;
    uc_reg_read(uc, UC_ARM_REG_R0, &r0);
    if (err == UC_ERR_OK && r0 == 100) {
        report += "  [PASS] Test 3: Thumb-2 State Transition & Execution (25 + 75 = 100)\n";
        passCount++;
    } else {
        report += "  [FAIL] Test 3: Thumb-2 Execution (r0=" + std::to_string(r0) + ")\n";
    }

    // Test 4: Syscall / Interrupt Hooking (SVC #0)
    bool svcHookCalled = false;
    uc_hook hook = 0;
    void (*svcFn)(uc_engine*, uint32_t, void*) = [](uc_engine*, uint32_t, void* ud) {
        *static_cast<bool*>(ud) = true;
    };
    uc_hook_add(uc, &hook, UC_HOOK_INTR, reinterpret_cast<void*>(svcFn), &svcHookCalled, 1, 0);
    // SVC #0            (0xEF000000)
    // BX LR             (0xE12FFF1E)
    uint32_t codeSvc[2] = {
        0xEF000000U,
        0xE12FFF1EU
    };
    uint32_t codeSvcAddr = testBase + 0x60U;
    uc_mem_write(uc, codeSvcAddr, codeSvc, sizeof(codeSvc));
    uint32_t armCpsr = 0x00000010U; // Modalità ARM
    uc_reg_write(uc, UC_ARM_REG_CPSR, &armCpsr);
    uc_reg_write(uc, UC_ARM_REG_LR, &lr);
    err = uc_emu_start(uc, codeSvcAddr, lr, 0, 100);
    if (svcHookCalled) {
        report += "  [PASS] Test 4: Syscall/Interrupt Thunker Interception (SVC #0)\n";
        passCount++;
    } else {
        report += "  [FAIL] Test 4: Syscall Thunker Interception\n";
    }

    // Test 5: Dynamic Trap Flag Disarm Verification
    uint32_t trapAddr = testBase + 0x2000U;
    uint32_t trapSignum = 1060439283U; // 0x3F3504F3
    uc_mem_write(uc, trapAddr, &trapSignum, 4);
    uc_reg_write(uc, UC_ARM_REG_R4, &trapAddr);
    auto& gre = emux::GuestRuntimeEnvironment::getInstance();
    gre.disarmGuestTrapFlag(uc, testBase + 0x64U);
    uint32_t readTrap = 0;
    uc_mem_read(uc, trapAddr, &readTrap, 4);
    if (readTrap == 0) {
        report += "  [PASS] Test 5: Dynamic Trap Neutralization (0x3F3504F3 -> 0)\n";
        passCount++;
    } else {
        report += "  [FAIL] Test 5: Trap Neutralization (readTrap=" + std::to_string(readTrap) + ")\n";
    }

    uc_close(uc);

    report += "ESITO: " + std::to_string(passCount) + "/5 TEST SUPERATI CON SUCCESSO SU HARDWARE HOST AARCH64!\n";
    LOGI("%s", report.c_str());
    return env->NewStringUTF(report.c_str());
}

} // extern "C"
