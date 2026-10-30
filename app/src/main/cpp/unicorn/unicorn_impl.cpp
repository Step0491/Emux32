#include "unicorn/unicorn.h"
#include "unicorn/arm.h"
#include "core/guest_memory.h"
#include "core/tb_cache.h"
#include "core/guest_signal.h"

#include <android/log.h>
#include <sys/mman.h>
#include <cmath>
#include <cstring>
#include <vector>
#include <memory>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <new>
#include <thread>
#include <sched.h>

#if defined(EMUX_ENABLE_DYNARMIC)
#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/A32/coprocessor.h"
#include "dynarmic/interface/exclusive_monitor.h"
#endif

#define TAG "UnicornCore"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace {

constexpr size_t UNICORN_PAGE_SIZE = 0x1000;

struct MemoryRegion {
    uint64_t address;
    size_t size;
    uint32_t perms;
    uint8_t* host_ptr;
    bool owned;
};

struct HookEntry {
    uc_hook id;
    int type;
    void* callback;
    void* user_data;
    uint64_t begin;
    uint64_t end;
};

// Bit di stato del registro CPSR
constexpr uint32_t FLAG_N = 1U << 31; // Negative
constexpr uint32_t FLAG_Z = 1U << 30; // Zero
constexpr uint32_t FLAG_C = 1U << 29; // Carry
constexpr uint32_t FLAG_V = 1U << 28; // Overflow
constexpr uint32_t FLAG_T = 1U << 5;  // Thumb state

inline bool checkCondition(uint32_t cpsr, uint32_t cond) {
    bool n = (cpsr & FLAG_N) != 0;
    bool z = (cpsr & FLAG_Z) != 0;
    bool c = (cpsr & FLAG_C) != 0;
    bool v = (cpsr & FLAG_V) != 0;

    switch (cond & 0xF) {
        case 0:  return z;                     // EQ
        case 1:  return !z;                    // NE
        case 2:  return c;                     // CS / HS
        case 3:  return !c;                    // CC / LO
        case 4:  return n;                     // MI
        case 5:  return !n;                    // PL
        case 6:  return v;                     // VS
        case 7:  return !v;                    // VC
        case 8:  return c && !z;               // HI
        case 9:  return !c || z;               // LS
        case 10: return n == v;                // GE
        case 11: return n != v;                // LT
        case 12: return !z && (n == v);        // GT
        case 13: return z || (n != v);         // LE
        case 14: return true;                  // AL (Always)
        default: return true;
    }
}

inline uint32_t ror32(uint32_t val, uint32_t shift) {
    shift &= 31U;
    if (shift == 0) return val;
    return (val >> shift) | (val << (32U - shift));
}

inline uint32_t bitrev32(uint32_t x) {
    x = ((x >> 1) & 0x55555555U) | ((x & 0x55555555U) << 1);
    x = ((x >> 2) & 0x33333333U) | ((x & 0x33333333U) << 2);
    x = ((x >> 4) & 0x0F0F0F0FU) | ((x & 0x0F0F0F0FU) << 4);
    x = ((x >> 8) & 0x00FF00FFU) | ((x & 0x00FF00FFU) << 8);
    return (x >> 16) | (x << 16);
}

inline uint32_t clz32(uint32_t x) {
    return (x == 0) ? 32U : static_cast<uint32_t>(__builtin_clz(x));
}

inline uint32_t barrelShiftImm(uint32_t val, uint32_t shiftType, uint32_t imm5, bool carryIn, bool& carryOut) {
    carryOut = carryIn;
    switch (shiftType & 3U) {
        case 0: // LSL
            if (imm5 == 0) return val;
            carryOut = ((val >> (32U - imm5)) & 1U) != 0;
            return val << imm5;
        case 1: // LSR
            if (imm5 == 0) {
                carryOut = (val & 0x80000000U) != 0;
                return 0;
            }
            carryOut = ((val >> (imm5 - 1U)) & 1U) != 0;
            return val >> imm5;
        case 2: // ASR
            if (imm5 == 0) {
                bool neg = (val & 0x80000000U) != 0;
                carryOut = neg;
                return neg ? 0xFFFFFFFFU : 0U;
            }
            carryOut = ((val >> (imm5 - 1U)) & 1U) != 0;
            return static_cast<uint32_t>(static_cast<int32_t>(val) >> imm5);
        case 3: // ROR / RRX
            if (imm5 == 0) { // RRX
                carryOut = (val & 1U) != 0;
                return (carryIn ? 0x80000000U : 0U) | (val >> 1);
            }
            carryOut = ((val >> (imm5 - 1U)) & 1U) != 0;
            return ror32(val, imm5);
    }
    return val;
}

inline uint32_t barrelShiftReg(uint32_t val, uint32_t shiftType, uint32_t rsByte, bool carryIn, bool& carryOut) {
    carryOut = carryIn;
    if (rsByte == 0) return val;
    switch (shiftType & 3U) {
        case 0: // LSL
            if (rsByte < 32U) {
                carryOut = ((val >> (32U - rsByte)) & 1U) != 0;
                return val << rsByte;
            } else if (rsByte == 32U) {
                carryOut = (val & 1U) != 0;
                return 0U;
            } else {
                carryOut = false;
                return 0U;
            }
        case 1: // LSR
            if (rsByte < 32U) {
                carryOut = ((val >> (rsByte - 1U)) & 1U) != 0;
                return val >> rsByte;
            } else if (rsByte == 32U) {
                carryOut = (val & 0x80000000U) != 0;
                return 0U;
            } else {
                carryOut = false;
                return 0U;
            }
        case 2: // ASR
            if (rsByte < 32U) {
                carryOut = ((val >> (rsByte - 1U)) & 1U) != 0;
                return static_cast<uint32_t>(static_cast<int32_t>(val) >> rsByte);
            } else {
                bool neg = (val & 0x80000000U) != 0;
                carryOut = neg;
                return neg ? 0xFFFFFFFFU : 0U;
            }
        case 3: { // ROR
            uint32_t r = rsByte & 31U;
            if (r == 0) {
                carryOut = (val & 0x80000000U) != 0;
                return val;
            }
            carryOut = ((val >> (r - 1U)) & 1U) != 0;
            return ror32(val, r);
        }
    }
    return val;
}

inline uint32_t thumbExpandImmC(uint32_t imm12, bool carryIn, bool& carryOut) {
    carryOut = carryIn;
    uint32_t imm8 = imm12 & 0xFFU;
    if ((imm12 >> 10) == 0) {
        switch ((imm12 >> 8) & 3U) {
            case 0: return imm8;
            case 1: return (imm8 << 16) | imm8;
            case 2: return (imm8 << 24) | (imm8 << 8);
            case 3: return (imm8 << 24) | (imm8 << 16) | (imm8 << 8) | imm8;
        }
    } else {
        uint32_t unrotated = 0x80U | (imm12 & 0x7FU);
        uint32_t rot = (imm12 >> 7) & 0x1FU;
        uint32_t res = ror32(unrotated, rot);
        carryOut = (res & 0x80000000U) != 0;
        return res;
    }
    return imm8;
}

inline float vfpExpandImm32(uint32_t imm8) {
    uint32_t sign = (imm8 >> 7) & 1U;
    uint32_t b = (imm8 >> 6) & 1U;
    uint32_t notB = b ^ 1U;
    uint32_t repB = b ? 0x1FU : 0U;
    uint32_t frac = imm8 & 0x3FU;
    uint32_t raw = (sign << 31) | (notB << 30) | (repB << 25) | (frac << 19);
    float f = 0.0f;
    std::memcpy(&f, &raw, 4);
    return f;
}

inline double vfpExpandImm64(uint32_t imm8) {
    uint64_t sign = (imm8 >> 7) & 1ULL;
    uint64_t b = (imm8 >> 6) & 1ULL;
    uint64_t notB = b ^ 1ULL;
    uint64_t repB = b ? 0xFFULL : 0ULL;
    uint64_t frac = imm8 & 0x3FULL;
    uint64_t raw = (sign << 63) | (notB << 62) | (repB << 54) | (frac << 48);
    double d = 0.0;
    std::memcpy(&d, &raw, 8);
    return d;
}

#if defined(EMUX_ENABLE_DYNARMIC)
class DynarmicUserCallbacks;
class DynarmicCp15;
#endif

#if defined(EMUX_ENABLE_DYNARMIC)
static std::shared_ptr<Dynarmic::ExclusiveMonitor>& getSharedExclusiveMonitor() {
    static auto sSharedMonitor = std::make_shared<Dynarmic::ExclusiveMonitor>(64);
    return sSharedMonitor;
}
static std::atomic<size_t> sNextProcessorId{0};
#endif

struct UnicornInstance {
    uc_arch arch{UC_ARCH_ARM};
    uc_mode mode{UC_MODE_ARM};
    size_t processor_id{0};
    std::atomic<bool> is_running{false};
    std::atomic<bool> stop_requested{false};

    // Registri ARM32 standard: r0..r12, r13(SP), r14(LR), r15(PC)
    uint32_t regs[16]{0};
    uint32_t cpsr{0x00000010};
    uint32_t fpscr{0};
    uint32_t cp15_tpidr_rw{0x7D020000};
    uint32_t cp15_tpidr_ro{0x7D020000};
    uint8_t  itstate{0};

    // Registri VFP (32 registri singola precisione s0..s31 + 16 doppia precisione estesi d16..d31)
    uint32_t vfp_s[32]{0};
    uint64_t vfp_d_ext[16]{0};

    inline float getVfpS(uint32_t regIdx) const {
        if (regIdx >= 32) return 0.0f;
        float val;
        std::memcpy(&val, &vfp_s[regIdx], 4);
        return val;
    }

    inline void setVfpS(uint32_t regIdx, float val) {
        if (regIdx < 32) {
            std::memcpy(&vfp_s[regIdx], &val, 4);
        }
    }

    inline uint64_t getVfpDRaw(uint32_t regIdx) const {
        if (regIdx < 16) {
            return (static_cast<uint64_t>(vfp_s[regIdx * 2 + 1]) << 32) | vfp_s[regIdx * 2];
        } else if (regIdx < 32) {
            return vfp_d_ext[regIdx - 16];
        }
        return 0ULL;
    }

    inline void setVfpDRaw(uint32_t regIdx, uint64_t raw) {
        if (regIdx < 16) {
            vfp_s[regIdx * 2] = static_cast<uint32_t>(raw & 0xFFFFFFFFU);
            vfp_s[regIdx * 2 + 1] = static_cast<uint32_t>(raw >> 32);
        } else if (regIdx < 32) {
            vfp_d_ext[regIdx - 16] = raw;
        }
    }

    inline double getVfpD(uint32_t regIdx) const {
        uint64_t raw = getVfpDRaw(regIdx);
        double val;
        std::memcpy(&val, &raw, 8);
        return val;
    }

    inline void setVfpD(uint32_t regIdx, double val) {
        uint64_t raw;
        std::memcpy(&raw, &val, 8);
        setVfpDRaw(regIdx, raw);
    }

    // Arena flat 4 GB GuestMemory O(1) (g2h = base + addr) + TranslationBlockCache + fallback regioni
    emux::GuestMemory& flat_mem;
    emux::TranslationBlockCache tb_cache;
    std::vector<MemoryRegion> regions;
    int last_region_idx{-1};
    std::mutex mem_mutex;

    // Hook registrati
    std::vector<HookEntry> hooks;
    uc_hook next_hook_id{1};
    std::mutex hook_mutex;

#if defined(EMUX_ENABLE_DYNARMIC)
    uint32_t target_until{0};
    std::unique_ptr<DynarmicUserCallbacks> dynarmic_cb;
    std::shared_ptr<DynarmicCp15> dynarmic_cp15;
    std::unique_ptr<Dynarmic::A32::Jit> dynarmic_jit;
    void initDynarmic();
    uc_err runDynarmic(uint64_t begin, uint64_t until, size_t count);
    void executeInterpreterStep(uint32_t pc, size_t num_instructions);
#endif
    void dispatchIntr(uint32_t svcComment);

    UnicornInstance(uc_arch a, uc_mode m) : arch(a), mode(m), flat_mem(emux::GuestMemory::getSharedInstance()) {
        regs[13] = 0x7E1FF000;
        cpsr = 0x00000010;
        flat_mem.initialize();
#if defined(EMUX_ENABLE_DYNARMIC)
        processor_id = sNextProcessorId.fetch_add(1) % 64;
        initDynarmic();
#endif
    }

    ~UnicornInstance() {
#if defined(EMUX_ENABLE_DYNARMIC)
        if (getSharedExclusiveMonitor()) {
            getSharedExclusiveMonitor()->ClearProcessor(processor_id);
        }
        dynarmic_jit.reset();
        dynarmic_cp15.reset();
        dynarmic_cb.reset();
#endif
        std::lock_guard<std::mutex> lock(mem_mutex);
        tb_cache.clear();
        flat_mem.destroy();
        for (auto& reg : regions) {
            if (reg.owned && reg.host_ptr) {
                munmap(reg.host_ptr, reg.size);
                reg.host_ptr = nullptr;
            }
        }
        regions.clear();
        last_region_idx = -1;
    }

    inline MemoryRegion* findRegion(uint64_t addr) {
        if (last_region_idx >= 0 && static_cast<size_t>(last_region_idx) < regions.size()) {
            MemoryRegion& mr = regions[last_region_idx];
            if (addr >= mr.address && addr < (mr.address + mr.size)) {
                return &mr;
            }
        }
        for (size_t i = 0; i < regions.size(); ++i) {
            MemoryRegion& r = regions[i];
            if (addr >= r.address && addr < (r.address + r.size)) {
                last_region_idx = static_cast<int>(i);
                return &r;
            }
        }
        return nullptr;
    }

    inline uc_err readMem(uint64_t addr, void* dst, size_t size) {
        if (!dst || size == 0) return UC_ERR_OK;
        if (flat_mem.isReady() && addr < emux::GuestMemory::kAddressSpaceSize) {
            uint32_t gaddr = static_cast<uint32_t>(addr);
            if (flat_mem.isMappedRange(gaddr, size)) {
                std::memcpy(dst, flat_mem.g2h(gaddr), size);
                return UC_ERR_OK;
            }
            return UC_ERR_READ_UNMAPPED;
        }
        MemoryRegion* first = findRegion(addr);
        if (first && (addr + size) <= (first->address + first->size)) {
            std::memcpy(dst, first->host_ptr + (addr - first->address), size);
            return UC_ERR_OK;
        }

        uint8_t* out = static_cast<uint8_t*>(dst);
        size_t readCount = 0;
        while (readCount < size) {
            uint64_t cur = addr + readCount;
            MemoryRegion* r = findRegion(cur);
            if (!r) return UC_ERR_READ_UNMAPPED;

            size_t off = static_cast<size_t>(cur - r->address);
            size_t avail = r->size - off;
            size_t take = std::min(size - readCount, avail);

            std::memcpy(out + readCount, r->host_ptr + off, take);
            readCount += take;
        }
        return UC_ERR_OK;
    }

    inline uc_err writeMem(uint64_t addr, const void* src, size_t size) {
        if (!src || size == 0) return UC_ERR_OK;
        if (flat_mem.isReady() && addr < emux::GuestMemory::kAddressSpaceSize) {
            uint32_t gaddr = static_cast<uint32_t>(addr);
            if (flat_mem.isMappedRange(gaddr, size)) {
                std::memcpy(flat_mem.g2h(gaddr), src, size);
                if (flat_mem.getPageProt(gaddr) & emux::GP_EXEC) {
                    tb_cache.invalidateRange(gaddr, static_cast<uint32_t>(size));
#if defined(EMUX_ENABLE_DYNARMIC)
                    if (dynarmic_jit) {
                        dynarmic_jit->InvalidateCacheRange(gaddr, size);
                    }
#endif
                }
                return UC_ERR_OK;
            }
            return UC_ERR_WRITE_UNMAPPED;
        }
        MemoryRegion* first = findRegion(addr);
        if (first && (addr + size) <= (first->address + first->size)) {
            std::memcpy(first->host_ptr + (addr - first->address), src, size);
            return UC_ERR_OK;
        }

        const uint8_t* in = static_cast<const uint8_t*>(src);
        size_t writeCount = 0;
        while (writeCount < size) {
            uint64_t cur = addr + writeCount;
            MemoryRegion* r = findRegion(cur);
            if (!r) return UC_ERR_WRITE_UNMAPPED;

            size_t off = static_cast<size_t>(cur - r->address);
            size_t avail = r->size - off;
            size_t take = std::min(size - writeCount, avail);

            std::memcpy(r->host_ptr + off, in + writeCount, take);
            writeCount += take;
        }
        return UC_ERR_OK;
    }

    inline uint8_t readU8(uint32_t addr) {
        if (flat_mem.isReady() && flat_mem.isMapped(addr)) {
            return *flat_mem.g2h(addr);
        }
        uint8_t val = 0;
        readMem(addr, &val, 1);
        return val;
    }

    inline uint16_t readU16(uint32_t addr) {
        if (flat_mem.isReady() && ((addr & 0xFFFU) <= 0xFFEU) && flat_mem.isMapped(addr)) {
            uint16_t val;
            std::memcpy(&val, flat_mem.g2h(addr), 2);
            return val;
        }
        uint16_t val = 0;
        readMem(addr, &val, 2);
        return val;
    }

    inline uint32_t readU32(uint32_t addr) {
        if (flat_mem.isReady() && ((addr & 0xFFFU) <= 0xFFCU) && flat_mem.isMapped(addr)) {
            uint32_t val;
            std::memcpy(&val, flat_mem.g2h(addr), 4);
            return val;
        }
        uint32_t val = 0;
        readMem(addr, &val, 4);
        return val;
    }

    inline void writeU8(uint32_t addr, uint8_t val) {
        if (flat_mem.isReady() && flat_mem.isMapped(addr)) {
            *flat_mem.g2h(addr) = val;
            return;
        }
        writeMem(addr, &val, 1);
    }

    inline void writeU16(uint32_t addr, uint16_t val) {
        if (flat_mem.isReady() && ((addr & 0xFFFU) <= 0xFFEU) && flat_mem.isMapped(addr)) {
            std::memcpy(flat_mem.g2h(addr), &val, 2);
            return;
        }
        writeMem(addr, &val, 2);
    }

    inline void writeU32(uint32_t addr, uint32_t val) {
        if (flat_mem.isReady() && ((addr & 0xFFFU) <= 0xFFCU) && flat_mem.isMapped(addr)) {
            std::memcpy(flat_mem.g2h(addr), &val, 4);
            return;
        }
        writeMem(addr, &val, 4);
    }

    inline void setFlagsNZ(uint32_t res) {
        if (res == 0) cpsr |= FLAG_Z; else cpsr &= ~FLAG_Z;
        if (res & 0x80000000U) cpsr |= FLAG_N; else cpsr &= ~FLAG_N;
    }

    inline void setFlagsNZC(uint32_t res, bool carry) {
        setFlagsNZ(res);
        if (carry) cpsr |= FLAG_C; else cpsr &= ~FLAG_C;
    }

    inline uint32_t addWithCarry(uint32_t a, uint32_t b, bool carryIn, bool updateFlags) {
        uint64_t uSum = static_cast<uint64_t>(a) + static_cast<uint64_t>(b) + (carryIn ? 1ULL : 0ULL);
        uint32_t res = static_cast<uint32_t>(uSum);
        if (updateFlags) {
            setFlagsNZ(res);
            if (uSum > 0xFFFFFFFFULL) cpsr |= FLAG_C; else cpsr &= ~FLAG_C;
            bool ovf = ((~(a ^ b) & (a ^ res)) & 0x80000000U) != 0;
            if (ovf) cpsr |= FLAG_V; else cpsr &= ~FLAG_V;
        }
        return res;
    }

    inline uint32_t subWithBorrow(uint32_t a, uint32_t b, bool carryIn, bool updateFlags) {
        return addWithCarry(a, ~b, carryIn, updateFlags);
    }

    inline void branchWritePC(uint32_t target) {
        itstate = 0;
        if ((target & 0xFFFF0000U) == 0xDEAD0000U || (target & ~1U) == 0xDEADBEEEU || target == 0) {
            regs[15] = target;
            return;
        }
        if (target & 1U) {
            cpsr |= FLAG_T;
            regs[15] = target & ~1U;
        } else {
            cpsr &= ~FLAG_T;
            regs[15] = target & ~3U;
        }
    }
};

