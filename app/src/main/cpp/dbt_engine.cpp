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

void DbtEngine::setNativeSurface(ANativeWindow* window) {
    if (nativeWindow_) {
        ANativeWindow_release(nativeWindow_);
    }
    nativeWindow_ = window;
    if (nativeWindow_) {
        ANativeWindow_acquire(nativeWindow_);
        ANativeWindow_setBuffersGeometry(nativeWindow_, 1920, 1080, WINDOW_FORMAT_RGBA_8888);
        LOGI("ANativeWindow collegata con successo all'engine DBT (Full HD 1080p).");
    } else {
        LOGI("ANativeWindow scollegata dall'engine DBT.");
    }
}

bool DbtEngine::injectTouchEvent(int action, float x, float y, int pointerId) {
    LOGI("DbtEngine Touch Injected: action=%d, x=%.1f, y=%.1f, id=%d", action, x, y, pointerId);
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

    // 2. SETUP DEI REGISTRI CPU E PARAMETRI JNI_OnLoad
    uint32_t pc = currentLib_->entry_point;
    uint32_t lr = 0xDEADBEEF; // Return address trap
    uint32_t cpsr = 0x00000010; // ARM User Mode (32-bit)

    // Argomento 0 (r0): Puntatore a JavaVM guest
    uint32_t javaVMPtr = GuestRuntimeEnvironment::getInstance().getJavaVMPointer();
    // Argomento 1 (r1): Riservato (NULL)
    uint32_t reserved = 0;

    uc_reg_write(uc_, UC_ARM_REG_R0, &javaVMPtr);
    uc_reg_write(uc_, UC_ARM_REG_R1, &reserved);
    uc_reg_write(uc_, UC_ARM_REG_PC, &pc);
    uc_reg_write(uc_, UC_ARM_REG_LR, &lr);
    uc_reg_write(uc_, UC_ARM_REG_CPSR, &cpsr);

    LOGI("Avvio uc_emu_start: PC=0x%08x, SP=0x%08x, JavaVM=0x%08x...", pc, guestStackPointer_, javaVMPtr);

    governor_.isRunning.store(true);
    auto frameStart = std::chrono::steady_clock::now();

    // Invocazione emulazione Unicorn
    uc_err emuErr = uc_emu_start(uc_, pc, 0, 0, 0);

    governor_.syncFrame(frameStart);
    governor_.isRunning.store(false);

    if (emuErr != UC_ERR_OK) {
        LOGE("uc_emu_start terminata con codice: %d (%s)", emuErr, uc_strerror(emuErr));
        return false;
    }

    LOGI("uc_emu_start completata con successo.");
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
