#include "dbt_engine.h"
#include "linker_hook.h"
#include "core/guest_thread.h"
#include "core/gles_virtualizer.h"

#include <android/log.h>
#include <android/native_window.h>
#include <cstring>
#include <chrono>
#include <algorithm>

#define TAG "EmuxDbtEngine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace emux {

DbtEngine::DbtEngine() {
    governor_.configure(60, true);
}

DbtEngine::~DbtEngine() {
    requestStop();
    if (uc_) {
        if (intrHook_ != 0) {
            uc_hook_del(uc_, intrHook_);
            intrHook_ = 0;
        }
        uc_close(uc_);
        uc_ = nullptr;
    }
    destroyEgl();
    std::lock_guard<std::mutex> lock(surfaceMutex_);
    if (nativeWindow_) {
        ANativeWindow_release(nativeWindow_);
        nativeWindow_ = nullptr;
    }
}

void DbtEngine::requestStop() {
    stopRequested_.store(true);
    governor_.isRunning.store(false);
    surfaceCv_.notify_all();
    GuestThreadScheduler::getInstance().shutdown();
    if (uc_) {
        uc_emu_stop(uc_);
    }
}

void DbtEngine::configurePerformance(uint32_t targetFps, bool enableFhd, bool throttleGuard) {
    governor_.configure(targetFps, throttleGuard);
    LOGI("DbtEngine Performance Governor: Target %u FPS, FHD: %s, Throttle Guard: %s",
         targetFps, enableFhd ? "ON" : "OFF", throttleGuard ? "ON" : "OFF");
}

bool DbtEngine::ensureWorkerEgl(bool waitIfNull) {
    ANativeWindow* targetWindow = nullptr;
    {
        std::unique_lock<std::mutex> lock(surfaceMutex_);
        if (waitIfNull && !nativeWindow_ && !stopRequested_.load()) {
            surfaceCv_.wait_for(lock, std::chrono::milliseconds(1200), [this]() {
                return nativeWindow_ != nullptr || stopRequested_.load();
            });
        }
        targetWindow = nativeWindow_;
    }

    if (!targetWindow) {
        if (eglSurface_ != EGL_NO_SURFACE && eglDisplay_ != EGL_NO_DISPLAY) {
            eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroySurface(eglDisplay_, eglSurface_);
            eglSurface_ = EGL_NO_SURFACE;
            boundEglWindow_ = nullptr;
        }
        return false;
    }

    if (eglDisplay_ != EGL_NO_DISPLAY && eglContext_ != EGL_NO_CONTEXT &&
        eglSurface_ != EGL_NO_SURFACE && boundEglWindow_ == targetWindow && !surfaceDirty_.load()) {
        return true;
    }

    surfaceDirty_.store(false);

    if (eglDisplay_ == EGL_NO_DISPLAY) {
        eglDisplay_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (eglDisplay_ == EGL_NO_DISPLAY) {
            LOGE("eglGetDisplay fallito");
            return false;
        }
        EGLint major = 0, minor = 0;
        if (!eglInitialize(eglDisplay_, &major, &minor)) {
            LOGE("eglInitialize fallito");
            eglDisplay_ = EGL_NO_DISPLAY;
            return false;
        }
        LOGI("EGL inizializzato sul Worker Thread: versione %d.%d", major, minor);

        const EGLint attribs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
            EGL_BLUE_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_RED_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 24,
            EGL_STENCIL_SIZE, 8,
            EGL_NONE
        };

        EGLint numConfigs = 0;
        if (!eglChooseConfig(eglDisplay_, attribs, &eglConfig_, 1, &numConfigs) || numConfigs <= 0) {
            const EGLint fallbackAttribs[] = {
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
                EGL_BLUE_SIZE, 8,
                EGL_GREEN_SIZE, 8,
                EGL_RED_SIZE, 8,
                EGL_DEPTH_SIZE, 16,
                EGL_NONE
            };
            if (!eglChooseConfig(eglDisplay_, fallbackAttribs, &eglConfig_, 1, &numConfigs) || numConfigs <= 0) {
                LOGE("eglChooseConfig fallito");
                return false;
            }
        }

        const EGLint contextAttribs[] = {
            EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL_NONE
        };
        eglContext_ = eglCreateContext(eglDisplay_, eglConfig_, EGL_NO_CONTEXT, contextAttribs);
        if (eglContext_ == EGL_NO_CONTEXT) {
            LOGE("eglCreateContext fallito: 0x%x", eglGetError());
            return false;
        }
    }

    if (eglSurface_ != EGL_NO_SURFACE) {
        eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(eglDisplay_, eglSurface_);
        eglSurface_ = EGL_NO_SURFACE;
        boundEglWindow_ = nullptr;
    }

    eglSurface_ = eglCreateWindowSurface(eglDisplay_, eglConfig_, targetWindow, nullptr);
    if (eglSurface_ == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface fallito: 0x%x", eglGetError());
        return false;
    }
    boundEglWindow_ = targetWindow;

    if (!eglMakeCurrent(eglDisplay_, eglSurface_, eglSurface_, eglContext_)) {
        LOGE("eglMakeCurrent fallito: 0x%x", eglGetError());
        return false;
    }

    int width = ANativeWindow_getWidth(targetWindow);
    int height = ANativeWindow_getHeight(targetWindow);
    if (width <= 0) width = 1920;
    if (height <= 0) height = 1080;

    glViewport(0, 0, width, height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    LOGI("initEgl [WorkerThread]: Contesto EGL attivo e collegato a ANativeWindow (%dx%d).", width, height);
    GuestRuntimeEnvironment::getInstance().setEglState(eglDisplay_, eglSurface_, eglContext_, eglConfig_, &governor_);
    return true;
}

