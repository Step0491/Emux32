#include "linker_hook.h"
#include "unicorn/arm.h"

#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <cstring>
#include <cerrno>
#include <cmath>
#include <mutex>
#include <vector>
#include <string>
#include <unordered_map>
#include <algorithm>

#define TAG "EmuxLinkerHook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace emux {

static std::mutex gHandleMutex;
static std::string sCustomStoragePath = "/sdcard/Emux32";
static uint32_t sHeapCurrent = 0x65000000;

// Tabella di mapping dei file aperti (FILE*) per la simulazione libc
static std::unordered_map<uint32_t, FILE*> sFileTable;
static uint32_t sNextFileHandle = 0x30000001;
static std::mutex sFileMutex;

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

void SyscallDispatcher::setStoragePath(const std::string& path) {
    sCustomStoragePath = path;
    LOGI("SyscallDispatcher: Percorso storage configurato a '%s'", path.c_str());
}

std::string SyscallDispatcher::resolveFilePath(const char* guestPath) {
    if (!guestPath || guestPath[0] == '\0') return "";

    // 1. Se il percorso esiste già direttamente sul filesystem host, usalo
    if (access(guestPath, F_OK) == 0) {
        return std::string(guestPath);
    }

    std::string pathStr(guestPath);

    // 2. Estrai il nome del file base
    size_t lastSlash = pathStr.find_last_of('/');
    std::string filename = (lastSlash != std::string::npos) ? pathStr.substr(lastSlash + 1) : pathStr;

    // 3. Elenco percorsi alternativi con priorità
    std::vector<std::string> candidates = {
        sCustomStoragePath + "/obb/" + filename,
        sCustomStoragePath + "/apk/" + filename,
        sCustomStoragePath + "/" + filename,
        "/storage/emulated/0/Emux32/obb/" + filename,
        "/storage/emulated/0/Emux32/apk/" + filename,
        "/storage/emulated/0/Emux32/" + filename,
        "/sdcard/Emux32/obb/" + filename,
        "/sdcard/Emux32/apk/" + filename,
        "/sdcard/Emux32/" + filename,
        "/storage/emulated/0/Android/obb/" + filename,
        "/sdcard/Android/obb/" + filename
    };

    for (const auto& cand : candidates) {
        if (access(cand.c_str(), F_OK) == 0) {
            LOGI("Risoluzione file guest: '%s' -> '%s'", guestPath, cand.c_str());
            return cand;
        }
    }

    return pathStr;
}

// Struttura standard stat64 su kernel Linux ARM a 32-bit (Bionic ABI)
#pragma pack(push, 4)
struct kernel_stat64_arm32 {
    uint64_t dev_val;
    uint32_t pad0;
    uint32_t ino_32;
    uint32_t mode_val;
    uint32_t nlink_val;
    uint32_t uid_val;
    uint32_t gid_val;
    uint64_t rdev_val;
    uint32_t pad3;
    int64_t  size_val;
    uint32_t blksize_val;
    uint64_t blocks_val;
    uint32_t atime_sec;
    uint32_t atime_nsec;
    uint32_t mtime_sec;
    uint32_t mtime_nsec;
    uint32_t ctime_sec;
    uint32_t ctime_nsec;
    uint64_t ino_64;
};
#pragma pack(pop)

static inline void fillStat64(const struct stat& hostSt, kernel_stat64_arm32& st32) {
    std::memset(&st32, 0, sizeof(st32));
    st32.dev_val = hostSt.st_dev;
    st32.ino_32 = static_cast<uint32_t>(hostSt.st_ino);
    st32.mode_val = hostSt.st_mode;
    st32.nlink_val = hostSt.st_nlink;
    st32.uid_val = hostSt.st_uid;
    st32.gid_val = hostSt.st_gid;
    st32.rdev_val = hostSt.st_rdev;
    st32.size_val = hostSt.st_size;
    st32.blksize_val = static_cast<uint32_t>(hostSt.st_blksize);
    st32.blocks_val = hostSt.st_blocks;
    st32.atime_sec = static_cast<uint32_t>(hostSt.st_atime);
    st32.mtime_sec = static_cast<uint32_t>(hostSt.st_mtime);
    st32.ctime_sec = static_cast<uint32_t>(hostSt.st_ctime);
    st32.ino_64 = hostSt.st_ino;
}

constexpr uint32_t ARM_NR_exit            = 1;
constexpr uint32_t ARM_NR_read            = 3;
constexpr uint32_t ARM_NR_write           = 4;
constexpr uint32_t ARM_NR_open            = 5;
constexpr uint32_t ARM_NR_close           = 6;
constexpr uint32_t ARM_NR_lseek           = 19;
constexpr uint32_t ARM_NR_getpid          = 20;
constexpr uint32_t ARM_NR_getuid          = 24;
constexpr uint32_t ARM_NR_access          = 33;
constexpr uint32_t ARM_NR_brk             = 45;
constexpr uint32_t ARM_NR_getgid          = 47;
constexpr uint32_t ARM_NR_geteuid         = 49;
constexpr uint32_t ARM_NR_getegid         = 50;
constexpr uint32_t ARM_NR_ioctl           = 54;
constexpr uint32_t ARM_NR_fcntl           = 55;
constexpr uint32_t ARM_NR_gettimeofday    = 78;
constexpr uint32_t ARM_NR_munmap          = 91;
constexpr uint32_t ARM_NR_stat            = 106;
constexpr uint32_t ARM_NR_fstat           = 108;
constexpr uint32_t ARM_NR_mprotect        = 125;
constexpr uint32_t ARM_NR__llseek         = 140;
constexpr uint32_t ARM_NR_sched_yield     = 158;
constexpr uint32_t ARM_NR_nanosleep       = 162;
constexpr uint32_t ARM_NR_prctl           = 172;
constexpr uint32_t ARM_NR_mmap2           = 192;
constexpr uint32_t ARM_NR_stat64          = 195;
constexpr uint32_t ARM_NR_lstat64         = 196;
constexpr uint32_t ARM_NR_fstat64         = 197;
constexpr uint32_t ARM_NR_fcntl64         = 221;
constexpr uint32_t ARM_NR_gettid          = 224;
constexpr uint32_t ARM_NR_futex           = 240;
constexpr uint32_t ARM_NR_clock_gettime   = 263;
constexpr uint32_t ARM_NR_openat          = 322;
constexpr uint32_t ARM_NR_fstatat64       = 327;
constexpr uint32_t ARM_NR_faccessat       = 334;

