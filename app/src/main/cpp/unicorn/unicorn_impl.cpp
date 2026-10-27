#include "unicorn/unicorn.h"
#include "unicorn/arm.h"

#include <android/log.h>
#include <sys/mman.h>
#include <cstring>
#include <vector>
#include <memory>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <new>

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

    switch (cond) {
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
        default: return false;
    }
}

struct UnicornInstance {
    uc_arch arch{UC_ARCH_ARM};
    uc_mode mode{UC_MODE_ARM};
    std::atomic<bool> is_running{false};
    std::atomic<bool> stop_requested{false};

    // Registri ARM32 standard: r0..r12, r13(SP), r14(LR), r15(PC)
    uint32_t regs[16]{0};
    uint32_t cpsr{0x00000010};
    uint32_t fpscr{0};

    // Registri VFP (32 registri singola precisione s0..s31, e 16 registri doppia precisione estesi d16..d31)
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

    inline double getVfpD(uint32_t regIdx) const {
        if (regIdx < 16) {
            uint64_t raw = (static_cast<uint64_t>(vfp_s[regIdx * 2 + 1]) << 32) | vfp_s[regIdx * 2];
            double val;
            std::memcpy(&val, &raw, 8);
            return val;
        } else if (regIdx < 32) {
            double val;
            std::memcpy(&val, &vfp_d_ext[regIdx - 16], 8);
            return val;
        }
        return 0.0;
    }

    inline void setVfpD(uint32_t regIdx, double val) {
        uint64_t raw;
        std::memcpy(&raw, &val, 8);
        if (regIdx < 16) {
            vfp_s[regIdx * 2] = static_cast<uint32_t>(raw & 0xFFFFFFFFU);
            vfp_s[regIdx * 2 + 1] = static_cast<uint32_t>(raw >> 32);
        } else if (regIdx < 32) {
            vfp_d_ext[regIdx - 16] = raw;
        }
    }

    // Mappe memoria virtuale guest
    std::vector<MemoryRegion> regions;
    std::mutex mem_mutex;

    // Hook registrati
    std::vector<HookEntry> hooks;
    uc_hook next_hook_id{1};
    std::mutex hook_mutex;

    UnicornInstance(uc_arch a, uc_mode m) : arch(a), mode(m) {
        regs[13] = 0x7E1FF000; // SP default
        cpsr = 0x00000010;     // ARM User Mode (32-bit)
    }

    ~UnicornInstance() {
        std::lock_guard<std::mutex> lock(mem_mutex);
        for (auto& reg : regions) {
            if (reg.owned && reg.host_ptr) {
                munmap(reg.host_ptr, reg.size);
                reg.host_ptr = nullptr;
            }
        }
        regions.clear();
    }

    inline MemoryRegion* findRegion(uint64_t addr) {
        for (auto& r : regions) {
            if (addr >= r.address && addr < (r.address + r.size)) {
                return &r;
            }
        }
        return nullptr;
    }

    inline uc_err readMem(uint64_t addr, void* dst, size_t size) {
        if (!dst || size == 0) return UC_ERR_OK;
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

    inline uint32_t readU32(uint32_t addr) {
        uint32_t val = 0;
        readMem(addr, &val, 4);
        return val;
    }

    inline void writeU32(uint32_t addr, uint32_t val) {
        writeMem(addr, &val, 4);
    }
};

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
    if (regid == UC_ARM_REG_CPSR) {
        *reinterpret_cast<uint32_t*>(value) = inst->cpsr;
        return UC_ERR_OK;
    }
    if (regid >= UC_ARM_REG_D0 && regid <= UC_ARM_REG_D31) {
        uint32_t dIdx = regid - UC_ARM_REG_D0;
        double dVal = inst->getVfpD(dIdx);
        std::memcpy(value, &dVal, 8);
        return UC_ERR_OK;
    }
    if (regid == UC_ARM_REG_FPSCR || regid == UC_ARM_REG_FPSCR_NZCV) {
        *reinterpret_cast<uint32_t*>(value) = inst->fpscr;
        return UC_ERR_OK;
    }

    return UC_ERR_ARG;
}