bool DbtEngine::initEgl() {
    return ensureWorkerEgl(false);
}

void DbtEngine::destroyEgl() {
    if (eglDisplay_ != EGL_NO_DISPLAY) {
        GlesVirtualizer::getInstance().shutdown();
        eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (eglSurface_ != EGL_NO_SURFACE) {
            eglDestroySurface(eglDisplay_, eglSurface_);
            eglSurface_ = EGL_NO_SURFACE;
        }
        if (eglContext_ != EGL_NO_CONTEXT) {
            eglDestroyContext(eglDisplay_, eglContext_);
            eglContext_ = EGL_NO_CONTEXT;
        }
        eglTerminate(eglDisplay_);
        eglDisplay_ = EGL_NO_DISPLAY;
        boundEglWindow_ = nullptr;
        LOGI("destroyEgl: Risorse grafiche EGL rilasciate.");
    }
    GuestRuntimeEnvironment::getInstance().setEglState(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_CONTEXT, nullptr, nullptr);
}

void DbtEngine::swapBuffers() {
    if (eglDisplay_ != EGL_NO_DISPLAY && eglSurface_ != EGL_NO_SURFACE) {
        static size_t sHostSwapCount = 0;
        if (sHostSwapCount++ < 5) {
            LOGI("DbtEngine: swapBuffers() eseguito su ANativeWindow EGLSurface (swapCount=%zu)", sHostSwapCount);
        }
        eglSwapBuffers(eglDisplay_, eglSurface_);
    }
}

void DbtEngine::setNativeSurface(ANativeWindow* window) {
    {
        std::lock_guard<std::mutex> lock(surfaceMutex_);
        if (nativeWindow_ == window && window != nullptr) {
            return;
        }
        if (nativeWindow_) {
            ANativeWindow_release(nativeWindow_);
            nativeWindow_ = nullptr;
        }
        nativeWindow_ = window;
        if (nativeWindow_) {
            ANativeWindow_acquire(nativeWindow_);
            ANativeWindow_setBuffersGeometry(nativeWindow_, 1920, 1080, WINDOW_FORMAT_RGBA_8888);
            LOGI("ANativeWindow registrata per il Worker Thread DBT (Full HD 1080p).");
        } else {
            LOGI("ANativeWindow scollegata.");
        }
        surfaceDirty_.store(true);
    }
    surfaceCv_.notify_all();
}

bool DbtEngine::injectTouchEvent(int action, float x, float y, int pointerId) {
    LOGI("DbtEngine Touch Queued: action=%d, x=%.1f, y=%.1f, id=%d", action, x, y, pointerId);
    GuestRuntimeEnvironment::getInstance().setLastTouchState(action, x, y, pointerId);
    if (!governor_.isRunning.load(std::memory_order_relaxed)) return false;

    std::lock_guard<std::mutex> lock(touchMutex_);
    if (touchQueue_.size() >= 64) {
        touchQueue_.pop_front();
    }
    touchQueue_.push_back({action, x, y, pointerId});
    return true;
}