// Esecutore condiviso Coprocessore CP10/CP11 (VFPv3/v4) e CP15 (TLS / Barriere) per ARM32 e Thumb-2
bool executeCoprocessorInsn(UnicornInstance* inst, uint32_t insn, uint32_t pcRelBase) {
    uint32_t coproc = (insn >> 8) & 0xFU;

    // CP15: TLS (TPIDRURO / TPIDRURW) & System Control / Barriers
    if (coproc == 15) {
        bool isMrc = (insn & (1U << 20)) != 0;
        uint32_t crn = (insn >> 16) & 0xFU;
        uint32_t rt = (insn >> 12) & 0xFU;
        uint32_t crm = insn & 0xFU;
        uint32_t opc2 = (insn >> 5) & 7U;

        if (crn == 13 && crm == 0 && (opc2 == 3 || opc2 == 2)) {
            if (isMrc) {
                uint32_t val = (opc2 == 3) ? inst->cp15_tpidr_ro : inst->cp15_tpidr_rw;
                if (rt != 15) inst->regs[rt] = val;
            } else {
                if (opc2 == 2) inst->cp15_tpidr_rw = inst->regs[rt];
                else inst->cp15_tpidr_ro = inst->regs[rt];
            }
            return true;
        }
        if (isMrc && rt != 15) {
            inst->regs[rt] = 0;
        }
        return true;
    }

    if (coproc != 10 && coproc != 11) {
        return false;
    }

    bool isDouble = (coproc == 11);

    // 1. VMRS / VMSR
    if ((insn & 0x0FFF0FFFU) == 0x0EF10A10U) { // VMRS APSR_nzcv, FPSCR
        inst->cpsr = (inst->cpsr & ~0xF0000000U) | (inst->fpscr & 0xF0000000U);
        return true;
    }
    if ((insn & 0x0FF00FFFU) == 0x0EF00A10U) { // VMRS Rt, FPSCR
        uint32_t rt = (insn >> 12) & 0xFU;
        if (rt != 15) inst->regs[rt] = inst->fpscr;
        return true;
    }
    if ((insn & 0x0FF00FFFU) == 0x0EE00A10U) { // VMSR FPSCR, Rt
        uint32_t rt = (insn >> 12) & 0xFU;
        inst->fpscr = inst->regs[rt];
        return true;
    }

    // 2. VMOV core <-> single (0x0E000A10 / 0x0E100A10) & scalar <-> core (0x0E000B10..)
    if ((insn & 0x0FE00F70U) == 0x0E000A10U && !isDouble) {
        bool isToCore = (insn & (1U << 20)) != 0;
        uint32_t vn = (insn >> 16) & 0xFU;
        uint32_t rt = (insn >> 12) & 0xFU;
        uint32_t n = (insn >> 7) & 1U;
        uint32_t sn = (vn << 1) | n;
        if (isToCore) {
            if (rt != 15) inst->regs[rt] = inst->vfp_s[sn];
        } else {
            inst->vfp_s[sn] = inst->regs[rt];
        }
        return true;
    }
    if ((insn & 0x0F900F70U) == 0x0E000B10U && isDouble) { // VMOV Dd[x] <-> Rt
        bool isToCore = (insn & (1U << 20)) != 0;
        uint32_t vd = (((insn >> 7) & 1U) << 4) | ((insn >> 16) & 0xFU);
        uint32_t rt = (insn >> 12) & 0xFU;
        bool hiWord = ((insn >> 21) & 1U) != 0;
        uint64_t raw = inst->getVfpDRaw(vd);
        if (isToCore) {
            if (rt != 15) inst->regs[rt] = hiWord ? static_cast<uint32_t>(raw >> 32) : static_cast<uint32_t>(raw);
        } else {
            if (hiWord) raw = (raw & 0x00000000FFFFFFFFULL) | (static_cast<uint64_t>(inst->regs[rt]) << 32);
            else raw = (raw & 0xFFFFFFFF00000000ULL) | static_cast<uint64_t>(inst->regs[rt]);
            inst->setVfpDRaw(vd, raw);
        }
        return true;
    }

    // 3. VMOV core pair <-> double / two singles (0x0C400A10 / 0x0C400B10)
    if ((insn & 0x0FE00FD0U) == 0x0C400A10U || (insn & 0x0FE00FD0U) == 0x0C400B10U) {
        bool isToCore = (insn & (1U << 20)) != 0;
        uint32_t rt = (insn >> 12) & 0xFU;
        uint32_t rt2 = (insn >> 16) & 0xFU;
        uint32_t m = (insn >> 5) & 1U;
        uint32_t vm = insn & 0xFU;
        if (!isDouble) {
            uint32_t sm = (vm << 1) | m;
            if (isToCore) {
                if (rt < 15) inst->regs[rt] = (sm < 32) ? inst->vfp_s[sm] : 0;
                if (rt2 < 15) inst->regs[rt2] = (sm + 1 < 32) ? inst->vfp_s[sm + 1] : 0;
            } else {
                if (sm < 32) inst->vfp_s[sm] = inst->regs[rt];
                if (sm + 1 < 32) inst->vfp_s[sm + 1] = inst->regs[rt2];
            }
        } else {
            uint32_t dm = (m << 4) | vm;
            if (isToCore) {
                uint64_t raw = inst->getVfpDRaw(dm);
                if (rt < 15) inst->regs[rt] = static_cast<uint32_t>(raw & 0xFFFFFFFFU);
                if (rt2 < 15) inst->regs[rt2] = static_cast<uint32_t>(raw >> 32);
            } else {
                uint64_t raw = (static_cast<uint64_t>(inst->regs[rt2]) << 32) | inst->regs[rt];
                inst->setVfpDRaw(dm, raw);
            }
        }
        return true;
    }

    // 4. VLDR / VSTR & VLDM / VSTM (VPUSH / VPOP): bits [27:25] == 110 (0x0C000000)
    if ((insn & 0x0E000000U) == 0x0C000000U) {
        bool p = (insn & (1U << 24)) != 0;
        bool u = (insn & (1U << 23)) != 0;
        bool d = (insn & (1U << 22)) != 0;
        bool w = (insn & (1U << 21)) != 0;
        bool isLoad = (insn & (1U << 20)) != 0;
        uint32_t rn = (insn >> 16) & 0xFU;
        uint32_t vd = (insn >> 12) & 0xFU;
        uint32_t imm8 = insn & 0xFFU;

        uint32_t base = (rn == 15) ? pcRelBase : inst->regs[rn];

        if (p && !w) {
            // VLDR / VSTR singolo
            uint32_t offset = imm8 * 4U;
            uint32_t effAddr = u ? (base + offset) : (base - offset);
            if (!isDouble) {
                uint32_t sd = (vd << 1) | (d ? 1U : 0U);
                if (sd < 32) {
                    if (isLoad) inst->vfp_s[sd] = inst->readU32(effAddr);
                    else inst->writeU32(effAddr, inst->vfp_s[sd]);
                }
            } else {
                uint32_t dd = ((d ? 1U : 0U) << 4) | vd;
                if (dd < 32) {
                    if (isLoad) {
                        uint64_t lo = inst->readU32(effAddr);
                        uint64_t hi = inst->readU32(effAddr + 4);
                        inst->setVfpDRaw(dd, (hi << 32) | lo);
                    } else {
                        uint64_t raw = inst->getVfpDRaw(dd);
                        inst->writeU32(effAddr, static_cast<uint32_t>(raw));
                        inst->writeU32(effAddr + 4, static_cast<uint32_t>(raw >> 32));
                    }
                }
            }
        } else if (p || u) {
            // VLDM / VSTM (IA: p=0,u=1 / DB: p=1,u=0, inclusi VPOP = VLDMIA sp! e VPUSH = VSTMDB sp!)
            uint32_t bytesTotal = imm8 * 4U;
            uint32_t startAddr = u ? base : (base - bytesTotal);
            uint32_t curAddr = startAddr;

            if (!isDouble) {
                uint32_t count = imm8;
                uint32_t firstReg = (vd << 1) | (d ? 1U : 0U);
                for (uint32_t idx = 0; idx < count; ++idx) {
                    uint32_t rIdx = firstReg + idx;
                    if (rIdx < 32) {
                        if (isLoad) inst->vfp_s[rIdx] = inst->readU32(curAddr);
                        else inst->writeU32(curAddr, inst->vfp_s[rIdx]);
                    }
                    curAddr += 4;
                }
            } else {
                uint32_t count = imm8 / 2U;
                uint32_t firstReg = ((d ? 1U : 0U) << 4) | vd;
                for (uint32_t idx = 0; idx < count; ++idx) {
                    uint32_t rIdx = firstReg + idx;
                    if (rIdx < 32) {
                        if (isLoad) {
                            uint64_t lo = inst->readU32(curAddr);
                            uint64_t hi = inst->readU32(curAddr + 4);
                            inst->setVfpDRaw(rIdx, (hi << 32) | lo);
                        } else {
                            uint64_t raw = inst->getVfpDRaw(rIdx);
                            inst->writeU32(curAddr, static_cast<uint32_t>(raw));
                            inst->writeU32(curAddr + 4, static_cast<uint32_t>(raw >> 32));
                        }
                    }
                    curAddr += 8;
                }
            }

            if (w && rn < 15) {
                inst->regs[rn] = u ? (base + bytesTotal) : (base - bytesTotal);
            }
        }
        return true;
    }

    // 5. VFP Data Processing (bits [27:24] == 1110, bit 4 == 0)
    if ((insn & 0x0F000010U) == 0x0E000000U) {
        uint32_t d = (insn >> 22) & 1U;
        uint32_t n = (insn >> 7) & 1U;
        uint32_t m = (insn >> 5) & 1U;
        uint32_t vdField = (insn >> 12) & 0xFU;
        uint32_t vnField = (insn >> 16) & 0xFU;
        uint32_t vmField = insn & 0xFU;

        uint32_t sd = isDouble ? ((d << 4) | vdField) : ((vdField << 1) | d);
        uint32_t sn = isDouble ? ((n << 4) | vnField) : ((vnField << 1) | n);
        uint32_t sm = isDouble ? ((m << 4) | vmField) : ((vmField << 1) | m);

        uint32_t opc1 = (insn >> 20) & 0xBU; // bits 23, 21, 20
        bool bit6 = (insn & (1U << 6)) != 0;

        if (opc1 == 0xBU) {
            // VMOV immediate oppure operazioni unarie / conversioni / VCMP
            if (!bit6) {
                uint32_t imm8 = (vnField << 4) | vmField;
                if (!isDouble) inst->setVfpS(sd, vfpExpandImm32(imm8));
                else inst->setVfpD(sd, vfpExpandImm64(imm8));
                return true;
            }

            uint32_t opc2 = vnField;
            uint32_t opc3 = (insn >> 6) & 3U; // bit 7 e bit 6 (bit 6 e' 1 -> opc3 = 1 o 3)

            if (opc2 == 0x0) {
                if (opc3 == 1) { // VMOV reg
                    if (!isDouble) inst->vfp_s[sd] = inst->vfp_s[sm];
                    else inst->setVfpDRaw(sd, inst->getVfpDRaw(sm));
                } else { // VABS
                    if (!isDouble) inst->setVfpS(sd, std::fabs(inst->getVfpS(sm)));
                    else inst->setVfpD(sd, std::fabs(inst->getVfpD(sm)));
                }
            } else if (opc2 == 0x1) {
                if (opc3 == 1) { // VNEG
                    if (!isDouble) inst->setVfpS(sd, -inst->getVfpS(sm));
                    else inst->setVfpD(sd, -inst->getVfpD(sm));
                } else { // VSQRT
                    if (!isDouble) {
                        float v = inst->getVfpS(sm);
                        inst->setVfpS(sd, v >= 0.0f ? std::sqrt(v) : 0.0f);
                    } else {
                        double v = inst->getVfpD(sm);
                        inst->setVfpD(sd, v >= 0.0 ? std::sqrt(v) : 0.0);
                    }
                }
            } else if (opc2 == 0x4 || opc2 == 0x5) { // VCMP / VCMPE
                bool withZero = (opc2 == 0x5);
                double fa = isDouble ? inst->getVfpD(sd) : static_cast<double>(inst->getVfpS(sd));
                double fb = withZero ? 0.0 : (isDouble ? inst->getVfpD(sm) : static_cast<double>(inst->getVfpS(sm)));
                inst->fpscr &= ~0xF0000000U;
                if (std::isnan(fa) || std::isnan(fb)) {
                    inst->fpscr |= (FLAG_C | FLAG_V);
                } else if (fa == fb) {
                    inst->fpscr |= (FLAG_Z | FLAG_C);
                } else if (fa < fb) {
                    inst->fpscr |= FLAG_N;
                } else {
                    inst->fpscr |= FLAG_C;
                }
            } else if (opc2 == 0x7 && opc3 == 3) { // VCVT.F64.F32 / VCVT.F32.F64
                if (!isDouble) {
                    uint32_t dDst = (d << 4) | vdField;
                    uint32_t sSrc = (vmField << 1) | m;
                    inst->setVfpD(dDst, static_cast<double>(inst->getVfpS(sSrc)));
                } else {
                    uint32_t sDst = (vdField << 1) | d;
                    uint32_t dSrc = (m << 4) | vmField;
                    inst->setVfpS(sDst, static_cast<float>(inst->getVfpD(dSrc)));
                }
            } else if (opc2 == 0x8) { // VCVT.F32/F64.U32/S32
                uint32_t sSrc = (vmField << 1) | m;
                uint32_t rawInt = inst->vfp_s[sSrc];
                bool isSigned = (opc3 == 3);
                if (!isDouble) {
                    float res = isSigned ? static_cast<float>(static_cast<int32_t>(rawInt)) : static_cast<float>(rawInt);
                    inst->setVfpS(sd, res);
                } else {
                    double res = isSigned ? static_cast<double>(static_cast<int32_t>(rawInt)) : static_cast<double>(rawInt);
                    inst->setVfpD(sd, res);
                }
            } else if (opc2 == 0xC || opc2 == 0xD) { // VCVT.U32/S32.F32/F64
                bool toSigned = (opc2 == 0xD);
                uint32_t sDst = (vdField << 1) | d;
                double srcVal = isDouble ? inst->getVfpD(sm) : static_cast<double>(inst->getVfpS(sm));
                if (std::isnan(srcVal)) srcVal = 0.0;
                if (toSigned) {
                    int32_t iVal = (srcVal >= 2147483647.0) ? 2147483647 :
                                   ((srcVal <= -2147483648.0) ? -2147483647 - 1 : static_cast<int32_t>(srcVal));
                    inst->vfp_s[sDst] = static_cast<uint32_t>(iVal);
                } else {
                    uint32_t uVal = (srcVal <= 0.0) ? 0U :
                                    ((srcVal >= 4294967295.0) ? 0xFFFFFFFFU : static_cast<uint32_t>(srcVal));
                    inst->vfp_s[sDst] = uVal;
                }
            }
            return true;
        }

        // Operazioni binarie VFP (VMLA, VMLS, VNMLA, VNMLS, VMUL, VNMUL, VADD, VSUB, VDIV, VFMA, VFMS)
        if (!isDouble) {
            float a = inst->getVfpS(sn);
            float b = inst->getVfpS(sm);
            float acc = inst->getVfpS(sd);
            switch (opc1) {
                case 0x0: inst->setVfpS(sd, bit6 ? (acc - a * b) : (acc + a * b)); break; // VMLA / VMLS
                case 0x1: inst->setVfpS(sd, bit6 ? (-acc + a * b) : (-acc - a * b)); break; // VNMLA / VNMLS
                case 0x2: inst->setVfpS(sd, bit6 ? -(a * b) : (a * b)); break; // VMUL / VNMUL
                case 0x3: inst->setVfpS(sd, bit6 ? (a - b) : (a + b)); break; // VADD / VSUB
                case 0x8: inst->setVfpS(sd, (b != 0.0f) ? (a / b) : 0.0f); break; // VDIV
                case 0xA: inst->setVfpS(sd, bit6 ? (acc - a * b) : (acc + a * b)); break; // VFMA / VFMS
                case 0x9: inst->setVfpS(sd, bit6 ? (-acc + a * b) : (-acc - a * b)); break; // VFNMA / VFNMS
                default: break;
            }
        } else {
            double a = inst->getVfpD(sn);
            double b = inst->getVfpD(sm);
            double acc = inst->getVfpD(sd);
            switch (opc1) {
                case 0x0: inst->setVfpD(sd, bit6 ? (acc - a * b) : (acc + a * b)); break;
                case 0x1: inst->setVfpD(sd, bit6 ? (-acc + a * b) : (-acc - a * b)); break;
                case 0x2: inst->setVfpD(sd, bit6 ? -(a * b) : (a * b)); break;
                case 0x3: inst->setVfpD(sd, bit6 ? (a - b) : (a + b)); break;
                case 0x8: inst->setVfpD(sd, (b != 0.0) ? (a / b) : 0.0); break;
                case 0xA: inst->setVfpD(sd, bit6 ? (acc - a * b) : (acc + a * b)); break;
                case 0x9: inst->setVfpD(sd, bit6 ? (-acc + a * b) : (-acc - a * b)); break;
                default: break;
            }
        }
        return true;
    }

    return true;
}

