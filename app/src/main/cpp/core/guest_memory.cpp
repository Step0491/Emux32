#include "core/guest_memory.h"
#include "unicorn/unicorn.h"

#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <algorithm>
#include <vector>
#include <android/log.h>

#define LOG_TAG "EmuxGuestMemory"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace emux {

namespace {
inline uintptr_t alignDown(uintptr_t val, size_t align) noexcept {
    return val & ~(static_cast<uintptr_t>(align) - 1U);
}

inline uintptr_t alignUp(uintptr_t val, size_t align) noexcept {
    return (val + static_cast<uintptr_t>(align) - 1U) & ~(static_cast<uintptr_t>(align) - 1U);
}
} // namespace

GuestMemory& GuestMemory::getSharedInstance() {
    static GuestMemory sSharedInstance;
    return sSharedInstance;
}

GuestMemory::GuestMemory() {
    long ps = sysconf(_SC_PAGESIZE);
    hostPageSize_ = (ps > 0) ? static_cast<size_t>(ps) : 4096U;
}

GuestMemory::~GuestMemory() {
    destroy();
}

bool GuestMemory::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    refCount_++;
    if (base_ != nullptr) {
        return true;
    }

    // Riserva 4 GB di spazio di indirizzamento virtuale contiguo senza allocare RAM fisica (MAP_NORESERVE)
    void* arena = mmap(
        nullptr,
        kAddressSpaceSize,
        PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
        -1,
        0
    );

    if (arena == MAP_FAILED || arena == nullptr) {
        LOGE("Impossibile riservare arena virtuale da 4 GB per lo spazio guest ARM32");
        return false;
    }

    void* protTable = mmap(
        nullptr,
        kTotalGuestPages,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0
    );

    if (protTable == MAP_FAILED || protTable == nullptr) {
        munmap(arena, kAddressSpaceSize);
        LOGE("Impossibile allocare tabella permessi software da 1 MB");
        return false;
    }

    base_ = static_cast<uint8_t*>(arena);
    pageProt_ = static_cast<uint8_t*>(protTable);

    LOGI("Arena GuestMemory 4 GB inizializzata: HostBase=%p, HostPageSize=%zu byte (GuestPage=4096 byte)",
         base_, hostPageSize_);
    return true;
}

void GuestMemory::destroy() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (--refCount_ > 0) {
        return; // Altre istanze o thread guest sono ancora attivi
    }
    if (base_ != nullptr) {
        munmap(base_, kAddressSpaceSize);
        base_ = nullptr;
    }
    if (pageProt_ != nullptr) {
        munmap(pageProt_, kTotalGuestPages);
        pageProt_ = nullptr;
    }
    refCount_ = 0;
    resetAllocators();
}

bool GuestMemory::map(guest_addr_t addr, uint32_t size, uint8_t prot) {
    if (size == 0) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!base_ || !pageProt_) return false;

    uint64_t end64 = static_cast<uint64_t>(addr) + static_cast<uint64_t>(size);
    if (end64 > kAddressSpaceSize) {
        LOGE("GuestMemory::map fuori range 32-bit: addr=0x%08x, size=0x%x", addr, size);
        return false;
    }

    // Allineamento rispetto alla pagina host reale (supporta sia kernel 4 KB che Android 15+ 16 KB)
    uintptr_t hostStart = alignDown(static_cast<uintptr_t>(addr), hostPageSize_);
    uintptr_t hostEnd   = alignUp(static_cast<uintptr_t>(end64), hostPageSize_);
    size_t hostLen      = hostEnd - hostStart;

    if (mprotect(base_ + hostStart, hostLen, PROT_READ | PROT_WRITE) != 0) {
        // Fallback mmap MAP_FIXED nella sotto-regione dell'arena se il kernel richiede commit esplicito
        void* fixed = mmap(
            base_ + hostStart,
            hostLen,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
            -1,
            0
        );
        if (fixed == MAP_FAILED) {
            LOGE("GuestMemory::map commit fallito per Guest 0x%08x (HostOffset=0x%zx, Len=0x%zx)",
                 addr, hostStart, hostLen);
            return false;
        }
    }

    // Imposta i permessi software a granularità 4 KB (ARM32 standard)
    uint32_t firstGuestPage = addr >> kGuestPageShift;
    uint32_t lastGuestPage  = static_cast<uint32_t>((end64 - 1ULL) >> kGuestPageShift);
    uint8_t effectiveProt   = (prot == GP_NONE) ? GP_RWX : prot;

    for (uint32_t p = firstGuestPage; p <= lastGuestPage; ++p) {
        pageProt_[p] |= effectiveProt;
    }

    return true;
}