void DbtEngine::processPendingTouchEvents() {
    if (!uc_) return;
    std::vector<TouchEventItem> events;
    {
        std::lock_guard<std::mutex> lock(touchMutex_);
        if (touchQueue_.empty()) return;
        events.assign(touchQueue_.begin(), touchQueue_.end());
        touchQueue_.clear();
    }

    auto& env = GuestRuntimeEnvironment::getInstance();
    const auto& nativeMethods = env.getRegisteredMethods();

    for (const auto& ev : events) {
        env.setLastTouchState(ev.action, ev.x, ev.y, ev.pointerId);
        bool handled = false;
        for (const auto& m : nativeMethods) {
            std::string lower = m.name;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower.find("touch") != std::string::npos || lower.find("injectevent") != std::string::npos) {
                uint32_t jniEnvPtr = env.getJNIEnvPointer();
                uint32_t dummyObj = 0x1002;
                uint32_t lr = 0xDEADBEEF;
                uint32_t sp = guestStackPointer_;
                uint32_t cpsr = (m.fnPtr & 1U) ? 0x00000030U : 0x00000010U;

                uc_reg_write(uc_, UC_ARM_REG_R0, &jniEnvPtr);
                uc_reg_write(uc_, UC_ARM_REG_R1, &dummyObj);

                if (m.signature.find("InputEvent") != std::string::npos ||
                    m.signature.find("MotionEvent") != std::string::npos) {
                    uint32_t motionEventHandle = 0x1050;
                    uc_reg_write(uc_, UC_ARM_REG_R2, &motionEventHandle);
                } else {
                    uint32_t act = static_cast<uint32_t>(ev.action);
                    uint32_t rawX = 0, rawY = 0;
                    std::memcpy(&rawX, &ev.x, 4);
                    std::memcpy(&rawY, &ev.y, 4);
                    uc_reg_write(uc_, UC_ARM_REG_R2, &act);
                    uc_reg_write(uc_, UC_ARM_REG_R3, &rawX);
                    uc_mem_write(uc_, sp, &rawY, 4);
                }

                uc_reg_write(uc_, UC_ARM_REG_SP, &sp);
                uc_reg_write(uc_, UC_ARM_REG_LR, &lr);
                uc_reg_write(uc_, UC_ARM_REG_CPSR, &cpsr);
                uc_emu_start(uc_, m.fnPtr, lr, 0, 200000);
                handled = true;
                break;
            }
        }

        // Fallback Cocos2d-x nativeTouchesBegin / nativeTouchesMove / nativeTouchesEnd
        if (!handled) {
            for (const auto& lib : loadedLibs_) {
                if (!lib) continue;
                for (const auto& sym : lib->symbols) {
                    if (sym.vaddr == 0) continue;
                    std::string lower = sym.name;
                    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                    bool match = false;
                    if (ev.action == 0 && (lower.find("touchesbegin") != std::string::npos || lower.find("touchdown") != std::string::npos)) match = true;
                    else if (ev.action == 2 && (lower.find("touchesmove") != std::string::npos || lower.find("touchmove") != std::string::npos)) match = true;
                    else if (ev.action == 1 && (lower.find("touchesend") != std::string::npos || lower.find("touchup") != std::string::npos)) match = true;
                    else if (ev.action == 3 && (lower.find("touchcancel") != std::string::npos)) match = true;

                    if (match) {
                        uint32_t fnPtr = lib->load_bias + sym.vaddr;
                        uint32_t jniEnvPtr = env.getJNIEnvPointer();
                        uint32_t dummyObj = 0x1002;
                        uint32_t pId = static_cast<uint32_t>(ev.pointerId);
                        uint32_t rawX = 0, rawY = 0;
                        std::memcpy(&rawX, &ev.x, 4);
                        std::memcpy(&rawY, &ev.y, 4);
                        uint32_t lr = 0xDEADBEEF;
                        uint32_t sp = guestStackPointer_;
                        uint32_t cpsr = (fnPtr & 1U) ? 0x00000030U : 0x00000010U;

                        uc_reg_write(uc_, UC_ARM_REG_R0, &jniEnvPtr);
                        uc_reg_write(uc_, UC_ARM_REG_R1, &dummyObj);
                        uc_reg_write(uc_, UC_ARM_REG_R2, &pId);
                        uc_reg_write(uc_, UC_ARM_REG_R3, &rawX);
                        uc_mem_write(uc_, sp, &rawY, 4);
                        uc_reg_write(uc_, UC_ARM_REG_SP, &sp);
                        uc_reg_write(uc_, UC_ARM_REG_LR, &lr);
                        uc_reg_write(uc_, UC_ARM_REG_CPSR, &cpsr);
                        uc_emu_start(uc_, fnPtr, lr, 0, 200000);
                        handled = true;
                        break;
                    }
                }
                if (handled) break;
            }
        }
    }
}

bool DbtEngine::initialize(size_t /* guestRamSize */) {
    LOGI("Inizializzazione Unicorn Engine per architettura ARMv7-A...");

    uc_err err = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc_);
    if (err != UC_ERR_OK) {
        LOGE("uc_open fallito: %s (codice: %d)", uc_strerror(err), err);
        return false;
    }

    GuestRuntimeEnvironment::getInstance().initialize(uc_);

    err = uc_hook_add(
        uc_,
        &intrHook_,
        UC_HOOK_INTR,
        reinterpret_cast<void*>(hookInterruptCallback),
        this,
        1,
        0
    );

    if (err != UC_ERR_OK) {
        LOGE("uc_hook_add UC_HOOK_INTR fallito: %s", uc_strerror(err));
        uc_close(uc_);
        uc_ = nullptr;
        return false;
    }

    isInitialized_ = true;
    LOGI("Unicorn Engine inizializzato con successo (hook id=%zu).", intrHook_);
    return true;
}