// Esecutore istruzioni Advanced SIMD NEON (64-bit 'd' e 128-bit 'q' registers)
bool executeNeonInsn(UnicornInstance* inst, uint32_t insn) {
    // 1. NEON Load / Store: VLD1 / VST1 (insn & 0xFE000000 == 0xF4000000)
    if ((insn & 0xFE000000U) == 0xF4000000U) {
        bool isLoad = (insn & (1U << 21)) != 0;
        uint32_t rn = (insn >> 16) & 0xFU;
        uint32_t vd = ((insn >> 18) & 0x10U) | ((insn >> 12) & 0xFU); // (D << 4) | Vd
        uint32_t itype = (insn >> 8) & 0xFU;
        uint32_t rm = insn & 0xFU;

        uint32_t numRegs = 1;
        if (itype == 0xA || itype == 0x2 || itype == 0x6) {
            numRegs = (itype == 0xA) ? 2 : ((itype == 0x6) ? 3 : 4);
        } else if (itype == 0x7) {
            numRegs = 1;
        }

        uint32_t curAddr = inst->regs[rn];
        for (uint32_t r = 0; r < numRegs && (vd + r) < 32; ++r) {
            uint32_t regIdx = vd + r;
            if (isLoad) {
                uint32_t lo = inst->readU32(curAddr);
                uint32_t hi = inst->readU32(curAddr + 4);
                inst->setVfpDRaw(regIdx, (static_cast<uint64_t>(hi) << 32) | lo);
            } else {
                uint64_t raw = inst->getVfpDRaw(regIdx);
                inst->writeU32(curAddr, static_cast<uint32_t>(raw & 0xFFFFFFFFU));
                inst->writeU32(curAddr + 4, static_cast<uint32_t>(raw >> 32));
            }
            curAddr += 8;
        }

        // Post-index writeback
        if (rm == 13) {
            inst->regs[rn] = curAddr;
        } else if (rm != 15) {
            inst->regs[rn] = inst->regs[rn] + inst->regs[rm];
        }
        return true;
    }

    // 2. VDUP (Scalar from ARM Core Register to all elements of vector)
    if ((insn & 0xFFB00F10U) == 0xFE800B10U) {
        uint32_t rt = (insn >> 12) & 0xFU;
        uint32_t vd = ((insn >> 18) & 0x10U) | ((insn >> 16) & 0xFU);
        bool q = (insn & (1U << 6)) != 0;
        uint32_t val = (rt < 15) ? inst->regs[rt] : 0;
        uint64_t dVal = (static_cast<uint64_t>(val) << 32) | val;
        uint32_t count = q ? 2 : 1;
        if (q) vd &= ~1U;
        for (uint32_t r = 0; r < count && (vd + r) < 32; ++r) {
            inst->setVfpDRaw(vd + r, dVal);
        }
        return true;
    }

    // 3. VDUP (Scalar from D-register lane)
    if ((insn & 0xFFB00F90U) == 0xF3B00C00U) {
        uint32_t vd = ((insn >> 18) & 0x10U) | ((insn >> 12) & 0xFU);
        uint32_t vm = ((insn >> 1) & 0x10U) | (insn & 0xFU);
        bool q = (insn & (1U << 6)) != 0;
        uint32_t lane = (insn >> 19) & 1U;
        uint64_t src = inst->getVfpDRaw(vm);
        uint32_t val = lane ? static_cast<uint32_t>(src >> 32) : static_cast<uint32_t>(src & 0xFFFFFFFFU);
        uint64_t dVal = (static_cast<uint64_t>(val) << 32) | val;
        uint32_t count = q ? 2 : 1;
        if (q) vd &= ~1U;
        for (uint32_t r = 0; r < count && (vd + r) < 32; ++r) {
            inst->setVfpDRaw(vd + r, dVal);
        }
        return true;
    }

    // 4. NEON Data Processing: insn & 0xFE000000 == 0xF2000000
    if ((insn & 0xFE000000U) == 0xF2000000U) {
        bool bit24 = (insn & (1U << 24)) != 0; // U bit
        bool bit23 = (insn & (1U << 23)) != 0;
        bool bit21 = (insn & (1U << 21)) != 0;
        bool bit20 = (insn & (1U << 20)) != 0;
        bool bit6 = (insn & (1U << 6)) != 0;   // Q bit (0 = 64-bit d registers, 1 = 128-bit q registers)
        bool bit4 = (insn & (1U << 4)) != 0;
        uint32_t vd = ((insn >> 18) & 0x10U) | ((insn >> 12) & 0xFU); // (D << 4) | Vd
        uint32_t vn = ((insn >> 3) & 0x10U) | ((insn >> 16) & 0xFU);  // (N << 4) | Vn
        uint32_t vm = ((insn >> 1) & 0x10U) | (insn & 0xFU);          // (M << 4) | Vm
        uint32_t opc4 = (insn >> 8) & 0xFU;                            // bits 11..8

        uint32_t countD = bit6 ? 2 : 1; // 128-bit usa due registri D consecutivi (q = 2 * d)
        if (bit6) {
            vd &= ~1U;
            vn &= ~1U;
            vm &= ~1U;
        }

        // 4a. Two-register misc (bit24 == 1, bit23 == 1 e bits 21:20 == 11)
        if (bit24 && bit23 && ((insn & 0x00300000U) == 0x00300000U)) {
            uint32_t miscOp = (insn >> 7) & 0x1FU;
            for (uint32_t r = 0; r < countD && (vd + r) < 32 && (vm + r) < 32; ++r) {
                uint64_t src = inst->getVfpDRaw(vm + r);
                uint64_t dst = src;
                if (miscOp == 0x00) { // VREV64
                    dst = __builtin_bswap64(src);
                } else if (miscOp == 0x01) { // VREV32
                    uint32_t lo = static_cast<uint32_t>(src & 0xFFFFFFFFU);
                    uint32_t hi = static_cast<uint32_t>(src >> 32);
                    dst = (static_cast<uint64_t>(__builtin_bswap32(hi)) << 32) | __builtin_bswap32(lo);
                } else if (miscOp == 0x03) { // VMVN (bitwise NOT)
                    dst = ~src;
                } else if (miscOp == 0x0F) { // VNEG (float)
                    float f0 = 0.0f, f1 = 0.0f;
                    std::memcpy(&f0, &src, 4);
                    std::memcpy(&f1, reinterpret_cast<const uint8_t*>(&src) + 4, 4);
                    f0 = -f0; f1 = -f1;
                    std::memcpy(&dst, &f0, 4);
                    std::memcpy(reinterpret_cast<uint8_t*>(&dst) + 4, &f1, 4);
                } else if (miscOp == 0x0E) { // VABS (float)
                    float f0 = 0.0f, f1 = 0.0f;
                    std::memcpy(&f0, &src, 4);
                    std::memcpy(&f1, reinterpret_cast<const uint8_t*>(&src) + 4, 4);
                    f0 = std::fabs(f0); f1 = std::fabs(f1);
                    std::memcpy(&dst, &f0, 4);
                    std::memcpy(reinterpret_cast<uint8_t*>(&dst) + 4, &f1, 4);
                } else if (miscOp == 0x1B || miscOp == 0x1A) { // VCVT (float <-> int)
                    float f0 = 0.0f, f1 = 0.0f;
                    std::memcpy(&f0, &src, 4);
                    std::memcpy(&f1, reinterpret_cast<const uint8_t*>(&src) + 4, 4);
                    int32_t i0 = static_cast<int32_t>(f0);
                    int32_t i1 = static_cast<int32_t>(f1);
                    std::memcpy(&dst, &i0, 4);
                    std::memcpy(reinterpret_cast<uint8_t*>(&dst) + 4, &i1, 4);
                }
                inst->setVfpDRaw(vd + r, dst);
            }
            return true;
        }

        // 4b. Three-register same length
        for (uint32_t r = 0; r < countD && (vd + r) < 32; ++r) {
            uint64_t aRaw = inst->getVfpDRaw(vn + r);
            uint64_t bRaw = inst->getVfpDRaw(vm + r);
            uint64_t dRaw = inst->getVfpDRaw(vd + r);

            // Floating point operations (opc4 == 0xD, 0xF, 0xC)
            if (opc4 == 0xD || opc4 == 0xF || opc4 == 0xC) {
                float a0 = 0.0f, a1 = 0.0f, b0 = 0.0f, b1 = 0.0f, d0 = 0.0f, d1 = 0.0f;
                std::memcpy(&a0, &aRaw, 4);
                std::memcpy(&a1, reinterpret_cast<const uint8_t*>(&aRaw) + 4, 4);
                std::memcpy(&b0, &bRaw, 4);
                std::memcpy(&b1, reinterpret_cast<const uint8_t*>(&bRaw) + 4, 4);
                std::memcpy(&d0, &dRaw, 4);
                std::memcpy(&d1, reinterpret_cast<const uint8_t*>(&dRaw) + 4, 4);

                float res0 = 0.0f, res1 = 0.0f;
                if (opc4 == 0xD) {
                    if (!bit4) {
                        if (!bit23 && !bit21) { // VADD.F32
                            res0 = a0 + b0; res1 = a1 + b1;
                        } else if (!bit23 && bit21) { // VSUB.F32
                            res0 = a0 - b0; res1 = a1 - b1;
                        } else if (bit23 && !bit21) { // VMUL.F32
                            res0 = a0 * b0; res1 = a1 * b1;
                        } else { // VABD.F32
                            res0 = std::fabs(a0 - b0); res1 = std::fabs(a1 - b1);
                        }
                    } else {
                        if (!bit21) { // VMLA.F32
                            res0 = d0 + a0 * b0; res1 = d1 + a1 * b1;
                        } else { // VMLS.F32
                            res0 = d0 - a0 * b0; res1 = d1 - a1 * b1;
                        }
                    }
                } else if (opc4 == 0xF) {
                    if (!bit21) { // VMAX.F32
                        res0 = std::max(a0, b0); res1 = std::max(a1, b1);
                    } else { // VMIN.F32
                        res0 = std::min(a0, b0); res1 = std::min(a1, b1);
                    }
                } else if (opc4 == 0xC) { // VMUL.F32
                    res0 = a0 * b0; res1 = a1 * b1;
                } else {
                    res0 = a0 + b0; res1 = a1 + b1;
                }

                uint64_t outRaw = 0;
                std::memcpy(&outRaw, &res0, 4);
                std::memcpy(reinterpret_cast<uint8_t*>(&outRaw) + 4, &res1, 4);
                inst->setVfpDRaw(vd + r, outRaw);
            } else if (opc4 == 0x8) { // Integer Add / Sub
                uint32_t a0 = static_cast<uint32_t>(aRaw & 0xFFFFFFFFU);
                uint32_t a1 = static_cast<uint32_t>(aRaw >> 32);
                uint32_t b0 = static_cast<uint32_t>(bRaw & 0xFFFFFFFFU);
                uint32_t b1 = static_cast<uint32_t>(bRaw >> 32);
                uint64_t outRaw = 0;
                if (!bit24) { // VADD.I32
                    outRaw = (static_cast<uint64_t>(a1 + b1) << 32) | (a0 + b0);
                } else { // VSUB.I32
                    outRaw = (static_cast<uint64_t>(a1 - b1) << 32) | (a0 - b0);
                }
                inst->setVfpDRaw(vd + r, outRaw);
            } else if (opc4 == 0x9) { // Integer Mul
                uint32_t a0 = static_cast<uint32_t>(aRaw & 0xFFFFFFFFU);
                uint32_t a1 = static_cast<uint32_t>(aRaw >> 32);
                uint32_t b0 = static_cast<uint32_t>(bRaw & 0xFFFFFFFFU);
                uint32_t b1 = static_cast<uint32_t>(bRaw >> 32);
                uint64_t outRaw = (static_cast<uint64_t>(a1 * b1) << 32) | (a0 * b0);
                inst->setVfpDRaw(vd + r, outRaw);
            } else if (bit4 && (opc4 == 0x1 || opc4 == 0x0)) { // Bitwise Logic
                uint32_t logicOp = ((insn >> 20) & 3U);
                uint64_t outRaw = 0;
                if (!bit24) {
                    if (logicOp == 0) outRaw = aRaw & bRaw;        // VAND
                    else if (logicOp == 1) outRaw = aRaw & ~bRaw;  // VBIC
                    else if (logicOp == 2) outRaw = aRaw | bRaw;   // VORR / VMOV
                    else outRaw = aRaw | ~bRaw;                    // VORN
                } else {
                    if (logicOp == 0) outRaw = aRaw ^ bRaw;                    // VEOR
                    else if (logicOp == 1) outRaw = (dRaw & aRaw) | (~dRaw & bRaw); // VBSL
                    else if (logicOp == 2) outRaw = (bRaw & aRaw) | (dRaw & ~aRaw); // VBIT
                    else outRaw = (bRaw & ~aRaw) | (dRaw & aRaw);             // VBIF
                }
                inst->setVfpDRaw(vd + r, outRaw);
            } else {
                // Default fallback: VMOV register pass-through
                inst->setVfpDRaw(vd + r, bRaw);
            }
        }
        return true;
    }

    return false;
}

uc_err runInterpreter(UnicornInstance* inst, uint64_t begin, uint64_t until, size_t count);

void UnicornInstance::dispatchIntr(uint32_t svcComment) {
    HookEntry localHooks[8];
    size_t hookCount = 0;
    {
        std::lock_guard<std::mutex> lock(hook_mutex);
        for (const auto& h : hooks) {
            if ((h.type & UC_HOOK_INTR) && hookCount < 8) {
                localHooks[hookCount++] = h;
            }
        }
    }
    uint32_t intnoToSend = (svcComment != 0) ? (0x80000000U | svcComment) : 2U;
    for (size_t i = 0; i < hookCount; ++i) {
        auto cb = reinterpret_cast<uc_cb_hookintr_t>(localHooks[i].callback);
        cb(reinterpret_cast<uc_engine*>(this), intnoToSend, localHooks[i].user_data);
    }
}

#if defined(EMUX_ENABLE_DYNARMIC)
class DynarmicCp15 final : public Dynarmic::A32::Coprocessor {
public:
    explicit DynarmicCp15(UnicornInstance* inst) : inst_(inst) {}

    std::optional<Callback> CompileInternalOperation(bool /*two*/, unsigned /*opc1*/, Dynarmic::A32::CoprocReg /*CRd*/,
                                                     Dynarmic::A32::CoprocReg /*CRn*/, Dynarmic::A32::CoprocReg /*CRm*/,
                                                     unsigned /*opc2*/) override {
        return std::nullopt;
    }

    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, Dynarmic::A32::CoprocReg CRn,
                                              Dynarmic::A32::CoprocReg CRm, unsigned opc2) override {
        if (!two && opc1 == 0 && CRn == Dynarmic::A32::CoprocReg::C13 && CRm == Dynarmic::A32::CoprocReg::C0) {
            if (opc2 == 2) return &inst_->cp15_tpidr_rw;
            if (opc2 == 3) return &inst_->cp15_tpidr_ro;
        }
        if (!two && opc1 == 0 && CRn == Dynarmic::A32::CoprocReg::C7) {
            Callback cb{};
            cb.function = [](void*, std::uint32_t, std::uint32_t) -> std::uint64_t {
                return 0;
            };
            return cb;
        }
        return std::monostate{};
    }

    CallbackOrAccessTwoWords CompileSendTwoWords(bool /*two*/, unsigned /*opc*/, Dynarmic::A32::CoprocReg /*CRm*/) override {
        return std::monostate{};
    }

    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, Dynarmic::A32::CoprocReg CRn,
                                             Dynarmic::A32::CoprocReg CRm, unsigned opc2) override {
        if (!two && opc1 == 0 && CRn == Dynarmic::A32::CoprocReg::C13 && CRm == Dynarmic::A32::CoprocReg::C0) {
            if (opc2 == 2) return &inst_->cp15_tpidr_rw;
            if (opc2 == 3) return &inst_->cp15_tpidr_ro;
        }
        return std::monostate{};
    }

    CallbackOrAccessTwoWords CompileGetTwoWords(bool /*two*/, unsigned /*opc*/, Dynarmic::A32::CoprocReg /*CRm*/) override {
        return std::monostate{};
    }

    std::optional<Callback> CompileLoadWords(bool /*two*/, bool /*long_transfer*/, Dynarmic::A32::CoprocReg /*CRd*/,
                                            std::optional<std::uint8_t> /*option*/) override {
        return std::nullopt;
    }

    std::optional<Callback> CompileStoreWords(bool /*two*/, bool /*long_transfer*/, Dynarmic::A32::CoprocReg /*CRd*/,
                                             std::optional<std::uint8_t> /*option*/) override {
        return std::nullopt;
    }

private:
    UnicornInstance* inst_;
};

class DynarmicUserCallbacks final : public Dynarmic::A32::UserCallbacks {
public:
    explicit DynarmicUserCallbacks(UnicornInstance* inst) : inst_(inst) {}

    std::uint8_t MemoryRead8(Dynarmic::A32::VAddr vaddr) override {
        return inst_->readU8(vaddr);
    }
    std::uint16_t MemoryRead16(Dynarmic::A32::VAddr vaddr) override {
        return inst_->readU16(vaddr);
    }
    std::uint32_t MemoryRead32(Dynarmic::A32::VAddr vaddr) override {
        return inst_->readU32(vaddr);
    }
    std::uint64_t MemoryRead64(Dynarmic::A32::VAddr vaddr) override {
        uint32_t lo = inst_->readU32(vaddr);
        uint32_t hi = inst_->readU32(vaddr + 4);
        return (static_cast<uint64_t>(hi) << 32) | lo;
    }

    void MemoryWrite8(Dynarmic::A32::VAddr vaddr, std::uint8_t value) override {
        inst_->writeU8(vaddr, value);
    }
    void MemoryWrite16(Dynarmic::A32::VAddr vaddr, std::uint16_t value) override {
        inst_->writeU16(vaddr, value);
    }
    void MemoryWrite32(Dynarmic::A32::VAddr vaddr, std::uint32_t value) override {
        inst_->writeU32(vaddr, value);
    }
    void MemoryWrite64(Dynarmic::A32::VAddr vaddr, std::uint64_t value) override {
        inst_->writeU32(vaddr, static_cast<uint32_t>(value));
        inst_->writeU32(vaddr + 4, static_cast<uint32_t>(value >> 32));
    }

    std::optional<std::uint32_t> MemoryReadCode(Dynarmic::A32::VAddr vaddr) override {
        // Intercetta ritorni da funzioni guest (LR=0xDEADxxxx) o raggiungimento del target di esecuzione:
        // restituisce l'istruzione ARM32 BKPT #0 (0xe1200070) per provocare un'interruzione pulita sia in ARM che Thumb
        if ((vaddr & 0xFFFF0000U) == 0xDEAD0000U || (vaddr & ~1U) == 0xDEADBEEEU || vaddr == 0 ||
            (inst_->target_until != 0 && (vaddr & ~1U) == (inst_->target_until & ~1U))) {
            return 0xe1200070U; // ARM32 BKPT #0
        }

        if ((vaddr & 0xFFFFF000U) == 0xFFFF0000U) {
            // [0xFFFF0FE0] __kuser_get_tls
            if (vaddr == 0xFFFF0FE0U) return 0xee1d0f70U; // mrc p15, 0, r0, c13, c0, 3
            if (vaddr == 0xFFFF0FE4U) return 0xe12fff1eU; // bx lr

            // [0xFFFF0FA0] __kuser_dmb
            if (vaddr == 0xFFFF0FA0U) return 0xf57ff05eU; // dmb sy
            if (vaddr == 0xFFFF0FA4U) return 0xe12fff1eU; // bx lr

            // [0xFFFF0FC0] __kuser_cmpxchg (r0=oldval, r1=newval, r2=ptr -> r0=0 on success)
            if (vaddr == 0xFFFF0FC0U) return 0xe1923f9fU; // ldrex r3, [r2]
            if (vaddr == 0xFFFF0FC4U) return 0xe1530000U; // cmp r3, r0
            if (vaddr == 0xFFFF0FC8U) return 0x13a00001U; // movne r0, #1
            if (vaddr == 0xFFFF0FCCU) return 0x112fff1eU; // bxne lr
            if (vaddr == 0xFFFF0FD0U) return 0xe1823f91U; // strex r3, r1, [r2]
            if (vaddr == 0xFFFF0FD4U) return 0xe3530000U; // cmp r3, #0
            if (vaddr == 0xFFFF0FD8U) return 0x1a00fff8U; // bne 0xffff0fc0
            if (vaddr == 0xFFFF0FDCU) return 0xe3a00000U; // mov r0, #0
            if (vaddr == 0xFFFF0FFCU) return 0x00000003U; // __kuser_helper_version = 3
        }

        if (inst_->flat_mem.isReady() && inst_->flat_mem.isMapped(vaddr)) {
            uint32_t val = 0;
            std::memcpy(&val, inst_->flat_mem.g2h(vaddr), 4);
            return val;
        }

        uint32_t val = 0;
        if (inst_->readMem(vaddr, &val, 4) == UC_ERR_OK) {
            return val;
        }
        return std::nullopt;
    }

    void InterpreterFallback(Dynarmic::A32::VAddr pc, size_t num_instructions) override {
        LOGW("Dynarmic: InterpreterFallback a PC=0x%08x (num_insns=%zu)", pc, num_instructions);
        inst_->executeInterpreterStep(pc, num_instructions);
    }

    void CallSVC(std::uint32_t swi) override {
        inst_->dispatchIntr(swi);
        if (inst_->stop_requested.load(std::memory_order_relaxed)) {
            inst_->dynarmic_jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
        }
    }

    void ExceptionRaised(Dynarmic::A32::VAddr pc, Dynarmic::A32::Exception exception) override {
        // Se il PC ha raggiunto l'indirizzo sentinella di ritorno (0xDEADxxxx) o il target_until esplicito,
        // arresta il JIT immediatamente senza propagare falsi segnali SIGSEGV al gestore del guest
        if ((pc & 0xFFFF0000U) == 0xDEAD0000U || pc == 0xDEADBEEFU || pc == 0 ||
            (inst_->target_until != 0 && (pc & ~1U) == (inst_->target_until & ~1U))) {
            inst_->dynarmic_jit->Regs()[15] = pc;
            inst_->dynarmic_jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
            return;
        }

        if (exception == Dynarmic::A32::Exception::Yield ||
            exception == Dynarmic::A32::Exception::WaitForEvent ||
            exception == Dynarmic::A32::Exception::WaitForInterrupt ||
            exception == Dynarmic::A32::Exception::SendEvent ||
            exception == Dynarmic::A32::Exception::SendEventLocal) {
            sched_yield();
            return;
        }

        if ((pc & ~1U) == 0xDEADBEEEU || (pc & 0xFFFF0000U) == 0xDEAD0000U || pc == 0) {
            inst_->dynarmic_jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return;
        }

        LOGW("Dynarmic: ExceptionRaised a PC=0x%08x (tipo=%d)", pc, static_cast<int>(exception));
        int sig = emux::GUEST_SIGSEGV;
        int code = 1;
        if (exception == Dynarmic::A32::Exception::Breakpoint) {
            sig = emux::GUEST_SIGTRAP;
        } else if (exception == Dynarmic::A32::Exception::UndefinedInstruction) {
            sig = emux::GUEST_SIGILL;
        }
        if (emux::GuestSignalManager::getInstance().deliverSignal(reinterpret_cast<uc_engine*>(inst_), sig, pc, code)) {
            auto& jRegs = inst_->dynarmic_jit->Regs();
            for (int i = 0; i < 16; ++i) jRegs[i] = inst_->regs[i];
            inst_->dynarmic_jit->SetCpsr(inst_->cpsr);
            return;
        }
        inst_->dynarmic_jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
    }

