#pragma once

#include <cstdint>
#include <cstddef>
#include <mutex>
#include <map>
#include <string>

#include "unicorn/unicorn.h"

namespace emux {

using guest_addr_t = uint32_t;

enum GuestProt : uint8_t {
    GP_NONE  = 0,
    GP_R     = 1 << 0,
    GP_W     = 1 << 1,
    GP_X     = 1 << 2,
    GP_READ  = GP_R,
    GP_WRITE = GP_W,
    GP_EXEC  = GP_X,
    GP_RWX   = GP_R | GP_W | GP_X
};

/**
 * GuestMemory: Arena flat contigua da 4 GB in user-space host (64-bit)
 * per traduzione O(1) (host_ptr = base_ + guest_addr) e compatibilità
 * trasparente tra pagine guest 4 KB (ARM32) e pagine host 4 KB / 16 KB (Android 15/16+).
 */
class GuestMemory {
public:
    static constexpr uint64_t kAddressSpaceSize = 1ULL << 32; // 4 GB
    static constexpr uint32_t kGuestPageShift   = 12U;
    static constexpr uint32_t kGuestPageSize    = 1U << kGuestPageShift; // 4096 byte
    static constexpr size_t   kTotalGuestPages  = 1ULL << (32 - kGuestPageShift); // 1.048.576 pagine

    // Partizionamento Layout Memoria Virtuale Guest (ARM32)
    static constexpr guest_addr_t kHeapBase   = 0x58000000U; // Inizio Heap brk
    static constexpr guest_addr_t kHeapLimit  = 0x70000000U; // Limite Heap brk (384 MB)
    static constexpr guest_addr_t kMmapBase   = 0x80000000U; // Inizio Arena Mmap anonima / file-backed
    static constexpr guest_addr_t kMmapLimit  = 0xC0000000U; // Limite Arena Mmap (1 GB)

    GuestMemory();
    ~GuestMemory();

    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;

    // Istanza condivisa del modello di memoria flat 4 GB (process-wide address space)
    static GuestMemory& getSharedInstance();

    // Riserva l'intero spazio 32-bit (4 GB) con MAP_NORESERVE e PROT_NONE sull'host
    bool initialize();

    // Rilascia l'arena virtuale da 4 GB
    void destroy();

    // Committa le pagine host sottostanti (allineate a 4KB o 16KB) e imposta i permessi guest a 4KB
    bool map(guest_addr_t addr, uint32_t size, uint8_t prot);

    // Rimuove i permessi logici delle pagine guest e rilascia le pagine fisiche allineate
    bool unmap(guest_addr_t addr, uint32_t size);

    // Aggiorna i permessi software a granularità 4KB
    bool protect(guest_addr_t addr, uint32_t size, uint8_t prot);

    // Gestione unificata Syscall e Libc per brk (espansione e contrazione dinamica dell'heap)
    guest_addr_t handleBrk(guest_addr_t newBrk, uc_engine* uc);

    // Gestione unificata Syscall e Libc per mmap/mmap2 (supporto anonimo e file-backed con pread)
    guest_addr_t handleMmap(guest_addr_t addr, uint32_t len, int prot, int flags, int fd, uint64_t offset, uc_engine* uc);

    // Gestione unificata Syscall e Libc per munmap
    int handleMunmap(guest_addr_t addr, uint32_t len, uc_engine* uc);

    // Gestione unificata Syscall e Libc per mprotect
    int handleMprotect(guest_addr_t addr, uint32_t len, int prot, uc_engine* uc);

    // Allocatore heap thread-safe per runtime guest (malloc, calloc, realloc)
    guest_addr_t allocateHeap(uint32_t size, uint32_t align, uc_engine* uc);

    // Reset allocatori virtuali (heap break e mmap cursor)
    void resetAllocators();

    // Traduzione diretta O(1) Guest -> Host (1 ciclo CPU: addizione base + offset 32-bit)
    inline uint8_t* g2h(guest_addr_t addr) const noexcept {
        return base_ + static_cast<uintptr_t>(addr);
    }

    // Traduzione diretta O(1) Host -> Guest
    inline guest_addr_t h2g(const void* hostPtr) const noexcept {
        return static_cast<guest_addr_t>(static_cast<const uint8_t*>(hostPtr) - base_);
    }

    // Verifica ultraveloce O(1) se l'intervallo [addr, addr + size) appartiene a pagine mappate
    inline bool isMappedRange(guest_addr_t addr, size_t size) const noexcept {
        if (!base_ || size == 0) return false;
        uint64_t end = static_cast<uint64_t>(addr) + size - 1ULL;
        if (end >= kAddressSpaceSize) return false;
        uint32_t firstPage = addr >> kGuestPageShift;
        uint32_t lastPage  = static_cast<uint32_t>(end >> kGuestPageShift);
        if (firstPage == lastPage) {
            return pageProt_[firstPage] != GP_NONE;
        }
        for (uint32_t p = firstPage; p <= lastPage; ++p) {
            if (pageProt_[p] == GP_NONE) return false;
        }
        return true;
    }

    inline bool isMapped(guest_addr_t addr) const noexcept {
        return pageProt_[addr >> kGuestPageShift] != GP_NONE;
    }

    inline uint8_t getPageProt(guest_addr_t addr) const noexcept {
        return pageProt_[addr >> kGuestPageShift];
    }

    inline bool isReady() const noexcept { return base_ != nullptr; }
    inline uint8_t* base() const noexcept { return base_; }
    inline size_t hostPageSize() const noexcept { return hostPageSize_; }

private:
    struct MmapRegion {
        guest_addr_t addr;
        uint32_t size;
        int prot;
        int flags;
        int fd;
    };

    uint8_t* base_{nullptr};
    uint8_t* pageProt_{nullptr}; // Tabella software da 1 MB (1 byte per pagina guest da 4 KB)
    size_t   hostPageSize_{4096};
    int      refCount_{0};
    mutable std::mutex mutex_;

    mutable std::mutex vmaMutex_;
    guest_addr_t heapBreak_{kHeapBase};
    guest_addr_t mmapCurrent_{kMmapBase};
    std::map<guest_addr_t, MmapRegion> mmapRegions_;
};

} // namespace emux
