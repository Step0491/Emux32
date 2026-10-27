#include "core/guest_memory.h"

#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
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

GuestMemory::GuestMemory() {
    long ps = sysconf(_SC_PAGESIZE);
    hostPageSize_ = (ps > 0) ? static_cast<size_t>(ps) : 4096U;
}

GuestMemory::~GuestMemory() {
    destroy();
}

bool GuestMemory::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
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
    if (base_ != nullptr) {
        munmap(base_, kAddressSpaceSize);
        base_ = nullptr;
    }
    if (pageProt_ != nullptr) {
        munmap(pageProt_, kTotalGuestPages);
        pageProt_ = nullptr;
    }
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

} // namespace emux