bool GuestMemory::unmap(guest_addr_t addr, uint32_t size) {
    if (size == 0) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!base_ || !pageProt_) return false;

    uint64_t end64 = static_cast<uint64_t>(addr) + static_cast<uint64_t>(size);
    if (end64 > kAddressSpaceSize) return false;

    uint32_t firstGuestPage = addr >> kGuestPageShift;
    uint32_t lastGuestPage  = static_cast<uint32_t>((end64 - 1ULL) >> kGuestPageShift);
    for (uint32_t p = firstGuestPage; p <= lastGuestPage; ++p) {
        pageProt_[p] = GP_NONE;
    }

    // Rilascia le pagine fisiche al kernel se l'intero blocco è allineato alla pagina host
    uintptr_t hostStart = alignUp(static_cast<uintptr_t>(addr), hostPageSize_);
    uintptr_t hostEnd   = alignDown(static_cast<uintptr_t>(end64), hostPageSize_);
    if (hostEnd > hostStart) {
        madvise(base_ + hostStart, hostEnd - hostStart, MADV_DONTNEED);
    }

    return true;
}

bool GuestMemory::protect(guest_addr_t addr, uint32_t size, uint8_t prot) {
    if (size == 0) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!base_ || !pageProt_) return false;

    uint64_t end64 = static_cast<uint64_t>(addr) + static_cast<uint64_t>(size);
    if (end64 > kAddressSpaceSize) return false;

    uint32_t firstGuestPage = addr >> kGuestPageShift;
    uint32_t lastGuestPage  = static_cast<uint32_t>((end64 - 1ULL) >> kGuestPageShift);
    uint8_t effectiveProt   = (prot == GP_NONE) ? GP_RWX : prot;

    for (uint32_t p = firstGuestPage; p <= lastGuestPage; ++p) {
        pageProt_[p] = effectiveProt;
    }
    return true;
}

guest_addr_t GuestMemory::handleBrk(guest_addr_t newBrk, uc_engine* uc) {
    std::lock_guard<std::mutex> lock(vmaMutex_);
    if (heapBreak_ < kHeapBase) heapBreak_ = kHeapBase;
    if (newBrk == 0 || newBrk == heapBreak_) {
        return heapBreak_;
    }
    if (newBrk < kHeapBase || newBrk > kHeapLimit) {
        LOGW("GuestMemory::handleBrk richiesta fuori limiti: 0x%08x (HeapBase=0x%08x, Limit=0x%08x)",
             newBrk, kHeapBase, kHeapLimit);
        return heapBreak_;
    }

    guest_addr_t alignedNew = (newBrk + 0xFFFU) & ~0xFFFU;
    guest_addr_t alignedCur = (heapBreak_ + 0xFFFU) & ~0xFFFU;

    if (alignedNew > alignedCur) {
        uint32_t expandSize = alignedNew - alignedCur;
        map(alignedCur, expandSize, GP_READ | GP_WRITE);
        if (uc) {
            uc_mem_map(uc, alignedCur, expandSize, UC_PROT_READ | UC_PROT_WRITE);
        }
    } else if (alignedNew < alignedCur) {
        uint32_t shrinkSize = alignedCur - alignedNew;
        unmap(alignedNew, shrinkSize);
        if (uc) {
            uc_mem_unmap(uc, alignedNew, shrinkSize);
        }
    }
    heapBreak_ = newBrk;
    return heapBreak_;
}

