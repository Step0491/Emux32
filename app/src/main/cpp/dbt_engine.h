#pragma once

#include "elf_loader.h"
#include <cstdint>
#include <memory>
#include <vector>
#include <atomic>
#include <chrono>

namespace emux {

// Stato dei registri ARM32 (ARMv7-A)
struct Arm32CpuState {
    uint32_t r[16];   // r0..r12, r13(SP), r14(LR), r15(PC)
    uint32_t cpsr;    // Program Status Register (N, Z, C, V)
    uint64_t q[16];   // Registri VFPv4/NEON a 128 bit
    uint32_t fpscr;   // Floating-Point Status and Control Register
};

/**
 * Configurazione termica e frame pace limiter per evitare thermal throttling
 */
struct PerformanceGovernor {
    uint32_t targetFps{60};
    uint64_t frameTimeTargetNs{16666666ULL}; // 16.66ms per 60 FPS stabili
    bool throttleGuardEnabled{true};
    std::atomic<bool> isRunning{false};

    void configure(uint32_t fps, bool throttleGuard) {
        targetFps = fps > 0 ? fps : 60;
        frameTimeTargetNs = 1000000000ULL / targetFps;
        throttleGuardEnabled = throttleGuard;
    }

    // Regola i cicli CPU per non saturare i core e mantenere la temperatura bassa
    inline void syncFrame(std::chrono::steady_clock::time_point frameStart) {
        if (!throttleGuardEnabled) return;
        auto now = std::chrono::steady_clock::now();
        auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now - frameStart).count();
        if (elapsedNs < frameTimeTargetNs) {
            auto sleepNs = frameTimeTargetNs - elapsedNs;
            // Sleep leggero o yield per abbassare il clock CPU e prevenire riscaldamento
            struct timespec req;
            req.tv_sec = sleepNs / 1000000000ULL;
            req.tv_nsec = sleepNs % 1000000000ULL;
            nanosleep(&req, nullptr);
        }
    }
};

/**
 * Interfaccia Memory Callbacks per Dynarmic JIT
 */
class DynarmicUserCallbacks {
public:
    virtual ~DynarmicUserCallbacks() = default;
    virtual uint8_t MemoryRead8(uint32_t vaddr) = 0;
    virtual uint16_t MemoryRead16(uint32_t vaddr) = 0;
    virtual uint32_t MemoryRead32(uint32_t vaddr) = 0;
    virtual uint64_t MemoryRead64(uint32_t vaddr) = 0;

    virtual void MemoryWrite8(uint32_t vaddr, uint8_t value) = 0;
    virtual void MemoryWrite16(uint32_t vaddr, uint16_t value) = 0;
    virtual void MemoryWrite32(uint32_t vaddr, uint32_t value) = 0;
    virtual void MemoryWrite64(uint32_t vaddr, uint64_t value) = 0;

    virtual void InterpreterFallback(uint32_t pc, size_t num_instructions) = 0;
    virtual void CallSVC(uint32_t swi) = 0;
    virtual void ExceptionRaised(uint32_t pc, uint32_t exception) = 0;
};

class DbtEngine : public DynarmicUserCallbacks {
public:
    DbtEngine();
    ~DbtEngine() override;

    bool initialize(size_t guestRamSize = 1024 * 1024 * 512); // Default 512MB RAM guest
    void configurePerformance(uint32_t targetFps, bool enableFhd, bool throttleGuard);
    bool loadLibrary(const std::string& libraryPath);
    bool executeEntryPoint();

    // Invoca una funzione nativa all'indirizzo target (es. JNI_OnLoad) con argomenti passati in r0, r1
    uint32_t callFunction(uint32_t functionAddress, uint32_t arg0 = 0, uint32_t arg1 = 0);

    // Dynarmic Memory Callbacks con supporto Fastmem diretto
    uint8_t MemoryRead8(uint32_t vaddr) override;
    uint16_t MemoryRead16(uint32_t vaddr) override;
    uint32_t MemoryRead32(uint32_t vaddr) override;
    uint64_t MemoryRead64(uint32_t vaddr) override;

    void MemoryWrite8(uint32_t vaddr, uint8_t value) override;
    void MemoryWrite16(uint32_t vaddr, uint16_t value) override;
    void MemoryWrite32(uint32_t vaddr, uint32_t value) override;
    void MemoryWrite64(uint32_t vaddr, uint64_t value) override;

    void InterpreterFallback(uint32_t pc, size_t num_instructions) override;
    void CallSVC(uint32_t swi) override;
    void ExceptionRaised(uint32_t pc, uint32_t exception) override;

    uintptr_t getGuestBaseAddress() const { return guestRamBase_; }
    Arm32CpuState& getCpuState() { return cpuState_; }
    PerformanceGovernor& getGovernor() { return governor_; }

private:
    void setupGuestStack();
    void resetRegisters();
    void* getHostAddress(uint32_t guestAddr) const;

    uintptr_t guestRamBase_{0};
    size_t guestRamSize_{0};
    uint32_t guestStackPointer_{0};

    Arm32CpuState cpuState_{};
    Elf32Loader loader_;
    std::shared_ptr<LoadedLibrary> currentLib_;
    PerformanceGovernor governor_;
    bool isRunning_{false};
};

} // namespace emux
