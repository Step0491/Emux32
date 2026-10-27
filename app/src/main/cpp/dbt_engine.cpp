#include "dbt_engine.h"
#include "linker_hook.h"

#include <android/log.h>
#include <android/native_window.h>
#include <cstring>
#include <chrono>

#define TAG "EmuxDbtEngine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace emux {

DbtEngine::DbtEngine() {
    governor_.configure(60, true);
}

DbtEngine::~DbtEngine() {
    stopRequested_.store(true);
    governor_.isRunning.store(false);
    if (uc_) {
        uc_emu_stop(uc_);
        if (intrHook_ != 0) {
            uc_hook_del(uc_, intrHook_);
            intrHook_ = 0;
        }
        uc_close(uc_);
        uc_ = nullptr;
    }
    destroyEgl();
    if (nativeWindow_) {
        ANativeWindow_release(nativeWindow_);
        nativeWindow_ = nullptr;
    }
}

void DbtEngine::configurePerformance(uint32_t targetFps, bool enableFhd, bool throttleGuard) {
    governor_.configure(targetFps, throttleGuard);
    LOGI("DbtEngine Performance Governor: Target %u FPS, FHD: %s, Throttle Guard: %s",
         targetFps, enableFhd ? "ON" : "OFF", throttleGuard ? "ON" : "OFF");
}

bool DbtEngine::initEgl() {
    if (!nativeWindow_) {
        LOGW("initEgl: nativeWindow_ non presente.");
        return false;
    }

    if (eglDisplay_ != EGL_NO_DISPLAY && eglSurface_ != EGL_NO_SURFACE && eglContext_ != EGL_NO_CONTEXT) {
        LOGI("initEgl: Contesto EGL hardware gia' attivo e pronto.");
        return true;
    }

    LOGI("initEgl: Inizializzazione display e surface hardware EGL...");
    eglDisplay_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (eglDisplay_ == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay fallito");
        return false;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(eglDisplay_, &major, &minor)) {
        LOGE("eglInitialize fallito");
        return false;
    }
    LOGI("EGL inizializzato: versione %d.%d", major, minor);

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
        LOGW("eglChooseConfig con depth 24 non disponibile, riprovo con depth 16");
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
            LOGE("eglChooseConfig fallito completamente");
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

    eglSurface_ = eglCreateWindowSurface(eglDisplay_, eglConfig_, nativeWindow_, nullptr);
    if (eglSurface_ == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface fallito: 0x%x", eglGetError());
        return false;
    }

    if (!eglMakeCurrent(eglDisplay_, eglSurface_, eglSurface_, eglContext_)) {
        LOGE("eglMakeCurrent fallito: 0x%x", eglGetError());
        return false;
    }

    int width = ANativeWindow_getWidth(nativeWindow_);
    int height = ANativeWindow_getHeight(nativeWindow_);
    if (width <= 0) width = 1920;
    if (height <= 0) height = 1080;

    glViewport(0, 0, width, height);

    // Primo frame di avvio per certificare hardware acceleration attiva su display
    glClearColor(0.04f, 0.05f, 0.09f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    eglSwapBuffers(eglDisplay_, eglSurface_);

    LOGI("initEgl: Setup hardware EGL completato con successo (Viewport %dx%d). Frame iniziale inviato.", width, height);
    GuestRuntimeEnvironment::getInstance().setEglState(eglDisplay_, eglSurface_, eglContext_, &governor_);
    return true;
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
    }
    GuestRuntimeEnvironment::getInstance().setEglState(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_CONTEXT, nullptr);
    LOGI("destroyEgl: Risorse grafiche EGL rilasciate.");
}

void DbtEngine::swapBuffers() {
    if (eglDisplay_ != EGL_NO_DISPLAY && eglSurface_ != EGL_NO_SURFACE) {
        eglSwapBuffers(eglDisplay_, eglSurface_);
    }
}

void DbtEngine::setNativeSurface(ANativeWindow* window) {
    if (nativeWindow_ == window && window != nullptr) {
        return;
    }
    if (nativeWindow_) {
        destroyEgl();
        ANativeWindow_release(nativeWindow_);
        nativeWindow_ = nullptr;
    }
    nativeWindow_ = window;
    if (nativeWindow_) {
        ANativeWindow_acquire(nativeWindow_);
        ANativeWindow_setBuffersGeometry(nativeWindow_, 1920, 1080, WINDOW_FORMAT_RGBA_8888);
        LOGI("ANativeWindow collegata con successo all'engine DBT (Full HD 1080p).");
        initEgl();
    } else {
        LOGI("ANativeWindow scollegata dall'engine DBT.");
        destroyEgl();
    }
}