void DbtEngine::setupGuestStack() {
    if (!uc_) return;

    uint32_t alignedStackBase = PAGE_ALIGN_DOWN(guestStackBase_);
    uint32_t alignedStackSize = PAGE_ALIGN_UP(guestStackSize_);

    uc_err err = uc_mem_map(uc_, alignedStackBase, alignedStackSize, UC_PROT_READ | UC_PROT_WRITE);
    if (err != UC_ERR_OK && err != UC_ERR_MAP) {
        LOGE("uc_mem_map dello Stack guest fallita: %s", uc_strerror(err));
        return;
    }

    guestStackPointer_ = (alignedStackBase + alignedStackSize - 0x1000) & ~7U;
    uc_reg_write(uc_, UC_ARM_REG_SP, &guestStackPointer_);
    LOGI("Stack Guest (2MB) configurato: SP=0x%08x", guestStackPointer_);
}

bool DbtEngine::loadLibrary(const std::string& libraryPath) {
    if (!uc_) {
        LOGE("Engine non inizializzato prima del caricamento ELF!");
        return false;
    }

    auto& env = GuestRuntimeEnvironment::getInstance();
    if (env.getNativeLibDir().empty()) {
        size_t slash = libraryPath.find_last_of('/');
        if (slash != std::string::npos) {
            env.setGameContext(
                env.getPackageName(),
                env.getApkPath(),
                env.getObbDir(),
                libraryPath.substr(0, slash),
                env.getFilesDir()
            );
        }
    }

    auto lib = loader_.loadFromFile(libraryPath);
    if (!lib) {
        LOGE("Caricamento ELF fallito da: %s", libraryPath.c_str());
        return false;
    }

    if (!lib->mapToUnicorn(uc_)) {
        LOGE("Mappatura segmenti ELF in Unicorn fallita per: %s", libraryPath.c_str());
        return false;
    }

    env.registerLoadedLibrary(lib);
    lib->applyRelocations(uc_);

    loadedLibs_.push_back(lib);
    currentLib_ = lib;

    LOGI("Libreria ELF caricata e rilocata: %s (Entry: 0x%08x)", libraryPath.c_str(), lib->entry_point);
    return true;
}

