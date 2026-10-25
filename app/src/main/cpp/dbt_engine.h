#pragma once

#include "elf_loader.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace emux {

// Struttura dello stato dei registri ARM32 (ARMv7-A)
struct Arm32CpuState {
    uint32_t r[16];   // r0..r12, r13(SP), r14(LR), r15(PC)
    uint32_t cpsr;    // Program Status Register (flags N, Z, C, V)
    uint64_t q[16];   // Registri VFP/NEON (q0..q15, oppure d0..d31)
    uint32_t fpscr;   // Floating-Point Status and Control Register
};

class DbtEngine {
public:
    DbtEngine();
    ~DbtEngine();

    bool initialize(size_t guestRamSize = 1024 * 1024 * 512); // Default 512MB RAM guest
    bool loadLibrary(const std::string& libraryPath);
    bool executeEntryPoint();

    // Invoca una funzione nativa all'indirizzo target (es. JNI_OnLoad) con argomenti passati in r0, r1
    uint32_t callFunction(uint32_t functionAddress, uint32_t arg0 = 0, uint32_t arg1 = 0);

    uintptr_t getGuestBaseAddress() const { return guestRamBase_; }
    Arm32CpuState& getCpuState() { return cpuState_; }

private:
    void setupGuestStack();
    void resetRegisters();

    uintptr_t guestRamBase_{0};
    size_t guestRamSize_{0};
    uint32_t guestStackPointer_{0};

    Arm32CpuState cpuState_{};
    Elf32Loader loader_;
    std::shared_ptr<LoadedLibrary> currentLib_;
    bool isRunning_{false};
};

} // namespace emux
