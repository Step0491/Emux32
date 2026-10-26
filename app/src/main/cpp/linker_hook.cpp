#include "linker_hook.h"
#include "unicorn/arm.h"

#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <cstring>
#include <cerrno>
#include <mutex>
#include <vector>

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
            return -1;
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
            return static_cast<int32_t>(bytesRead);
        }

        case ARM_NR_open: {
            uint32_t pathAddr = r0;
            int flags = static_cast<int>(r1);
            mode_t mode = static_cast<mode_t>(r2);
            if (pathAddr == 0) return -1;

            char pathBuffer[512] = {0};
            if (uc && uc_mem_read(uc, pathAddr, pathBuffer, sizeof(pathBuffer) - 1) == UC_ERR_OK) {
                pathBuffer[sizeof(pathBuffer) - 1] = '\0';
                return open(pathBuffer, flags, mode);
            }
            return -1;
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
            int request = static_cast<int>(r1);
            return ioctl(fd, request, nullptr);
        }

        case ARM_NR_mmap2: {
            uint32_t guestAddr = r0;
            size_t length = static_cast<size_t>(r1);
            if (guestAddr == 0) guestAddr = 0x60000000;
            uint32_t alignedBase = guestAddr & ~0xFFFU;
            uint32_t alignedSize = (length + 0xFFFU) & ~0xFFFU;

            if (uc) {
                uc_mem_map(uc, alignedBase, alignedSize, UC_PROT_ALL);
            }
            return static_cast<int32_t>(alignedBase);
        }

        case ARM_NR_mprotect: {
            return 0;
        }

        case ARM_NR_clock_gettime: {
            clockid_t clk_id = static_cast<clockid_t>(r0);
            uint32_t tpAddr = r1;
            if (tpAddr == 0) return -1;

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
            if (tvAddr == 0) return -1;

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

    // 1. Alloca regione JNI (0x7D000000, 64KB allineata)
    uc_mem_map(uc, guestJniBase_, 0x10000, UC_PROT_READ | UC_PROT_WRITE);

    // 2. Alloca regione Trampoline (0x70000000, 64KB allineata)
    uc_mem_map(uc, guestTrampolineBase_, 0x10000, UC_PROT_ALL);

    // 3. Inizializza struttura JavaVM32
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

    // 4. Inizializza struttura JNIEnv32
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
    uint32_t stubGetVersion[2]       = {0xEF003002, 0xE12FFF1E};
    uint32_t stubFindClass[2]        = {0xEF003003, 0xE12FFF1E};
    uint32_t stubGetMethodID[2]      = {0xEF003004, 0xE12FFF1E};
    uint32_t stubGetStaticMethodID[2]= {0xEF003005, 0xE12FFF1E};
    uint32_t stubRegisterNatives[2]  = {0xEF003006, 0xE12FFF1E};

    uc_mem_write(uc, guestJniBase_ + 0x300, stubGetVersion, sizeof(stubGetVersion));
    uc_mem_write(uc, guestJniBase_ + 0x308, stubFindClass, sizeof(stubFindClass));
    uc_mem_write(uc, guestJniBase_ + 0x310, stubGetMethodID, sizeof(stubGetMethodID));
    uc_mem_write(uc, guestJniBase_ + 0x318, stubGetStaticMethodID, sizeof(stubGetStaticMethodID));
    uc_mem_write(uc, guestJniBase_ + 0x320, stubRegisterNatives, sizeof(stubRegisterNatives));

    LOGI("GuestRuntimeEnvironment inizializzato: JavaVM32=0x%08x, JNIEnv32=0x%08x",
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
        uint32_t ret = 0; // JNI_OK
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        LOGI("JNI: Metodi nativi registrati con successo (RegisterNatives).");
        return;
    }

    // Gestione simboli esterni registrati (libc / OpenGL ES)
    auto it = trampolineToSymbolMap_.find(trampolineId - 0x2000);
    if (it != trampolineToSymbolMap_.end()) {
        const std::string& name = it->second;

        if (name == "__android_log_print") {
            char tag[64] = {0};
            char msg[256] = {0};
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

        if (name == "malloc") {
            uint32_t size = 0;
            uc_reg_read(uc, UC_ARM_REG_R0, &size);
            static uint32_t sHeapCurrent = 0x65000000;
            uint32_t allocAddr = sHeapCurrent;
            sHeapCurrent += (size + 15) & ~15U;
            uc_reg_write(uc, UC_ARM_REG_R0, &allocAddr);
            return;
        }

        if (name == "free") {
            return;
        }

        // Simbolo generico inoltrato all'host
        LOGI("Trampoline invocato per simbolo: %s", name.c_str());
        uint32_t retVal = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &retVal);
    }
}

} // namespace emux