bool DbtEngine::startEmulation() {
    if (!uc_ || !currentLib_) {
        LOGE("Engine o libreria ELF non pronta per startEmulation");
        return false;
    }

    governor_.isRunning.store(true);
    stopRequested_.store(false);

    // 1. Setup Stack Guest
    setupGuestStack();

    // Inizializzazione Sottosistema Multi-Threading Guest
    GuestThreadScheduler::getInstance().initialize(uc_);

    // 2. Inizializzazione EGL esclusivamente sul Worker Thread (attende fino a 1.2s se SurfaceView in creazione)
    ensureWorkerEgl(true);

    // Inizializzazione Layer Virtualizzazione Vertex Array & Shadow VBO
    GlesVirtualizer::getInstance().initialize();

    // 3. Esecuzione costruttori statici C++ (DT_PREINIT_ARRAY / DT_INIT / DT_INIT_ARRAY) per tutte le librerie
    for (const auto& lib : loadedLibs_) {
        if (lib) {
            lib->executeInitFunctions(uc_, guestStackPointer_);
        }
    }

    auto& env = GuestRuntimeEnvironment::getInstance();
    uint32_t javaVMPtr = env.getJavaVMPointer();
    uint32_t jniEnvPtr = env.getJNIEnvPointer();
    uint32_t lr = 0xDEADBEEF;

    auto invokeGuestFunction = [&](uint32_t fnAddr, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, size_t maxInsns) -> uint32_t {
        if (fnAddr == 0) return 0;
        uint32_t sp = guestStackPointer_;
        uint32_t cpsr = (fnAddr & 1U) ? 0x00000030U : 0x00000010U;
        uint32_t startPc = fnAddr & ~1U;
        uc_reg_write(uc_, UC_ARM_REG_R0, &r0);
        uc_reg_write(uc_, UC_ARM_REG_R1, &r1);
        uc_reg_write(uc_, UC_ARM_REG_R2, &r2);
        uc_reg_write(uc_, UC_ARM_REG_R3, &r3);
        uc_reg_write(uc_, UC_ARM_REG_SP, &sp);
        uc_reg_write(uc_, UC_ARM_REG_LR, &lr);
        uc_reg_write(uc_, UC_ARM_REG_CPSR, &cpsr);
        uc_err startErr = uc_emu_start(uc_, startPc, lr, 0, maxInsns);
        if (startErr != UC_ERR_OK) {
            uint32_t curPc = 0;
            uc_reg_read(uc_, UC_ARM_REG_PC, &curPc);
            LOGE("invokeGuestFunction(0x%08x): uc_emu_start terminato con errore: %s (PC=0x%08x)",
                 fnAddr, uc_strerror(startErr), curPc);
        }
        uint32_t ret = 0;
        uc_reg_read(uc_, UC_ARM_REG_R0, &ret);
        return ret;
    };

    // 4. Invocazione JNI_OnLoad su tutte le librerie caricate che esportano JNI_OnLoad
    for (const auto& lib : loadedLibs_) {
        if (!lib) continue;
        uint32_t jniOnLoadAddr = 0;
        for (const auto& sym : lib->symbols) {
            if (sym.name == "JNI_OnLoad" && sym.vaddr != 0) {
                jniOnLoadAddr = lib->load_bias + sym.vaddr;
                break;
            }
        }
        if (jniOnLoadAddr == 0 && lib->entry_point != 0 && lib->path.find("libmono") == std::string::npos) {
            bool isNativeActivity = false;
            for (const auto& sym : lib->symbols) {
                if (sym.name == "ANativeActivity_onCreate") {
                    isNativeActivity = true;
                    break;
                }
            }
            if (!isNativeActivity) {
                jniOnLoadAddr = lib->entry_point;
            }
        }

        if (jniOnLoadAddr != 0) {
            LOGI("Avvio JNI_OnLoad per '%s': PC=0x%08x, SP=0x%08x, JavaVM=0x%08x...",
                 lib->path.c_str(), jniOnLoadAddr, guestStackPointer_, javaVMPtr);
            uint32_t jniVer = invokeGuestFunction(jniOnLoadAddr, javaVMPtr, 0, 0, 0, 50000000);
            LOGI("JNI_OnLoad '%s' completato (Versione ritornata: 0x%08x, Metodi registrati finora: %zu).",
                 lib->path.c_str(), jniVer, env.getRegisteredMethods().size());
        }
    }

    // 5. Sequenza di avvio signature-aware per i metodi JNI registrati (Unity / NDK / libg.so)
    const auto& nativeMethods = env.getRegisteredMethods();
    uint32_t dummyInstance = 0x1001;
    uint32_t dummySurface = 0x1003;

    // 5a. Unity/Bionic init del contesto (initJni) - DEVE ESSERE PRIMA DI nativeFile!
    for (const auto& m : nativeMethods) {
        std::string lower = m.name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower == "initjni" || lower == "nativeinitjni") {
            LOGI("Invocazione metodo guest '%s' (Context=0x1001)...", m.name.c_str());
            invokeGuestFunction(m.fnPtr, jniEnvPtr, dummyInstance, 0x1001, 0, 2000000);
        }
    }

    // 5b. Unity nativeFile(String apkPath) - eseguito dopo initJni
    for (const auto& m : nativeMethods) {
        if (m.name == "nativeFile" && !env.getApkPath().empty()) {
            uint32_t jstrApk = env.createGuestJString(uc_, env.getApkPath());
            LOGI("Invocazione metodo guest '%s' con APK '%s'...", m.name.c_str(), env.getApkPath().c_str());
            invokeGuestFunction(m.fnPtr, jniEnvPtr, dummyInstance, jstrApk, 0, 20000000);
        }
    }

    // 5c. Altri metodi di init dedicati (init, nativeInit, create)
    for (const auto& m : nativeMethods) {
        std::string lower = m.name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower == "initjni" || lower == "nativeinitjni" || lower == "nativefile") continue;
        if (lower.find("recreate") != std::string::npos || lower.find("surface") != std::string::npos || lower.find("gfx") != std::string::npos) {
            continue; // Riservato alla fase 5d con la Surface valida
        }

        if (lower == "init" && m.signature == "(IZ)V") {
            LOGI("Invocazione metodo guest '%s' (0, false)...", m.name.c_str());
            invokeGuestFunction(m.fnPtr, jniEnvPtr, dummyInstance, 0, 0, 20000000);
        } else if (lower == "nativeinit" || lower == "init" || lower == "nativecreate" || lower == "create" || lower.find("init") != std::string::npos) {
            LOGI("Invocazione metodo guest di setup '%s' (0x%08x, sig='%s')...", m.name.c_str(), m.fnPtr, m.signature.c_str());
            if (m.signature.find("(II)") != std::string::npos) {
                invokeGuestFunction(m.fnPtr, jniEnvPtr, dummyInstance, 1920, 1080, 50000000);
            } else if (m.signature.find("(Landroid/content/Context;)") != std::string::npos) {
                invokeGuestFunction(m.fnPtr, jniEnvPtr, dummyInstance, 0x1001, 0, 20000000);
            } else if (m.signature.empty() || m.signature == "()V" || m.signature == "()Z") {
                invokeGuestFunction(m.fnPtr, jniEnvPtr, dummyInstance, 0, 0, 20000000);
            } else {
                invokeGuestFunction(m.fnPtr, jniEnvPtr, dummyInstance, 0x1001, 0, 20000000);
            }
        }
    }

    // 5d. Collegamento Surface e Focus/Resume (nativeRecreateGfxState, onSurfaceChanged, nativeFocusChanged, nativeResume)
    for (const auto& m : nativeMethods) {
        std::string lower = m.name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.find("recreategfxstate") != std::string::npos || lower.find("setsurface") != std::string::npos) {
            LOGI("Invocazione metodo grafico Surface '%s' (0x%08x)...", m.name.c_str(), m.fnPtr);
            if (m.signature.find("(IL") != std::string::npos) {
                invokeGuestFunction(m.fnPtr, jniEnvPtr, dummyInstance, 0, dummySurface, 30000000);
            } else {
                invokeGuestFunction(m.fnPtr, jniEnvPtr, dummyInstance, dummySurface, 0, 30000000);
            }
        } else if (lower.find("surfacechanged") != std::string::npos || lower.find("resize") != std::string::npos || lower.find("onsurfacechanged") != std::string::npos) {
            LOGI("Invocazione resize grafico '%s' (1920x1080)...", m.name.c_str());
            invokeGuestFunction(m.fnPtr, jniEnvPtr, dummyInstance, 1920, 1080, 30000000);
        } else if (lower.find("resume") != std::string::npos) {
            LOGI("Invocazione metodo di unpause/resume '%s'...", m.name.c_str());
            invokeGuestFunction(m.fnPtr, jniEnvPtr, dummyInstance, 0, 0, 10000000);
        } else if (lower.find("focuschanged") != std::string::npos) {
            LOGI("Invocazione metodo di window focus '%s' (focus=true)...", m.name.c_str());
            invokeGuestFunction(m.fnPtr, jniEnvPtr, dummyInstance, 1, 0, 10000000);
        }
    }

    // 5e. Fallback dinamico su simboli esportati .dynsym (Cocos2d-x / Flappy Bird / NDK)
    for (const auto& lib : loadedLibs_) {
        if (!lib) continue;
        for (const auto& sym : lib->symbols) {
            if (sym.vaddr == 0) continue;
            std::string sName = sym.name;
            std::string lower = sName;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

            uint32_t symAddr = lib->load_bias + sym.vaddr;

            if (lower.find("setapkpath") != std::string::npos || lower.find("nativesetapkpath") != std::string::npos) {
                if (!env.getApkPath().empty()) {
                    uint32_t jstrApk = env.createGuestJString(uc_, env.getApkPath());
                    LOGI("Invocazione dinamica '%s' @ 0x%08x (apk='%s')...", sName.c_str(), symAddr, env.getApkPath().c_str());
                    invokeGuestFunction(symAddr, jniEnvPtr, dummyInstance, jstrApk, 0, 1000000);
                }
            } else if (lower.find("nativeinit") != std::string::npos || (lower.find("renderer") != std::string::npos && lower.find("init") != std::string::npos)) {
                LOGI("Invocazione dinamica Cocos2d-x '%s' @ 0x%08x (1920x1080)...", sName.c_str(), symAddr);
                invokeGuestFunction(symAddr, jniEnvPtr, dummyInstance, 1920, 1080, 3000000);
            } else if (lower.find("onsurfacechanged") != std::string::npos || lower.find("nativeonsurfacechanged") != std::string::npos) {
                LOGI("Invocazione dinamica SurfaceChanged '%s' @ 0x%08x (1920x1080)...", sName.c_str(), symAddr);
                invokeGuestFunction(symAddr, jniEnvPtr, dummyInstance, 1920, 1080, 2000000);
            } else if (lower.find("nativeonresume") != std::string::npos || lower.find("onresume") != std::string::npos) {
                LOGI("Invocazione dinamica Resume '%s' @ 0x%08x...", sName.c_str(), symAddr);
                invokeGuestFunction(symAddr, jniEnvPtr, dummyInstance, 0, 0, 1000000);
            }
        }
    }

    // 5f. Supporto Android NativeActivity (Blood & Glory / Glu Mobile / libmain.so)
    uint32_t nativeActivityOnCreateAddr = 0;
    for (const auto& lib : loadedLibs_) {
        if (!lib) continue;
        for (const auto& sym : lib->symbols) {
            if (sym.name == "ANativeActivity_onCreate" && sym.vaddr != 0) {
                nativeActivityOnCreateAddr = lib->load_bias + sym.vaddr;
                break;
            }
        }
        if (nativeActivityOnCreateAddr != 0) break;
    }

    uint32_t nativeActivityRedrawAddr = 0;
    uint32_t activityPtr = 0x7d002900U;
    uint32_t winHandle = env.getJNIEnvPointer() + 0x2800U;

    if (nativeActivityOnCreateAddr != 0 && env.getRegisteredMethods().empty()) {
        LOGI("ANativeActivity rilevata: avvio ANativeActivity_onCreate @ 0x%08x...", nativeActivityOnCreateAddr);
        uint32_t callbacksPtr = 0x7d002880U;
        uint32_t internalPathStr = env.createGuestCString(uc_, env.getFilesDir());
        uint32_t externalPathStr = env.createGuestCString(uc_, env.getObbDir().empty() ? env.getFilesDir() : env.getObbDir());
        uint32_t obbPathStr = env.createGuestCString(uc_, env.getObbDir());

        uint32_t sdkVer = 27;
        uint32_t mockAssetMgr = env.getJNIEnvPointer() + 0x2800U;
        uc_mem_write(uc_, activityPtr + 0, &callbacksPtr, 4);
        uc_mem_write(uc_, activityPtr + 4, &javaVMPtr, 4);
        uc_mem_write(uc_, activityPtr + 8, &jniEnvPtr, 4);
        uc_mem_write(uc_, activityPtr + 12, &dummyInstance, 4);
        uc_mem_write(uc_, activityPtr + 16, &internalPathStr, 4);
        uc_mem_write(uc_, activityPtr + 20, &externalPathStr, 4);
        uc_mem_write(uc_, activityPtr + 24, &sdkVer, 4);
        uc_mem_write(uc_, activityPtr + 32, &mockAssetMgr, 4);
        uc_mem_write(uc_, activityPtr + 36, &obbPathStr, 4);

        invokeGuestFunction(nativeActivityOnCreateAddr, activityPtr, 0, 0, 0, 3000000);
        LOGI("ANativeActivity_onCreate completata.");

        uint32_t onStart = 0, onResume = 0, onWindowFocus = 0, onWindowCreated = 0, onRedraw = 0;
        uc_mem_read(uc_, callbacksPtr + 0, &onStart, 4);
        uc_mem_read(uc_, callbacksPtr + 4, &onResume, 4);
        uc_mem_read(uc_, callbacksPtr + 24, &onWindowFocus, 4);
        uc_mem_read(uc_, callbacksPtr + 28, &onWindowCreated, 4);
        uc_mem_read(uc_, callbacksPtr + 36, &onRedraw, 4);

        if (onStart) {
            LOGI("Invocazione ANativeActivity onStart @ 0x%08x...", onStart);
            invokeGuestFunction(onStart, activityPtr, 0, 0, 0, 1000000);
        }
        if (onWindowCreated) {
            LOGI("Invocazione ANativeActivity onNativeWindowCreated @ 0x%08x...", onWindowCreated);
            invokeGuestFunction(onWindowCreated, activityPtr, winHandle, 0, 0, 2000000);
        }
        if (onResume) {
            LOGI("Invocazione ANativeActivity onResume @ 0x%08x...", onResume);
            invokeGuestFunction(onResume, activityPtr, 0, 0, 0, 1000000);
        }
        if (onWindowFocus) {
            LOGI("Invocazione ANativeActivity onWindowFocusChanged @ 0x%08x...", onWindowFocus);
            invokeGuestFunction(onWindowFocus, activityPtr, 1, 0, 0, 1000000);
        }
        if (onRedraw) {
            nativeActivityRedrawAddr = onRedraw;
        }
    }

    // 6. Ricerca metodo di rendering per il loop continuo a 60 FPS
    uint32_t targetRenderPtr = 0;
    std::string targetRenderName;
    for (const auto& m : nativeMethods) {
        std::string lower = m.name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower == "nativerender" || lower.find("render") != std::string::npos ||
            lower.find("ondrawframe") != std::string::npos || lower.find("draw") != std::string::npos ||
            lower.find("step") != std::string::npos || lower.find("update") != std::string::npos ||
            lower.find("frame") != std::string::npos || lower.find("tick") != std::string::npos) {
            targetRenderPtr = m.fnPtr;
            targetRenderName = m.name;
            break;
        }
    }

    if (targetRenderPtr == 0) {
        for (const auto& lib : loadedLibs_) {
            if (!lib) continue;
            for (const auto& sym : lib->symbols) {
                std::string lower = sym.name;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                if ((lower.find("nativerender") != std::string::npos ||
                     lower.find("ondrawframe") != std::string::npos ||
                     lower.find("renderframe") != std::string::npos ||
                     lower.find("nativestep") != std::string::npos) && sym.vaddr != 0) {
                    targetRenderPtr = lib->load_bias + sym.vaddr;
                    targetRenderName = sym.name;
                    break;
                }
            }
            if (targetRenderPtr != 0) break;
        }
    }

    if (targetRenderPtr == 0 && nativeActivityRedrawAddr != 0) {
        targetRenderPtr = nativeActivityRedrawAddr;
        targetRenderName = "ANativeActivity_onNativeWindowRedrawNeeded";
    }

    LOGI("DbtEngine: Avvio LOOP DI GIOCO ATTIVO A 60 FPS (RenderMethod='%s' @ 0x%08x)...",
         targetRenderName.empty() ? "EGL-PresentLoop" : targetRenderName.c_str(), targetRenderPtr);

    size_t frameCount = 0;
    auto lastHeartbeat = std::chrono::steady_clock::now();
    size_t framesSinceHeartbeat = 0;

    while (governor_.isRunning.load(std::memory_order_relaxed) && !stopRequested_.load(std::memory_order_relaxed)) {
        auto frameStart = std::chrono::steady_clock::now();

        ensureWorkerEgl(false);
        processPendingTouchEvents();
        env.consumeFrameSwapFlag();

        if (targetRenderPtr != 0) {
            uint32_t arg0 = (targetRenderPtr == nativeActivityRedrawAddr) ? activityPtr : jniEnvPtr;
            uint32_t arg1 = (targetRenderPtr == nativeActivityRedrawAddr) ? winHandle : dummyInstance;
            uint32_t renderRet = invokeGuestFunction(targetRenderPtr, arg0, arg1, 0, 0, 25000000);
            if (frameCount < 10 || frameCount % 30 == 0) {
                LOGI("DbtEngine: Frame %zu eseguito su %s (status=0x%x)", frameCount + 1, targetRenderName.c_str(), renderRet);
            }
        }

        if (!env.consumeFrameSwapFlag()) {
            swapBuffers();
        }

        frameCount++;
        framesSinceHeartbeat++;

        auto now = std::chrono::steady_clock::now();
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastHeartbeat).count();
        if (elapsedMs >= 2000) {
            float fps = (framesSinceHeartbeat * 1000.0f) / static_cast<float>(elapsedMs);
            size_t pendingCount = 0;
            {
                std::lock_guard<std::mutex> lock(touchMutex_);
                pendingCount = touchQueue_.size();
            }
            LOGI("DbtEngine Heartbeat: %zu frame renderizzati (velocità media: %.1f FPS, touch in coda: %zu)",
                 frameCount, fps, pendingCount);
            lastHeartbeat = now;
            framesSinceHeartbeat = 0;
        }

        // Nei primi 60 frame (warm-up e parsing risorse Unity) lasciamo girare la CPU a piena potenza
        if (frameCount > 60) {
            governor_.syncFrame(frameStart);
        }
    }

    LOGI("DbtEngine: Ciclo di gioco terminato (totale frame: %zu). Rilascio EGL sul Worker Thread.", frameCount);
    destroyEgl();
    governor_.isRunning.store(false);
    return true;
}

