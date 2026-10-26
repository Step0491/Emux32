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

struct UnicornInstance {
    uc_arch arch{UC_ARCH_ARM};
    uc_mode mode{UC_MODE_ARM};
    std::atomic<bool> is_running{false};
    std::atomic<bool> stop_requested{false};

    // Registri ARM32 standard
    uint32_t regs[16]{0};
    uint32_t cpsr{0x00000010};
    uint32_t fpscr{0};

    // Mappe memoria
    std::vector<MemoryRegion> regions;
    std::mutex mem_mutex;

    // Hook registrati
    std::vector<HookEntry> hooks;
    uc_hook next_hook_id{1};
    std::mutex hook_mutex;

    UnicornInstance(uc_arch a, uc_mode m) : arch(a), mode(m) {
        regs[13] = 0x7FFF0000; // SP default
        cpsr = 0x00000010;     // ARM User Mode
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

    MemoryRegion* findRegion(uint64_t addr) {
        for (auto& r : regions) {
            if (addr >= r.address && addr < (r.address + r.size)) {
                return &r;
            }
        }
        return nullptr;
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

    size_t written = 0;
    const uint8_t* src = static_cast<const uint8_t*>(bytes);

    while (written < size) {
        uint64_t curAddr = address + written;
        MemoryRegion* reg = inst->findRegion(curAddr);
        if (!reg) {
            LOGE("uc_mem_write: UC_ERR_WRITE_UNMAPPED all'indirizzo guest 0x%llx", (unsigned long long)curAddr);
            return UC_ERR_WRITE_UNMAPPED;
        }

        size_t offset = static_cast<size_t>(curAddr - reg->address);
        size_t available = reg->size - offset;
        size_t toWrite = std::min(size - written, available);

        std::memcpy(reg->host_ptr + offset, src + written, toWrite);
        written += toWrite;
    }

    return UC_ERR_OK;
}

uc_err uc_mem_read(uc_engine *uc, uint64_t address, void *bytes, size_t size) {
    if (!uc || !bytes) return UC_ERR_HANDLE;
    if (size == 0) return UC_ERR_OK;

    auto* inst = reinterpret_cast<UnicornInstance*>(uc);
    std::lock_guard<std::mutex> lock(inst->mem_mutex);

    size_t readCount = 0;
    uint8_t* dst = static_cast<uint8_t*>(bytes);

    while (readCount < size) {
        uint64_t curAddr = address + readCount;
        MemoryRegion* reg = inst->findRegion(curAddr);
        if (!reg) {
            LOGE("uc_mem_read: UC_ERR_READ_UNMAPPED all'indirizzo guest 0x%llx", (unsigned long long)curAddr);
            return UC_ERR_READ_UNMAPPED;
        }

        size_t offset = static_cast<size_t>(curAddr - reg->address);
        size_t available = reg->size - offset;
        size_t toRead = std::min(size - readCount, available);

        std::memcpy(dst + readCount, reg->host_ptr + offset, toRead);
        readCount += toRead;
    }

    return UC_ERR_OK;
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

    inst->regs[15] = static_cast<uint32_t>(begin);
    inst->is_running.store(true);
    inst->stop_requested.store(false);

    LOGI("uc_emu_start: Avvio emulazione guest da PC=0x%08llx (until=0x%llx, SP=0x%08x)...",
         (unsigned long long)begin, (unsigned long long)until, inst->regs[13]);

    size_t instructionsExecuted = 0;

    while (inst->is_running.load() && !inst->stop_requested.load()) {
        uint32_t pc = inst->regs[15];

        if (until != 0 && pc == until) {
            LOGI("uc_emu_start: Raggiunto target address until=0x%llx", (unsigned long long)until);
            break;
        }

        if (pc == 0 || pc == 0xDEADBEEF) {
            LOGI("uc_emu_start: Raggiunto return/trap address 0x%08x", pc);
            break;
        }

        if (count > 0 && instructionsExecuted >= count) {
            break;
        }

        uint32_t insn = 0;
        uc_err readErr = uc_mem_read(uc, pc, &insn, sizeof(insn));
        if (readErr != UC_ERR_OK) {
            LOGE("uc_emu_start: Fault lettura istruzione al PC=0x%08x (%s)", pc, uc_strerror(readErr));
            inst->is_running.store(false);
            return readErr;
        }

        // Intercetta istruzioni ARM Supervisor Call (SVC): 0xEF000000 o condizionali (0x0F000000 mask)
        if ((insn & 0x0F000000) == 0x0F000000) {
            uint32_t svcComment = insn & 0x00FFFFFF;

            std::vector<HookEntry> intrHooks;
            {
                std::lock_guard<std::mutex> lock(inst->hook_mutex);
                for (const auto& h : inst->hooks) {
                    if (h.type & UC_HOOK_INTR) {
                        intrHooks.push_back(h);
                    }
                }
            }

            for (const auto& h : intrHooks) {
                auto cb = reinterpret_cast<uc_cb_hookintr_t>(h.callback);
                // In Unicorn ARM, intno per SVC/SWI è 2 (EXCP_SWI)
                cb(uc, 2, h.user_data);
            }

            inst->regs[15] += 4;
            instructionsExecuted++;
            continue;
        }

        // Istruzione ARM BX LR o ritorno funzione
        if (insn == 0xE12FFF1E) { // bx lr
            uint32_t lr = inst->regs[14];
            inst->regs[15] = lr;
            instructionsExecuted++;
            if (lr == 0 || lr == 0xDEADBEEF) {
                break;
            }
            continue;
        }

        // Avanzamento PC standard 4 byte ARM
        inst->regs[15] += 4;
        instructionsExecuted++;

        // Salvaguardia loop
        if (instructionsExecuted >= 1000000) {
            break;
        }
    }

    inst->is_running.store(false);
    LOGI("uc_emu_start: Emulazione terminata al PC=0x%08x (Istruzioni eseguite: %zu)",
         inst->regs[15], instructionsExecuted);
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
