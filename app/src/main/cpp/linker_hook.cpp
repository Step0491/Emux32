#include "linker_hook.h"

#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <cstring>
#include <mutex>

#define TAG "EmuxLinkerHook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace emux {

static std::mutex gHandleMutex;

JniHandleTable& JniHandleTable::getInstance() {
    static JniHandleTable instance;
    return instance;
}

JniHandleTable::JniHandleTable() = default;

jptr32_t JniHandleTable::registerObject(jobject obj) {
    if (!obj) return 0;
    std::lock_guard<std::mutex> lock(gHandleMutex);
    jptr32_t handle = nextHandle_++;
    objectMap_[handle] = obj;
    return handle;
}

jobject JniHandleTable::getObject(jptr32_t handle) {
    if (handle == 0) return nullptr;
    std::lock_guard<std::mutex> lock(gHandleMutex);
    auto it = objectMap_.find(handle);
    if (it != objectMap_.end()) {
        return it->second;
    }
    return nullptr;
}

void JniHandleTable::unregisterObject(jptr32_t handle) {
    if (handle == 0) return;
    std::lock_guard<std::mutex> lock(gHandleMutex);
    objectMap_.erase(handle);
}

jptr32_t JniHandleTable::registerMethod(jmethodID method) {
    if (!method) return 0;
    std::lock_guard<std::mutex> lock(gHandleMutex);
    jptr32_t handle = nextHandle_++;
    methodMap_[handle] = method;
    return handle;
}

jmethodID JniHandleTable::getMethod(jptr32_t handle) {
    if (handle == 0) return nullptr;
    std::lock_guard<std::mutex> lock(gHandleMutex);
    auto it = methodMap_.find(handle);
    if (it != methodMap_.end()) {
        return it->second;
    }
    return nullptr;
}

// Numeri di syscall Linux per architettura ARM (EABI a 32-bit)
constexpr uint32_t ARM_NR_exit            = 1;
constexpr uint32_t ARM_NR_read            = 3;
constexpr uint32_t ARM_NR_write           = 4;
constexpr uint32_t ARM_NR_open            = 5;
constexpr uint32_t ARM_NR_close           = 6;
constexpr uint32_t ARM_NR_getpid          = 20;
constexpr uint32_t ARM_NR_ioctl           = 54;
constexpr uint32_t ARM_NR_gettimeofday    = 78;
constexpr uint32_t ARM_NR_mprotect        = 125;
constexpr uint32_t ARM_NR_mmap2           = 192;
constexpr uint32_t ARM_NR_clock_gettime   = 263;

int32_t SyscallDispatcher::handleSyscall(
    uint32_t syscallNum,
    uint32_t r0,
    uint32_t r1,
    uint32_t r2,
    uint32_t r3,
    uint32_t r4,
    uint32_t r5,
    uintptr_t guestMemoryBase
) {
    // Funzione di utilità per convertire puntatori a 32-bit guest in puntatori host
    auto translateGuestPtr = [guestMemoryBase](uint32_t guestPtr) -> void* {
        if (guestPtr == 0) return nullptr;
        return reinterpret_cast<void*>(guestMemoryBase + guestPtr);
    };

    switch (syscallNum) {
        case ARM_NR_write: {
            int fd = static_cast<int>(r0);
            void* buf = translateGuestPtr(r1);
            size_t count = static_cast<size_t>(r2);
            if (!buf) return -1;
            return static_cast<int32_t>(write(fd, buf, count));
        }

        case ARM_NR_read: {
            int fd = static_cast<int>(r0);
            void* buf = translateGuestPtr(r1);
            size_t count = static_cast<size_t>(r2);
            if (!buf) return -1;
            return static_cast<int32_t>(read(fd, buf, count));
        }

        case ARM_NR_open: {
            const char* pathname = reinterpret_cast<const char*>(translateGuestPtr(r0));
            int flags = static_cast<int>(r1);
            mode_t mode = static_cast<mode_t>(r2);
            if (!pathname) return -1;
            return open(pathname, flags, mode);
        }

        case ARM_NR_close: {
            int fd = static_cast<int>(r0);
            return close(fd);
        }

        case ARM_NR_getpid: {
            return static_cast<int32_t>(getpid());
        }

        case ARM_NR_ioctl: {
            int fd = static_cast<int>(r0);
            unsigned long request = static_cast<unsigned long>(r1);
            void* argp = translateGuestPtr(r2);
            return ioctl(fd, request, argp);
        }

        case ARM_NR_mmap2: {
            // mmap2(addr, length, prot, flags, fd, pgoffset)
            uint32_t guestAddr = r0;
            size_t length = static_cast<size_t>(r1);
            int prot = static_cast<int>(r2);
            int flags = static_cast<int>(r3);
            int fd = static_cast<int>(r4);
            off_t pgoff = static_cast<off_t>(r5); // Unità di 4096 byte

            void* hostHint = (guestAddr != 0) ? translateGuestPtr(guestAddr) : nullptr;
            void* result = mmap(hostHint, length, prot, flags, fd, pgoff * 4096);
            if (result == MAP_FAILED) {
                return -1;
            }

            uintptr_t mappedGuest = reinterpret_cast<uintptr_t>(result) - guestMemoryBase;
            return static_cast<int32_t>(mappedGuest);
        }

        case ARM_NR_mprotect: {
            void* addr = translateGuestPtr(r0);
            size_t len = static_cast<size_t>(r1);
            int prot = static_cast<int>(r2);
            return mprotect(addr, len, prot);
        }

        case ARM_NR_clock_gettime: {
            clockid_t clk_id = static_cast<clockid_t>(r0);
            auto* tp32 = reinterpret_cast<uint32_t*>(translateGuestPtr(r1));
            if (!tp32) return -1;

            struct timespec ts{};
            int ret = clock_gettime(clk_id, &ts);
            if (ret == 0) {
                tp32[0] = static_cast<uint32_t>(ts.tv_sec);
                tp32[1] = static_cast<uint32_t>(ts.tv_nsec);
            }
            return ret;
        }

        case ARM_NR_gettimeofday: {
            auto* tv32 = reinterpret_cast<uint32_t*>(translateGuestPtr(r0));
            struct timeval tv{};
            int ret = gettimeofday(&tv, nullptr);
            if (ret == 0 && tv32) {
                tv32[0] = static_cast<uint32_t>(tv.tv_sec);
                tv32[1] = static_cast<uint32_t>(tv.tv_usec);
            }
            return ret;
        }

        default:
            LOGW("Syscall ARM32 non implementata intercettata: NR=%u (r0=0x%x, r1=0x%x)",
                 syscallNum, r0, r1);
            return -38; // -ENOSYS
    }
}

// Forwarding verso librerie grafiche native host (OpenGL ES 2/3)
static void* sGlesHandle = nullptr;

void* SymbolResolver::resolveHostSymbol(const char* symbolName) {
    if (!symbolName) return nullptr;

    if (!sGlesHandle) {
        sGlesHandle = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
        if (!sGlesHandle) {
            LOGE("Impossibile caricare libGLESv2.so host: %s", dlerror());
        }
    }

    if (sGlesHandle) {
        void* sym = dlsym(sGlesHandle, symbolName);
        if (sym) {
            return sym;
        }
    }

    // Risoluzione simboli standard della libc host
    return dlsym(RTLD_DEFAULT, symbolName);
}

} // namespace emux