int32_t SyscallDispatcher::handleSyscall(
    uint32_t syscallNum,
    uint32_t r0,
    uint32_t r1,
    uint32_t r2,
    uint32_t r3,
    uint32_t r4,
    uint32_t /* r5 */,
    uc_engine* uc
) {
    switch (syscallNum) {
        case ARM_NR_write: {
            int fd = static_cast<int>(r0);
            uint32_t bufAddr = r1;
            size_t count = static_cast<size_t>(r2);
            if (bufAddr == 0 || count == 0) return 0;

            std::vector<char> tempBuf(count);
            if (uc && uc_mem_read(uc, bufAddr, tempBuf.data(), count) == UC_ERR_OK) {
                return static_cast<int32_t>(write(fd, tempBuf.data(), count));
            }
            return -EFAULT;
        }

        case ARM_NR_read: {
            int fd = static_cast<int>(r0);
            uint32_t bufAddr = r1;
            size_t count = static_cast<size_t>(r2);
            if (bufAddr == 0 || count == 0) return 0;

            std::vector<char> tempBuf(count);
            ssize_t bytesRead = read(fd, tempBuf.data(), count);
            if (bytesRead > 0 && uc) {
                uc_mem_write(uc, bufAddr, tempBuf.data(), static_cast<size_t>(bytesRead));
            }
            return (bytesRead >= 0) ? static_cast<int32_t>(bytesRead) : -errno;
        }

        case ARM_NR_open: {
            uint32_t pathAddr = r0;
            int flags = static_cast<int>(r1);
            mode_t mode = static_cast<mode_t>(r2);
            if (pathAddr == 0) return -EFAULT;

            char pathBuffer[512] = {0};
            if (uc && uc_mem_read(uc, pathAddr, pathBuffer, sizeof(pathBuffer) - 1) == UC_ERR_OK) {
                pathBuffer[sizeof(pathBuffer) - 1] = '\0';
                std::string resolved = resolveFilePath(pathBuffer);
                int fd = open(resolved.c_str(), flags, mode);
                return (fd >= 0) ? fd : -errno;
            }
            return -EFAULT;
        }

        case ARM_NR_openat: {
            int dirfd = static_cast<int>(r0);
            uint32_t pathAddr = r1;
            int flags = static_cast<int>(r2);
            mode_t mode = static_cast<mode_t>(r3);
            if (pathAddr == 0) return -EFAULT;

            char pathBuffer[512] = {0};
            if (uc && uc_mem_read(uc, pathAddr, pathBuffer, sizeof(pathBuffer) - 1) == UC_ERR_OK) {
                pathBuffer[sizeof(pathBuffer) - 1] = '\0';
                std::string resolved = resolveFilePath(pathBuffer);
                int fd = openat(dirfd, resolved.c_str(), flags, mode);
                return (fd >= 0) ? fd : -errno;
            }
            return -EFAULT;
        }

        case ARM_NR_close: {
            int fd = static_cast<int>(r0);
            int ret = close(fd);
            return (ret == 0) ? 0 : -errno;
        }

        case ARM_NR_lseek: {
            int fd = static_cast<int>(r0);
            off_t offset = static_cast<off_t>(static_cast<int32_t>(r1));
            int whence = static_cast<int>(r2);
            off_t res = lseek(fd, offset, whence);
            return (res != (off_t)-1) ? static_cast<int32_t>(res) : -errno;
        }

        case ARM_NR__llseek: {
            int fd = static_cast<int>(r0);
            uint32_t offset_high = r1;
            uint32_t offset_low = r2;
            uint32_t resultAddr = r3;
            uint32_t whence = r4;
            int64_t offset = (static_cast<int64_t>(offset_high) << 32) | static_cast<int64_t>(offset_low);

            off_t newOff = lseek(fd, static_cast<off_t>(offset), static_cast<int>(whence));
            if (newOff == (off_t)-1) {
                return -errno;
            }
            if (resultAddr != 0 && uc) {
                int64_t res64 = static_cast<int64_t>(newOff);
                uc_mem_write(uc, resultAddr, &res64, sizeof(res64));
            }
            return 0;
        }

        case ARM_NR_access: {
            uint32_t pathAddr = r0;
            int mode = static_cast<int>(r1);
            if (pathAddr == 0) return -EFAULT;

            char pathBuffer[512] = {0};
            if (uc && uc_mem_read(uc, pathAddr, pathBuffer, sizeof(pathBuffer) - 1) == UC_ERR_OK) {
                std::string resolved = resolveFilePath(pathBuffer);
                int ret = access(resolved.c_str(), mode);
                return (ret == 0) ? 0 : -errno;
            }
            return -EFAULT;
        }

        case ARM_NR_faccessat: {
            uint32_t pathAddr = r1;
            int mode = static_cast<int>(r2);
            if (pathAddr == 0) return -EFAULT;

            char pathBuffer[512] = {0};
            if (uc && uc_mem_read(uc, pathAddr, pathBuffer, sizeof(pathBuffer) - 1) == UC_ERR_OK) {
                std::string resolved = resolveFilePath(pathBuffer);
                int ret = access(resolved.c_str(), mode);
                return (ret == 0) ? 0 : -errno;
            }
            return -EFAULT;
        }

        case ARM_NR_stat64:
        case ARM_NR_lstat64: {
            uint32_t pathAddr = r0;
            uint32_t statBufAddr = r1;
            if (pathAddr == 0 || statBufAddr == 0) return -EFAULT;

            char pathBuffer[512] = {0};
            if (uc && uc_mem_read(uc, pathAddr, pathBuffer, sizeof(pathBuffer) - 1) == UC_ERR_OK) {
                std::string resolved = resolveFilePath(pathBuffer);
                struct stat hostSt{};
                int ret = stat(resolved.c_str(), &hostSt);
                if (ret == 0) {
                    kernel_stat64_arm32 st32{};
                    fillStat64(hostSt, st32);
                    uc_mem_write(uc, statBufAddr, &st32, sizeof(st32));
                    return 0;
                }
                return -errno;
            }
            return -EFAULT;
        }

        case ARM_NR_fstat64: {
            int fd = static_cast<int>(r0);
            uint32_t statBufAddr = r1;
            if (statBufAddr == 0) return -EFAULT;

            struct stat hostSt{};
            int ret = fstat(fd, &hostSt);
            if (ret == 0 && uc) {
                kernel_stat64_arm32 st32{};
                fillStat64(hostSt, st32);
                uc_mem_write(uc, statBufAddr, &st32, sizeof(st32));
                return 0;
            }
            return -errno;
        }

        case ARM_NR_fstatat64: {
            uint32_t pathAddr = r1;
            uint32_t statBufAddr = r2;
            if (pathAddr == 0 || statBufAddr == 0) return -EFAULT;

            char pathBuffer[512] = {0};
            if (uc && uc_mem_read(uc, pathAddr, pathBuffer, sizeof(pathBuffer) - 1) == UC_ERR_OK) {
                std::string resolved = resolveFilePath(pathBuffer);
                struct stat hostSt{};
                int ret = stat(resolved.c_str(), &hostSt);
                if (ret == 0) {
                    kernel_stat64_arm32 st32{};
                    fillStat64(hostSt, st32);
                    uc_mem_write(uc, statBufAddr, &st32, sizeof(st32));
                    return 0;
                }
                return -errno;
            }
            return -EFAULT;
        }

        case ARM_NR_getpid:  return static_cast<int32_t>(getpid());
        case ARM_NR_gettid:  return static_cast<int32_t>(getpid());
        case ARM_NR_getuid:  return static_cast<int32_t>(getuid());
        case ARM_NR_getgid:  return static_cast<int32_t>(getgid());
        case ARM_NR_geteuid: return static_cast<int32_t>(geteuid());
        case ARM_NR_getegid: return static_cast<int32_t>(getegid());

        case ARM_NR_ioctl: {
            int fd = static_cast<int>(r0);
            int request = static_cast<int>(r1);
            return ioctl(fd, request, nullptr);
        }

        case ARM_NR_fcntl:
        case ARM_NR_fcntl64: {
            return 0;
        }

        case ARM_NR_brk: {
            uint32_t newBrk = r0;
            if (newBrk != 0 && newBrk > sHeapCurrent) {
                sHeapCurrent = (newBrk + 0xFFFU) & ~0xFFFU;
            }
            return static_cast<int32_t>(sHeapCurrent);
        }

        case ARM_NR_mmap2: {
            uint32_t guestAddr = r0;
            size_t length = static_cast<size_t>(r1);
            if (guestAddr == 0) {
                guestAddr = sHeapCurrent;
                sHeapCurrent += (length + 0xFFFU) & ~0xFFFU;
            }
            uint32_t alignedBase = guestAddr & ~0xFFFU;
            uint32_t alignedSize = (length + 0xFFFU) & ~0xFFFU;

            if (uc) {
                uc_mem_map(uc, alignedBase, alignedSize, UC_PROT_ALL);
            }
            return static_cast<int32_t>(alignedBase);
        }

        case ARM_NR_munmap: {
            return 0;
        }

        case ARM_NR_mprotect: {
            return 0;
        }

        case ARM_NR_sched_yield: {
            sched_yield();
            return 0;
        }

        case ARM_NR_nanosleep: {
            return 0;
        }

        case ARM_NR_prctl: {
            return 0;
        }

        case ARM_NR_futex: {
            return 0;
        }

        case ARM_NR_clock_gettime: {
            clockid_t clk_id = static_cast<clockid_t>(r0);
            uint32_t tpAddr = r1;
            if (tpAddr == 0) return -EFAULT;

            struct timespec ts{};
            int ret = clock_gettime(clk_id, &ts);
            if (ret == 0 && uc) {
                uint32_t tp32[2] = {
                    static_cast<uint32_t>(ts.tv_sec),
                    static_cast<uint32_t>(ts.tv_nsec)
                };
                uc_mem_write(uc, tpAddr, tp32, sizeof(tp32));
            }
            return ret;
        }

        case ARM_NR_gettimeofday: {
            uint32_t tvAddr = r0;
            if (tvAddr == 0) return -EFAULT;

            struct timeval tv{};
            int ret = gettimeofday(&tv, nullptr);
            if (ret == 0 && uc) {
                uint32_t tv32[2] = {
                    static_cast<uint32_t>(tv.tv_sec),
                    static_cast<uint32_t>(tv.tv_usec)
                };
                uc_mem_write(uc, tvAddr, tv32, sizeof(tv32));
            }
            return ret;
        }

        case ARM_NR_exit: {
            LOGI("Guest process invocato exit(%u)", r0);
            if (uc) uc_emu_stop(uc);
            return 0;
        }

        default:
            LOGW("Syscall ARM32 non implementata: NR=%u (r0=0x%x, r1=0x%x)", syscallNum, r0, r1);
            return 0;
    }
}

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

    return dlsym(RTLD_DEFAULT, symbolName);
}