    void AddTicks(std::uint64_t ticks) override {
        ticks_ += ticks;
    }

    std::uint64_t GetTicksRemaining() override {
        return 1000000000ULL;
    }

private:
    UnicornInstance* inst_;
    std::uint64_t ticks_{0};
};

void UnicornInstance::initDynarmic() {
    try {
        dynarmic_cb = std::make_unique<DynarmicUserCallbacks>(this);
        dynarmic_cp15 = std::make_shared<DynarmicCp15>(this);

        Dynarmic::A32::UserConfig conf;
        conf.callbacks = dynarmic_cb.get();
        conf.coprocessors[15] = dynarmic_cp15;
        conf.arch_version = Dynarmic::A32::ArchVersion::v7;
        conf.enable_cycle_counting = false;
        conf.unsafe_optimizations = false;
        conf.global_monitor = getSharedExclusiveMonitor().get();
        conf.processor_id = processor_id;

        // Ottimizzazioni JIT ad alte prestazioni per ARM32 -> AArch64 Gaming
        conf.optimizations = Dynarmic::all_safe_optimizations;
        conf.fastmem_exclusive_access = true;
        conf.code_cache_size = 128 * 1024 * 1024; // 128 MB code cache per thread
        conf.always_little_endian = true;         // Android ARM32 è strettamente little-endian
        conf.wall_clock_cntpct = true;            // Usa clock reale per evitare overhead timer emulati
        conf.hook_hint_instructions = true;       // Intercetta yield/wfe per rilasciare la CPU in spinlock
        conf.define_unpredictable_behaviour = true;

        if (flat_mem.isReady() && flat_mem.base() != nullptr) {
            conf.fastmem_pointer = reinterpret_cast<uintptr_t>(flat_mem.base());
            conf.recompile_on_fastmem_failure = true;
            LOGI("UnicornCore: Dynarmic Fastmem agganciato all'arena base %p", flat_mem.base());
        }

        dynarmic_jit = std::make_unique<Dynarmic::A32::Jit>(conf);
        LOGI("UnicornCore: Dynarmic ARM32 JIT compiler istanziato con successo (BlockLinking, Fastmem, RSB: ON)!");
    } catch (const std::exception& e) {
        LOGE("UnicornCore: Eccezione durante initDynarmic: %s (fallback su interprete)", e.what());
        dynarmic_jit.reset();
    } catch (...) {
        LOGE("UnicornCore: Errore sconosciuto durante initDynarmic (fallback su interprete)");
        dynarmic_jit.reset();
    }
}

uc_err UnicornInstance::runDynarmic(uint64_t begin, uint64_t until, size_t count) {
    itstate = 0;
    if (begin & 1ULL) {
        cpsr |= FLAG_T;
        regs[15] = static_cast<uint32_t>(begin & ~1ULL);
    } else {
        cpsr &= ~FLAG_T;
        regs[15] = static_cast<uint32_t>(begin & ~3ULL);
    }

    auto& jitRegs = dynarmic_jit->Regs();
    for (int i = 0; i < 16; ++i) {
        jitRegs[i] = regs[i];
    }
    dynarmic_jit->SetCpsr(cpsr);
    dynarmic_jit->SetFpscr(fpscr);

    auto& extRegs = dynarmic_jit->ExtRegs();
    for (int i = 0; i < 32; ++i) {
        extRegs[i] = vfp_s[i];
    }
    for (int i = 0; i < 16; ++i) {
        extRegs[32 + i * 2] = static_cast<uint32_t>(vfp_d_ext[i]);
        extRegs[32 + i * 2 + 1] = static_cast<uint32_t>(vfp_d_ext[i] >> 32);
    }

    dynarmic_jit->ClearHalt(Dynarmic::HaltReason::UserDefined1 | Dynarmic::HaltReason::UserDefined2);

    bool wasRunning = is_running.exchange(true);
    bool prevStop = stop_requested.exchange(false);

    target_until = static_cast<uint32_t>(until);
    uint32_t targetUntil = target_until;

    while (is_running.load(std::memory_order_relaxed) &&
           !stop_requested.load(std::memory_order_relaxed)) {
        uint32_t curPc = dynarmic_jit->Regs()[15];

        if (targetUntil != 0 && (curPc & ~1U) == (targetUntil & ~1U)) {
            break;
        }
        if ((curPc & ~1U) == 0xDEADBEEEU || curPc == 0xDEADBEEFU || (curPc & 0xFFFF0000U) == 0xDEAD0000U || curPc == 0) {
            break;
        }

        Dynarmic::HaltReason hr = dynarmic_jit->Run();

        curPc = dynarmic_jit->Regs()[15];
        if (targetUntil != 0 && (curPc & ~1U) == (targetUntil & ~1U)) {
            break;
        }
        if ((curPc & ~1U) == 0xDEADBEEEU || curPc == 0xDEADBEEFU || (curPc & 0xFFFF0000U) == 0xDEAD0000U || curPc == 0) {
            break;
        }

        if (stop_requested.load(std::memory_order_relaxed)) {
            break;
        }

        if (Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined1) ||
            Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined2)) {
            break;
        }

        if (count > 0) {
            break;
        }
    }
    target_until = 0;

    if ((dynarmic_jit->Regs()[15] & ~1U) == 0xDEADBEEEU ||
        (dynarmic_jit->Regs()[15] & 0xFFFF0000U) == 0xDEAD0000U) {
        if (targetUntil != 0) {
            dynarmic_jit->Regs()[15] = targetUntil;
        }
    }

    auto& finalRegs = dynarmic_jit->Regs();
    for (int i = 0; i < 16; ++i) {
        regs[i] = finalRegs[i];
    }
    cpsr = dynarmic_jit->Cpsr();
    fpscr = dynarmic_jit->Fpscr();

    auto& finalExt = dynarmic_jit->ExtRegs();
    for (int i = 0; i < 32; ++i) {
        vfp_s[i] = finalExt[i];
    }
    for (int i = 0; i < 16; ++i) {
        vfp_d_ext[i] = (static_cast<uint64_t>(finalExt[32 + i * 2 + 1]) << 32) | finalExt[32 + i * 2];
    }

    is_running.store(wasRunning);
    if (prevStop) stop_requested.store(true);
    return UC_ERR_OK;
}

void UnicornInstance::executeInterpreterStep(uint32_t pc, size_t num_instructions) {
    if (dynarmic_jit) {
        auto& jRegs = dynarmic_jit->Regs();
        for (int i = 0; i < 16; ++i) regs[i] = jRegs[i];
        cpsr = dynarmic_jit->Cpsr();
        fpscr = dynarmic_jit->Fpscr();
        auto& jExt = dynarmic_jit->ExtRegs();
        for (int i = 0; i < 32; ++i) vfp_s[i] = jExt[i];
        for (int i = 0; i < 16; ++i) {
            vfp_d_ext[i] = (static_cast<uint64_t>(jExt[32 + i * 2 + 1]) << 32) | jExt[32 + i * 2];
        }
    }

    runInterpreter(this, pc, 0, num_instructions);

    if (dynarmic_jit) {
        auto& jRegs = dynarmic_jit->Regs();
        for (int i = 0; i < 16; ++i) jRegs[i] = regs[i];
        dynarmic_jit->SetCpsr(cpsr);
        dynarmic_jit->SetFpscr(fpscr);
        auto& jExt = dynarmic_jit->ExtRegs();
        for (int i = 0; i < 32; ++i) jExt[i] = vfp_s[i];
        for (int i = 0; i < 16; ++i) {
            jExt[32 + i * 2] = static_cast<uint32_t>(vfp_d_ext[i]);
            jExt[32 + i * 2 + 1] = static_cast<uint32_t>(vfp_d_ext[i] >> 32);
        }
    }
}
#endif

} // namespace

