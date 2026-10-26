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
    if (jit_) {
        jit_->HaltExecution();
        jit_.reset();
    }
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

    // Fastmem mapping virtuale
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

    // Configurazione e creazione istanza Dynarmic JIT
    Dynarmic::A32::UserConfig config;
    config.callbacks = this;
    config.arch_version = Dynarmic::A32::ArchVersion::v8; // ARMv8-A32 mode compatibile ARMv7
    config.optimizations = Dynarmic::all_safe_optimizations;
    config.unsafe_optimizations = false;

    try {
        jit_ = std::make_unique<Dynarmic::A32::Jit>(config);
        LOGI("Dynarmic A32 JIT Recompiler istanziato con successo!");
    } catch (const std::exception& ex) {
        LOGE("Errore istanziazione Dynarmic JIT: %s", ex.what());
        return false;
    } catch (...) {
        LOGE("Errore sconosciuto istanziazione Dynarmic JIT");
        return false;
    }

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
    cpuState_.cpsr = 0x00000010; // User mode ARMv7
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
    if (!currentLib_ || !jit_) {
        LOGE("Libreria o JIT non inizializzati per l'avvio");
        return false;
    }

    if (currentLib_->entry_point == 0) {
        LOGW("Libreria JNI senza ELF Entry Point diretto (.so dinamica)");
        return true;
    }

    LOGI("Avvio ricompilazione Dynarmic JIT da Entry Point 0x%08x...", currentLib_->entry_point);
    jit_->Regs()[15] = currentLib_->entry_point;
    jit_->Regs()[13] = guestStackPointer_;
    jit_->SetCpsr(0x00000010);

    ticksRemaining_ = 1000000;
    governor_.isRunning.store(true);

    auto frameStart = std::chrono::steady_clock::now();
    jit_->Run();
    governor_.syncFrame(frameStart);

    return true;
}

uint32_t DbtEngine::callFunction(uint32_t functionAddress, uint32_t arg0, uint32_t arg1) {
    if (!jit_) {
        LOGE("Dynarmic JIT non pronto per callFunction");
        return 0;
    }

    LOGI("Dynarmic JIT invoke func 0x%08x (arg0=0x%x, arg1=0x%x)", functionAddress, arg0, arg1);

    auto frameStart = std::chrono::steady_clock::now();

    jit_->Regs()[0] = arg0;
    jit_->Regs()[1] = arg1;
    jit_->Regs()[14] = 0xDEADBEEF; // Return address trap
    jit_->Regs()[15] = functionAddress;
    jit_->Regs()[13] = guestStackPointer_;
    jit_->SetCpsr(0x00000010);

    ticksRemaining_ = 5000000;
    jit_->Run();

    governor_.syncFrame(frameStart);

    uint32_t retVal = jit_->Regs()[0];
    return retVal;
}

void* DbtEngine::getHostAddress(uint32_t guestAddr) const {
    if (guestAddr == 0) return nullptr;
    if (guestAddr < guestRamSize_) {
        return reinterpret_cast<void*>(guestRamBase_ + guestAddr);
    }
    if (currentLib_ && guestAddr >= currentLib_->base_address) {
        uint32_t offset = guestAddr - currentLib_->base_address;
        if (offset < currentLib_->total_memory_size) {
            return reinterpret_cast<void*>(currentLib_->base_address + offset);
        }
    }
    return reinterpret_cast<void*>(guestRamBase_ + (guestAddr % guestRamSize_));
}

// Memory Callbacks per Dynarmic
std::uint8_t DbtEngine::MemoryRead8(Dynarmic::A32::VAddr vaddr) {
    void* host = getHostAddress(vaddr);
    return host ? *reinterpret_cast<uint8_t*>(host) : 0;
}

std::uint16_t DbtEngine::MemoryRead16(Dynarmic::A32::VAddr vaddr) {
    void* host = getHostAddress(vaddr);
    return host ? *reinterpret_cast<uint16_t*>(host) : 0;
}

std::uint32_t DbtEngine::MemoryRead32(Dynarmic::A32::VAddr vaddr) {
    void* host = getHostAddress(vaddr);
    return host ? *reinterpret_cast<uint32_t*>(host) : 0;
}

std::uint64_t DbtEngine::MemoryRead64(Dynarmic::A32::VAddr vaddr) {
    void* host = getHostAddress(vaddr);
    return host ? *reinterpret_cast<uint64_t*>(host) : 0;
}

void DbtEngine::MemoryWrite8(Dynarmic::A32::VAddr vaddr, std::uint8_t value) {
    void* host = getHostAddress(vaddr);
    if (host) *reinterpret_cast<uint8_t*>(host) = value;
}

void DbtEngine::MemoryWrite16(Dynarmic::A32::VAddr vaddr, std::uint16_t value) {
    void* host = getHostAddress(vaddr);
    if (host) *reinterpret_cast<uint16_t*>(host) = value;
}

void DbtEngine::MemoryWrite32(Dynarmic::A32::VAddr vaddr, std::uint32_t value) {
    void* host = getHostAddress(vaddr);
    if (host) *reinterpret_cast<uint32_t*>(host) = value;
}

void DbtEngine::MemoryWrite64(Dynarmic::A32::VAddr vaddr, std::uint64_t value) {
    void* host = getHostAddress(vaddr);
    if (host) *reinterpret_cast<uint64_t*>(host) = value;
}

void DbtEngine::InterpreterFallback(Dynarmic::A32::VAddr pc, size_t num_instructions) {
    LOGW("Dynarmic fallback a PC 0x%08x (%zu istruzioni)", pc, num_instructions);
}

void DbtEngine::CallSVC(std::uint32_t swi) {
    if (!jit_) return;
    uint32_t syscallNum = jit_->Regs()[7];
    int32_t result = SyscallDispatcher::handleSyscall(
        syscallNum,
        jit_->Regs()[0],
        jit_->Regs()[1],
        jit_->Regs()[2],
        jit_->Regs()[3],
        jit_->Regs()[4],
        jit_->Regs()[5],
        guestRamBase_
    );
    jit_->Regs()[0] = static_cast<uint32_t>(result);
}

void DbtEngine::ExceptionRaised(Dynarmic::A32::VAddr pc, Dynarmic::A32::Exception exception) {
    LOGE("Dynarmic Exception at PC 0x%08x: enum %d", pc, static_cast<int>(exception));
}

void DbtEngine::AddTicks(std::uint64_t ticks) {
    if (ticksRemaining_ > ticks) {
        ticksRemaining_ -= ticks;
    } else {
        ticksRemaining_ = 0;
        if (jit_) {
            jit_->HaltExecution();
        }
    }
}

std::uint64_t DbtEngine::GetTicksRemaining() {
    return ticksRemaining_;
}

} // namespace emux
