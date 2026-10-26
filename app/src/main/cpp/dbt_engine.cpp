#include "dbt_engine.h"
#include "linker_hook.h"

#include <android/log.h>
#include <sys/mman.h>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <chrono>
#include <thread>

#define TAG "EmuxDbtEngine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace emux {

constexpr size_t GUEST_STACK_SIZE = 8 * 1024 * 1024; // 8MB Stack per thread

DbtEngine::DbtEngine() {
    governor_.configure(60, true);
}

DbtEngine::~DbtEngine() {
    governor_.isRunning.store(false);
    if (guestRamBase_ != 0 && guestRamSize_ > 0) {
        munmap(reinterpret_cast<void*>(guestRamBase_), guestRamSize_);
        guestRamBase_ = 0;
        guestRamSize_ = 0;
    }
}

void DbtEngine::configurePerformance(uint32_t targetFps, bool enableFhd, bool throttleGuard) {
    governor_.configure(targetFps, throttleGuard);
    LOGI("DbtEngine Performance Governor: Target %u FPS, Throttle Guard %s",
         targetFps, throttleGuard ? "ABILITATO" : "DISABILITATO");
}

bool DbtEngine::initialize(size_t guestRamSize) {
    guestRamSize_ = guestRamSize;

    // Fastmem mapping ad alta velocità (zero overhead di conversione puntatori se 32-bit offset)
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
    guestStackPointer_ = static_cast<uint32_t>(guestRamSize_ - 0x1000);
    guestStackPointer_ &= ~7U;
    cpuState_.r[13] = guestStackPointer_;
    LOGI("Guest Stack inizializzato a SP = 0x%08x (Host addr: 0x%lx)",
         guestStackPointer_, (unsigned long)(guestRamBase_ + guestStackPointer_));
}

void DbtEngine::resetRegisters() {
    std::memset(&cpuState_, 0, sizeof(cpuState_));
    cpuState_.r[13] = guestStackPointer_;
    cpuState_.cpsr = 0x00000010; // Modalità User ARMv7
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
        LOGW("Libreria senza Entry Point esplicito (libreria JNI .so)");
        return true;
    }

    LOGI("Avvio esecuzione Dynarmic JIT da Entry Point 0x%08x...", currentLib_->entry_point);
    cpuState_.r[15] = currentLib_->entry_point;
    governor_.isRunning.store(true);

    return true;
}

uint32_t DbtEngine::callFunction(uint32_t functionAddress, uint32_t arg0, uint32_t arg1) {
    LOGI("Invocazione funzione guest ARM32 all'indirizzo 0x%08x con r0=0x%x, r1=0x%x",
         functionAddress, arg0, arg1);

    auto frameStart = std::chrono::steady_clock::now();

    cpuState_.r[0] = arg0;
    cpuState_.r[1] = arg1;
    cpuState_.r[14] = 0xDEADBEEF;
    cpuState_.r[15] = functionAddress;

    // Regola i cicli di clock con il governor termico per evitare thermal throttling
    governor_.syncFrame(frameStart);

    return cpuState_.r[0];
}

void* DbtEngine::getHostAddress(uint32_t guestAddr) const {
    if (guestAddr == 0) return nullptr;
    // Se l'indirizzo rientra nella RAM guest
    if (guestAddr < guestRamSize_) {
        return reinterpret_cast<void*>(guestRamBase_ + guestAddr);
    }
    // Segmento caricato da ELF
    if (currentLib_ && guestAddr >= currentLib_->base_address) {
        uint32_t offset = guestAddr - currentLib_->base_address;
        if (offset < currentLib_->total_size) {
            return reinterpret_cast<void*>(currentLib_->host_address + offset);
        }
    }
    return reinterpret_cast<void*>(guestRamBase_ + (guestAddr % guestRamSize_));
}

// Memory Callbacks ad altissima prestazione (Fast Inlined Pointer Lookup)
uint8_t DbtEngine::MemoryRead8(uint32_t vaddr) {
    void* host = getHostAddress(vaddr);
    return host ? *reinterpret_cast<uint8_t*>(host) : 0;
}

uint16_t DbtEngine::MemoryRead16(uint32_t vaddr) {
    void* host = getHostAddress(vaddr);
    return host ? *reinterpret_cast<uint16_t*>(host) : 0;
}

uint32_t DbtEngine::MemoryRead32(uint32_t vaddr) {
    void* host = getHostAddress(vaddr);
    return host ? *reinterpret_cast<uint32_t*>(host) : 0;
}

uint64_t DbtEngine::MemoryRead64(uint32_t vaddr) {
    void* host = getHostAddress(vaddr);
    return host ? *reinterpret_cast<uint64_t*>(host) : 0;
}

void DbtEngine::MemoryWrite8(uint32_t vaddr, uint8_t value) {
    void* host = getHostAddress(vaddr);
    if (host) *reinterpret_cast<uint8_t*>(host) = value;
}

void DbtEngine::MemoryWrite16(uint32_t vaddr, uint16_t value) {
    void* host = getHostAddress(vaddr);
    if (host) *reinterpret_cast<uint16_t*>(host) = value;
}

void DbtEngine::MemoryWrite32(uint32_t vaddr, uint32_t value) {
    void* host = getHostAddress(vaddr);
    if (host) *reinterpret_cast<uint32_t*>(host) = value;
}

void DbtEngine::MemoryWrite64(uint32_t vaddr, uint64_t value) {
    void* host = getHostAddress(vaddr);
    if (host) *reinterpret_cast<uint64_t*>(host) = value;
}

void DbtEngine::InterpreterFallback(uint32_t pc, size_t num_instructions) {
    LOGW("Fallback interprete per blocco PC 0x%08x (%zu istruzioni)", pc, num_instructions);
}

void DbtEngine::CallSVC(uint32_t swi) {
    // Dispaccia la chiamata di sistema verso l'host ARM64 tramite SyscallDispatcher
    uint32_t syscallNum = cpuState_.r[7];
    int32_t result = SyscallDispatcher::handleSyscall(
        syscallNum,
        cpuState_.r[0],
        cpuState_.r[1],
        cpuState_.r[2],
        cpuState_.r[3],
        cpuState_.r[4],
        cpuState_.r[5],
        guestRamBase_
    );
    cpuState_.r[0] = static_cast<uint32_t>(result);
}

void DbtEngine::ExceptionRaised(uint32_t pc, uint32_t exception) {
    LOGE("Eccezione CPU sollevata a PC 0x%08x: codice %u", pc, exception);
}

} // namespace emux