bool DbtEngine::injectTouchEvent(int action, float x, float y, int pointerId) {
    LOGI("DbtEngine Touch Injected: action=%d, x=%.1f, y=%.1f, id=%d", action, x, y, pointerId);
    if (!uc_ || !governor_.isRunning.load()) return false;
    auto& env = GuestRuntimeEnvironment::getInstance();
    const auto& nativeMethods = env.getRegisteredMethods();
    for (const auto& m : nativeMethods) {
        std::string lower = m.name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.find("touch") != std::string::npos || lower.find("injectevent") != std::string::npos) {
            uint32_t jniEnvPtr = env.getJNIEnvPointer();
            uint32_t dummyObj = 0x1002;
            uint32_t lr = 0xDEADBEEF;
            uint32_t iX = static_cast<uint32_t>(x);
            uint32_t iY = static_cast<uint32_t>(y);
            uc_reg_write(uc_, UC_ARM_REG_R0, &jniEnvPtr);
            uc_reg_write(uc_, UC_ARM_REG_R1, &dummyObj);
            uc_reg_write(uc_, UC_ARM_REG_R2, &action);
            uc_reg_write(uc_, UC_ARM_REG_R3, &iX);
            uc_reg_write(uc_, UC_ARM_REG_LR, &lr);
            uc_reg_write(uc_, UC_ARM_REG_PC, &m.fnPtr);
            uc_emu_start(uc_, m.fnPtr, lr, 0, 0);
            break;
        }
    }
    return true;
}

bool DbtEngine::initialize(size_t /* guestRamSize */) {
    LOGI("Inizializzazione Unicorn Engine per architettura ARMv7-A...");

    uc_err err = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc_);
    if (err != UC_ERR_OK) {
        LOGE("uc_open fallito: %s (codice: %d)", uc_strerror(err), err);
        return false;
    }

    // Inizializza l'ambiente JNI a 32-bit (JavaVM32 e JNIEnv32) e la regione dei Trampoline
    GuestRuntimeEnvironment::getInstance().initialize(uc_);

    // Registra hook UC_HOOK_INTR per intercettare sia syscall (SVC #0) sia chiamate a funzioni host (Trampoline)
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
    LOGI("Unicorn Engine inizializzato con successo e hook UC_HOOK_INTR registrato (id=%zu).", intrHook_);
    return true;
}

void DbtEngine::setupGuestStack() {
    if (!uc_) return;

    // SETUP DEL CONTESTO CPU - AREA STACK 2MB DEDICATA:
    // Mappa un'area di 2MB allineata rigorosamente alla pagina 0x1000 byte
    uint32_t alignedStackBase = PAGE_ALIGN_DOWN(guestStackBase_);
    uint32_t alignedStackSize = PAGE_ALIGN_UP(guestStackSize_); // 2MB esatti (0x200000)

    LOGI("Setup Stack Guest dedicato (2MB): Base=0x%08x, Dimensione=0x%08x (%u byte)",
         alignedStackBase, alignedStackSize, alignedStackSize);

    uc_err err = uc_mem_map(uc_, alignedStackBase, alignedStackSize, UC_PROT_READ | UC_PROT_WRITE);
    if (err != UC_ERR_OK && err != UC_ERR_MAP) {
        LOGE("uc_mem_map dello Stack guest fallita: %s", uc_strerror(err));
        return;
    }

    // Stack Pointer posizionato al limite superiore dell'area allineata (decrescente, 8-byte aligned)
    guestStackPointer_ = (alignedStackBase + alignedStackSize - 0x1000) & ~7U;
    uc_reg_write(uc_, UC_ARM_REG_SP, &guestStackPointer_);

    LOGI("Registro UC_ARM_REG_SP inizializzato a: 0x%08x", guestStackPointer_);
}

bool DbtEngine::loadLibrary(const std::string& libraryPath) {
    if (!uc_) {
        LOGE("Engine non inizializzato prima del caricamento ELF!");
        return false;
    }

    currentLib_ = loader_.loadFromFile(libraryPath);
    if (!currentLib_) {
        LOGE("Caricamento della libreria ELF fallito da: %s", libraryPath.c_str());
        return false;
    }

    // 1. Mappatura automatica di tutti i segmenti PT_LOAD in Unicorn Engine con strict page alignment
    bool mapped = currentLib_->mapToUnicorn(uc_);
    if (!mapped) {
        LOGE("Mappatura dei segmenti ELF in memoria Unicorn fallita!");
        return false;
    }

    // 2. Risoluzione e applicazione delle rilocazioni dinamiche (R_ARM_RELATIVE, R_ARM_GLOB_DAT, R_ARM_JUMP_SLOT)
    bool relocated = currentLib_->applyRelocations(uc_);
    if (!relocated) {
        LOGW("Attenzione durante l'applicazione delle rilocazioni dinamiche");
    }

    LOGI("Libreria ELF caricata e rilocata con successo: %s (Entry Point: 0x%08x)",
         libraryPath.c_str(), currentLib_->entry_point);
    return true;
}