void DbtEngine::hookInterruptCallback(uc_engine* /* uc */, uint32_t intno, void* userData) {
    if (!userData) return;
    auto* engine = static_cast<DbtEngine*>(userData);
    engine->onInterrupt(intno);
}

void DbtEngine::onInterrupt(uint32_t intno) {
    if (!uc_) return;

    if (intno & 0x80000000U) {
        uint32_t svcComment = intno & 0x00FFFFFFU;
        GuestRuntimeEnvironment::getInstance().handleTrampolineCall(svcComment, uc_);
        return;
    }

    uint32_t r7 = 0, r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0;
    uc_reg_read(uc_, UC_ARM_REG_R7, &r7);
    uc_reg_read(uc_, UC_ARM_REG_R0, &r0);
    uc_reg_read(uc_, UC_ARM_REG_R1, &r1);
    uc_reg_read(uc_, UC_ARM_REG_R2, &r2);
    uc_reg_read(uc_, UC_ARM_REG_R3, &r3);
    uc_reg_read(uc_, UC_ARM_REG_R4, &r4);
    uc_reg_read(uc_, UC_ARM_REG_R5, &r5);

    int32_t ret = SyscallDispatcher::handleSyscall(r7, r0, r1, r2, r3, r4, r5, uc_);
    if (r7 != 173) { // 173 = rt_sigreturn (lo stato dei registri è già stato ripristinato da ucontext_t)
        uint32_t retVal = static_cast<uint32_t>(ret);
        uc_reg_write(uc_, UC_ARM_REG_R0, &retVal);
    }
}

} // namespace emux
