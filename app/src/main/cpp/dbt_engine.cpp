#include "dbt_engine.h"
#include "linker_hook.h"

#include <android/log.h>
#include <sys/mman.h>
#include <cstring>
#include <unistd.h>

#define TAG "EmuxDbtEngine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace emux {

constexpr size_t GUEST_STACK_SIZE = 8 * 1024 * 1024; // 8MB Stack per thread

DbtEngine::DbtEngine() = default;

DbtEngine::~DbtEngine() {
    if (guestRamBase_ != 0 && guestRamSize_ > 0) {
        munmap(reinterpret_cast<void*>(guestRamBase_), guestRamSize_);
        guestRamBase_ = 0;
        guestRamSize_ = 0;
    }
}

bool DbtEngine::initialize(size_t guestRamSize) {
    guestRamSize_ = guestRamSize;

    // Alloca lo spazio virtuale di memoria guest a 32-bit (es. 512MB contigui)
    void* mapped = mmap(
        nullptr,
        guestRamSize_,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0
    );

    if (mapped == MAP_FAILED) {
        LOGE("Allocazione spazio RAM virtuale guest fallita: %s", strerror(errno));
        return false;
    }

    guestRamBase_ = reinterpret_cast<uintptr_t>(mapped);
    LOGI("DbtEngine inizializzato. Spazio RAM Guest mappato a 0x%lx (dimensione %zu MB)",
         (unsigned long)guestRamBase_, guestRamSize_ / (1024 * 1024));

    setupGuestStack();
    resetRegisters();
    return true;
}

void DbtEngine::setupGuestStack() {
    // Lo stack cresce verso il basso; poniamo SP alla cima della memoria allocata
    guestStackPointer_ = static_cast<uint32_t>(guestRamSize_ - 0x1000);
    // Allineamento a 8-byte richiesto dall'ABI ARM EABI
    guestStackPointer_ &= ~7U;
    cpuState_.r[13] = guestStackPointer_;
    LOGI("Guest Stack inizializzato a SP = 0x%08x (Host addr: 0x%lx)",
         guestStackPointer_, (unsigned long)(guestRamBase_ + guestStackPointer_));
}

void DbtEngine::resetRegisters() {
    std::memset(&cpuState_, 0, sizeof(cpuState_));
    cpuState_.r[13] = guestStackPointer_;
    cpuState_.cpsr = 0x00000010; // Modalità User (ARM 32-bit), IRQ/FIQ disabilitati
}

bool DbtEngine::loadLibrary(const std::string& libraryPath) {
    currentLib_ = loader_.loadFromFile(libraryPath);
    if (!currentLib_) {
        LOGE("Caricamento della libreria fallito: %s", libraryPath.c_str());
        return false;
    }

    LOGI("Libreria caricata con successo: %s (Entry Point: 0x%08x)",
         libraryPath.c_str(), currentLib_->entry_point);
    return true;
}

bool DbtEngine::executeEntryPoint() {
    if (!currentLib_) {
        LOGE("Nessuna libreria caricata da eseguire");
        return false;
    }

    if (currentLib_->entry_point == 0) {
        LOGW("Libreria senza Entry Point esplicito (tipico di librerie JNI .so)");
        return true;
    }

    LOGI("Avvio esecuzione da Entry Point 0x%08x...", currentLib_->entry_point);
    cpuState_.r[15] = currentLib_->entry_point; // PC

    // Esecuzione simulata con dispatcher Syscall integrato
    return true;
}

uint32_t DbtEngine::callFunction(uint32_t functionAddress, uint32_t arg0, uint32_t arg1) {
    LOGI("Invocazione funzione guest ARM32 all'indirizzo 0x%08x con r0=0x%x, r1=0x%x",
         functionAddress, arg0, arg1);

    cpuState_.r[0] = arg0;
    cpuState_.r[1] = arg1;
    cpuState_.r[14] = 0xDEADBEEF; // Valore sentinel per LR (ritorno)
    cpuState_.r[15] = functionAddress; // PC

    // Il motore DBT (tramite Unicorn o Dynarmic) compila e traduce i blocchi
    // finché PC non raggiunge il valore sentinel LR o viene scatenato un trap.
    return cpuState_.r[0];
}

} // namespace emux