GuestRuntimeEnvironment& GuestRuntimeEnvironment::getInstance() {
    static GuestRuntimeEnvironment instance;
    return instance;
}

GuestRuntimeEnvironment::GuestRuntimeEnvironment() = default;

bool GuestRuntimeEnvironment::initialize(uc_engine* uc) {
    if (!uc) return false;

    // Reset dello stato runtime
    sHeapCurrent = 0x65000000;
    {
        std::lock_guard<std::mutex> lock(sFileMutex);
        for (auto& pair : sFileTable) {
            if (pair.second) fclose(pair.second);
        }
        sFileTable.clear();
        sNextFileHandle = 0x30000001;
    }

    // 1. Alloca regione JNI (0x7D000000, 64KB allineata)
    uc_mem_map(uc, guestJniBase_, 0x10000, UC_PROT_READ | UC_PROT_WRITE);

    // 2. Alloca regione Trampoline (0x70000000, 64KB allineata)
    uc_mem_map(uc, guestTrampolineBase_, 0x10000, UC_PROT_ALL);

    // 3. Mappa Heap guest a 0x65000000 (64MB) per allocazioni dinamiche (malloc, calloc, new)
    uc_mem_map(uc, 0x65000000, 0x04000000, UC_PROT_READ | UC_PROT_WRITE);

    // 4. Inizializza struttura JavaVM32
    JavaVM32 vm32{};
    vm32.functions = guestJniBase_ + 0x10;
    uc_mem_write(uc, guestJavaVMAddr_, &vm32, sizeof(vm32));

    // Inizializza tabella JNIInvokeInterface32
    JNIInvokeInterface32 vmVtable{};
    vmVtable.GetEnv = guestJniBase_ + 0x100;
    vmVtable.AttachCurrentThread = guestJniBase_ + 0x108;
    vmVtable.DetachCurrentThread = guestJniBase_ + 0x110;
    uc_mem_write(uc, vm32.functions, &vmVtable, sizeof(vmVtable));

    // Stub assembly ARM per GetEnv (ritorna JNI_OK e imposta *env = guestJNIEnvAddr_)
    // Istruzioni ARM: SVC #0x3001, BX LR
    uint32_t stubGetEnv[2] = {0xEF003001, 0xE12FFF1E};
    uc_mem_write(uc, guestJniBase_ + 0x100, stubGetEnv, sizeof(stubGetEnv));

    // 5. Inizializza struttura JNIEnv32
    JNIEnv32 env32{};
    env32.functions = guestJniBase_ + 0x220;
    uc_mem_write(uc, guestJNIEnvAddr_, &env32, sizeof(env32));

    // Inizializza tabella JNINativeInterface32
    JNINativeInterface32 envVtable{};
    envVtable.GetVersion = guestJniBase_ + 0x300;
    envVtable.FindClass = guestJniBase_ + 0x308;
    envVtable.GetMethodID = guestJniBase_ + 0x310;
    envVtable.GetStaticMethodID = guestJniBase_ + 0x318;
    envVtable.RegisterNatives = guestJniBase_ + 0x320;

    uc_mem_write(uc, env32.functions, &envVtable, sizeof(envVtable));

    // Stub assembly ARM: SVC #0x3002..#0x3006, BX LR
    uint32_t stubGetVersion[2]        = {0xEF003002, 0xE12FFF1E};
    uint32_t stubFindClass[2]         = {0xEF003003, 0xE12FFF1E};
    uint32_t stubGetMethodID[2]       = {0xEF003004, 0xE12FFF1E};
    uint32_t stubGetStaticMethodID[2] = {0xEF003005, 0xE12FFF1E};
    uint32_t stubRegisterNatives[2]   = {0xEF003006, 0xE12FFF1E};

    uc_mem_write(uc, guestJniBase_ + 0x300, stubGetVersion, sizeof(stubGetVersion));
    uc_mem_write(uc, guestJniBase_ + 0x308, stubFindClass, sizeof(stubFindClass));
    uc_mem_write(uc, guestJniBase_ + 0x310, stubGetMethodID, sizeof(stubGetMethodID));
    uc_mem_write(uc, guestJniBase_ + 0x318, stubGetStaticMethodID, sizeof(stubGetStaticMethodID));
    uc_mem_write(uc, guestJniBase_ + 0x320, stubRegisterNatives, sizeof(stubRegisterNatives));

    LOGI("GuestRuntimeEnvironment inizializzato: JavaVM32=0x%08x, JNIEnv32=0x%08x, Heap=0x65000000 (64MB)",
         guestJavaVMAddr_, guestJNIEnvAddr_);
    return true;
}