extern "C" {

uc_err uc_open(uc_arch arch, uc_mode mode, uc_engine **uc) {
    if (!uc) return UC_ERR_ARG;
    if (arch != UC_ARCH_ARM) {
        LOGE("Unicorn uc_open: Architettura %d non supportata (solo ARM)", arch);
        return UC_ERR_ARCH;
    }

    auto* inst = new (std::nothrow) UnicornInstance(arch, mode);
    if (!inst) return UC_ERR_NOMEM;

    *uc = reinterpret_cast<uc_engine*>(inst);
    LOGI("Unicorn Engine ARM istanziato con successo (%p)", *uc);
    return UC_ERR_OK;
}

uc_err uc_close(uc_engine *uc) {
    if (!uc) return UC_ERR_HANDLE;
    auto* inst = reinterpret_cast<UnicornInstance*>(uc);
    delete inst;
    LOGI("Unicorn Engine rilasciato");
    return UC_ERR_OK;
}

const char *uc_strerror(uc_err code) {
    switch (code) {
        case UC_ERR_OK: return "OK";
        case UC_ERR_NOMEM: return "Out of memory";
        case UC_ERR_ARCH: return "Invalid architecture";
        case UC_ERR_HANDLE: return "Invalid handle";
        case UC_ERR_MODE: return "Invalid mode";
        case UC_ERR_VERSION: return "Different API version between core and engine";
        case UC_ERR_READ_UNMAPPED: return "Read from invalid memory (UC_ERR_READ_UNMAPPED)";
        case UC_ERR_WRITE_UNMAPPED: return "Write to invalid memory (UC_ERR_WRITE_UNMAPPED)";
        case UC_ERR_FETCH_UNMAPPED: return "Fetch from invalid memory (UC_ERR_FETCH_UNMAPPED)";
        case UC_ERR_HOOK: return "Invalid hook type";
        case UC_ERR_INSN_INVALID: return "Invalid instruction";
        case UC_ERR_MAP: return "Invalid memory mapping";
        case UC_ERR_WRITE_PROT: return "Write to write-protected memory";
        case UC_ERR_READ_PROT: return "Read from non-readable memory";
        case UC_ERR_FETCH_PROT: return "Fetch from non-executable memory";
        case UC_ERR_ARG: return "Invalid argument (indirizzo o dimensione non allineati alla pagina 0x1000)";
        case UC_ERR_READ_UNALIGNED: return "Read from unaligned memory";
        case UC_ERR_WRITE_UNALIGNED: return "Write to unaligned memory";
        case UC_ERR_FETCH_UNALIGNED: return "Fetch from unaligned memory";
        case UC_ERR_HOOK_EXIST: return "Hook already exists";
        case UC_ERR_RESOURCE: return "Insufficient resource";
        case UC_ERR_EXCEPTION: return "Unhandled CPU exception";
        default: return "Unknown error";
    }
}

uc_err uc_reg_read(uc_engine *uc, int regid, void *value) {
    if (!uc || !value) return UC_ERR_HANDLE;
    auto* inst = reinterpret_cast<UnicornInstance*>(uc);

#if defined(EMUX_ENABLE_DYNARMIC)
    if (inst->dynarmic_jit) {
        if (regid >= UC_ARM_REG_R0 && regid <= UC_ARM_REG_R12) {
            *reinterpret_cast<uint32_t*>(value) = inst->dynarmic_jit->Regs()[regid - UC_ARM_REG_R0];
            return UC_ERR_OK;
        }
        if (regid == UC_ARM_REG_SP || regid == UC_ARM_REG_R13) {
            *reinterpret_cast<uint32_t*>(value) = inst->dynarmic_jit->Regs()[13];
            return UC_ERR_OK;
        }
        if (regid == UC_ARM_REG_LR || regid == UC_ARM_REG_R14) {
            *reinterpret_cast<uint32_t*>(value) = inst->dynarmic_jit->Regs()[14];
            return UC_ERR_OK;
        }
        if (regid == UC_ARM_REG_PC || regid == UC_ARM_REG_R15) {
            *reinterpret_cast<uint32_t*>(value) = inst->dynarmic_jit->Regs()[15];
            return UC_ERR_OK;
        }
        if (regid == UC_ARM_REG_CPSR || regid == UC_ARM_REG_APSR) {
            *reinterpret_cast<uint32_t*>(value) = inst->dynarmic_jit->Cpsr();
            return UC_ERR_OK;
        }
        if (regid >= UC_ARM_REG_D0 && regid <= UC_ARM_REG_D31) {
            uint32_t dIdx = regid - UC_ARM_REG_D0;
            uint64_t raw = 0;
            if (dIdx < 16) {
                raw = (static_cast<uint64_t>(inst->dynarmic_jit->ExtRegs()[dIdx * 2 + 1]) << 32) | inst->dynarmic_jit->ExtRegs()[dIdx * 2];
            } else {
                raw = (static_cast<uint64_t>(inst->dynarmic_jit->ExtRegs()[32 + (dIdx - 16) * 2 + 1]) << 32) | inst->dynarmic_jit->ExtRegs()[32 + (dIdx - 16) * 2];
            }
            std::memcpy(value, &raw, 8);
            return UC_ERR_OK;
        }
        if (regid >= UC_ARM_REG_Q0 && regid <= UC_ARM_REG_Q15) {
            uint32_t qIdx = regid - UC_ARM_REG_Q0;
            uint64_t d0 = (static_cast<uint64_t>(inst->dynarmic_jit->ExtRegs()[qIdx * 4 + 1]) << 32) | inst->dynarmic_jit->ExtRegs()[qIdx * 4];
            uint64_t d1 = (static_cast<uint64_t>(inst->dynarmic_jit->ExtRegs()[qIdx * 4 + 3]) << 32) | inst->dynarmic_jit->ExtRegs()[qIdx * 4 + 2];
            std::memcpy(value, &d0, 8);
            std::memcpy(reinterpret_cast<uint8_t*>(value) + 8, &d1, 8);
            return UC_ERR_OK;
        }
        if (regid == UC_ARM_REG_FPSCR || regid == UC_ARM_REG_FPSCR_NZCV) {
            *reinterpret_cast<uint32_t*>(value) = inst->dynarmic_jit->Fpscr();
            return UC_ERR_OK;
        }
        if (regid == UC_ARM_REG_C13_C0_2) {
            *reinterpret_cast<uint32_t*>(value) = inst->cp15_tpidr_rw;
            return UC_ERR_OK;
        }
        if (regid == UC_ARM_REG_C13_C0_3) {
            *reinterpret_cast<uint32_t*>(value) = inst->cp15_tpidr_ro;
            return UC_ERR_OK;
        }
        return UC_ERR_ARG;
    }
#endif

    if (regid >= UC_ARM_REG_R0 && regid <= UC_ARM_REG_R12) {
        *reinterpret_cast<uint32_t*>(value) = inst->regs[regid - UC_ARM_REG_R0];
        return UC_ERR_OK;
    }
    if (regid == UC_ARM_REG_SP || regid == UC_ARM_REG_R13) {
        *reinterpret_cast<uint32_t*>(value) = inst->regs[13];
        return UC_ERR_OK;
    }
    if (regid == UC_ARM_REG_LR || regid == UC_ARM_REG_R14) {
        *reinterpret_cast<uint32_t*>(value) = inst->regs[14];
        return UC_ERR_OK;
    }
    if (regid == UC_ARM_REG_PC || regid == UC_ARM_REG_R15) {
        *reinterpret_cast<uint32_t*>(value) = inst->regs[15];
        return UC_ERR_OK;
    }
    if (regid == UC_ARM_REG_CPSR || regid == UC_ARM_REG_APSR) {
        *reinterpret_cast<uint32_t*>(value) = inst->cpsr;
        return UC_ERR_OK;
    }
    if (regid >= UC_ARM_REG_D0 && regid <= UC_ARM_REG_D31) {
        uint32_t dIdx = regid - UC_ARM_REG_D0;
        double dVal = inst->getVfpD(dIdx);
        std::memcpy(value, &dVal, 8);
        return UC_ERR_OK;
    }
    if (regid >= UC_ARM_REG_Q0 && regid <= UC_ARM_REG_Q15) {
        uint32_t qIdx = regid - UC_ARM_REG_Q0;
        uint64_t d0 = inst->getVfpDRaw(qIdx * 2);
        uint64_t d1 = inst->getVfpDRaw(qIdx * 2 + 1);
        std::memcpy(value, &d0, 8);
        std::memcpy(reinterpret_cast<uint8_t*>(value) + 8, &d1, 8);
        return UC_ERR_OK;
    }
    if (regid == UC_ARM_REG_FPSCR || regid == UC_ARM_REG_FPSCR_NZCV) {
        *reinterpret_cast<uint32_t*>(value) = inst->fpscr;
        return UC_ERR_OK;
    }
    if (regid == UC_ARM_REG_C13_C0_2) {
        *reinterpret_cast<uint32_t*>(value) = inst->cp15_tpidr_rw;
        return UC_ERR_OK;
    }
    if (regid == UC_ARM_REG_C13_C0_3) {
        *reinterpret_cast<uint32_t*>(value) = inst->cp15_tpidr_ro;
        return UC_ERR_OK;
    }

    return UC_ERR_ARG;
}

uc_err uc_reg_write(uc_engine *uc, int regid, const void *value) {
    if (!uc || !value) return UC_ERR_HANDLE;
    auto* inst = reinterpret_cast<UnicornInstance*>(uc);
    uint32_t val = *reinterpret_cast<const uint32_t*>(value);

#if defined(EMUX_ENABLE_DYNARMIC)
    if (inst->dynarmic_jit) {
        if (regid >= UC_ARM_REG_R0 && regid <= UC_ARM_REG_R12) {
            inst->dynarmic_jit->Regs()[regid - UC_ARM_REG_R0] = val;
            inst->regs[regid - UC_ARM_REG_R0] = val;
            return UC_ERR_OK;
        }
        if (regid == UC_ARM_REG_SP || regid == UC_ARM_REG_R13) {
            inst->dynarmic_jit->Regs()[13] = val;
            inst->regs[13] = val;
            return UC_ERR_OK;
        }
        if (regid == UC_ARM_REG_LR || regid == UC_ARM_REG_R14) {
            inst->dynarmic_jit->Regs()[14] = val;
            inst->regs[14] = val;
            return UC_ERR_OK;
        }
        if (regid == UC_ARM_REG_PC || regid == UC_ARM_REG_R15) {
            inst->dynarmic_jit->Regs()[15] = val;
            inst->regs[15] = val;
            return UC_ERR_OK;
        }
        if (regid == UC_ARM_REG_CPSR || regid == UC_ARM_REG_APSR) {
            inst->dynarmic_jit->SetCpsr(val);
            inst->cpsr = val;
            return UC_ERR_OK;
        }
        if (regid >= UC_ARM_REG_D0 && regid <= UC_ARM_REG_D31) {
            uint32_t dIdx = regid - UC_ARM_REG_D0;
            uint64_t raw = 0;
            std::memcpy(&raw, value, 8);
            if (dIdx < 16) {
                inst->dynarmic_jit->ExtRegs()[dIdx * 2] = static_cast<uint32_t>(raw);
                inst->dynarmic_jit->ExtRegs()[dIdx * 2 + 1] = static_cast<uint32_t>(raw >> 32);
            } else {
                inst->dynarmic_jit->ExtRegs()[32 + (dIdx - 16) * 2] = static_cast<uint32_t>(raw);
                inst->dynarmic_jit->ExtRegs()[32 + (dIdx - 16) * 2 + 1] = static_cast<uint32_t>(raw >> 32);
            }
            double dVal = 0.0;
            std::memcpy(&dVal, value, 8);
            inst->setVfpD(dIdx, dVal);
            return UC_ERR_OK;
        }
        if (regid >= UC_ARM_REG_Q0 && regid <= UC_ARM_REG_Q15) {
            uint32_t qIdx = regid - UC_ARM_REG_Q0;
            uint64_t d0 = 0, d1 = 0;
            std::memcpy(&d0, value, 8);
            std::memcpy(&d1, reinterpret_cast<const uint8_t*>(value) + 8, 8);
            inst->dynarmic_jit->ExtRegs()[qIdx * 4] = static_cast<uint32_t>(d0);
            inst->dynarmic_jit->ExtRegs()[qIdx * 4 + 1] = static_cast<uint32_t>(d0 >> 32);
            inst->dynarmic_jit->ExtRegs()[qIdx * 4 + 2] = static_cast<uint32_t>(d1);
            inst->dynarmic_jit->ExtRegs()[qIdx * 4 + 3] = static_cast<uint32_t>(d1 >> 32);
            inst->setVfpDRaw(qIdx * 2, d0);
            inst->setVfpDRaw(qIdx * 2 + 1, d1);
            return UC_ERR_OK;
        }
        if (regid == UC_ARM_REG_FPSCR || regid == UC_ARM_REG_FPSCR_NZCV) {
            inst->dynarmic_jit->SetFpscr(val);
            inst->fpscr = val;
            return UC_ERR_OK;
        }
        if (regid == UC_ARM_REG_C13_C0_2) {
            inst->cp15_tpidr_rw = val;
            return UC_ERR_OK;
        }
        if (regid == UC_ARM_REG_C13_C0_3) {
            inst->cp15_tpidr_ro = val;
            return UC_ERR_OK;
        }
        return UC_ERR_ARG;
    }
#endif

    if (regid >= UC_ARM_REG_R0 && regid <= UC_ARM_REG_R12) {
        inst->regs[regid - UC_ARM_REG_R0] = val;
        return UC_ERR_OK;
    }
    if (regid == UC_ARM_REG_SP || regid == UC_ARM_REG_R13) {
        inst->regs[13] = val;
        return UC_ERR_OK;
    }
    if (regid == UC_ARM_REG_LR || regid == UC_ARM_REG_R14) {
        inst->regs[14] = val;
        return UC_ERR_OK;
    }
    if (regid == UC_ARM_REG_PC || regid == UC_ARM_REG_R15) {
        inst->regs[15] = val;
        return UC_ERR_OK;
    }
    if (regid == UC_ARM_REG_CPSR || regid == UC_ARM_REG_APSR) {
        inst->cpsr = val;
        return UC_ERR_OK;
    }
    if (regid >= UC_ARM_REG_D0 && regid <= UC_ARM_REG_D31) {
        uint32_t dIdx = regid - UC_ARM_REG_D0;
        double dVal = 0.0;
        std::memcpy(&dVal, value, 8);
        inst->setVfpD(dIdx, dVal);
        return UC_ERR_OK;
    }
    if (regid >= UC_ARM_REG_Q0 && regid <= UC_ARM_REG_Q15) {
        uint32_t qIdx = regid - UC_ARM_REG_Q0;
        uint64_t d0 = 0, d1 = 0;
        std::memcpy(&d0, value, 8);
        std::memcpy(&d1, reinterpret_cast<const uint8_t*>(value) + 8, 8);
        inst->setVfpDRaw(qIdx * 2, d0);
        inst->setVfpDRaw(qIdx * 2 + 1, d1);
        return UC_ERR_OK;
    }
    if (regid == UC_ARM_REG_FPSCR || regid == UC_ARM_REG_FPSCR_NZCV) {
        inst->fpscr = *reinterpret_cast<const uint32_t*>(value);
        return UC_ERR_OK;
    }
    if (regid == UC_ARM_REG_C13_C0_2) {
        inst->cp15_tpidr_rw = val;
        return UC_ERR_OK;
    }
    if (regid == UC_ARM_REG_C13_C0_3) {
        inst->cp15_tpidr_ro = val;
        return UC_ERR_OK;
    }

    return UC_ERR_ARG;
}

uc_err uc_mem_map(uc_engine *uc, uint64_t address, size_t size, uint32_t perms) {
    if (!uc) return UC_ERR_HANDLE;
    if (size == 0) return UC_ERR_ARG;

    if ((address & (UNICORN_PAGE_SIZE - 1)) != 0 || (size & (UNICORN_PAGE_SIZE - 1)) != 0) {
        LOGE("uc_mem_map: Errore fatale UC_ERR_ARG: Indirizzo 0x%llx o dimensione 0x%zx non allineati alla pagina 0x1000!",
             (unsigned long long)address, size);
        return UC_ERR_ARG;
    }

    auto* inst = reinterpret_cast<UnicornInstance*>(uc);
    std::lock_guard<std::mutex> lock(inst->mem_mutex);

    if (inst->flat_mem.isReady() && (address + size) <= emux::GuestMemory::kAddressSpaceSize) {
        uint8_t gprot = emux::GP_NONE;
        if (perms & UC_PROT_READ)  gprot |= emux::GP_READ;
        if (perms & UC_PROT_WRITE) gprot |= emux::GP_WRITE;
        if (perms & UC_PROT_EXEC)  gprot |= emux::GP_EXEC;
        if (gprot == emux::GP_NONE) gprot = emux::GP_RWX;

        if (!inst->flat_mem.map(static_cast<uint32_t>(address), static_cast<uint32_t>(size), gprot)) {
            LOGE("uc_mem_map: GuestMemory::map fallita per guest 0x%llx (%zu byte)",
                 (unsigned long long)address, size);
            return UC_ERR_NOMEM;
        }
        return UC_ERR_OK;
    }

    for (auto& r : inst->regions) {
        if (address >= r.address && (address + size) <= (r.address + r.size)) {
            r.perms |= perms;
            return UC_ERR_OK;
        }
    }

    for (const auto& r : inst->regions) {
        if (!(address + size <= r.address || address >= r.address + r.size)) {
            LOGW("uc_mem_map: Regione 0x%llx (size 0x%zx) si sovrappone con 0x%llx (size 0x%zx)",
                 (unsigned long long)address, size, (unsigned long long)r.address, r.size);
            return UC_ERR_MAP;
        }
    }

    int mprot = PROT_READ | PROT_WRITE;
    void* host = mmap(nullptr, size, mprot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (host == MAP_FAILED) {
        LOGE("uc_mem_map: Allocazione host mmap fallita per guest 0x%llx (%zu byte)",
             (unsigned long long)address, size);
        return UC_ERR_NOMEM;
    }

    MemoryRegion reg{};
    reg.address = address;
    reg.size = size;
    reg.perms = perms;
    reg.host_ptr = static_cast<uint8_t*>(host);
    reg.owned = true;
    inst->regions.push_back(reg);
    inst->last_region_idx = static_cast<int>(inst->regions.size() - 1);

    LOGI("uc_mem_map successo: Guest 0x%08llx - 0x%08llx (Size: %zu KB, Perms: 0x%x)",
         (unsigned long long)address, (unsigned long long)(address + size), size / 1024, perms);
    return UC_ERR_OK;
}

uc_err uc_mem_unmap(uc_engine *uc, uint64_t address, size_t size) {
    if (!uc) return UC_ERR_HANDLE;
    if ((address & (UNICORN_PAGE_SIZE - 1)) != 0 || (size & (UNICORN_PAGE_SIZE - 1)) != 0) {
        return UC_ERR_ARG;
    }

    auto* inst = reinterpret_cast<UnicornInstance*>(uc);
    std::lock_guard<std::mutex> lock(inst->mem_mutex);

    if (inst->flat_mem.isReady() && (address + size) <= emux::GuestMemory::kAddressSpaceSize) {
        bool res = inst->flat_mem.unmap(static_cast<uint32_t>(address), static_cast<uint32_t>(size));
#if defined(EMUX_ENABLE_DYNARMIC)
        if (inst->dynarmic_jit) {
            inst->dynarmic_jit->InvalidateCacheRange(static_cast<uint32_t>(address), size);
        }
#endif
        return res ? UC_ERR_OK : UC_ERR_NOMEM;
    }

    for (auto it = inst->regions.begin(); it != inst->regions.end(); ++it) {
        if (it->address == address && it->size == size) {
            if (it->owned && it->host_ptr) {
                munmap(it->host_ptr, it->size);
            }
            inst->regions.erase(it);
            inst->last_region_idx = -1;
            return UC_ERR_OK;
        }
    }

    return UC_ERR_NOMEM;
}

uc_err uc_mem_write(uc_engine *uc, uint64_t address, const void *bytes, size_t size) {
    if (!uc || !bytes) return UC_ERR_HANDLE;
    if (size == 0) return UC_ERR_OK;

    auto* inst = reinterpret_cast<UnicornInstance*>(uc);
    std::lock_guard<std::mutex> lock(inst->mem_mutex);
    return inst->writeMem(address, bytes, size);
}

uc_err uc_mem_read(uc_engine *uc, uint64_t address, void *bytes, size_t size) {
    if (!uc || !bytes) return UC_ERR_HANDLE;
    if (size == 0) return UC_ERR_OK;

    auto* inst = reinterpret_cast<UnicornInstance*>(uc);
    std::lock_guard<std::mutex> lock(inst->mem_mutex);
    return inst->readMem(address, bytes, size);
}

uc_err uc_hook_add(
    uc_engine *uc,
    uc_hook *hh,
    int type,
    void *callback,
    void *user_data,
    uint64_t begin,
    uint64_t end,
    ...
) {
    if (!uc || !callback) return UC_ERR_HANDLE;
    auto* inst = reinterpret_cast<UnicornInstance*>(uc);

    std::lock_guard<std::mutex> lock(inst->hook_mutex);
    HookEntry entry{};
    entry.id = inst->next_hook_id++;
    entry.type = type;
    entry.callback = callback;
    entry.user_data = user_data;
    entry.begin = begin;
    entry.end = end;

    inst->hooks.push_back(entry);
    if (hh) {
        *hh = entry.id;
    }

    LOGI("uc_hook_add: Registrato hook tipo %d (id=%zu, begin=0x%llx, end=0x%llx)",
         type, entry.id, (unsigned long long)begin, (unsigned long long)end);
    return UC_ERR_OK;
}

uc_err uc_hook_del(uc_engine *uc, uc_hook hh) {
    if (!uc) return UC_ERR_HANDLE;
    auto* inst = reinterpret_cast<UnicornInstance*>(uc);

    std::lock_guard<std::mutex> lock(inst->hook_mutex);
    for (auto it = inst->hooks.begin(); it != inst->hooks.end(); ++it) {
        if (it->id == hh) {
            inst->hooks.erase(it);
            return UC_ERR_OK;
        }
    }
    return UC_ERR_HOOK;
}

} // extern "C"

namespace {

uc_err runInterpreter(UnicornInstance* inst, uint64_t begin, uint64_t until, size_t count) {
    if (!inst) return UC_ERR_HANDLE;

    inst->itstate = 0;
    if (begin & 1ULL) {
        inst->cpsr |= FLAG_T;
        inst->regs[15] = static_cast<uint32_t>(begin & ~1ULL);
    } else {
        inst->cpsr &= ~FLAG_T;
        inst->regs[15] = static_cast<uint32_t>(begin & ~3ULL);
    }

    bool wasRunning = inst->is_running.exchange(true);
    bool prevStop = inst->stop_requested.exchange(false);

    uint32_t lastValidPc = inst->regs[15];
    size_t instructionsExecuted = 0;
    uint32_t nullRecoveries = 0;

    auto dispatchIntr = [&](uint32_t svcComment) {
        HookEntry localHooks[8];
        size_t hookCount = 0;
        {
            std::lock_guard<std::mutex> lock(inst->hook_mutex);
            for (const auto& h : inst->hooks) {
                if ((h.type & UC_HOOK_INTR) && hookCount < 8) {
                    localHooks[hookCount++] = h;
                }
            }
        }
        uint32_t intnoToSend = (svcComment != 0) ? (0x80000000U | svcComment) : 2U;
        for (size_t i = 0; i < hookCount; ++i) {
            auto cb = reinterpret_cast<uc_cb_hookintr_t>(localHooks[i].callback);
            cb(reinterpret_cast<uc_engine*>(inst), intnoToSend, localHooks[i].user_data);
        }
    };

    while (inst->is_running.load(std::memory_order_relaxed) && !inst->stop_requested.load(std::memory_order_relaxed)) {
        uint32_t pc = inst->regs[15];

        if (until != 0 && (pc & ~1U) == (static_cast<uint32_t>(until) & ~1U)) break;
        if ((pc & ~1U) == 0xDEADBEEEU || (pc & 0xFFFF0000U) == 0xDEAD0000U || pc == 0) break;

        // Linux ARM32 Kernel User Helpers (0xFFFF0F60 .. 0xFFFF0FE0)
        if ((pc & 0xFFFFF000U) == 0xFFFF0000U) {
            if (pc == 0xFFFF0FE0U) { // __kuser_get_tls
                inst->regs[0] = inst->cp15_tpidr_ro;
                inst->branchWritePC(inst->regs[14]);
                instructionsExecuted++;
                continue;
            }
            if (pc == 0xFFFF0FA0U) { // __kuser_memory_barrier
                inst->branchWritePC(inst->regs[14]);
                instructionsExecuted++;
                continue;
            }
            if (pc == 0xFFFF0FC0U) { // __kuser_cmpxchg
                uint32_t cur = inst->readU32(inst->regs[2]);
                if (cur == inst->regs[0]) {
                    inst->writeU32(inst->regs[2], inst->regs[1]);
                    inst->regs[0] = 0;
                    inst->cpsr |= FLAG_C;
                } else {
                    inst->regs[0] = 1;
                    inst->cpsr &= ~FLAG_C;
                }
                inst->branchWritePC(inst->regs[14]);
                instructionsExecuted++;
                continue;
            }
            if (pc == 0xFFFF0F60U) { // __kuser_cmpxchg64
                uint64_t oldV = (static_cast<uint64_t>(inst->readU32(inst->regs[0] + 4)) << 32) | inst->readU32(inst->regs[0]);
                uint64_t newV = (static_cast<uint64_t>(inst->readU32(inst->regs[1] + 4)) << 32) | inst->readU32(inst->regs[1]);
                uint64_t curV = (static_cast<uint64_t>(inst->readU32(inst->regs[2] + 4)) << 32) | inst->readU32(inst->regs[2]);
                if (curV == oldV) {
                    inst->writeU32(inst->regs[2], static_cast<uint32_t>(newV));
                    inst->writeU32(inst->regs[2] + 4, static_cast<uint32_t>(newV >> 32));
                    inst->regs[0] = 0;
                    inst->cpsr |= FLAG_C;
                } else {
                    inst->regs[0] = 1;
                    inst->cpsr &= ~FLAG_C;
                }
                inst->branchWritePC(inst->regs[14]);
                instructionsExecuted++;
                continue;
            }
        }

        if (pc < 0x1000U) {
            uint32_t lr = inst->regs[14];
            LOGW("uc_emu_start: Salto a NULL/LowAddr PC=0x%08x da lastPC=0x%08x (LR=0x%08x, SP=0x%08x, r0=0x%08x, insns=%zu)",
                 pc, lastValidPc, lr, inst->regs[13], inst->regs[0], instructionsExecuted);
            if (emux::GuestSignalManager::getInstance().deliverSignal(reinterpret_cast<uc_engine*>(inst), emux::GUEST_SIGSEGV, pc, 1)) {
                continue;
            }
            if (lr >= 0x1000U && (lr & 0xFFFF0000U) != 0xDEAD0000U && (lr & ~1U) != 0xDEADBEEEU && lr != lastValidPc && nullRecoveries < 64) {
                nullRecoveries++;
                inst->regs[0] = 0;
                inst->branchWritePC(lr);
                continue;
            }
            break;
        }

        if (count > 0 && instructionsExecuted >= count) break;

        lastValidPc = pc;
        bool isThumb = (inst->cpsr & FLAG_T) != 0;

        if (!isThumb) {
            uint32_t insn = 0;
            bool fetchOk = false;
            if (inst->flat_mem.isReady() && ((pc & 0xFFFU) <= 0xFFCU) && inst->flat_mem.isMapped(pc)) {
                std::memcpy(&insn, inst->flat_mem.g2h(pc), 4);
                fetchOk = true;
            } else {
                fetchOk = (inst->readMem(pc, &insn, 4) == UC_ERR_OK);
            }
            if (!fetchOk) {
                LOGE("uc_emu_start [ARM]: Fault lettura PC=0x%08x (LR=0x%08x)", pc, inst->regs[14]);
                if (emux::GuestSignalManager::getInstance().deliverSignal(reinterpret_cast<uc_engine*>(inst), emux::GUEST_SIGSEGV, pc, 1)) {
                    continue;
                }
                if (inst->regs[14] >= 0x1000U && (inst->regs[14] & 0xFFFF0000U) != 0xDEAD0000U && (inst->regs[14] & ~1U) != 0xDEADBEEEU && nullRecoveries < 32) {
                    nullRecoveries++;
                    inst->regs[0] = 0;
                    inst->branchWritePC(inst->regs[14]);
                    continue;
                }
                inst->is_running.store(false);
                return UC_ERR_FETCH_UNMAPPED;
            }

            uint32_t cond = (insn >> 28) & 0xFU;
            if (cond == 15) {
                if ((insn & 0xFE000000U) == 0xFA000000U) { // BLX imm
                    int32_t imm24 = static_cast<int32_t>((insn & 0x00FFFFFFU) << 8) >> 6;
                    uint32_t hBit = ((insn >> 24) & 1U) << 1;
                    inst->regs[14] = pc + 4;
                    inst->cpsr |= FLAG_T;
                    inst->regs[15] = (pc + 8 + imm24 + hBit) & ~1U;
                    instructionsExecuted++;
                    continue;
                }
                if (executeNeonInsn(inst, insn)) {
                    inst->regs[15] = pc + 4;
                    instructionsExecuted++;
                    continue;
                }
                inst->regs[15] = pc + 4;
                instructionsExecuted++;
                continue;
            }

            if (!checkCondition(inst->cpsr, cond)) {
                inst->regs[15] = pc + 4;
                instructionsExecuted++;
                continue;
            }

            if ((insn & 0x0F000000U) == 0x0F000000U) { // SVC
                uint32_t svcComment = insn & 0x00FFFFFFU;
                inst->regs[15] = pc + 4;
                dispatchIntr(svcComment);
                instructionsExecuted++;
                continue;
            }

            if ((insn & 0x0C000000U) == 0x0C000000U) { // Coprocessor VFP/CP15
                executeCoprocessorInsn(inst, insn, pc + 8);
                inst->regs[15] = pc + 4;
                instructionsExecuted++;
                continue;
            }

            if ((insn & 0x0E000000U) == 0x0A000000U) { // B / BL
                if (insn & 0x01000000U) inst->regs[14] = pc + 4;
                int32_t imm24 = static_cast<int32_t>((insn & 0x00FFFFFFU) << 8) >> 6;
                inst->regs[15] = pc + 8 + imm24;
                instructionsExecuted++;
                continue;
            }

            if ((insn & 0x0E000000U) == 0x08000000U) { // LDM / STM
                uint32_t rn = (insn >> 16) & 0xFU;
                bool isLoad = (insn & (1U << 20)) != 0;
                bool writeBack = (insn & (1U << 21)) != 0;
                bool up = (insn & (1U << 23)) != 0;
                bool preIndex = (insn & (1U << 24)) != 0;
                uint16_t regList = static_cast<uint16_t>(insn & 0xFFFFU);

                uint32_t nRegs = 0;
                for (int i = 0; i < 16; ++i) if (regList & (1U << i)) nRegs++;

                uint32_t base = inst->regs[rn];
                uint32_t totalBytes = nRegs * 4U;
                uint32_t startAddr = up ? (preIndex ? (base + 4U) : base)
                                        : (preIndex ? (base - totalBytes) : (base - totalBytes + 4U));
                uint32_t wbVal = up ? (base + totalBytes) : (base - totalBytes);
                uint32_t curAddr = startAddr;
                uint32_t loadedPc = 0;
                bool loadedPcFlag = false;

                for (int i = 0; i < 16; ++i) {
                    if (regList & (1U << i)) {
                        if (isLoad) {
                            uint32_t v = inst->readU32(curAddr);
                            if (i == 15) { loadedPc = v; loadedPcFlag = true; }
                            else inst->regs[i] = v;
                        } else {
                            inst->writeU32(curAddr, (i == 15) ? (pc + 8) : inst->regs[i]);
                        }
                        curAddr += 4;
                    }
                }
                if (writeBack && rn < 15 && !(isLoad && (regList & (1U << rn)))) inst->regs[rn] = wbVal;
                if (loadedPcFlag) inst->branchWritePC(loadedPc);
                else inst->regs[15] = pc + 4;
                instructionsExecuted++;
                continue;
            }

            if ((insn & 0x0E000010U) == 0x06000010U) { // Media instructions
                uint32_t op1 = (insn >> 20) & 0x1FU;
                uint32_t op2 = (insn >> 5) & 0x7U;
                if ((op1 == 0x11 || op1 == 0x13) && op2 == 0) {
                    uint32_t rd = (insn >> 16) & 0xFU, rm = (insn >> 8) & 0xFU, rn = insn & 0xFU;
                    if (op1 == 0x11) {
                        int32_t a = static_cast<int32_t>(inst->regs[rn]), b = static_cast<int32_t>(inst->regs[rm]);
                        inst->regs[rd] = (b == 0) ? 0U : ((a == -2147483647 - 1 && b == -1) ? 0x80000000U : static_cast<uint32_t>(a / b));
                    } else {
                        uint32_t a = inst->regs[rn], b = inst->regs[rm];
                        inst->regs[rd] = (b == 0) ? 0U : (a / b);
                    }
                } else if ((op1 & 0x1EU) == 0x1CU && (op2 & 3U) == 0) {
                    uint32_t msb = (insn >> 16) & 0x1FU, rd = (insn >> 12) & 0xFU, lsb = (insn >> 7) & 0x1FU, rn = insn & 0xFU;
                    if (msb >= lsb) {
                        uint32_t width = msb - lsb + 1U;
                        uint32_t mask = (width >= 32U) ? 0xFFFFFFFFU : ((1U << width) - 1U);
                        uint32_t src = (rn == 15) ? 0U : (inst->regs[rn] & mask);
                        inst->regs[rd] = (inst->regs[rd] & ~(mask << lsb)) | (src << lsb);
                    }
                } else if (((op1 & 0x1EU) == 0x1AU || (op1 & 0x1EU) == 0x1EU) && (op2 & 3U) == 2U) {
                    bool isSigned = ((op1 & 0x1EU) == 0x1AU);
                    uint32_t width = ((insn >> 16) & 0x1FU) + 1U, rd = (insn >> 12) & 0xFU, lsb = (insn >> 7) & 0x1FU, rn = insn & 0xFU;
                    uint32_t val = inst->regs[rn] >> lsb;
                    if (width < 32U) {
                        val &= (1U << width) - 1U;
                        if (isSigned && (val & (1U << (width - 1U)))) val |= ~((1U << width) - 1U);
                    }
                    inst->regs[rd] = val;
                } else if ((insn & 0x0FF00FF0U) == 0x06BF0F30U) {
                    inst->regs[(insn >> 12) & 0xFU] = __builtin_bswap32(inst->regs[insn & 0xFU]);
                } else if ((insn & 0x0FF00FF0U) == 0x06BF0FB0U) {
                    uint32_t v = inst->regs[insn & 0xFU];
                    inst->regs[(insn >> 12) & 0xFU] = ((v >> 8) & 0x00FF00FFU) | ((v & 0x00FF00FFU) << 8);
                } else if ((insn & 0x0FF00FF0U) == 0x06FF0F30U) {
                    inst->regs[(insn >> 12) & 0xFU] = bitrev32(inst->regs[insn & 0xFU]);
                } else if ((insn & 0x0FF00FF0U) == 0x06FF0FB0U) {
                    uint16_t h = static_cast<uint16_t>(inst->regs[insn & 0xFU] & 0xFFFFU);
                    inst->regs[(insn >> 12) & 0xFU] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>((h >> 8) | (h << 8))));
                } else if ((insn & 0x0F8000F0U) == 0x06800070U) {
                    uint32_t opExt = (insn >> 20) & 7U, rn = (insn >> 16) & 0xFU, rd = (insn >> 12) & 0xFU;
                    uint32_t rmVal = ror32(inst->regs[insn & 0xFU], ((insn >> 10) & 3U) * 8U);
                    uint32_t addBase = (rn == 15) ? 0U : inst->regs[rn];
                    uint32_t ext = rmVal;
                    if (opExt == 2) ext = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(rmVal & 0xFFU)));
                    else if (opExt == 3) ext = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(rmVal & 0xFFFFU)));
                    else if (opExt == 6) ext = rmVal & 0xFFU;
                    else if (opExt == 7) ext = rmVal & 0xFFFFU;
                    inst->regs[rd] = addBase + ext;
                }
                inst->regs[15] = pc + 4;
                instructionsExecuted++;
                continue;
            }

            if ((insn & 0x0C000000U) == 0x04000000U) { // LDR / STR / LDRB / STRB
                bool isRegOffset = (insn & (1U << 25)) != 0;
                bool preIndex = (insn & (1U << 24)) != 0;
                bool up = (insn & (1U << 23)) != 0;
                bool isByte = (insn & (1U << 22)) != 0;
                bool writeBack = (insn & (1U << 21)) != 0;
                bool isLoad = (insn & (1U << 20)) != 0;
                uint32_t rn = (insn >> 16) & 0xFU, rd = (insn >> 12) & 0xFU;
                bool dummyC = false;
                uint32_t offset = !isRegOffset ? (insn & 0xFFFU)
                    : barrelShiftImm(inst->regs[insn & 0xFU], (insn >> 5) & 3U, (insn >> 7) & 0x1FU, (inst->cpsr & FLAG_C) != 0, dummyC);
                uint32_t base = (rn == 15) ? (pc + 8) : inst->regs[rn];
                uint32_t modAddr = up ? (base + offset) : (base - offset);
                uint32_t effAddr = preIndex ? modAddr : base;

                if (isLoad) {
                    uint32_t loaded = isByte ? static_cast<uint32_t>(inst->readU8(effAddr)) : inst->readU32(effAddr);
                    if ((!preIndex || writeBack) && rn < 15 && rn != rd) inst->regs[rn] = modAddr;
                    if (rd == 15) {
                        inst->branchWritePC(loaded);
                        instructionsExecuted++;
                        continue;
                    }
                    inst->regs[rd] = loaded;
                } else {
                    uint32_t storeVal = (rd == 15) ? (pc + 8) : inst->regs[rd];
                    if (isByte) inst->writeU8(effAddr, static_cast<uint8_t>(storeVal & 0xFFU));
                    else inst->writeU32(effAddr, storeVal);
                    if ((!preIndex || writeBack) && rn < 15) inst->regs[rn] = modAddr;
                }
                inst->regs[15] = pc + 4;
                instructionsExecuted++;
                continue;
            }

            if ((insn & 0x0C000000U) == 0x00000000U) {
                bool isImm = (insn & (1U << 25)) != 0;

                if (!isImm && (insn & 0x00000090U) == 0x00000090U) {
                    uint32_t shBits = (insn >> 5) & 3U;
                    if (shBits == 0) {
                        if ((insn & 0x01000000U) != 0) { // LDREX / STREX / SWP
                            uint32_t opSync = (insn >> 20) & 0xFU, rn = (insn >> 16) & 0xFU, rd = (insn >> 12) & 0xFU, rt = insn & 0xFU;
                            uint32_t addr = inst->regs[rn];
                            if (opSync == 0x0 || opSync == 0x4) {
                                if (opSync == 0x4) { uint8_t oldV = inst->readU8(addr); inst->writeU8(addr, static_cast<uint8_t>(inst->regs[rt])); inst->regs[rd] = oldV; }
                                else { uint32_t oldV = inst->readU32(addr); inst->writeU32(addr, inst->regs[rt]); inst->regs[rd] = oldV; }
                            } else if ((opSync & 1U) != 0) {
                                if (opSync == 0x9) inst->regs[rd] = inst->readU32(addr);
                                else if (opSync == 0xB) { inst->regs[rd] = inst->readU32(addr); if (rd + 1 < 15) inst->regs[rd + 1] = inst->readU32(addr + 4); }
                                else if (opSync == 0xD) inst->regs[rd] = inst->readU8(addr);
                                else if (opSync == 0xF) inst->regs[rd] = inst->readU16(addr);
                            } else {
                                if (opSync == 0x8) inst->writeU32(addr, inst->regs[rt]);
                                else if (opSync == 0xA) { inst->writeU32(addr, inst->regs[rt]); if (rt + 1 < 15) inst->writeU32(addr + 4, inst->regs[rt + 1]); }
                                else if (opSync == 0xC) inst->writeU8(addr, static_cast<uint8_t>(inst->regs[rt]));
                                else if (opSync == 0xE) inst->writeU16(addr, static_cast<uint16_t>(inst->regs[rt]));
                                inst->regs[rd] = 0;
                            }
                            inst->regs[15] = pc + 4;
                            instructionsExecuted++;
                            continue;
                        }

                        uint32_t mulOp = (insn >> 21) & 7U;
                        bool setFlags = (insn & (1U << 20)) != 0;
                        uint32_t rdHi = (insn >> 16) & 0xFU, rdLo = (insn >> 12) & 0xFU, rs = (insn >> 8) & 0xFU, rm = insn & 0xFU;
                        if (mulOp == 0) {
                            uint32_t res = inst->regs[rm] * inst->regs[rs];
                            inst->regs[rdHi] = res;
                            if (setFlags) inst->setFlagsNZ(res);
                        } else if (mulOp == 1) {
                            uint32_t res = inst->regs[rm] * inst->regs[rs] + inst->regs[rdLo];
                            inst->regs[rdHi] = res;
                            if (setFlags) inst->setFlagsNZ(res);
                        } else if (mulOp == 3) {
                            inst->regs[rdHi] = inst->regs[rdLo] - (inst->regs[rm] * inst->regs[rs]);
                        } else if (mulOp >= 4) {
                            uint64_t prod = 0;
                            if (mulOp == 4 || mulOp == 5) {
                                prod = static_cast<uint64_t>(inst->regs[rm]) * static_cast<uint64_t>(inst->regs[rs]);
                                if (mulOp == 5) prod += (static_cast<uint64_t>(inst->regs[rdHi]) << 32) | inst->regs[rdLo];
                            } else {
                                int64_t sprod = static_cast<int64_t>(static_cast<int32_t>(inst->regs[rm])) *
                                                static_cast<int64_t>(static_cast<int32_t>(inst->regs[rs]));
                                if (mulOp == 7) sprod += static_cast<int64_t>((static_cast<uint64_t>(inst->regs[rdHi]) << 32) | inst->regs[rdLo]);
                                prod = static_cast<uint64_t>(sprod);
                            }
                            inst->regs[rdLo] = static_cast<uint32_t>(prod);
                            inst->regs[rdHi] = static_cast<uint32_t>(prod >> 32);
                            if (setFlags) {
                                if (prod == 0) inst->cpsr |= FLAG_Z; else inst->cpsr &= ~FLAG_Z;
                                if (prod & (1ULL << 63)) inst->cpsr |= FLAG_N; else inst->cpsr &= ~FLAG_N;
                            }
                        }
                        inst->regs[15] = pc + 4;
                        instructionsExecuted++;
                        continue;
                    } else { // Extra Load/Store: STRH, LDRH, LDRD, STRD, LDRSB, LDRSH
                        bool preIndex = (insn & (1U << 24)) != 0, up = (insn & (1U << 23)) != 0;
                        bool immForm = (insn & (1U << 22)) != 0, writeBack = (insn & (1U << 21)) != 0, isLoad = (insn & (1U << 20)) != 0;
                        uint32_t rn = (insn >> 16) & 0xFU, rt = (insn >> 12) & 0xFU;
                        uint32_t offset = immForm ? (((insn >> 4) & 0xF0U) | (insn & 0xFU)) : inst->regs[insn & 0xFU];
                        uint32_t base = (rn == 15) ? (pc + 8) : inst->regs[rn];
                        uint32_t modAddr = up ? (base + offset) : (base - offset);
                        uint32_t effAddr = preIndex ? modAddr : base;

                        if (!isLoad && shBits == 1) inst->writeU16(effAddr, static_cast<uint16_t>(inst->regs[rt]));
                        else if (isLoad && shBits == 1) inst->regs[rt] = inst->readU16(effAddr);
                        else if (!isLoad && shBits == 2) { inst->regs[rt] = inst->readU32(effAddr); if (rt + 1 < 15) inst->regs[rt + 1] = inst->readU32(effAddr + 4); }
                        else if (!isLoad && shBits == 3) { inst->writeU32(effAddr, inst->regs[rt]); if (rt + 1 < 15) inst->writeU32(effAddr + 4, inst->regs[rt + 1]); }
                        else if (isLoad && shBits == 2) inst->regs[rt] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(inst->readU8(effAddr))));
                        else if (isLoad && shBits == 3) inst->regs[rt] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(inst->readU16(effAddr))));

                        if ((!preIndex || writeBack) && rn < 15 && !(isLoad && rn == rt)) inst->regs[rn] = modAddr;
                        inst->regs[15] = pc + 4;
                        instructionsExecuted++;
                        continue;
                    }
                }

                if (!isImm && (insn & 0x01900000U) == 0x01000000U) { // BX, BLX, CLZ, MRS, SMULxy
                    if ((insn & 0x0FFFFFF0U) == 0x012FFF10U) {
                        inst->branchWritePC(inst->regs[insn & 0xFU]);
                        instructionsExecuted++;
                        continue;
                    }
                    if ((insn & 0x0FFFFFF0U) == 0x012FFF30U) {
                        uint32_t target = inst->regs[insn & 0xFU];
                        inst->regs[14] = pc + 4;
                        inst->branchWritePC(target);
                        instructionsExecuted++;
                        continue;
                    }
                    if ((insn & 0x0FFF0FF0U) == 0x016F0F10U) {
                        inst->regs[(insn >> 12) & 0xFU] = clz32(inst->regs[insn & 0xFU]);
                        inst->regs[15] = pc + 4;
                        instructionsExecuted++;
                        continue;
                    }
                    if ((insn & 0x0FBF0FFFU) == 0x010F0000U) {
                        inst->regs[(insn >> 12) & 0xFU] = inst->cpsr;
                        inst->regs[15] = pc + 4;
                        instructionsExecuted++;
                        continue;
                    }
                    if ((insn & 0x01900090U) == 0x01000080U) {
                        uint32_t subOp = (insn >> 21) & 3U, rd = (insn >> 16) & 0xFU, ra = (insn >> 12) & 0xFU, rm = (insn >> 8) & 0xFU, rn = insn & 0xFU;
                        int16_t opN = static_cast<int16_t>((insn & 0x20U) ? (inst->regs[rn] >> 16) : (inst->regs[rn] & 0xFFFFU));
                        int16_t opM = static_cast<int16_t>((insn & 0x40U) ? (inst->regs[rm] >> 16) : (inst->regs[rm] & 0xFFFFU));
                        if (subOp == 0) inst->regs[rd] = static_cast<uint32_t>(static_cast<int32_t>(opN) * static_cast<int32_t>(opM) + static_cast<int32_t>(inst->regs[ra]));
                        else if (subOp == 3) inst->regs[rd] = static_cast<uint32_t>(static_cast<int32_t>(opN) * static_cast<int32_t>(opM));
                    }
                    inst->regs[15] = pc + 4;
                    instructionsExecuted++;
                    continue;
                }

                if (isImm && (insn & 0x01900000U) == 0x01000000U) { // MOVW / MOVT
                    uint32_t sub = (insn >> 20) & 0xFU, rd = (insn >> 12) & 0xFU;
                    uint32_t imm16 = ((insn >> 4) & 0xF000U) | (insn & 0xFFFU);
                    if (sub == 0x0) inst->regs[rd] = imm16;
                    else if (sub == 0x4) inst->regs[rd] = (inst->regs[rd] & 0x0000FFFFU) | (imm16 << 16);
                    inst->regs[15] = pc + 4;
                    instructionsExecuted++;
                    continue;
                }

                uint32_t opcode = (insn >> 21) & 0xFU, rn = (insn >> 16) & 0xFU, rd = (insn >> 12) & 0xFU;
                bool setFlags = (insn & (1U << 20)) != 0;
                bool carryIn = (inst->cpsr & FLAG_C) != 0;
                bool shifterCarry = carryIn;
                uint32_t op2 = 0;

                if (isImm) {
                    uint32_t imm8 = insn & 0xFFU, rot = ((insn >> 8) & 0xFU) * 2U;
                    if (rot == 0) op2 = imm8;
                    else { op2 = ror32(imm8, rot); shifterCarry = (op2 & 0x80000000U) != 0; }
                } else {
                    uint32_t rm = insn & 0xFU, shiftType = (insn >> 5) & 3U;
                    bool regShift = (insn & (1U << 4)) != 0;
                    uint32_t rmVal = (rm == 15) ? (pc + (regShift ? 12U : 8U)) : inst->regs[rm];
                    if (regShift) op2 = barrelShiftReg(rmVal, shiftType, inst->regs[(insn >> 8) & 0xFU] & 0xFFU, carryIn, shifterCarry);
                    else op2 = barrelShiftImm(rmVal, shiftType, (insn >> 7) & 0x1FU, carryIn, shifterCarry);
                }

                uint32_t val1 = (rn == 15) ? (pc + ((!isImm && (insn & (1U << 4))) ? 12U : 8U)) : inst->regs[rn];
                uint32_t res = 0;
                bool writeDest = true;

                switch (opcode) {
                    case 0x0: res = val1 & op2; if (setFlags && rd != 15) inst->setFlagsNZC(res, shifterCarry); break;
                    case 0x1: res = val1 ^ op2; if (setFlags && rd != 15) inst->setFlagsNZC(res, shifterCarry); break;
                    case 0x2: res = inst->subWithBorrow(val1, op2, true, setFlags && rd != 15); break;
                    case 0x3: res = inst->subWithBorrow(op2, val1, true, setFlags && rd != 15); break;
                    case 0x4: res = inst->addWithCarry(val1, op2, false, setFlags && rd != 15); break;
                    case 0x5: res = inst->addWithCarry(val1, op2, carryIn, setFlags && rd != 15); break;
                    case 0x6: res = inst->subWithBorrow(val1, op2, carryIn, setFlags && rd != 15); break;
                    case 0x7: res = inst->subWithBorrow(op2, val1, carryIn, setFlags && rd != 15); break;
                    case 0x8: inst->setFlagsNZC(val1 & op2, shifterCarry); writeDest = false; break;
                    case 0x9: inst->setFlagsNZC(val1 ^ op2, shifterCarry); writeDest = false; break;
                    case 0xA: inst->subWithBorrow(val1, op2, true, true); writeDest = false; break;
                    case 0xB: inst->addWithCarry(val1, op2, false, true); writeDest = false; break;
                    case 0xC: res = val1 | op2; if (setFlags && rd != 15) inst->setFlagsNZC(res, shifterCarry); break;
                    case 0xD: res = op2; if (setFlags && rd != 15) inst->setFlagsNZC(res, shifterCarry); break;
                    case 0xE: res = val1 & ~op2; if (setFlags && rd != 15) inst->setFlagsNZC(res, shifterCarry); break;
                    case 0xF: res = ~op2; if (setFlags && rd != 15) inst->setFlagsNZC(res, shifterCarry); break;
                }

                if (writeDest) {
                    if (rd == 15) {
                        inst->branchWritePC(res);
                        instructionsExecuted++;
                        continue;
                    }
                    inst->regs[rd] = res;
                }
                inst->regs[15] = pc + 4;
                instructionsExecuted++;
                continue;
            }

            inst->regs[15] = pc + 4;
            instructionsExecuted++;
        } else {
            // ==================== DECODIFICA & ESECUZIONE THUMB / THUMB-2 ====================
            uint16_t tInsn = 0;
            bool tFetchOk = false;
            if (inst->flat_mem.isReady() && ((pc & 0xFFFU) <= 0xFFEU) && inst->flat_mem.isMapped(pc)) {
                std::memcpy(&tInsn, inst->flat_mem.g2h(pc), 2);
                tFetchOk = true;
            } else {
                tFetchOk = (inst->readMem(pc, &tInsn, 2) == UC_ERR_OK);
            }
            if (!tFetchOk) {
                LOGE("uc_emu_start [Thumb]: Fault lettura PC=0x%08x (LR=0x%08x)", pc, inst->regs[14]);
                if (emux::GuestSignalManager::getInstance().deliverSignal(reinterpret_cast<uc_engine*>(inst), emux::GUEST_SIGSEGV, pc, 1)) {
                    continue;
                }
                if (inst->regs[14] >= 0x1000U && (inst->regs[14] & 0xFFFF0000U) != 0xDEAD0000U && (inst->regs[14] & ~1U) != 0xDEADBEEEU && nullRecoveries < 32) {
                    nullRecoveries++;
                    inst->regs[0] = 0;
                    inst->branchWritePC(inst->regs[14]);
                    continue;
                }
                inst->is_running.store(false);
                return UC_ERR_FETCH_UNMAPPED;
            }

            bool isThumb32 = ((tInsn & 0xF800U) == 0xE800U) || ((tInsn & 0xF000U) == 0xF000U);
            uint32_t insnLen = isThumb32 ? 4U : 2U;

            bool inItBlock = (inst->itstate & 0x0FU) != 0;
            if (inItBlock) {
                uint32_t itCond = (inst->itstate >> 4) & 0xFU;
                if ((inst->itstate & 0x07U) == 0) inst->itstate = 0;
                else inst->itstate = static_cast<uint8_t>((inst->itstate & 0xE0U) | ((inst->itstate << 1) & 0x1FU));
                if (!checkCondition(inst->cpsr, itCond)) {
                    inst->regs[15] = pc + insnLen;
                    instructionsExecuted++;
                    continue;
                }
            }

            if (isThumb32) {
                uint16_t tInsn2 = inst->readU16(pc + 2);
                uint32_t insn32 = (static_cast<uint32_t>(tInsn) << 16) | tInsn2;

                if ((tInsn & 0xFC00U) == 0xEC00U) { // VFP / CP15
                    executeCoprocessorInsn(inst, 0xE0000000U | (insn32 & 0x0FFFFFFFU), (pc + 4U) & ~3U);
                    inst->regs[15] = pc + 4;
                    instructionsExecuted++;
                    continue;
                }

                // Advanced SIMD NEON Data Processing in Thumb-2 (0xEF.. o 0xFF..)
                if ((tInsn & 0xEF00U) == 0xEF00U) {
                    uint32_t uBit = (tInsn >> 12) & 1U;
                    uint32_t armEquiv = 0xF2000000U | (uBit << 24) | (insn32 & 0x00FFFFFFU);
                    if (executeNeonInsn(inst, armEquiv)) {
                        inst->regs[15] = pc + 4;
                        instructionsExecuted++;
                        continue;
                    }
                }

                // Advanced SIMD NEON Load/Store in Thumb-2 (0xF9..)
                if ((tInsn & 0xFF00U) == 0xF900U) {
                    bool isLoad = (tInsn & (1U << 5)) != 0;
                    uint32_t rn = tInsn & 0xFU;
                    uint32_t vd = (tInsn2 >> 12) & 0xFU;
                    uint32_t itype = (tInsn2 >> 8) & 0xFU;
                    uint32_t rm = tInsn2 & 0xFU;
                    uint32_t armEquiv = 0xF4000000U | (isLoad ? (1U << 21) : 0U) | (rn << 16) | (vd << 12) | (itype << 8) | rm;
                    if (executeNeonInsn(inst, armEquiv)) {
                        inst->regs[15] = pc + 4;
                        instructionsExecuted++;
                        continue;
                    }
                }

                if ((tInsn & 0xF800U) == 0xF000U && (tInsn2 & 0x8000U) != 0) { // B.W, B<cond>.W, BL, BLX
                    uint32_t op1 = (tInsn2 >> 12) & 7U;
                    if ((op1 & 4U) != 0 || (op1 & 1U) != 0) {
                        uint32_t s = (tInsn >> 10) & 1U, imm10 = tInsn & 0x3FFU;
                        uint32_t j1 = (tInsn2 >> 13) & 1U, j2 = (tInsn2 >> 11) & 1U, imm11 = tInsn2 & 0x7FFU;
                        uint32_t i1 = !(j1 ^ s), i2 = !(j2 ^ s);
                        int32_t offset = static_cast<int32_t>((s << 24) | (i1 << 23) | (i2 << 22) | (imm10 << 12) | (imm11 << 1));
                        if (s) offset |= static_cast<int32_t>(0xFE000000U);

                        if ((op1 & 4U) != 0) {
                            inst->regs[14] = (pc + 4U) | 1U;
                            bool isBlx = (op1 & 1U) == 0;
                            uint32_t target = (isBlx ? ((pc + 4U) & ~3U) : (pc + 4U)) + offset;
                            if (isBlx) { inst->cpsr &= ~FLAG_T; inst->regs[15] = target & ~3U; }
                            else { inst->cpsr |= FLAG_T; inst->regs[15] = target & ~1U; }
                        } else {
                            inst->regs[15] = (pc + 4U + offset) & ~1U;
                        }
                        inst->itstate = 0;
                        instructionsExecuted++;
                        continue;
                    } else if ((op1 & 5U) == 0U) {
                        uint32_t bCond = (tInsn >> 6) & 0xFU;
                        if (bCond < 14 && checkCondition(inst->cpsr, bCond)) {
                            uint32_t s = (tInsn >> 10) & 1U, imm6 = tInsn & 0x3FU;
                            uint32_t j1 = (tInsn2 >> 13) & 1U, j2 = (tInsn2 >> 11) & 1U, imm11 = tInsn2 & 0x7FFU;
                            int32_t offset = static_cast<int32_t>((s << 20) | (j2 << 19) | (j1 << 18) | (imm6 << 12) | (imm11 << 1));
                            if (s) offset |= static_cast<int32_t>(0xFFE00000U);
                            inst->regs[15] = (pc + 4U + offset) & ~1U;
                            inst->itstate = 0;
                            instructionsExecuted++;
                            continue;
                        }
                    }
                    inst->regs[15] = pc + 4;
                    instructionsExecuted++;
                    continue;
                }

                if ((tInsn & 0xFA00U) == 0xF200U && (tInsn2 & 0x8000U) == 0) { // MOVW, MOVT, ADDW, SUBW, UBFX, SBFX, BFI, BFC
                    uint32_t op = (tInsn >> 4) & 0x1FU, rn = tInsn & 0xFU, rd = (tInsn2 >> 8) & 0xFU;
                    uint32_t imm3 = (tInsn2 >> 12) & 7U, imm12 = (((tInsn >> 10) & 1U) << 11) | (imm3 << 8) | (tInsn2 & 0xFFU);
                    if (op == 0x00) inst->regs[rd] = ((rn == 15) ? ((pc + 4U) & ~3U) : inst->regs[rn]) + imm12;
                    else if (op == 0x04) inst->regs[rd] = (rn << 12) | imm12;
                    else if (op == 0x0A) inst->regs[rd] = ((rn == 15) ? ((pc + 4U) & ~3U) : inst->regs[rn]) - imm12;
                    else if (op == 0x0C) inst->regs[rd] = (inst->regs[rd] & 0x0000FFFFU) | (((rn << 12) | imm12) << 16);
                    else if (op == 0x14 || op == 0x1C) {
                        uint32_t lsb = (imm3 << 2) | ((tInsn2 >> 6) & 3U), width = (tInsn2 & 0x1FU) + 1U;
                        uint32_t val = inst->regs[rn] >> lsb;
                        if (width < 32U) {
                            val &= (1U << width) - 1U;
                            if (op == 0x14 && (val & (1U << (width - 1U)))) val |= ~((1U << width) - 1U);
                        }
                        inst->regs[rd] = val;
                    } else if (op == 0x16) {
                        uint32_t lsb = (imm3 << 2) | ((tInsn2 >> 6) & 3U), msb = tInsn2 & 0x1FU;
                        if (msb >= lsb) {
                            uint32_t width = msb - lsb + 1U;
                            uint32_t mask = (width >= 32U) ? 0xFFFFFFFFU : ((1U << width) - 1U);
                            uint32_t src = (rn == 15) ? 0U : (inst->regs[rn] & mask);
                            inst->regs[rd] = (inst->regs[rd] & ~(mask << lsb)) | (src << lsb);
                        }
                    }
                    inst->regs[15] = pc + 4;
                    instructionsExecuted++;
                    continue;
                }

                bool isT2ModImm = ((tInsn & 0xFA00U) == 0xF000U) && ((tInsn2 & 0x8000U) == 0);
                bool isT2ShiftReg = ((tInsn & 0xFE00U) == 0xEA00U);
                if (isT2ModImm || isT2ShiftReg) {
                    uint32_t op = (tInsn >> 5) & 0xFU, rn = tInsn & 0xFU, rd = (tInsn2 >> 8) & 0xFU;
                    bool setFlags = (tInsn & (1U << 4)) != 0, carryIn = (inst->cpsr & FLAG_C) != 0, shifterCarry = carryIn;
                    uint32_t op2 = isT2ModImm
                        ? thumbExpandImmC((((tInsn >> 10) & 1U) << 11) | (((tInsn2 >> 12) & 7U) << 8) | (tInsn2 & 0xFFU), carryIn, shifterCarry)
                        : barrelShiftImm(inst->regs[tInsn2 & 0xFU], (tInsn2 >> 4) & 3U, (((tInsn2 >> 12) & 7U) << 2) | ((tInsn2 >> 6) & 3U), carryIn, shifterCarry);
                    uint32_t val1 = inst->regs[rn], res = 0;
                    bool writeRd = true;

                    switch (op) {
                        case 0x0: res = val1 & op2; if (rd == 15 && setFlags) { inst->setFlagsNZC(res, shifterCarry); writeRd = false; } else if (setFlags) inst->setFlagsNZC(res, shifterCarry); break;
                        case 0x1: res = val1 & ~op2; if (setFlags) inst->setFlagsNZC(res, shifterCarry); break;
                        case 0x2: res = (rn == 15) ? op2 : (val1 | op2); if (setFlags) inst->setFlagsNZC(res, shifterCarry); break;
                        case 0x3: res = (rn == 15) ? ~op2 : (val1 | ~op2); if (setFlags) inst->setFlagsNZC(res, shifterCarry); break;
                        case 0x4: res = val1 ^ op2; if (rd == 15 && setFlags) { inst->setFlagsNZC(res, shifterCarry); writeRd = false; } else if (setFlags) inst->setFlagsNZC(res, shifterCarry); break;
                        case 0x8: if (rd == 15 && setFlags) { inst->addWithCarry(val1, op2, false, true); writeRd = false; } else res = inst->addWithCarry(val1, op2, false, setFlags); break;
                        case 0xA: res = inst->addWithCarry(val1, op2, carryIn, setFlags); break;
                        case 0xB: res = inst->subWithBorrow(val1, op2, carryIn, setFlags); break;
                        case 0xD: if (rd == 15 && setFlags) { inst->subWithBorrow(val1, op2, true, true); writeRd = false; } else res = inst->subWithBorrow(val1, op2, true, setFlags); break;
                        case 0xE: res = inst->subWithBorrow(op2, val1, true, setFlags); break;
                        default: writeRd = false; break;
                    }
                    if (writeRd) {
                        if (rd == 15) { inst->branchWritePC(res); instructionsExecuted++; continue; }
                        inst->regs[rd] = res;
                    }
                    inst->regs[15] = pc + 4;
                    instructionsExecuted++;
                    continue;
                }

                if ((tInsn & 0xFE40U) == 0xE800U) { // LDM.W / STM.W
                    bool up = (tInsn & (1U << 7)) != 0, writeBack = (tInsn & (1U << 5)) != 0, isLoad = (tInsn & (1U << 4)) != 0;
                    uint32_t rn = tInsn & 0xFU, nRegs = 0;
                    for (int i = 0; i < 16; ++i) if (tInsn2 & (1U << i)) nRegs++;
                    uint32_t base = inst->regs[rn], totalBytes = nRegs * 4U, curAddr = up ? base : (base - totalBytes);
                    bool loadedPcFlag = false;
                    uint32_t loadedPc = 0;

                    for (int i = 0; i < 16; ++i) {
                        if (tInsn2 & (1U << i)) {
                            if (isLoad) {
                                uint32_t v = inst->readU32(curAddr);
                                if (i == 15) { loadedPc = v; loadedPcFlag = true; }
                                else inst->regs[i] = v;
                            } else inst->writeU32(curAddr, inst->regs[i]);
                            curAddr += 4;
                        }
                    }
                    if (writeBack && rn < 15 && !(isLoad && (tInsn2 & (1U << rn)))) inst->regs[rn] = up ? (base + totalBytes) : (base - totalBytes);
                    if (loadedPcFlag) inst->branchWritePC(loadedPc);
                    else inst->regs[15] = pc + 4;
                    instructionsExecuted++;
                    continue;
                }

                if ((tInsn & 0xFE40U) == 0xE840U) { // TBB, TBH, LDREX/STREX, LDRD/STRD
                    uint32_t rn = tInsn & 0xFU;
                    if ((tInsn & 0xFFF0U) == 0xE8D0U && (tInsn2 & 0xFFE0U) == 0xF000U) {
                        bool isTbh = (tInsn2 & 0x0010U) != 0;
                        uint32_t rm = tInsn2 & 0xFU, base = (rn == 15) ? (pc + 4U) : inst->regs[rn];
                        uint32_t hw = isTbh ? inst->readU16(base + (inst->regs[rm] << 1)) : inst->readU8(base + inst->regs[rm]);
                        inst->regs[15] = (pc + 4U + (hw * 2U)) & ~1U;
                        inst->itstate = 0;
                        instructionsExecuted++;
                        continue;
                    }
                    if ((tInsn & 0xFFE0U) == 0xE840U) {
                        bool isLoad = (tInsn & (1U << 4)) != 0;
                        uint32_t rt = (tInsn2 >> 12) & 0xFU, rd = (tInsn2 >> 8) & 0xFU, addr = inst->regs[rn] + (tInsn2 & 0xFFU) * 4U;
                        if (isLoad) inst->regs[rt] = inst->readU32(addr);
                        else { inst->writeU32(addr, inst->regs[rt]); inst->regs[rd] = 0; }
                        inst->regs[15] = pc + 4;
                        instructionsExecuted++;
                        continue;
                    }
                    if ((tInsn & 0xFFE0U) == 0xE8C0U) {
                        bool isLoad = (tInsn & (1U << 4)) != 0;
                        uint32_t rt = (tInsn2 >> 12) & 0xFU, rt2 = (tInsn2 >> 8) & 0xFU, opEx = (tInsn2 >> 4) & 0xFU, rd = tInsn2 & 0xFU;
                        uint32_t addr = inst->regs[rn];
                        if (isLoad) {
                            if (opEx == 4) inst->regs[rt] = inst->readU8(addr);
                            else if (opEx == 5) inst->regs[rt] = inst->readU16(addr);
                            else if (opEx == 7) { inst->regs[rt] = inst->readU32(addr); inst->regs[rt2] = inst->readU32(addr + 4); }
                        } else {
                            if (opEx == 4) inst->writeU8(addr, static_cast<uint8_t>(inst->regs[rt]));
                            else if (opEx == 5) inst->writeU16(addr, static_cast<uint16_t>(inst->regs[rt]));
                            else if (opEx == 7) { inst->writeU32(addr, inst->regs[rt]); inst->writeU32(addr + 4, inst->regs[rt2]); }
                            inst->regs[rd] = 0;
                        }
                        inst->regs[15] = pc + 4;
                        instructionsExecuted++;
                        continue;
                    }
                    bool preIndex = (tInsn & (1U << 8)) != 0, up = (tInsn & (1U << 7)) != 0, writeBack = (tInsn & (1U << 5)) != 0, isLoad = (tInsn & (1U << 4)) != 0;
                    uint32_t rt = (tInsn2 >> 12) & 0xFU, rt2 = (tInsn2 >> 8) & 0xFU, offset = (tInsn2 & 0xFFU) * 4U;
                    uint32_t base = (rn == 15) ? ((pc + 4U) & ~3U) : inst->regs[rn];
                    uint32_t modAddr = up ? (base + offset) : (base - offset);
                    uint32_t effAddr = preIndex ? modAddr : base;
                    if (isLoad) { inst->regs[rt] = inst->readU32(effAddr); inst->regs[rt2] = inst->readU32(effAddr + 4); }
                    else { inst->writeU32(effAddr, inst->regs[rt]); inst->writeU32(effAddr + 4, inst->regs[rt2]); }
                    if (writeBack && rn < 15) inst->regs[rn] = modAddr;
                    inst->regs[15] = pc + 4;
                    instructionsExecuted++;
                    continue;
                }

                if ((tInsn & 0xFE00U) == 0xF800U) { // LDR.W / STR.W / LDRB.W / LDRH.W / LDRSB.W / LDRSH.W
                    bool signExt = (tInsn & (1U << 8)) != 0, imm12Form = (tInsn & (1U << 7)) != 0;
                    uint32_t sizeCode = (tInsn >> 5) & 3U;
                    bool isLoad = (tInsn & (1U << 4)) != 0;
                    uint32_t rn = tInsn & 0xFU, rt = (tInsn2 >> 12) & 0xFU;
                    if (isLoad && rt == 15 && sizeCode != 2) { inst->regs[15] = pc + 4; instructionsExecuted++; continue; }

                    uint32_t effAddr = 0;
                    if (rn == 15) {
                        uint32_t base = (pc + 4U) & ~3U;
                        effAddr = (tInsn & (1U << 7)) ? (base + (tInsn2 & 0xFFFU)) : (base - (tInsn2 & 0xFFFU));
                    } else if (imm12Form) {
                        effAddr = inst->regs[rn] + (tInsn2 & 0xFFFU);
                    } else if ((tInsn2 & 0x0800U) != 0) {
                        bool pre = (tInsn2 & (1U << 10)) != 0, up = (tInsn2 & (1U << 9)) != 0, wb = (tInsn2 & (1U << 8)) != 0;
                        uint32_t base = inst->regs[rn], modAddr = up ? (base + (tInsn2 & 0xFFU)) : (base - (tInsn2 & 0xFFU));
                        effAddr = pre ? modAddr : base;
                        if ((!pre || wb) && rn < 15 && !(isLoad && rn == rt)) inst->regs[rn] = modAddr;
                    } else {
                        effAddr = inst->regs[rn] + (inst->regs[tInsn2 & 0xFU] << ((tInsn2 >> 4) & 3U));
                    }

                    if (isLoad) {
                        uint32_t val = 0;
                        if (sizeCode == 0) { uint8_t b = inst->readU8(effAddr); val = signExt ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(b))) : b; }
                        else if (sizeCode == 1) { uint16_t h = inst->readU16(effAddr); val = signExt ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(h))) : h; }
                        else val = inst->readU32(effAddr);
                        if (rt == 15) { inst->branchWritePC(val); instructionsExecuted++; continue; }
                        inst->regs[rt] = val;
                    } else {
                        if (sizeCode == 0) inst->writeU8(effAddr, static_cast<uint8_t>(inst->regs[rt]));
                        else if (sizeCode == 1) inst->writeU16(effAddr, static_cast<uint16_t>(inst->regs[rt]));
                        else inst->writeU32(effAddr, inst->regs[rt]);
                    }
                    inst->regs[15] = pc + 4;
                    instructionsExecuted++;
                    continue;
                }

                if ((tInsn & 0xFE00U) == 0xFA00U) { // Reg shifts, extend, REV, CLZ, MUL/MLA/MLS/SMULL/UMULL/SDIV/UDIV
                    uint32_t rn = tInsn & 0xFU, rd = (tInsn2 >> 8) & 0xFU, rm = tInsn2 & 0xFU;
                    if ((tInsn & 0xFF00U) == 0xFA00U && (tInsn2 & 0xF000U) == 0xF000U) {
                        uint32_t op1 = (tInsn >> 4) & 0xFU, op2 = (tInsn2 >> 4) & 0xFU;
                        if ((op1 & 8U) == 0 && (op2 & 0xCU) == 0) {
                            bool cOut = (inst->cpsr & FLAG_C) != 0;
                            uint32_t res = barrelShiftReg(inst->regs[rn], (op1 >> 1) & 3U, inst->regs[rm] & 0xFFU, cOut, cOut);
                            inst->regs[rd] = res;
                            if (op1 & 1U) inst->setFlagsNZC(res, cOut);
                        } else if ((op1 & 8U) == 0 && (op2 & 8U) != 0) {
                            uint32_t v = ror32(inst->regs[rm], (op2 & 3U) * 8U), base = (rn == 15) ? 0U : inst->regs[rn], ext = v;
                            if (op1 == 0) ext = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v & 0xFFFFU)));
                            else if (op1 == 1) ext = v & 0xFFFFU;
                            else if (op1 == 4) ext = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(v & 0xFFU)));
                            else if (op1 == 5) ext = v & 0xFFU;
                            inst->regs[rd] = base + ext;
                        } else if (op1 == 0x9 && (op2 & 0xCU) == 0x8U) {
                            uint32_t v = inst->regs[rm];
                            if ((op2 & 3U) == 0) inst->regs[rd] = __builtin_bswap32(v);
                            else if ((op2 & 3U) == 1) inst->regs[rd] = ((v >> 8) & 0x00FF00FFU) | ((v & 0x00FF00FFU) << 8);
                            else if ((op2 & 3U) == 2) inst->regs[rd] = bitrev32(v);
                            else inst->regs[rd] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(((v & 0xFFFFU) >> 8) | ((v & 0xFFU) << 8))));
                        } else if (op1 == 0xB && op2 == 0x8) {
                            inst->regs[rd] = clz32(inst->regs[rm]);
                        }
                    } else if ((tInsn & 0xFF80U) == 0xFB00U) {
                        uint32_t op1 = (tInsn >> 4) & 7U, op2 = (tInsn2 >> 4) & 0xFU, ra = (tInsn2 >> 12) & 0xFU;
                        if (op1 == 0 && op2 == 0) inst->regs[rd] = (ra == 15) ? (inst->regs[rn] * inst->regs[rm]) : (inst->regs[rn] * inst->regs[rm] + inst->regs[ra]);
                        else if (op1 == 0 && op2 == 1) inst->regs[rd] = inst->regs[ra] - (inst->regs[rn] * inst->regs[rm]);
                    } else if ((tInsn & 0xFF80U) == 0xFB80U) {
                        uint32_t op1 = (tInsn >> 4) & 7U, op2 = (tInsn2 >> 4) & 0xFU, rdLo = (tInsn2 >> 12) & 0xFU, rdHi = rd;
                        if (op1 == 1 && op2 == 0xF) {
                            int32_t a = static_cast<int32_t>(inst->regs[rn]), b = static_cast<int32_t>(inst->regs[rm]);
                            inst->regs[rd] = (b == 0) ? 0U : ((a == -2147483647 - 1 && b == -1) ? 0x80000000U : static_cast<uint32_t>(a / b));
                        } else if (op1 == 3 && op2 == 0xF) {
                            uint32_t a = inst->regs[rn], b = inst->regs[rm];
                            inst->regs[rd] = (b == 0) ? 0U : (a / b);
                        } else if (op1 == 0 && op2 == 0) {
                            int64_t p = static_cast<int64_t>(static_cast<int32_t>(inst->regs[rn])) * static_cast<int64_t>(static_cast<int32_t>(inst->regs[rm]));
                            inst->regs[rdLo] = static_cast<uint32_t>(static_cast<uint64_t>(p));
                            inst->regs[rdHi] = static_cast<uint32_t>(static_cast<uint64_t>(p) >> 32);
                        } else if (op1 == 2 && op2 == 0) {
                            uint64_t p = static_cast<uint64_t>(inst->regs[rn]) * static_cast<uint64_t>(inst->regs[rm]);
                            inst->regs[rdLo] = static_cast<uint32_t>(p);
                            inst->regs[rdHi] = static_cast<uint32_t>(p >> 32);
                        } else if (op1 == 4 && op2 == 0) {
                            int64_t acc = static_cast<int64_t>((static_cast<uint64_t>(inst->regs[rdHi]) << 32) | inst->regs[rdLo]);
                            acc += static_cast<int64_t>(static_cast<int32_t>(inst->regs[rn])) * static_cast<int64_t>(static_cast<int32_t>(inst->regs[rm]));
                            inst->regs[rdLo] = static_cast<uint32_t>(static_cast<uint64_t>(acc));
                            inst->regs[rdHi] = static_cast<uint32_t>(static_cast<uint64_t>(acc) >> 32);
                        } else if (op1 == 6 && op2 == 0) {
                            uint64_t acc = (static_cast<uint64_t>(inst->regs[rdHi]) << 32) | inst->regs[rdLo];
                            acc += static_cast<uint64_t>(inst->regs[rn]) * static_cast<uint64_t>(inst->regs[rm]);
                            inst->regs[rdLo] = static_cast<uint32_t>(acc);
                            inst->regs[rdHi] = static_cast<uint32_t>(acc >> 32);
                        }
                    }
                    inst->regs[15] = pc + 4;
                    instructionsExecuted++;
                    continue;
                }

                inst->regs[15] = pc + 4;
                instructionsExecuted++;
                continue;
            }

            // ==================== THUMB 16-BIT ====================
            bool updateFlags16 = !inItBlock;

            if ((tInsn & 0xE000U) == 0x0000U) {
                uint32_t subOp = (tInsn >> 11) & 3U, rd = tInsn & 7U, rn = (tInsn >> 3) & 7U;
                if (subOp != 3U) {
                    bool cOut = (inst->cpsr & FLAG_C) != 0;
                    uint32_t res = barrelShiftImm(inst->regs[rn], subOp, (tInsn >> 6) & 0x1FU, cOut, cOut);
                    inst->regs[rd] = res;
                    if (updateFlags16) inst->setFlagsNZC(res, cOut);
                } else {
                    bool isImm3 = (tInsn & (1U << 10)) != 0, isSub = (tInsn & (1U << 9)) != 0;
                    uint32_t rmOrImm = (tInsn >> 6) & 7U, op2 = isImm3 ? rmOrImm : inst->regs[rmOrImm];
                    inst->regs[rd] = isSub ? inst->subWithBorrow(inst->regs[rn], op2, true, updateFlags16)
                                           : inst->addWithCarry(inst->regs[rn], op2, false, updateFlags16);
                }
                inst->regs[15] = pc + 2;
                instructionsExecuted++;
                continue;
            }

            if ((tInsn & 0xE000U) == 0x2000U) {
                uint32_t op = (tInsn >> 11) & 3U, rdn = (tInsn >> 8) & 7U, imm8 = tInsn & 0xFFU;
                if (op == 0) { inst->regs[rdn] = imm8; if (updateFlags16) inst->setFlagsNZ(imm8); }
                else if (op == 1) inst->subWithBorrow(inst->regs[rdn], imm8, true, true);
                else if (op == 2) inst->regs[rdn] = inst->addWithCarry(inst->regs[rdn], imm8, false, updateFlags16);
                else inst->regs[rdn] = inst->subWithBorrow(inst->regs[rdn], imm8, true, updateFlags16);
                inst->regs[15] = pc + 2;
                instructionsExecuted++;
                continue;
            }

            if ((tInsn & 0xFC00U) == 0x4000U) {
                uint32_t op = (tInsn >> 6) & 0xFU, rm = (tInsn >> 3) & 7U, rdn = tInsn & 7U;
                uint32_t a = inst->regs[rdn], b = inst->regs[rm], res = 0;
                bool cIn = (inst->cpsr & FLAG_C) != 0, cOut = cIn;
                switch (op) {
                    case 0x0: res = a & b; inst->regs[rdn] = res; if (updateFlags16) inst->setFlagsNZ(res); break;
                    case 0x1: res = a ^ b; inst->regs[rdn] = res; if (updateFlags16) inst->setFlagsNZ(res); break;
                    case 0x2: res = barrelShiftReg(a, 0, b & 0xFFU, cIn, cOut); inst->regs[rdn] = res; if (updateFlags16) inst->setFlagsNZC(res, cOut); break;
                    case 0x3: res = barrelShiftReg(a, 1, b & 0xFFU, cIn, cOut); inst->regs[rdn] = res; if (updateFlags16) inst->setFlagsNZC(res, cOut); break;
                    case 0x4: res = barrelShiftReg(a, 2, b & 0xFFU, cIn, cOut); inst->regs[rdn] = res; if (updateFlags16) inst->setFlagsNZC(res, cOut); break;
                    case 0x5: inst->regs[rdn] = inst->addWithCarry(a, b, cIn, updateFlags16); break;
                    case 0x6: inst->regs[rdn] = inst->subWithBorrow(a, b, cIn, updateFlags16); break;
                    case 0x7: res = barrelShiftReg(a, 3, b & 0xFFU, cIn, cOut); inst->regs[rdn] = res; if (updateFlags16) inst->setFlagsNZC(res, cOut); break;
                    case 0x8: inst->setFlagsNZ(a & b); break;
                    case 0x9: inst->regs[rdn] = inst->subWithBorrow(0, b, true, updateFlags16); break;
                    case 0xA: inst->subWithBorrow(a, b, true, true); break;
                    case 0xB: inst->addWithCarry(a, b, false, true); break;
                    case 0xC: res = a | b; inst->regs[rdn] = res; if (updateFlags16) inst->setFlagsNZ(res); break;
                    case 0xD: res = a * b; inst->regs[rdn] = res; if (updateFlags16) inst->setFlagsNZ(res); break;
                    case 0xE: res = a & ~b; inst->regs[rdn] = res; if (updateFlags16) inst->setFlagsNZ(res); break;
                    case 0xF: res = ~b; inst->regs[rdn] = res; if (updateFlags16) inst->setFlagsNZ(res); break;
                }
                inst->regs[15] = pc + 2;
                instructionsExecuted++;
                continue;
            }

            if ((tInsn & 0xFC00U) == 0x4400U) {
                uint32_t op = (tInsn >> 8) & 3U, rm = (tInsn >> 3) & 0xFU, rdn = ((tInsn >> 4) & 8U) | (tInsn & 7U);
                uint32_t mVal = (rm == 15) ? (pc + 4U) : inst->regs[rm], dVal = (rdn == 15) ? (pc + 4U) : inst->regs[rdn];
                if (op == 0) {
                    uint32_t res = dVal + mVal;
                    if (rdn == 15) { inst->regs[15] = res & ~1U; inst->itstate = 0; instructionsExecuted++; continue; }
                    inst->regs[rdn] = res;
                } else if (op == 1) {
                    inst->subWithBorrow(dVal, mVal, true, true);
                } else if (op == 2) {
                    if (rdn == 15) { inst->regs[15] = mVal & ~1U; inst->itstate = 0; instructionsExecuted++; continue; }
                    inst->regs[rdn] = mVal;
                } else {
                    if (tInsn & 0x0080U) inst->regs[14] = (pc + 2U) | 1U;
                    inst->branchWritePC(mVal);
                    instructionsExecuted++;
                    continue;
                }
                inst->regs[15] = pc + 2;
                instructionsExecuted++;
                continue;
            }

            if ((tInsn & 0xF800U) == 0x4800U) {
                inst->regs[(tInsn >> 8) & 7U] = inst->readU32(((pc + 4U) & ~3U) + ((tInsn & 0xFFU) * 4U));
                inst->regs[15] = pc + 2;
                instructionsExecuted++;
                continue;
            }

            if ((tInsn & 0xF000U) == 0x5000U) {
                uint32_t opB = (tInsn >> 9) & 7U, rm = (tInsn >> 6) & 7U, rn = (tInsn >> 3) & 7U, rt = tInsn & 7U;
                uint32_t addr = inst->regs[rn] + inst->regs[rm];
                switch (opB) {
                    case 0: inst->writeU32(addr, inst->regs[rt]); break;
                    case 1: inst->writeU16(addr, static_cast<uint16_t>(inst->regs[rt])); break;
                    case 2: inst->writeU8(addr, static_cast<uint8_t>(inst->regs[rt])); break;
                    case 3: inst->regs[rt] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(inst->readU8(addr)))); break;
                    case 4: inst->regs[rt] = inst->readU32(addr); break;
                    case 5: inst->regs[rt] = inst->readU16(addr); break;
                    case 6: inst->regs[rt] = inst->readU8(addr); break;
                    case 7: inst->regs[rt] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(inst->readU16(addr)))); break;
                }
                inst->regs[15] = pc + 2;
                instructionsExecuted++;
                continue;
            }

            if ((tInsn & 0xE000U) == 0x6000U) {
                bool isByte = (tInsn & (1U << 12)) != 0, isLoad = (tInsn & (1U << 11)) != 0;
                uint32_t imm5 = (tInsn >> 6) & 0x1FU, rn = (tInsn >> 3) & 7U, rt = tInsn & 7U;
                uint32_t addr = inst->regs[rn] + (isByte ? imm5 : (imm5 * 4U));
                if (isLoad) inst->regs[rt] = isByte ? static_cast<uint32_t>(inst->readU8(addr)) : inst->readU32(addr);
                else { if (isByte) inst->writeU8(addr, static_cast<uint8_t>(inst->regs[rt])); else inst->writeU32(addr, inst->regs[rt]); }
                inst->regs[15] = pc + 2;
                instructionsExecuted++;
                continue;
            }

            if ((tInsn & 0xF000U) == 0x8000U) {
                bool isLoad = (tInsn & (1U << 11)) != 0;
                uint32_t imm5 = (tInsn >> 6) & 0x1FU, rn = (tInsn >> 3) & 7U, rt = tInsn & 7U;
                uint32_t addr = inst->regs[rn] + (imm5 * 2U);
                if (isLoad) inst->regs[rt] = inst->readU16(addr);
                else inst->writeU16(addr, static_cast<uint16_t>(inst->regs[rt]));
                inst->regs[15] = pc + 2;
                instructionsExecuted++;
                continue;
            }

            if ((tInsn & 0xF000U) == 0x9000U) {
                bool isLoad = (tInsn & (1U << 11)) != 0;
                uint32_t rt = (tInsn >> 8) & 7U, addr = inst->regs[13] + ((tInsn & 0xFFU) * 4U);
                if (isLoad) inst->regs[rt] = inst->readU32(addr);
                else inst->writeU32(addr, inst->regs[rt]);
                inst->regs[15] = pc + 2;
                instructionsExecuted++;
                continue;
            }

            if ((tInsn & 0xF000U) == 0xA000U) {
                bool fromSp = (tInsn & (1U << 11)) != 0;
                inst->regs[(tInsn >> 8) & 7U] = (fromSp ? inst->regs[13] : ((pc + 4U) & ~3U)) + ((tInsn & 0xFFU) * 4U);
                inst->regs[15] = pc + 2;
                instructionsExecuted++;
                continue;
            }

            if ((tInsn & 0xF000U) == 0xB000U) {
                if ((tInsn & 0xFF00U) == 0xB000U) {
                    uint32_t imm = (tInsn & 0x7FU) * 4U;
                    if (tInsn & 0x80U) inst->regs[13] -= imm; else inst->regs[13] += imm;
                    inst->regs[15] = pc + 2;
                    instructionsExecuted++;
                    continue;
                }
                if ((tInsn & 0xF500U) == 0xB100U) {
                    bool nonZero = (tInsn & 0x0800U) != 0;
                    uint32_t rn = tInsn & 7U, imm = (((tInsn >> 9) & 1U) << 6) | (((tInsn >> 3) & 0x1FU) << 1);
                    bool take = nonZero ? (inst->regs[rn] != 0) : (inst->regs[rn] == 0);
                    inst->regs[15] = take ? (pc + 4U + imm) : (pc + 2U);
                    instructionsExecuted++;
                    continue;
                }
                if ((tInsn & 0xFF00U) == 0xB200U) {
                    uint32_t opExt = (tInsn >> 6) & 3U, v = inst->regs[(tInsn >> 3) & 7U], rd = tInsn & 7U;
                    if (opExt == 0) inst->regs[rd] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v & 0xFFFFU)));
                    else if (opExt == 1) inst->regs[rd] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(v & 0xFFU)));
                    else if (opExt == 2) inst->regs[rd] = v & 0xFFFFU;
                    else inst->regs[rd] = v & 0xFFU;
                    inst->regs[15] = pc + 2;
                    instructionsExecuted++;
                    continue;
                }
                if ((tInsn & 0xFE00U) == 0xB400U) {
                    bool hasLr = (tInsn & (1U << 8)) != 0;
                    uint8_t rlist = static_cast<uint8_t>(tInsn & 0xFFU);
                    uint32_t nRegs = hasLr ? 1U : 0U;
                    for (int i = 0; i < 8; ++i) if (rlist & (1U << i)) nRegs++;
                    inst->regs[13] -= nRegs * 4U;
                    uint32_t sp = inst->regs[13];
                    for (int i = 0; i < 8; ++i) if (rlist & (1U << i)) { inst->writeU32(sp, inst->regs[i]); sp += 4; }
                    if (hasLr) inst->writeU32(sp, inst->regs[14]);
                    inst->regs[15] = pc + 2;
                    instructionsExecuted++;
                    continue;
                }
                if ((tInsn & 0xFF00U) == 0xBA00U) {
                    uint32_t opRev = (tInsn >> 6) & 3U, v = inst->regs[(tInsn >> 3) & 7U], rd = tInsn & 7U;
                    if (opRev == 0) inst->regs[rd] = __builtin_bswap32(v);
                    else if (opRev == 1) inst->regs[rd] = ((v >> 8) & 0x00FF00FFU) | ((v & 0x00FF00FFU) << 8);
                    else if (opRev == 3) inst->regs[rd] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(((v & 0xFFFFU) >> 8) | ((v & 0xFFU) << 8))));
                    inst->regs[15] = pc + 2;
                    instructionsExecuted++;
                    continue;
                }
                if ((tInsn & 0xFE00U) == 0xBC00U) {
                    bool hasPc = (tInsn & (1U << 8)) != 0;
                    uint8_t rlist = static_cast<uint8_t>(tInsn & 0xFFU);
                    uint32_t sp = inst->regs[13];
                    for (int i = 0; i < 8; ++i) if (rlist & (1U << i)) { inst->regs[i] = inst->readU32(sp); sp += 4; }
                    uint32_t newPc = 0;
                    if (hasPc) { newPc = inst->readU32(sp); sp += 4; }
                    inst->regs[13] = sp;
                    if (hasPc) { inst->branchWritePC(newPc); instructionsExecuted++; continue; }
                    inst->regs[15] = pc + 2;
                    instructionsExecuted++;
                    continue;
                }
                if ((tInsn & 0xFF00U) == 0xBF00U) {
                    if ((tInsn & 0x0FU) != 0) inst->itstate = static_cast<uint8_t>(tInsn & 0xFFU);
                    inst->regs[15] = pc + 2;
                    instructionsExecuted++;
                    continue;
                }
                inst->regs[15] = pc + 2;
                instructionsExecuted++;
                continue;
            }

            if ((tInsn & 0xF000U) == 0xC000U) {
                bool isLoad = (tInsn & (1U << 11)) != 0;
                uint32_t rn = (tInsn >> 8) & 7U, addr = inst->regs[rn];
                uint8_t rlist = static_cast<uint8_t>(tInsn & 0xFFU);
                for (int i = 0; i < 8; ++i) {
                    if (rlist & (1U << i)) {
                        if (isLoad) inst->regs[i] = inst->readU32(addr);
                        else inst->writeU32(addr, inst->regs[i]);
                        addr += 4;
                    }
                }
                if (!(isLoad && (rlist & (1U << rn)))) inst->regs[rn] = addr;
                inst->regs[15] = pc + 2;
                instructionsExecuted++;
                continue;
            }

            if ((tInsn & 0xF000U) == 0xD000U) {
                uint32_t bCond = (tInsn >> 8) & 0xFU;
                if (bCond == 0xFU) {
                    inst->regs[15] = pc + 2;
                    dispatchIntr(tInsn & 0xFFU);
                    instructionsExecuted++;
                    continue;
                }
                if (bCond < 14 && checkCondition(inst->cpsr, bCond)) {
                    int32_t imm8 = static_cast<int32_t>(static_cast<int8_t>(tInsn & 0xFFU)) * 2;
                    inst->regs[15] = (pc + 4U + imm8) & ~1U;
                    inst->itstate = 0;
                } else {
                    inst->regs[15] = pc + 2;
                }
                instructionsExecuted++;
                continue;
            }

            if ((tInsn & 0xF800U) == 0xE000U) {
                int32_t imm11 = static_cast<int32_t>((tInsn & 0x7FFU) << 21) >> 20;
                inst->regs[15] = (pc + 4U + imm11) & ~1U;
                inst->itstate = 0;
                instructionsExecuted++;
                continue;
            }

            inst->regs[15] = pc + 2;
            instructionsExecuted++;
        }
    }

    inst->is_running.store(wasRunning);
    if (prevStop) inst->stop_requested.store(true);
    return UC_ERR_OK;
}

} // namespace

extern "C" {

uc_err uc_emu_start(uc_engine *uc, uint64_t begin, uint64_t until, uint64_t /* timeout */, size_t count) {
    if (!uc) return UC_ERR_HANDLE;
    auto* inst = reinterpret_cast<UnicornInstance*>(uc);

#if defined(EMUX_ENABLE_DYNARMIC)
    if (inst->dynarmic_jit) {
        return inst->runDynarmic(begin, until, count);
    }
#endif

    return runInterpreter(inst, begin, until, count);
}

uc_err uc_emu_stop(uc_engine *uc) {
    if (!uc) return UC_ERR_HANDLE;
    auto* inst = reinterpret_cast<UnicornInstance*>(uc);
    inst->stop_requested.store(true);
    inst->is_running.store(false);
#if defined(EMUX_ENABLE_DYNARMIC)
    if (inst->dynarmic_jit) {
        inst->dynarmic_jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
    }
#endif
    return UC_ERR_OK;
}

} // extern "C"
