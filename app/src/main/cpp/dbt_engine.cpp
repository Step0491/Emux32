#include "dbt_engine.h"
#include "linker_hook.h"

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
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
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
                EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
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
    GuestRuntimeEnvironment::getInstance().setEglState(eglDisplay_, eglSurface_, eglContext_, &governor_);
    return true;
}

bool DbtEngine::initEgl() {
    return ensureWorkerEgl(false);
}

void DbtEngine::destroyEgl() {
    if (eglDisplay_ != EGL_NO_DISPLAY) {
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
    GuestRuntimeEnvironment::getInstance().setEglState(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_CONTEXT, nullptr);
}

void DbtEngine::swapBuffers() {
    if (eglDisplay_ != EGL_NO_DISPLAY && eglSurface_ != EGL_NO_SURFACE) {
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
                break;
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

    // 2. Inizializzazione EGL esclusivamente sul Worker Thread (attende fino a 1.2s se SurfaceView in creazione)
    ensureWorkerEgl(true);

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
        uc_reg_write(uc_, UC_ARM_REG_R0, &r0);
        uc_reg_write(uc_, UC_ARM_REG_R1, &r1);
        uc_reg_write(uc_, UC_ARM_REG_R2, &r2);
        uc_reg_write(uc_, UC_ARM_REG_R3, &r3);
        uc_reg_write(uc_, UC_ARM_REG_SP, &sp);
        uc_reg_write(uc_, UC_ARM_REG_LR, &lr);
        uc_reg_write(uc_, UC_ARM_REG_CPSR, &cpsr);
        uc_emu_start(uc_, fnAddr, lr, 0, maxInsns);
        uint32_t ret = 0;
        uc_reg_read(uc_, UC_ARM_REG_R0, &ret);
        return ret;
    };

    // 4. Invocazione JNI_OnLoad della libreria primaria
    uint32_t pc = currentLib_->entry_point;
    LOGI("Avvio JNI_OnLoad / Entry Point guest: PC=0x%08x, SP=0x%08x, JavaVM=0x%08x...",
         pc, guestStackPointer_, javaVMPtr);
    invokeGuestFunction(pc, javaVMPtr, 0, 0, 0, 2000000);

    uint32_t retR0 = 0;
    uc_reg_read(uc_, UC_ARM_REG_R0, &retR0);
    LOGI("JNI_OnLoad completato (Valore r0: 0x%08x, Metodi registrati: %zu).",
         retR0, env.getRegisteredMethods().size());

    // 5. Sequenza di avvio signature-aware per i metodi JNI registrati (Unity / NDK / libg.so)
    const auto& nativeMethods = env.getRegisteredMethods();
    uint32_t dummyInstance = 0x1001;
    uint32_t dummySurface = 0x1003;

    // 5a. Unity nativeFile(String apkPath)
    for (const auto& m : nativeMethods) {
        if (m.name == "nativeFile" && !env.getApkPath().empty()) {
            uint32_t jstrApk = env.createGuestJString(uc_, env.getApkPath());
            LOGI("Invocazione metodo guest '%s' con APK '%s'...", m.name.c_str(), env.getApkPath().c_str());
            invokeGuestFunction(m.fnPtr, jniEnvPtr, dummyInstance, jstrApk, 0, 500000);
        }
    }

    // 5b. Metodi di init (initJni, init, nativeInit) - Escludiamo tassativamente qualsiasi metodo grafico/surface/recreate
    for (const auto& m : nativeMethods) {
        std::string lower = m.name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.find("recreate") != std::string::npos || lower.find("surface") != std::string::npos || lower.find("gfx") != std::string::npos) {
            continue; // Riservato alla fase 5c con la Surface valida
        }

        if (lower == "initjni" || lower == "nativeinitjni") {
            LOGI("Invocazione metodo guest '%s' (Context=0x1001)...", m.name.c_str());
            invokeGuestFunction(m.fnPtr, jniEnvPtr, dummyInstance, 0x1001, 0, 1000000);
        } else if (lower == "init" && m.signature == "(IZ)V") {
            LOGI("Invocazione metodo guest '%s' (0, false)...", m.name.c_str());
            invokeGuestFunction(m.fnPtr, jniEnvPtr, dummyInstance, 0, 0, 1000000);
        } else if (lower.find("init") != std::string::npos || lower == "create" || lower == "nativecreate") {
            LOGI("Invocazione metodo guest di setup '%s' (0x%08x)...", m.name.c_str(), m.fnPtr);
            if (m.signature.find("(II)") != std::string::npos) {
                // Metodi tipo Cocos2d-x nativeInit(int w, int h)
                invokeGuestFunction(m.fnPtr, jniEnvPtr, dummyInstance, 1920, 1080, 2000000);
            } else {
                invokeGuestFunction(m.fnPtr, jniEnvPtr, dummyInstance, 0x1001, 0, 1000000);
            }
        }
    }

    // 5c. Collegamento Surface e Focus/Resume (nativeRecreateGfxState, onSurfaceChanged, nativeFocusChanged, nativeResume)
    for (const auto& m : nativeMethods) {
        std::string lower = m.name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.find("recreategfxstate") != std::string::npos || lower.find("setsurface") != std::string::npos) {
            LOGI("Invocazione metodo grafico Surface '%s' (0x%08x)...", m.name.c_str(), m.fnPtr);
            if (m.signature.find("(IL") != std::string::npos) {
                invokeGuestFunction(m.fnPtr, jniEnvPtr, dummyInstance, 0, dummySurface, 1500000);
            } else {
                invokeGuestFunction(m.fnPtr, jniEnvPtr, dummyInstance, dummySurface, 0, 1500000);
            }
        } else if (lower.find("surfacechanged") != std::string::npos || lower.find("resize") != std::string::npos || lower.find("onsurfacechanged") != std::string::npos) {
            LOGI("Invocazione resize grafico '%s' (1920x1080)...", m.name.c_str());
            invokeGuestFunction(m.fnPtr, jniEnvPtr, dummyInstance, 1920, 1080, 1500000);
        } else if (lower.find("focuschanged") != std::string::npos) {
            invokeGuestFunction(m.fnPtr, jniEnvPtr, dummyInstance, 1, 0, 500000);
        } else if (lower.find("resume") != std::string::npos) {
            invokeGuestFunction(m.fnPtr, jniEnvPtr, dummyInstance, 0, 0, 500000);
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
        for (const auto& sym : currentLib_->symbols) {
            std::string lower = sym.name;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if ((lower.find("nativerender") != std::string::npos ||
                 lower.find("ondrawframe") != std::string::npos ||
                 lower.find("renderframe") != std::string::npos ||
                 lower.find("nativestep") != std::string::npos) && sym.vaddr != 0) {
                targetRenderPtr = currentLib_->load_bias + sym.vaddr;
                targetRenderName = sym.name;
                break;
            }
        }
    }

    LOGI("DbtEngine: Avvio LOOP DI GIOCO ATTIVO A 60 FPS (RenderMethod='%s' @ 0x%08x)...",
         targetRenderName.empty() ? "EGL-PresentLoop" : targetRenderName.c_str(), targetRenderPtr);

    size_t frameCount = 0;
    while (governor_.isRunning.load(std::memory_order_relaxed) && !stopRequested_.load(std::memory_order_relaxed)) {
        auto frameStart = std::chrono::steady_clock::now();

        ensureWorkerEgl(false);
        processPendingTouchEvents();
        env.consumeFrameSwapFlag();

        if (targetRenderPtr != 0) {
            uint32_t renderRet = invokeGuestFunction(targetRenderPtr, jniEnvPtr, dummyInstance, 0, 0, 3000000);
            if (frameCount < 5 || frameCount % 600 == 0) {
                LOGI("DbtEngine: Frame %zu invocato su %s -> r0=0x%x", frameCount + 1, targetRenderName.c_str(), renderRet);
            }
        }

        if (!env.consumeFrameSwapFlag()) {
            swapBuffers();
        }

        frameCount++;
        if (frameCount == 1 || frameCount % 300 == 0) {
            LOGI("DbtEngine: Frame %zu renderizzato a 60 FPS.", frameCount);
        }

        governor_.syncFrame(frameStart);
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
    uint32_t retVal = static_cast<uint32_t>(ret);
    uc_reg_write(uc_, UC_ARM_REG_R0, &retVal);
}

} // namespace emux