bool DbtEngine::startEmulation() {
    if (!uc_ || !currentLib_) {
        LOGE("Engine o libreria ELF non pronta per startEmulation");
        return false;
    }

    // 1. SETUP DELLO STACK GUEST
    setupGuestStack();

    // 2. ASSICURA CONTESTO EGL HARDWARE ATTIVO
    if (nativeWindow_ && eglSurface_ == EGL_NO_SURFACE) {
        initEgl();
    }

    // 3. ESECUZIONE JNI_OnLoad O ENTRY POINT INIZIALE
    uint32_t pc = currentLib_->entry_point;
    uint32_t lr = 0xDEADBEEF; // Return address trap
    uint32_t cpsr = 0x00000010; // ARM User Mode (32-bit)

    uint32_t javaVMPtr = GuestRuntimeEnvironment::getInstance().getJavaVMPointer();
    uint32_t jniEnvPtr = GuestRuntimeEnvironment::getInstance().getJNIEnvPointer();
    uint32_t reserved = 0;

    uc_reg_write(uc_, UC_ARM_REG_R0, &javaVMPtr);
    uc_reg_write(uc_, UC_ARM_REG_R1, &reserved);
    uc_reg_write(uc_, UC_ARM_REG_PC, &pc);
    uc_reg_write(uc_, UC_ARM_REG_LR, &lr);
    uc_reg_write(uc_, UC_ARM_REG_CPSR, &cpsr);

    LOGI("Avvio JNI_OnLoad / Entry Point guest: PC=0x%08x, SP=0x%08x, JavaVM=0x%08x...",
         pc, guestStackPointer_, javaVMPtr);

    governor_.isRunning.store(true);
    stopRequested_.store(false);

    // Invocazione iniziale di configurazione JNI_OnLoad
    uc_err emuErr = uc_emu_start(uc_, pc, lr, 0, 0);
    if (emuErr != UC_ERR_OK && emuErr != UC_ERR_EXCEPTION) {
        LOGW("JNI_OnLoad / Entry Point ha restituito codice: %d (%s)", emuErr, uc_strerror(emuErr));
    } else {
        uint32_t retR0 = 0;
        uc_reg_read(uc_, UC_ARM_REG_R0, &retR0);
        LOGI("JNI_OnLoad completato (Valore r0: 0x%08x).", retR0);
    }

    // 4. INVOCA METODI DI INIZIALIZZAZIONE GRAFICA REGISTRATI (nativeInit, onSurfaceCreated, onSurfaceChanged)
    auto& env = GuestRuntimeEnvironment::getInstance();
    const auto& nativeMethods = env.getRegisteredMethods();
    LOGI("DbtEngine: Trovati %zu metodi nativi registrati dinamicamente.", nativeMethods.size());

    uint32_t dummyObj = 0x1001;
    for (const auto& m : nativeMethods) {
        std::string lower = m.name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.find("init") != std::string::npos ||
            lower.find("create") != std::string::npos ||
            lower.find("surfacecreated") != std::string::npos ||
            lower.find("surfacechanged") != std::string::npos) {
            LOGI("Invocazione metodo guest di setup grafico: '%s' (0x%08x)...", m.name.c_str(), m.fnPtr);
            uint32_t w = 1920, h = 1080;
            uc_reg_write(uc_, UC_ARM_REG_R0, &jniEnvPtr);
            uc_reg_write(uc_, UC_ARM_REG_R1, &dummyObj);
            uc_reg_write(uc_, UC_ARM_REG_R2, &w);
            uc_reg_write(uc_, UC_ARM_REG_R3, &h);
            uc_reg_write(uc_, UC_ARM_REG_LR, &lr);
            uc_reg_write(uc_, UC_ARM_REG_PC, &m.fnPtr);
            uc_emu_start(uc_, m.fnPtr, lr, 0, 0);
        }
    }

    // 5. DETERMINAZIONE METODO DI RENDERING PER IL LOOP 60 FPS
    const RegisteredNativeMethod* renderMethod = nullptr;
    for (const auto& m : nativeMethods) {
        std::string lower = m.name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.find("render") != std::string::npos ||
            lower.find("ondrawframe") != std::string::npos ||
            lower.find("draw") != std::string::npos ||
            lower.find("step") != std::string::npos ||
            lower.find("update") != std::string::npos ||
            lower.find("frame") != std::string::npos ||
            lower.find("tick") != std::string::npos) {
            renderMethod = &m;
            break;
        }
    }

    uint32_t targetRenderPtr = 0;
    std::string targetRenderName;
    if (renderMethod) {
        targetRenderPtr = renderMethod->fnPtr;
        targetRenderName = renderMethod->name;
    } else {
        // Cerca nei simboli esportati dal binario ELF
        for (const auto& sym : currentLib_->symbols) {
            std::string lower = sym.name;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if ((lower.find("render") != std::string::npos ||
                 lower.find("ondraw") != std::string::npos ||
                 lower.find("nativemain") != std::string::npos ||
                 lower.find("android_main") != std::string::npos) &&
                sym.vaddr != 0) {
                targetRenderPtr = currentLib_->load_bias + sym.vaddr;
                targetRenderName = sym.name;
                break;
            }
        }
    }

    // 6. AVVIO CICLO CONTINUO DI GIOCO 60 FPS
    if (targetRenderPtr != 0) {
        LOGI("DbtEngine: Avvio LOOP DI GIOCO ATTIVO A 60 FPS su metodo '%s' (0x%08x)...",
             targetRenderName.c_str(), targetRenderPtr);
        size_t frameCount = 0;
        while (governor_.isRunning.load() && !stopRequested_.load()) {
            auto frameStart = std::chrono::steady_clock::now();

            uc_reg_write(uc_, UC_ARM_REG_R0, &jniEnvPtr);
            uc_reg_write(uc_, UC_ARM_REG_R1, &dummyObj);
            uc_reg_write(uc_, UC_ARM_REG_LR, &lr);
            uc_reg_write(uc_, UC_ARM_REG_PC, &targetRenderPtr);

            uc_err fErr = uc_emu_start(uc_, targetRenderPtr, lr, 0, 0);
            if (fErr != UC_ERR_OK && fErr != UC_ERR_EXCEPTION) {
                // Se un frame fallisce, logghiamo senza interrompere la continuita'
                LOGW("DbtEngine: Errore durante esecuzione frame %zu: %d (%s)", frameCount, fErr, uc_strerror(fErr));
            }

            // Presentazione hardware del buffer EGL su display
            swapBuffers();

            frameCount++;
            if (frameCount % 300 == 0) {
                LOGI("DbtEngine: Generati e renderizzati %zu frame di gioco attivi a 60 FPS.", frameCount);
            }

            governor_.syncFrame(frameStart);
        }
        LOGI("DbtEngine: Ciclo di gioco terminato. Totale frame renderizzati: %zu", frameCount);
    } else {
        LOGI("DbtEngine: Nessun metodo di rendering loop esplicito registrato; esecuzione gestita dal thread di ingresso guest.");
    }

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

    // Se l'interrupt include il flag 0x80000000, si tratta di una chiamata a un Trampoline registrato
    if (intno & 0x80000000U) {
        uint32_t svcComment = intno & 0x00FFFFFFU;
        GuestRuntimeEnvironment::getInstance().handleTrampolineCall(svcComment, uc_);
        return;
    }

    // Altrimenti è una Supervisor Call standard Linux/Bionic (SVC #0)
    uint32_t r7 = 0;
    uc_reg_read(uc_, UC_ARM_REG_R7, &r7);

    uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0;
    uc_reg_read(uc_, UC_ARM_REG_R0, &r0);
    uc_reg_read(uc_, UC_ARM_REG_R1, &r1);
    uc_reg_read(uc_, UC_ARM_REG_R2, &r2);
    uc_reg_read(uc_, UC_ARM_REG_R3, &r3);
    uc_reg_read(uc_, UC_ARM_REG_R4, &r4);
    uc_reg_read(uc_, UC_ARM_REG_R5, &r5);

    // Inoltro alla classe SyscallDispatcher
    int32_t ret = SyscallDispatcher::handleSyscall(r7, r0, r1, r2, r3, r4, r5, uc_);

    // Scrittura valore di ritorno nel registro r0 (UC_ARM_REG_R0) conforme alle convenzioni AAPCS
    uint32_t retVal = static_cast<uint32_t>(ret);
    uc_reg_write(uc_, UC_ARM_REG_R0, &retVal);
}

} // namespace emux