uc_err uc_reg_write(uc_engine *uc, int regid, const void *value) {
    if (!uc || !value) return UC_ERR_HANDLE;
    auto* inst = reinterpret_cast<UnicornInstance*>(uc);
    uint32_t val = *reinterpret_cast<const uint32_t*>(value);

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
    if (regid == UC_ARM_REG_CPSR) {
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
    if (regid == UC_ARM_REG_FPSCR || regid == UC_ARM_REG_FPSCR_NZCV) {
        inst->fpscr = *reinterpret_cast<const uint32_t*>(value);
        return UC_ERR_OK;
    }

    return UC_ERR_ARG;
}

uc_err uc_mem_map(uc_engine *uc, uint64_t address, size_t size, uint32_t perms) {
    if (!uc) return UC_ERR_HANDLE;
    if (size == 0) return UC_ERR_ARG;

    // Rigorosa verifica allineamento pagina 0x1000 byte richiesta da Unicorn Engine
    if ((address & (UNICORN_PAGE_SIZE - 1)) != 0 || (size & (UNICORN_PAGE_SIZE - 1)) != 0) {
        LOGE("uc_mem_map: Errore fatale UC_ERR_ARG: Indirizzo 0x%llx o dimensione 0x%zx non allineati alla pagina 0x1000!",
             (unsigned long long)address, size);
        return UC_ERR_ARG;
    }

    auto* inst = reinterpret_cast<UnicornInstance*>(uc);
    std::lock_guard<std::mutex> lock(inst->mem_mutex);

    // Se l'intervallo richiesto è già interamente compreso in una regione esistente, aggiorna i permessi e ritorna UC_ERR_OK
    for (auto& r : inst->regions) {
        if (address >= r.address && (address + size) <= (r.address + r.size)) {
            r.perms |= perms;
            LOGI("uc_mem_map: Range 0x%llx (size 0x%zx) già coperto da regione a 0x%llx (size 0x%zx)",
                 (unsigned long long)address, size, (unsigned long long)r.address, r.size);
            return UC_ERR_OK;
        }
    }

    // Verifica sovrapposizioni parziali
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

    std::memset(host, 0, size);

    MemoryRegion reg{};
    reg.address = address;
    reg.size = size;
    reg.perms = perms;
    reg.host_ptr = static_cast<uint8_t*>(host);
    reg.owned = true;
    inst->regions.push_back(reg);

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

    for (auto it = inst->regions.begin(); it != inst->regions.end(); ++it) {
        if (it->address == address && it->size == size) {
            if (it->owned && it->host_ptr) {
                munmap(it->host_ptr, it->size);
            }
            inst->regions.erase(it);
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

uc_err uc_emu_start(uc_engine *uc, uint64_t begin, uint64_t until, uint64_t timeout, size_t count) {
    if (!uc) return UC_ERR_HANDLE;
    auto* inst = reinterpret_cast<UnicornInstance*>(uc);

    // Gestione modalità iniziale ARM vs Thumb in base al bit 0 dell'entry point
    if (begin & 1) {
        inst->cpsr |= FLAG_T;
        inst->regs[15] = static_cast<uint32_t>(begin & ~1ULL);
    } else {
        inst->cpsr &= ~FLAG_T;
        inst->regs[15] = static_cast<uint32_t>(begin & ~3ULL);
    }

    inst->is_running.store(true);
    inst->stop_requested.store(false);

    LOGI("uc_emu_start: Avvio emulazione guest da PC=0x%08x (Mode: %s, SP=0x%08x, LR=0x%08x, r0=0x%08x)...",
         inst->regs[15], (inst->cpsr & FLAG_T) ? "Thumb" : "ARM32", inst->regs[13], inst->regs[14], inst->regs[0]);

    size_t instructionsExecuted = 0;

    while (inst->is_running.load() && !inst->stop_requested.load()) {
        uint32_t pc = inst->regs[15];

        if (until != 0 && pc == until) {
            LOGI("uc_emu_start: Raggiunto target address until=0x%llx", (unsigned long long)until);
            break;
        }

        if (pc == 0 || pc == 0xDEADBEEF) {
            LOGI("uc_emu_start: Raggiunto return/trap address 0x%08x (Return Value in r0: 0x%08x)", pc, inst->regs[0]);
            break;
        }

        if (count > 0 && instructionsExecuted >= count) {
            break;
        }

        bool isThumb = (inst->cpsr & FLAG_T) != 0;

        if (!isThumb) {
            // ==================== DECODIFICA & ESECUZIONE ARM (32-bit) ====================
            uint32_t insn = 0;
            uc_err readErr = inst->readMem(pc, &insn, 4);
            if (readErr != UC_ERR_OK) {
                LOGE("uc_emu_start [ARM]: Fault lettura istruzione al PC=0x%08x (%s)", pc, uc_strerror(readErr));
                inst->is_running.store(false);
                return readErr;
            }

            uint32_t cond = (insn >> 28) & 0xF;
            if (cond != 15 && !checkCondition(inst->cpsr, cond)) {
                // Condizione non verificata: avanza al prossimo comando
                inst->regs[15] += 4;
                instructionsExecuted++;
                continue;
            }

            // 1. Supervisor Call (SVC): 0xEF000000 o condizionale
            if ((insn & 0x0F000000) == 0x0F000000) {
                uint32_t svcComment = insn & 0x00FFFFFF;

                std::vector<HookEntry> intrHooks;
                {
                    std::lock_guard<std::mutex> lock(inst->hook_mutex);
                    for (const auto& h : inst->hooks) {
                        if (h.type & UC_HOOK_INTR) intrHooks.push_back(h);
                    }
                }

                for (const auto& h : intrHooks) {
                    auto cb = reinterpret_cast<uc_cb_hookintr_t>(h.callback);
                    uint32_t intnoToSend = (svcComment != 0) ? (0x80000000U | svcComment) : 2U;
                    cb(uc, intnoToSend, h.user_data);
                }

                inst->regs[15] += 4;
                instructionsExecuted++;
                continue;
            }

            // 2. Branch & Exchange: BX Rm (0x012FFF10) / BLX Rm (0x012FFF30)
            if ((insn & 0x0FFFFFF0) == 0x012FFF10) { // BX Rm
                uint32_t rm = insn & 0xF;
                uint32_t target = inst->regs[rm];
                if (target == 0xDEADBEEF || target == 0) {
                    LOGI("uc_emu_start [ARM]: Raggiunto return/trap address 0x%08x (Return Value in r0: 0x%08x)",
                         target, inst->regs[0]);
                    instructionsExecuted++;
                    break;
                }
                if (target & 1) {
                    inst->cpsr |= FLAG_T;
                    inst->regs[15] = target & ~1U;
                } else {
                    inst->cpsr &= ~FLAG_T;
                    inst->regs[15] = target & ~3U;
                }
                instructionsExecuted++;
                continue;
            }

            if ((insn & 0x0FFFFFF0) == 0x012FFF30) { // BLX Rm
                uint32_t rm = insn & 0xF;
                uint32_t target = inst->regs[rm];
                inst->regs[14] = pc + 4; // Imposta LR di ritorno
                if (target & 1) {
                    inst->cpsr |= FLAG_T;
                    inst->regs[15] = target & ~1U;
                } else {
                    inst->cpsr &= ~FLAG_T;
                    inst->regs[15] = target & ~3U;
                }
                instructionsExecuted++;
                continue;
            }

            // 3. Branch / Branch with Link: B / BL (0x0A000000 / 0x0B000000)
            if ((insn & 0x0E000000) == 0x0A000000) {
                bool isBl = (insn & 0x01000000) != 0;
                if (isBl) {
                    inst->regs[14] = pc + 4;
                }
                int32_t imm24 = static_cast<int32_t>((insn & 0x00FFFFFF) << 8) >> 6;
                inst->regs[15] = pc + 8 + imm24;
                instructionsExecuted++;
                continue;
            }

            // 4. Block Data Transfer: PUSH / POP / LDM / STM (0x08000000)
            if ((insn & 0x0E000000) == 0x08000000) {
                uint32_t rn = (insn >> 16) & 0xF;
                bool isLoad = (insn & (1 << 20)) != 0;
                bool writeBack = (insn & (1 << 21)) != 0;
                bool preIndex = (insn & (1 << 24)) != 0;
                bool up = (insn & (1 << 23)) != 0;
                uint16_t regList = insn & 0xFFFF;

                uint32_t addr = inst->regs[rn];
                std::vector<int> regsToProcess;
                for (int i = 0; i < 16; ++i) {
                    if (regList & (1 << i)) regsToProcess.push_back(i);
                }

                if (!up) { // Decrement (es. PUSH = STMDB SP!)
                    addr -= static_cast<uint32_t>(regsToProcess.size() * 4);
                }

                uint32_t curAddr = addr;
                for (int regIdx : regsToProcess) {
                    if (isLoad) {
                        inst->regs[regIdx] = inst->readU32(curAddr);
                    } else {
                        inst->writeU32(curAddr, inst->regs[regIdx]);
                    }
                    curAddr += 4;
                }

                if (writeBack) {
                    if (up) inst->regs[rn] += static_cast<uint32_t>(regsToProcess.size() * 4);
                    else inst->regs[rn] -= static_cast<uint32_t>(regsToProcess.size() * 4);
                }

                if (isLoad && (regList & (1 << 15))) {
                    // PC caricato dallo stack (es. POP {..., PC})
                    instructionsExecuted++;
                    continue;
                }

                inst->regs[15] += 4;
                instructionsExecuted++;
                continue;
            }

            // 5. Single Data Transfer: LDR / STR (0x04000000)
            if ((insn & 0x0C000000) == 0x04000000) {
                uint32_t rn = (insn >> 16) & 0xF;
                uint32_t rd = (insn >> 12) & 0xF;
                bool isLoad = (insn & (1 << 20)) != 0;
                bool isByte = (insn & (1 << 22)) != 0;
                bool up = (insn & (1 << 23)) != 0;
                uint32_t offset = insn & 0xFFF;

                uint32_t base = (rn == 15) ? (pc + 8) : inst->regs[rn];
                uint32_t effAddr = up ? (base + offset) : (base - offset);

                if (isLoad) {
                    if (isByte) {
                        uint8_t byteVal = 0;
                        inst->readMem(effAddr, &byteVal, 1);
                        inst->regs[rd] = byteVal;
                    } else {
                        inst->regs[rd] = inst->readU32(effAddr);
                    }
                    if (rd == 15) {
                        instructionsExecuted++;
                        continue;
                    }
                } else {
                    if (isByte) {
                        uint8_t byteVal = static_cast<uint8_t>(inst->regs[rd] & 0xFF);
                        inst->writeMem(effAddr, &byteVal, 1);
                    } else {
                        inst->writeU32(effAddr, inst->regs[rd]);
                    }
                }

                inst->regs[15] += 4;
                instructionsExecuted++;
                continue;
            }

            // 6. Data Processing (MOV, ADD, SUB, CMP, ecc.)
            if ((insn & 0x0C000000) == 0x00000000) {
                uint32_t opcode = (insn >> 21) & 0xF;
                uint32_t rn = (insn >> 16) & 0xF;
                uint32_t rd = (insn >> 12) & 0xF;
                bool setFlags = (insn & (1 << 20)) != 0;
                bool isImm = (insn & (1 << 25)) != 0;

                uint32_t op2 = 0;
                if (isImm) {
                    uint32_t imm8 = insn & 0xFF;
                    uint32_t rot = ((insn >> 8) & 0xF) * 2;
                    op2 = (rot == 0) ? imm8 : ((imm8 >> rot) | (imm8 << (32 - rot)));
                } else {
                    op2 = inst->regs[insn & 0xF];
                }

                uint32_t val1 = (rn == 15) ? (pc + 8) : inst->regs[rn];
                uint32_t res = 0;

                switch (opcode) {
                    case 0x0: res = val1 & op2; break;         // AND
                    case 0x1: res = val1 ^ op2; break;         // EOR
                    case 0x2: res = val1 - op2; break;         // SUB
                    case 0x4: res = val1 + op2; break;         // ADD
                    case 0xA: res = val1 - op2; setFlags = true; break; // CMP
                    case 0xB: res = val1 + op2; setFlags = true; break; // CMN
                    case 0xC: res = val1 | op2; break;         // ORR
                    case 0xD: res = op2; break;                // MOV
                    case 0xE: res = val1 & ~op2; break;        // BIC
                    case 0xF: res = ~op2; break;               // MVN
                    default: res = op2; break;
                }

                if (opcode != 0xA && opcode != 0xB) {
                    inst->regs[rd] = res;
                }

                if (setFlags) {
                    if (res == 0) inst->cpsr |= FLAG_Z; else inst->cpsr &= ~FLAG_Z;
                    if (res & 0x80000000U) inst->cpsr |= FLAG_N; else inst->cpsr &= ~FLAG_N;
                }

                if (rd == 15 && opcode != 0xA && opcode != 0xB) {
                    instructionsExecuted++;
                    continue;
                }

                inst->regs[15] += 4;
                instructionsExecuted++;
                continue;
            }

            // 7. VFP / NEON Instructions (CP10 e CP11)
            // 7a. VMRS / VMSR
            if ((insn & 0x0FFF0FFF) == 0x0EF10A10) {
                // VMRS APSR_nzcv, FPSCR (0xEEF1FA10)
                inst->cpsr = (inst->cpsr & ~0xF0000000U) | (inst->fpscr & 0xF0000000U);
                inst->regs[15] += 4;
                instructionsExecuted++;
                continue;
            }
            if ((insn & 0x0FF00FFF) == 0x0EF00A10) { // VMRS Rt, FPSCR
                uint32_t rt = (insn >> 12) & 0xF;
                if (rt != 15) inst->regs[rt] = inst->fpscr;
                inst->regs[15] += 4;
                instructionsExecuted++;
                continue;
            }
            if ((insn & 0x0FF00FFF) == 0x0EE00A10) { // VMSR FPSCR, Rt
                uint32_t rt = (insn >> 12) & 0xF;
                inst->fpscr = inst->regs[rt];
                inst->regs[15] += 4;
                instructionsExecuted++;
                continue;
            }

            // 7b. VMOV core <-> single
            if ((insn & 0x0FE00FD0) == 0x0E000A10) {
                bool isToCore = (insn & (1 << 20)) != 0;
                uint32_t vn = (insn >> 16) & 0xF;
                uint32_t rt = (insn >> 12) & 0xF;
                uint32_t n = (insn >> 7) & 1;
                uint32_t sn = (vn << 1) | n;
                if (isToCore) {
                    inst->regs[rt] = inst->vfp_s[sn];
                } else {
                    inst->vfp_s[sn] = inst->regs[rt];
                }
                inst->regs[15] += 4;
                instructionsExecuted++;
                continue;
            }

            // 7c. VMOV core pair <-> double / two singles
            if ((insn & 0x0FE00FD0) == 0x0C400A10 || (insn & 0x0FE00FD0) == 0x0C400B10) {
                bool isToCore = (insn & (1 << 20)) != 0;
                uint32_t rt = (insn >> 12) & 0xF;
                uint32_t rt2 = (insn >> 16) & 0xF;
                uint32_t vm = insn & 0xF;
                uint32_t sBase = vm * 2;
                if (isToCore) {
                    inst->regs[rt] = inst->vfp_s[sBase];
                    inst->regs[rt2] = inst->vfp_s[sBase + 1];
                } else {
                    inst->vfp_s[sBase] = inst->regs[rt];
                    inst->vfp_s[sBase + 1] = inst->regs[rt2];
                }
                inst->regs[15] += 4;
                instructionsExecuted++;
                continue;
            }

            // 7d. VLDR / VSTR (Single / Double) & VPUSH / VPOP
            if (((insn & 0x0E200F00) == 0x0D000A00) || ((insn & 0x0E200F00) == 0x0D000B00)) {
                bool isDouble = ((insn >> 8) & 1) != 0;
                bool isLoad = (insn & (1 << 20)) != 0;
                bool writeBack = (insn & (1 << 21)) != 0;
                bool up = (insn & (1 << 23)) != 0;
                uint32_t rn = (insn >> 16) & 0xF;
                uint32_t vd = (insn >> 12) & 0xF;
                uint32_t d = (insn >> 22) & 1;
                uint32_t imm8 = insn & 0xFF;

                uint32_t base = (rn == 15) ? (pc + 8) : inst->regs[rn];

                if (writeBack) {
                    // VPUSH / VPOP (VSTM / VLDM)
                    uint32_t count = imm8; // numero di registri 32-bit
                    if (isDouble) count /= 2;
                    uint32_t bytesTotal = count * (isDouble ? 8 : 4);
                    uint32_t curAddr = up ? base : (base - bytesTotal);

                    for (uint32_t idx = 0; idx < count; ++idx) {
                        if (!isDouble) {
                            uint32_t regIdx = ((vd << 1) | d) + idx;
                            if (regIdx < 32) {
                                if (isLoad) inst->vfp_s[regIdx] = inst->readU32(curAddr);
                                else inst->writeU32(curAddr, inst->vfp_s[regIdx]);
                            }
                            curAddr += 4;
                        } else {
                            uint32_t regIdx = ((d << 4) | vd) + idx;
                            if (regIdx < 32) {
                                if (isLoad) {
                                    uint32_t lo = inst->readU32(curAddr);
                                    uint32_t hi = inst->readU32(curAddr + 4);
                                    uint64_t raw = (static_cast<uint64_t>(hi) << 32) | lo;
                                    inst->setVfpD(regIdx, *reinterpret_cast<double*>(&raw));
                                } else {
                                    double dval = inst->getVfpD(regIdx);
                                    uint64_t raw = *reinterpret_cast<uint64_t*>(&dval);
                                    inst->writeU32(curAddr, static_cast<uint32_t>(raw & 0xFFFFFFFFU));
                                    inst->writeU32(curAddr + 4, static_cast<uint32_t>(raw >> 32));
                                }
                            }
                            curAddr += 8;
                        }
                    }

                    if (up) inst->regs[rn] += bytesTotal;
                    else inst->regs[rn] -= bytesTotal;
                } else {
                    // VLDR / VSTR singolo
                    uint32_t offset = imm8 * 4;
                    uint32_t effAddr = up ? (base + offset) : (base - offset);

                    if (!isDouble) {
                        uint32_t regIdx = (vd << 1) | d;
                        if (regIdx < 32) {
                            if (isLoad) inst->vfp_s[regIdx] = inst->readU32(effAddr);
                            else inst->writeU32(effAddr, inst->vfp_s[regIdx]);
                        }
                    } else {
                        uint32_t regIdx = (d << 4) | vd;
                        if (regIdx < 32) {
                            if (isLoad) {
                                uint32_t lo = inst->readU32(effAddr);
                                uint32_t hi = inst->readU32(effAddr + 4);
                                uint64_t raw = (static_cast<uint64_t>(hi) << 32) | lo;
                                inst->setVfpD(regIdx, *reinterpret_cast<double*>(&raw));
                            } else {
                                double dval = inst->getVfpD(regIdx);
                                uint64_t raw = *reinterpret_cast<uint64_t*>(&dval);
                                inst->writeU32(effAddr, static_cast<uint32_t>(raw & 0xFFFFFFFFU));
                                inst->writeU32(effAddr + 4, static_cast<uint32_t>(raw >> 32));
                            }
                        }
                    }
                }

                inst->regs[15] += 4;
                instructionsExecuted++;
                continue;
            }

            // 7e. VADD.F32, VSUB.F32, VMUL.F32, VDIV.F32
            if ((insn & 0x0E000E00) == 0x0E000A00) {
                uint32_t d = (insn >> 22) & 1;
                uint32_t n = (insn >> 7) & 1;
                uint32_t m = (insn >> 5) & 1;
                uint32_t sd = (((insn >> 12) & 0xF) << 1) | d;
                uint32_t sn = (((insn >> 16) & 0xF) << 1) | n;
                uint32_t sm = ((insn & 0xF) << 1) | m;

                float a = inst->getVfpS(sn);
                float b = inst->getVfpS(sm);
                float res = 0.0f;

                if ((insn & 0x0F300F50) == 0x0E300A00) res = a + b;           // VADD.F32
                else if ((insn & 0x0F300F50) == 0x0E300A40) res = a - b;      // VSUB.F32
                else if ((insn & 0x0F300F50) == 0x0E200A00) res = a * b;      // VMUL.F32
                else if ((insn & 0x0F300F50) == 0x0E800A00) res = (b != 0.0f) ? (a / b) : 0.0f; // VDIV.F32
                else res = a;

                inst->setVfpS(sd, res);
                inst->regs[15] += 4;
                instructionsExecuted++;
                continue;
            }

            // 7f. VCMP.F32
            if ((insn & 0x0FF00FD0) == 0x0EB40A40 || (insn & 0x0FF00FD0) == 0x0EB50A40) {
                bool withZero = (insn & (1 << 16)) != 0;
                uint32_t d = (insn >> 22) & 1;
                uint32_t m = (insn >> 5) & 1;
                uint32_t sd = (((insn >> 12) & 0xF) << 1) | d;
                uint32_t sm = ((insn & 0xF) << 1) | m;

                float fa = inst->getVfpS(sd);
                float fb = withZero ? 0.0f : inst->getVfpS(sm);

                inst->fpscr &= ~0xF0000000U;
                if (fa == fb) inst->fpscr |= (FLAG_Z | FLAG_C);
                else if (fa < fb) inst->fpscr |= FLAG_N;
                else inst->fpscr |= FLAG_C;

                inst->regs[15] += 4;
                instructionsExecuted++;
                continue;
            }

            // 8. MUL / MLA
            if ((insn & 0x0FC000F0) == 0x00000090) { // MUL Rd, Rm, Rs
                uint32_t rd = (insn >> 16) & 0xF;
                uint32_t rs = (insn >> 8) & 0xF;
                uint32_t rm = insn & 0xF;
                inst->regs[rd] = inst->regs[rm] * inst->regs[rs];
                inst->regs[15] += 4;
                instructionsExecuted++;
                continue;
            }
            if ((insn & 0x0FC000F0) == 0x00200090) { // MLA Rd, Rm, Rs, Rn
                uint32_t rd = (insn >> 16) & 0xF;
                uint32_t rn = (insn >> 12) & 0xF;
                uint32_t rs = (insn >> 8) & 0xF;
                uint32_t rm = insn & 0xF;
                inst->regs[rd] = (inst->regs[rm] * inst->regs[rs]) + inst->regs[rn];
                inst->regs[15] += 4;
                instructionsExecuted++;
                continue;
            }

            // Fallback avanzamento PC ARM standard
            inst->regs[15] += 4;
            instructionsExecuted++;
        } else {
            // ==================== DECODIFICA & ESECUZIONE THUMB ====================
            uint16_t tInsn = 0;
            uc_err tReadErr = inst->readMem(pc, &tInsn, 2);
            if (tReadErr != UC_ERR_OK) {
                LOGE("uc_emu_start [Thumb]: Fault lettura istruzione al PC=0x%08x", pc);
                inst->is_running.store(false);
                return tReadErr;
            }

            // Verifica istruzione Thumb a 32-bit (Thumb-2)
            bool isThumb32 = ((tInsn & 0xF800) == 0xE800) || ((tInsn & 0xF000) == 0xF000);
            if (isThumb32) {
                uint16_t tInsn2 = 0;
                inst->readMem(pc + 2, &tInsn2, 2);

                // Thumb-2 BL / BLX
                if ((tInsn & 0xF800) == 0xF000 && (tInsn2 & 0xD000) == 0xD000) {
                    uint32_t s = (tInsn >> 10) & 1;
                    uint32_t imm10 = tInsn & 0x3FF;
                    uint32_t j1 = (tInsn2 >> 13) & 1;
                    uint32_t j2 = (tInsn2 >> 11) & 1;
                    uint32_t imm11 = tInsn2 & 0x7FF;

                    uint32_t i1 = !(j1 ^ s);
                    uint32_t i2 = !(j2 ^ s);
                    int32_t offset = (s << 24) | (i1 << 23) | (i2 << 22) | (imm10 << 12) | (imm11 << 1);
                    if (s) offset |= 0xFE000000;

                    bool isBlx = (tInsn2 & (1 << 12)) == 0;
                    inst->regs[14] = (pc + 4) | 1U;

                    uint32_t target = pc + 4 + offset;
                    if (isBlx) {
                        inst->cpsr &= ~FLAG_T;
                        inst->regs[15] = target & ~3U;
                    } else {
                        inst->cpsr |= FLAG_T;
                        inst->regs[15] = target & ~1U;
                    }
                    instructionsExecuted++;
                    continue;
                }

                // Per qualsiasi altra istruzione Thumb-2 (inclusi VFP a 32-bit), avanziamo di 4 byte
                inst->regs[15] += 4;
                instructionsExecuted++;
                continue;
            }

            // 1. Thumb PUSH (0xB400 / 0xB500) & POP (0xBC00 / 0xBD00)
            if ((tInsn & 0xFE00) == 0xB400) { // PUSH {rlist, lr}
                bool hasLr = (tInsn & (1 << 8)) != 0;
                uint8_t rlist = tInsn & 0xFF;
                std::vector<int> regsToPush;
                for (int i = 0; i < 8; ++i) if (rlist & (1 << i)) regsToPush.push_back(i);
                if (hasLr) regsToPush.push_back(14);

                inst->regs[13] -= static_cast<uint32_t>(regsToPush.size() * 4);
                uint32_t sp = inst->regs[13];
                for (int regIdx : regsToPush) {
                    inst->writeU32(sp, inst->regs[regIdx]);
                    sp += 4;
                }
                inst->regs[15] += 2;
                instructionsExecuted++;
                continue;
            }

            if ((tInsn & 0xFE00) == 0xBC00) { // POP {rlist, pc}
                bool hasPc = (tInsn & (1 << 8)) != 0;
                uint8_t rlist = tInsn & 0xFF;
                std::vector<int> regsToPop;
                for (int i = 0; i < 8; ++i) if (rlist & (1 << i)) regsToPop.push_back(i);
                if (hasPc) regsToPop.push_back(15);

                uint32_t sp = inst->regs[13];
                for (int regIdx : regsToPop) {
                    inst->regs[regIdx] = inst->readU32(sp);
                    sp += 4;
                }
                inst->regs[13] = sp;

                if (hasPc) {
                    uint32_t newPc = inst->regs[15];
                    if (newPc == 0xDEADBEEF || newPc == 0) {
                        LOGI("uc_emu_start [Thumb]: Raggiunto return/trap address 0x%08x (Return Value in r0: 0x%08x)",
                             newPc, inst->regs[0]);
                        instructionsExecuted++;
                        break;
                    }
                    if (newPc & 1) {
                        inst->cpsr |= FLAG_T;
                        inst->regs[15] = newPc & ~1U;
                    } else {
                        inst->cpsr &= ~FLAG_T;
                        inst->regs[15] = newPc & ~3U;
                    }
                    instructionsExecuted++;
                    continue;
                }

                inst->regs[15] += 2;
                instructionsExecuted++;
                continue;
            }

            // 2. Thumb BX Rm (0x4700) / BLX Rm (0x4780)
            if ((tInsn & 0xFF87) == 0x4700) { // BX Rm
                uint32_t rm = (tInsn >> 3) & 0xF;
                uint32_t target = inst->regs[rm];
                if (target == 0xDEADBEEF || target == 0) {
                    LOGI("uc_emu_start [Thumb BX]: Raggiunto return/trap address 0x%08x (Return Value in r0: 0x%08x)",
                         target, inst->regs[0]);
                    instructionsExecuted++;
                    break;
                }
                if (target & 1) {
                    inst->cpsr |= FLAG_T;
                    inst->regs[15] = target & ~1U;
                } else {
                    inst->cpsr &= ~FLAG_T;
                    inst->regs[15] = target & ~3U;
                }
                instructionsExecuted++;
                continue;
            }

            if ((tInsn & 0xFF87) == 0x4780) { // BLX Rm
                uint32_t rm = (tInsn >> 3) & 0xF;
                uint32_t target = inst->regs[rm];
                inst->regs[14] = (pc + 2) | 1U;
                if (target & 1) {
                    inst->cpsr |= FLAG_T;
                    inst->regs[15] = target & ~1U;
                } else {
                    inst->cpsr &= ~FLAG_T;
                    inst->regs[15] = target & ~3U;
                }
                instructionsExecuted++;
                continue;
            }

            // 3. Thumb Branch incondizionato B (0xE000)
            if ((tInsn & 0xF800) == 0xE000) {
                int32_t imm11 = static_cast<int32_t>((tInsn & 0x7FF) << 21) >> 20;
                inst->regs[15] = pc + 4 + imm11;
                instructionsExecuted++;
                continue;
            }

            // 4. Thumb Supervisor Call: SVC #imm (0xDF00)
            if ((tInsn & 0xFF00) == 0xDF00) {
                uint32_t svcComment = tInsn & 0xFF;
                std::vector<HookEntry> intrHooks;
                {
                    std::lock_guard<std::mutex> lock(inst->hook_mutex);
                    for (const auto& h : inst->hooks) {
                        if (h.type & UC_HOOK_INTR) intrHooks.push_back(h);
                    }
                }
                for (const auto& h : intrHooks) {
                    auto cb = reinterpret_cast<uc_cb_hookintr_t>(h.callback);
                    uint32_t intnoToSend = (svcComment != 0) ? (0x80000000U | svcComment) : 2U;
                    cb(uc, intnoToSend, h.user_data);
                }
                inst->regs[15] += 2;
                instructionsExecuted++;
                continue;
            }

            // Fallback avanzamento PC Thumb standard (2 byte)
            inst->regs[15] += 2;
            instructionsExecuted++;
        }
    }

    inst->is_running.store(false);
    LOGI("uc_emu_start: Emulazione completata al PC=0x%08x (Istruzioni eseguite: %zu, r0=0x%08x)",
         inst->regs[15], instructionsExecuted, inst->regs[0]);
    return UC_ERR_OK;
}

uc_err uc_emu_stop(uc_engine *uc) {
    if (!uc) return UC_ERR_HANDLE;
    auto* inst = reinterpret_cast<UnicornInstance*>(uc);
    inst->stop_requested.store(true);
    inst->is_running.store(false);
    LOGI("uc_emu_stop: Richiesta terminazione emulazione inviata");
    return UC_ERR_OK;
}

} // extern "C"