uint32_t GuestRuntimeEnvironment::registerSymbolTrampoline(const std::string& symbolName, uc_engine* uc) {
    auto it = symbolToTrampolineMap_.find(symbolName);
    if (it != symbolToTrampolineMap_.end()) {
        return it->second;
    }

    uint32_t trampId = nextTrampolineId_++;
    uint32_t trampAddr = guestTrampolineBase_ + (trampId * 8);

    symbolToTrampolineMap_[symbolName] = trampAddr;
    trampolineToSymbolMap_[trampId] = symbolName;

    // Scrive lo stub assembly ARM: SVC #(0x2000 + trampId), BX LR
    uint32_t stubInsn[2] = {
        0xEF000000U | (0x2000U + trampId),
        0xE12FFF1EU // bx lr
    };

    if (uc) {
        uc_mem_write(uc, trampAddr, stubInsn, sizeof(stubInsn));
    }

    return trampAddr;
}

void GuestRuntimeEnvironment::handleTrampolineCall(uint32_t trampolineId, uc_engine* uc) {
    if (!uc) return;

    // Gestione chiamate interne dell'interfaccia JavaVM e JNIEnv
    if (trampolineId == 0x3001) { // GetEnv(vm, void** env, jint version)
        uint32_t envPtrAddr = 0;
        uc_reg_read(uc, UC_ARM_REG_R1, &envPtrAddr);
        if (envPtrAddr != 0) {
            uint32_t envVal = guestJNIEnvAddr_;
            uc_mem_write(uc, envPtrAddr, &envVal, sizeof(envVal));
        }
        uint32_t ret = 0; // JNI_OK
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }

    if (trampolineId == 0x3002) { // GetVersion()
        uint32_t version = 0x00010006; // JNI_VERSION_1_6
        uc_reg_write(uc, UC_ARM_REG_R0, &version);
        return;
    }

    if (trampolineId == 0x3003) { // FindClass(env, name)
        uint32_t classHandle = 0x1001;
        uc_reg_write(uc, UC_ARM_REG_R0, &classHandle);
        return;
    }

    if (trampolineId == 0x3004 || trampolineId == 0x3005) { // GetMethodID / GetStaticMethodID
        uint32_t methodHandle = 0x2001;
        uc_reg_write(uc, UC_ARM_REG_R0, &methodHandle);
        return;
    }

    if (trampolineId == 0x3006) { // RegisterNatives(env, class, methods, nMethods)
        uint32_t methodsAddr = 0;
        uint32_t nMethods = 0;
        uc_reg_read(uc, UC_ARM_REG_R2, &methodsAddr);
        uc_reg_read(uc, UC_ARM_REG_R3, &nMethods);
        LOGI("JNI: RegisterNatives invocato per %u metodi nativi (methodsAddr=0x%08x)", nMethods, methodsAddr);

        // Struttura JNINativeMethod su ARM32: const char* name (4B), const char* signature (4B), void* fnPtr (4B)
        for (uint32_t i = 0; i < nMethods && i < 32; ++i) {
            uint32_t entryAddr = methodsAddr + (i * 12);
            uint32_t namePtr = 0, sigPtr = 0, fnPtr = 0;
            uc_mem_read(uc, entryAddr, &namePtr, 4);
            uc_mem_read(uc, entryAddr + 4, &sigPtr, 4);
            uc_mem_read(uc, entryAddr + 8, &fnPtr, 4);

            char mName[128] = {0};
            char mSig[128] = {0};
            if (namePtr) uc_mem_read(uc, namePtr, mName, sizeof(mName) - 1);
            if (sigPtr) uc_mem_read(uc, sigPtr, mSig, sizeof(mSig) - 1);
            LOGI("   -> Registrato JNINativeMethod[%u]: '%s' (%s) -> Entry Point: 0x%08x", i, mName, mSig, fnPtr);
        }

        uint32_t ret = 0; // JNI_OK
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }

    // Gestione simboli esterni registrati (libc / OpenGL ES / Bionic)
    auto it = trampolineToSymbolMap_.find(trampolineId - 0x2000);
    if (it != trampolineToSymbolMap_.end()) {
        const std::string& name = it->second;

        // 1. Android Logcat
        if (name == "__android_log_print" || name == "__android_log_vprint") {
            char tag[64] = {0};
            char msg[512] = {0};
            uint32_t tagAddr = 0, msgAddr = 0;
            uc_reg_read(uc, UC_ARM_REG_R1, &tagAddr);
            uc_reg_read(uc, UC_ARM_REG_R2, &msgAddr);
            if (tagAddr) uc_mem_read(uc, tagAddr, tag, sizeof(tag) - 1);
            if (msgAddr) uc_mem_read(uc, msgAddr, msg, sizeof(msg) - 1);
            LOGI("[GuestLog][%s] %s", tag, msg);
            uint32_t ret = 1;
            uc_reg_write(uc, UC_ARM_REG_R0, &ret);
            return;
        }

        if (name == "__android_log_write") {
            char tag[64] = {0};
            char msg[512] = {0};
            uint32_t tagAddr = 0, msgAddr = 0;
            uc_reg_read(uc, UC_ARM_REG_R1, &tagAddr);
            uc_reg_read(uc, UC_ARM_REG_R2, &msgAddr);
            if (tagAddr) uc_mem_read(uc, tagAddr, tag, sizeof(tag) - 1);
            if (msgAddr) uc_mem_read(uc, msgAddr, msg, sizeof(msg) - 1);
            LOGI("[GuestLog][%s] %s", tag, msg);
            uint32_t ret = 1;
            uc_reg_write(uc, UC_ARM_REG_R0, &ret);
            return;
        }

        // 2. Gestione Memoria (Heap Guest)
        if (name == "malloc") {
            uint32_t size = 0;
            uc_reg_read(uc, UC_ARM_REG_R0, &size);
            uint32_t allocAddr = sHeapCurrent;
            sHeapCurrent += (size + 15) & ~15U;
            uc_reg_write(uc, UC_ARM_REG_R0, &allocAddr);
            return;
        }

        if (name == "calloc") {
            uint32_t num = 0, size = 0;
            uc_reg_read(uc, UC_ARM_REG_R0, &num);
            uc_reg_read(uc, UC_ARM_REG_R1, &size);
            uint32_t total = num * size;
            uint32_t allocAddr = sHeapCurrent;
            sHeapCurrent += (total + 15) & ~15U;

            std::vector<uint8_t> zeroes(total, 0);
            uc_mem_write(uc, allocAddr, zeroes.data(), total);
            uc_reg_write(uc, UC_ARM_REG_R0, &allocAddr);
            return;
        }

        if (name == "realloc") {
            uint32_t ptr = 0, size = 0;
            uc_reg_read(uc, UC_ARM_REG_R0, &ptr);
            uc_reg_read(uc, UC_ARM_REG_R1, &size);
            uint32_t newAddr = sHeapCurrent;
            sHeapCurrent += (size + 15) & ~15U;
            if (ptr != 0 && size > 0) {
                std::vector<uint8_t> buf(size, 0);
                uc_mem_read(uc, ptr, buf.data(), size);
                uc_mem_write(uc, newAddr, buf.data(), size);
            }
            uc_reg_write(uc, UC_ARM_REG_R0, &newAddr);
            return;
        }

        if (name == "free") {
            return;
        }

        // 3. Operazioni di Memoria e Stringhe
        if (name == "memcpy" || name == "memmove") {
            uint32_t dst = 0, src = 0, count = 0;
            uc_reg_read(uc, UC_ARM_REG_R0, &dst);
            uc_reg_read(uc, UC_ARM_REG_R1, &src);
            uc_reg_read(uc, UC_ARM_REG_R2, &count);
            if (count > 0 && dst != 0 && src != 0) {
                std::vector<uint8_t> tmp(count);
                if (uc_mem_read(uc, src, tmp.data(), count) == UC_ERR_OK) {
                    uc_mem_write(uc, dst, tmp.data(), count);
                }
            }
            uc_reg_write(uc, UC_ARM_REG_R0, &dst);
            return;
        }

        if (name == "memset") {
            uint32_t dst = 0, val = 0, count = 0;
            uc_reg_read(uc, UC_ARM_REG_R0, &dst);
            uc_reg_read(uc, UC_ARM_REG_R1, &val);
            uc_reg_read(uc, UC_ARM_REG_R2, &count);
            if (count > 0 && dst != 0) {
                std::vector<uint8_t> tmp(count, static_cast<uint8_t>(val));
                uc_mem_write(uc, dst, tmp.data(), count);
            }
            uc_reg_write(uc, UC_ARM_REG_R0, &dst);
            return;
        }

        if (name == "memcmp") {
            uint32_t s1 = 0, s2 = 0, count = 0;
            uc_reg_read(uc, UC_ARM_REG_R0, &s1);
            uc_reg_read(uc, UC_ARM_REG_R1, &s2);
            uc_reg_read(uc, UC_ARM_REG_R2, &count);
            int res = 0;
            if (count > 0 && s1 != 0 && s2 != 0) {
                std::vector<uint8_t> b1(count), b2(count);
                uc_mem_read(uc, s1, b1.data(), count);
                uc_mem_read(uc, s2, b2.data(), count);
                res = std::memcmp(b1.data(), b2.data(), count);
            }
            uint32_t ret = static_cast<uint32_t>(res);
            uc_reg_write(uc, UC_ARM_REG_R0, &ret);
            return;
        }

        if (name == "strlen") {
            uint32_t sAddr = 0;
            uc_reg_read(uc, UC_ARM_REG_R0, &sAddr);
            uint32_t len = 0;
            if (sAddr != 0) {
                char c = 0;
                while (uc_mem_read(uc, sAddr + len, &c, 1) == UC_ERR_OK && c != '\0' && len < 4096) {
                    len++;
                }
            }
            uc_reg_write(uc, UC_ARM_REG_R0, &len);
            return;
        }

        if (name == "strcpy") {
            uint32_t dst = 0, src = 0;
            uc_reg_read(uc, UC_ARM_REG_R0, &dst);
            uc_reg_read(uc, UC_ARM_REG_R1, &src);
            if (dst != 0 && src != 0) {
                char c = 0;
                uint32_t offset = 0;
                while (uc_mem_read(uc, src + offset, &c, 1) == UC_ERR_OK && offset < 4096) {
                    uc_mem_write(uc, dst + offset, &c, 1);
                    if (c == '\0') break;
                    offset++;
                }
            }
            uc_reg_write(uc, UC_ARM_REG_R0, &dst);
            return;
        }

        if (name == "strncpy") {
            uint32_t dst = 0, src = 0, n = 0;
            uc_reg_read(uc, UC_ARM_REG_R0, &dst);
            uc_reg_read(uc, UC_ARM_REG_R1, &src);
            uc_reg_read(uc, UC_ARM_REG_R2, &n);
            if (dst != 0 && src != 0 && n > 0) {
                char c = 0;
                bool padNull = false;
                for (uint32_t i = 0; i < n; ++i) {
                    if (!padNull) {
                        if (uc_mem_read(uc, src + i, &c, 1) == UC_ERR_OK) {
                            uc_mem_write(uc, dst + i, &c, 1);
                            if (c == '\0') padNull = true;
                        }
                    } else {
                        char z = 0;
                        uc_mem_write(uc, dst + i, &z, 1);
                    }
                }
            }
            uc_reg_write(uc, UC_ARM_REG_R0, &dst);
            return;
        }

        if (name == "strcmp") {
            uint32_t s1 = 0, s2 = 0;
            uc_reg_read(uc, UC_ARM_REG_R0, &s1);
            uc_reg_read(uc, UC_ARM_REG_R1, &s2);
            int res = 0;
            if (s1 != 0 && s2 != 0) {
                char c1 = 0, c2 = 0;
                uint32_t offset = 0;
                while (offset < 4096) {
                    uc_mem_read(uc, s1 + offset, &c1, 1);
                    uc_mem_read(uc, s2 + offset, &c2, 1);
                    if (c1 != c2 || c1 == '\0') {
                        res = static_cast<int>(c1) - static_cast<int>(c2);
                        break;
                    }
                    offset++;
                }
            }
            uint32_t ret = static_cast<uint32_t>(res);
            uc_reg_write(uc, UC_ARM_REG_R0, &ret);
            return;
        }

        if (name == "strncmp") {
            uint32_t s1 = 0, s2 = 0, n = 0;
            uc_reg_read(uc, UC_ARM_REG_R0, &s1);
            uc_reg_read(uc, UC_ARM_REG_R1, &s2);
            uc_reg_read(uc, UC_ARM_REG_R2, &n);
            int res = 0;
            if (s1 != 0 && s2 != 0 && n > 0) {
                char c1 = 0, c2 = 0;
                for (uint32_t i = 0; i < n; ++i) {
                    uc_mem_read(uc, s1 + i, &c1, 1);
                    uc_mem_read(uc, s2 + i, &c2, 1);
                    if (c1 != c2 || c1 == '\0') {
                        res = static_cast<int>(c1) - static_cast<int>(c2);
                        break;
                    }
                }
            }
            uint32_t ret = static_cast<uint32_t>(res);
            uc_reg_write(uc, UC_ARM_REG_R0, &ret);
            return;
        }

        if (name == "strdup") {
            uint32_t sAddr = 0;
            uc_reg_read(uc, UC_ARM_REG_R0, &sAddr);
            uint32_t newAddr = 0;
            if (sAddr != 0) {
                std::string s;
                char c = 0;
                uint32_t off = 0;
                while (uc_mem_read(uc, sAddr + off, &c, 1) == UC_ERR_OK && c != '\0' && off < 4096) {
                    s.push_back(c);
                    off++;
                }
                newAddr = sHeapCurrent;
                sHeapCurrent += (s.size() + 16) & ~15U;
                uc_mem_write(uc, newAddr, s.c_str(), s.size() + 1);
            }
            uc_reg_write(uc, UC_ARM_REG_R0, &newAddr);
            return;
        }

        // 4. File I/O Libc (fopen, fread, fwrite, fseek, ftell, fclose)
        if (name == "fopen" || name == "fopen64") {
            uint32_t pathAddr = 0, modeAddr = 0;
            uc_reg_read(uc, UC_ARM_REG_R0, &pathAddr);
            uc_reg_read(uc, UC_ARM_REG_R1, &modeAddr);
            uint32_t fileHandle = 0;

            if (pathAddr != 0 && modeAddr != 0) {
                char pathBuf[512] = {0};
                char modeBuf[32] = {0};
                uc_mem_read(uc, pathAddr, pathBuf, sizeof(pathBuf) - 1);
                uc_mem_read(uc, modeAddr, modeBuf, sizeof(modeBuf) - 1);

                std::string resolved = SyscallDispatcher::resolveFilePath(pathBuf);
                FILE* fp = fopen(resolved.c_str(), modeBuf);
                if (fp) {
                    std::lock_guard<std::mutex> lock(sFileMutex);
                    fileHandle = sNextFileHandle++;
                    sFileTable[fileHandle] = fp;
                    LOGI("fopen riuscito: '%s' (Handle: 0x%08x)", resolved.c_str(), fileHandle);
                } else {
                    LOGW("fopen fallito: '%s' (mode: %s, errno: %d)", resolved.c_str(), modeBuf, errno);
                }
            }
            uc_reg_write(uc, UC_ARM_REG_R0, &fileHandle);
            return;
        }

        if (name == "fread") {
            uint32_t ptr = 0, size = 0, nmemb = 0, handle = 0;
            uc_reg_read(uc, UC_ARM_REG_R0, &ptr);
            uc_reg_read(uc, UC_ARM_REG_R1, &size);
            uc_reg_read(uc, UC_ARM_REG_R2, &nmemb);
            uc_reg_read(uc, UC_ARM_REG_R3, &handle);
            uint32_t readCount = 0;

            FILE* fp = nullptr;
            {
                std::lock_guard<std::mutex> lock(sFileMutex);
                auto fit = sFileTable.find(handle);
                if (fit != sFileTable.end()) fp = fit->second;
            }

            if (fp && size > 0 && nmemb > 0 && ptr != 0) {
                std::vector<uint8_t> buf(size * nmemb);
                size_t r = fread(buf.data(), size, nmemb, fp);
                if (r > 0) {
                    uc_mem_write(uc, ptr, buf.data(), r * size);
                }
                readCount = static_cast<uint32_t>(r);
            }
            uc_reg_write(uc, UC_ARM_REG_R0, &readCount);
            return;
        }

        if (name == "fseek" || name == "fseeko" || name == "fseeko64") {
            uint32_t handle = 0, offset = 0, whence = 0;
            uc_reg_read(uc, UC_ARM_REG_R0, &handle);
            uc_reg_read(uc, UC_ARM_REG_R1, &offset);
            uc_reg_read(uc, UC_ARM_REG_R2, &whence);
            int res = -1;

            FILE* fp = nullptr;
            {
                std::lock_guard<std::mutex> lock(sFileMutex);
                auto fit = sFileTable.find(handle);
                if (fit != sFileTable.end()) fp = fit->second;
            }

            if (fp) {
                res = fseek(fp, static_cast<long>(static_cast<int32_t>(offset)), static_cast<int>(whence));
            }
            uint32_t ret = static_cast<uint32_t>(res);
            uc_reg_write(uc, UC_ARM_REG_R0, &ret);
            return;
        }

        if (name == "ftell" || name == "ftello" || name == "ftello64") {
            uint32_t handle = 0;
            uc_reg_read(uc, UC_ARM_REG_R0, &handle);
            long pos = -1;

            FILE* fp = nullptr;
            {
                std::lock_guard<std::mutex> lock(sFileMutex);
                auto fit = sFileTable.find(handle);
                if (fit != sFileTable.end()) fp = fit->second;
            }

            if (fp) {
                pos = ftell(fp);
            }
            uint32_t ret = static_cast<uint32_t>(pos);
            uc_reg_write(uc, UC_ARM_REG_R0, &ret);
            return;
        }

        if (name == "fclose") {
            uint32_t handle = 0;
            uc_reg_read(uc, UC_ARM_REG_R0, &handle);
            int res = 0;

            std::lock_guard<std::mutex> lock(sFileMutex);
            auto fit = sFileTable.find(handle);
            if (fit != sFileTable.end()) {
                if (fit->second) fclose(fit->second);
                sFileTable.erase(fit);
            }
            uint32_t ret = static_cast<uint32_t>(res);
            uc_reg_write(uc, UC_ARM_REG_R0, &ret);
            return;
        }

        // 5. Dynamic Linker
        if (name == "dlopen") {
            uint32_t handle = 0x50000001;
            uc_reg_write(uc, UC_ARM_REG_R0, &handle);
            return;
        }

        if (name == "dlsym") {
            uint32_t symNameAddr = 0;
            uc_reg_read(uc, UC_ARM_REG_R1, &symNameAddr);
            uint32_t trampAddr = 0;

            if (symNameAddr != 0) {
                char symNameBuf[128] = {0};
                uc_mem_read(uc, symNameAddr, symNameBuf, sizeof(symNameBuf) - 1);
                trampAddr = registerSymbolTrampoline(symNameBuf, uc);
                LOGI("dlsym('%s') risolto tramite Trampoline a 0x%08x", symNameBuf, trampAddr);
            }
            uc_reg_write(uc, UC_ARM_REG_R0, &trampAddr);
            return;
        }

        if (name == "dlclose" || name == "dlerror") {
            uint32_t zero = 0;
            uc_reg_write(uc, UC_ARM_REG_R0, &zero);
            return;
        }

        // 6. Concorrenza Pthread
        if (name == "pthread_mutex_init" || name == "pthread_mutex_lock" ||
            name == "pthread_mutex_unlock" || name == "pthread_mutex_destroy" ||
            name == "pthread_once") {
            uint32_t zero = 0;
            uc_reg_write(uc, UC_ARM_REG_R0, &zero);
            return;
        }

        if (name == "pthread_key_create") {
            uint32_t keyAddr = 0;
            uc_reg_read(uc, UC_ARM_REG_R0, &keyAddr);
            if (keyAddr != 0) {
                uint32_t dummyKey = 1;
                uc_mem_write(uc, keyAddr, &dummyKey, sizeof(dummyKey));
            }
            uint32_t zero = 0;
            uc_reg_write(uc, UC_ARM_REG_R0, &zero);
            return;
        }

        if (name == "pthread_getspecific") {
            uint32_t zero = 0;
            uc_reg_write(uc, UC_ARM_REG_R0, &zero);
            return;
        }

        if (name == "pthread_setspecific") {
            uint32_t zero = 0;
            uc_reg_write(uc, UC_ARM_REG_R0, &zero);
            return;
        }

        // 7. OpenGL ES 2.0 Stubs sicuri
        if (name == "glGetString") {
            uint32_t nameParam = 0;
            uc_reg_read(uc, UC_ARM_REG_R0, &nameParam);
            static const char sGlesVersion[] = "OpenGL ES 2.0 Emux32";
            static const char sGlesRenderer[] = "Adreno (Emux32 DBT Translator)";
            static const char sGlesVendor[] = "Qualcomm";
            static const char sGlesExt[] = "GL_OES_texture_npot GL_OES_rgb8_rgba8 GL_EXT_texture_format_BGRA8888";

            const char* targetStr = sGlesVersion;
            if (nameParam == 0x1F01) targetStr = sGlesRenderer; // GL_RENDERER
            if (nameParam == 0x1F00) targetStr = sGlesVendor;   // GL_VENDOR
            if (nameParam == 0x1F03) targetStr = sGlesExt;      // GL_EXTENSIONS

            uint32_t strAddr = sHeapCurrent;
            size_t len = std::strlen(targetStr) + 1;
            sHeapCurrent += (len + 15) & ~15U;
            uc_mem_write(uc, strAddr, targetStr, len);
            uc_reg_write(uc, UC_ARM_REG_R0, &strAddr);
            return;
        }

        if (name == "glGetIntegerv") {
            uint32_t pname = 0, paramsAddr = 0;
            uc_reg_read(uc, UC_ARM_REG_R0, &pname);
            uc_reg_read(uc, UC_ARM_REG_R1, &paramsAddr);
            if (paramsAddr != 0) {
                int32_t val[4] = {0, 0, 1920, 1080}; // Viewport default FHD
                uc_mem_write(uc, paramsAddr, val, sizeof(val));
            }
            return;
        }

        if (name == "glGetError") {
            uint32_t noError = 0; // GL_NO_ERROR
            uc_reg_write(uc, UC_ARM_REG_R0, &noError);
            return;
        }

        if (name == "glGenTextures" || name == "glGenBuffers" || name == "glGenFramebuffers" || name == "glGenRenderbuffers") {
            uint32_t count = 0, ptr = 0;
            uc_reg_read(uc, UC_ARM_REG_R0, &count);
            uc_reg_read(uc, UC_ARM_REG_R1, &ptr);
            if (count > 0 && ptr != 0) {
                static uint32_t sNextGlId = 1;
                std::vector<uint32_t> ids(count);
                for (uint32_t i = 0; i < count; ++i) ids[i] = sNextGlId++;
                uc_mem_write(uc, ptr, ids.data(), count * sizeof(uint32_t));
            }
            return;
        }

        // Funzioni GLES grafiche generiche (no-op sicure per evitare crash)
        if (name.rfind("gl", 0) == 0 || name.rfind("egl", 0) == 0) {
            uint32_t retVal = 1;
            uc_reg_write(uc, UC_ARM_REG_R0, &retVal);
            return;
        }

        // Fallback generico per qualsiasi altro simbolo Bionic / libc
        LOGI("Trampoline invocato per simbolo: %s (ritorno default 0)", name.c_str());
        uint32_t retVal = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &retVal);
    }
}

} // namespace emux