guest_addr_t GuestMemory::handleMmap(guest_addr_t addr, uint32_t len, int prot, int flags, int fd, uint64_t offset, uc_engine* uc) {
    if (len == 0) return static_cast<guest_addr_t>(-EINVAL);
    std::lock_guard<std::mutex> lock(vmaMutex_);

    uint32_t alignedSize = (len + 0xFFFU) & ~0xFFFU;
    guest_addr_t targetAddr = 0;

    bool isFixed = (flags & MAP_FIXED) != 0;
    if (addr != 0 && isFixed) {
        targetAddr = addr & ~0xFFFU;
    } else if (addr != 0 && (addr >= kMmapBase && (addr + alignedSize) <= kMmapLimit)) {
        bool conflict = false;
        for (const auto& reg : mmapRegions_) {
            if (!(addr + alignedSize <= reg.first || addr >= (reg.first + reg.second.size))) {
                conflict = true;
                break;
            }
        }
        targetAddr = conflict ? 0 : (addr & ~0xFFFU);
    }

    if (targetAddr == 0) {
        if (mmapCurrent_ + alignedSize > kMmapLimit) {
            mmapCurrent_ = kMmapBase;
        }
        targetAddr = mmapCurrent_;
        mmapCurrent_ += alignedSize;
    }

    uint8_t gprot = GP_NONE;
    if (prot & PROT_READ)  gprot |= GP_READ;
    if (prot & PROT_WRITE) gprot |= GP_WRITE;
    if (prot & PROT_EXEC)  gprot |= GP_EXEC;
    if (gprot == GP_NONE)  gprot = GP_RWX;

    uint32_t ucProt = UC_PROT_NONE;
    if (prot & PROT_READ)  ucProt |= UC_PROT_READ;
    if (prot & PROT_WRITE) ucProt |= UC_PROT_WRITE;
    if (prot & PROT_EXEC)  ucProt |= UC_PROT_EXEC;
    if (ucProt == UC_PROT_NONE) ucProt = UC_PROT_ALL;

    if (!map(targetAddr, alignedSize, gprot)) {
        LOGE("GuestMemory::handleMmap fallita allocazione arena: addr=0x%08x, size=0x%x", targetAddr, alignedSize);
        return static_cast<guest_addr_t>(-ENOMEM);
    }

    if (uc) {
        uc_mem_map(uc, targetAddr, alignedSize, ucProt);
    }

    if (fd >= 0 && !(flags & MAP_ANONYMOUS)) {
        uint8_t* hostDest = g2h(targetAddr);
        ssize_t bytesRead = pread(fd, hostDest, len, static_cast<off_t>(offset));
        if (bytesRead > 0 && static_cast<size_t>(bytesRead) < alignedSize) {
            std::memset(hostDest + bytesRead, 0, alignedSize - static_cast<size_t>(bytesRead));
        }
        if (uc && bytesRead > 0) {
            uc_mem_write(uc, targetAddr, hostDest, bytesRead);
        }
    } else {
        std::memset(g2h(targetAddr), 0, alignedSize);
    }

    mmapRegions_[targetAddr] = { targetAddr, alignedSize, prot, flags, fd };
    return targetAddr;
}

int GuestMemory::handleMunmap(guest_addr_t addr, uint32_t len, uc_engine* uc) {
    if (addr == 0 || len == 0) return -EINVAL;
    std::lock_guard<std::mutex> lock(vmaMutex_);

    guest_addr_t alignedAddr = addr & ~0xFFFU;
    uint32_t alignedSize = (len + 0xFFFU) & ~0xFFFU;

    unmap(alignedAddr, alignedSize);
    if (uc) {
        uc_mem_unmap(uc, alignedAddr, alignedSize);
    }

    auto it = mmapRegions_.find(alignedAddr);
    if (it != mmapRegions_.end()) {
        mmapRegions_.erase(it);
    }
    return 0;
}

int GuestMemory::handleMprotect(guest_addr_t addr, uint32_t len, int prot, uc_engine* uc) {
    if (addr == 0 || len == 0) return -EINVAL;
    std::lock_guard<std::mutex> lock(vmaMutex_);

    guest_addr_t alignedAddr = addr & ~0xFFFU;
    uint32_t alignedSize = (len + 0xFFFU) & ~0xFFFU;

    uint8_t gprot = GP_NONE;
    if (prot & PROT_READ)  gprot |= GP_READ;
    if (prot & PROT_WRITE) gprot |= GP_WRITE;
    if (prot & PROT_EXEC)  gprot |= GP_EXEC;
    if (gprot == GP_NONE)  gprot = GP_RWX;

    protect(alignedAddr, alignedSize, gprot);
    return 0;
}

guest_addr_t GuestMemory::allocateHeap(uint32_t size, uint32_t align, uc_engine* uc) {
    std::lock_guard<std::mutex> lock(vmaMutex_);
    if (size == 0) size = 16;
    if (align < 8) align = 8;
    if (heapBreak_ < kHeapBase) heapBreak_ = kHeapBase;

    guest_addr_t alignedStart = (heapBreak_ + (align - 1U)) & ~(align - 1U);
    guest_addr_t nextBreak = alignedStart + ((size + 15U) & ~15U);

    if (nextBreak > kHeapLimit) {
        LOGE("GuestMemory::allocateHeap Out Of Memory: richiesta size=%u eccede HeapLimit 0x%08x", size, kHeapLimit);
        return 0;
    }

    guest_addr_t pageStart = alignedStart & ~0xFFFU;
    guest_addr_t pageEnd = (nextBreak + 0xFFFU) & ~0xFFFU;
    if (pageEnd > pageStart && !isMappedRange(pageStart, pageEnd - pageStart)) {
        map(pageStart, pageEnd - pageStart, GP_READ | GP_WRITE);
        if (uc) {
            uc_mem_map(uc, pageStart, pageEnd - pageStart, UC_PROT_READ | UC_PROT_WRITE);
        }
    }
    heapBreak_ = nextBreak;
    return alignedStart;
}

void GuestMemory::resetAllocators() {
    std::lock_guard<std::mutex> lock(vmaMutex_);
    heapBreak_ = kHeapBase;
    mmapCurrent_ = kMmapBase;
    mmapRegions_.clear();
}

} // namespace emux
