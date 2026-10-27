#pragma once

#include <cstdint>
#include <unordered_map>
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <jni.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include "unicorn/unicorn.h"

namespace emux {

struct PerformanceGovernor;
struct LoadedLibrary;

struct RegisteredNativeMethod {
    std::string className;
    std::string name;
    std::string signature;
    uint32_t fnPtr{0};
};

struct GuestJniMethodInfo {
    std::string className;
    std::string name;
    std::string signature;
    bool isStatic{false};
};

struct GuestJniFieldInfo {
    std::string className;
    std::string name;
    std::string signature;
    bool isStatic{false};
};

using jint32_t = int32_t;
using jlong32_t = int64_t;
using jptr32_t = uint32_t;

class JniHandleTable {
public:
    static JniHandleTable& getInstance();

    jptr32_t registerObject(jobject obj);
    jobject getObject(jptr32_t handle);
    void unregisterObject(jptr32_t handle);

    jptr32_t registerMethod(jmethodID method);
    jmethodID getMethod(jptr32_t handle);

private:
    JniHandleTable();
    jptr32_t nextHandle_{0x1000};
    std::unordered_map<jptr32_t, jobject> objectMap_;
    std::unordered_map<jptr32_t, jmethodID> methodMap_;
};

// Tabella JNIInvokeInterface32 standard a 8 slot (JavaVM)
struct JNIInvokeInterface32 {
    jptr32_t slots[8];
};

struct JavaVM32 {
    jptr32_t functions;
};

// Tabella JNINativeInterface32 standard completa a 256 slot (JNIEnv)
// Slot 215 (offset 0x35C) = RegisterNatives conforme all'ABI Android NDK
struct JNINativeInterface32 {
    jptr32_t slots[256];
};

struct JNIEnv32 {
    jptr32_t functions;
};

// Dispatcher Syscall Linux ARM32 -> Host Android 64-bit
class SyscallDispatcher {
public:
    static void setStoragePath(const std::string& path);
    static std::string resolveFilePath(const char* guestPath);

    static int32_t handleSyscall(
        uint32_t syscallNum,
        uint32_t r0,
        uint32_t r1,
        uint32_t r2,
        uint32_t r3,
        uint32_t r4,
        uint32_t r5,
        uc_engine* uc
    );
};

class SymbolResolver {
public:
    static void* resolveHostSymbol(const char* symbolName);
};

class GuestRuntimeEnvironment {
public:
    static GuestRuntimeEnvironment& getInstance();

    bool initialize(uc_engine* uc);
    void setGameContext(
        const std::string& pkg,
        const std::string& apkPath,
        const std::string& obbDir,
        const std::string& nativeLibDir,
        const std::string& filesDir
    );

    const std::string& getPackageName() const { return packageName_; }
    const std::string& getApkPath() const { return apkPath_; }
    const std::string& getObbDir() const { return obbDir_; }
    const std::string& getNativeLibDir() const { return nativeLibDir_; }
    const std::string& getFilesDir() const { return filesDir_; }

    uint32_t getJavaVMPointer() const { return guestJavaVMAddr_; }
    uint32_t getJNIEnvPointer() const { return guestJNIEnvAddr_; }
    uint32_t getCtypePointer() const { return guestJniBase_ + 0x2400U; }
    uint32_t getSFilePointer() const { return guestJniBase_ + 0x2100U; }
    uint32_t getStackChkGuardPointer() const { return guestJniBase_ + 0x2010U; }

    uint32_t registerSymbolTrampoline(const std::string& symbolName, uc_engine* uc);
    uint32_t resolveOrRegisterSymbol(const std::string& symbolName, uc_engine* uc);
    void registerLoadedLibrary(const std::shared_ptr<LoadedLibrary>& lib);
    uint32_t findExportedSymbol(const std::string& symbolName) const;

    uint32_t createGuestString(uc_engine* uc, const std::string& str);
    uint32_t createGuestJString(uc_engine* uc, const std::string& str);

    void handleTrampolineCall(uint32_t trampolineId, uc_engine* uc);

    const std::vector<RegisteredNativeMethod>& getRegisteredMethods() const { return registeredMethods_; }
    void addRegisteredMethod(const std::string& cls, const std::string& name, const std::string& sig, uint32_t fnPtr) {
        registeredMethods_.push_back({cls, name, sig, fnPtr});
    }

    void setLastTouchState(int action, float x, float y, int pointerId) {
        lastTouchAction_.store(action, std::memory_order_relaxed);
        lastTouchX_.store(x, std::memory_order_relaxed);
        lastTouchY_.store(y, std::memory_order_relaxed);
        lastTouchPointerId_.store(pointerId, std::memory_order_relaxed);
    }

    void setEglState(EGLDisplay dpy, EGLSurface surf, EGLContext ctx, PerformanceGovernor* gov) {
        eglDisplay_ = dpy;
        eglSurface_ = surf;
        eglContext_ = ctx;
        governor_ = gov;
    }

    void swapBuffers() {
        if (eglDisplay_ != EGL_NO_DISPLAY && eglSurface_ != EGL_NO_SURFACE) {
            ::eglSwapBuffers(eglDisplay_, eglSurface_);
            swappedThisFrame_.store(true, std::memory_order_relaxed);
        }
    }

    bool consumeFrameSwapFlag() {
        return swappedThisFrame_.exchange(false, std::memory_order_relaxed);
    }

    EGLDisplay getEglDisplay() const { return eglDisplay_; }
    EGLSurface getEglSurface() const { return eglSurface_; }
    EGLContext getEglContext() const { return eglContext_; }

private:
    GuestRuntimeEnvironment();
    void handleJniCall(uint32_t slotIndex, uc_engine* uc);
    void handleJavaVmCall(uint32_t slotIndex, uc_engine* uc);

    uint32_t guestJniBase_{0x7D000000};
    uint32_t guestJavaVMAddr_{0x7D000000};
    uint32_t guestJNIEnvAddr_{0x7D000200};
    uint32_t guestTlsBase_{0x7D020000};

    uint32_t guestTrampolineBase_{0x70000000};
    uint32_t nextTrampolineId_{1};

    std::unordered_map<std::string, uint32_t> symbolToTrampolineMap_;
    std::unordered_map<uint32_t, std::string> trampolineToSymbolMap_;

    uint32_t nextMockHandle_{0x2000};
    std::unordered_map<uint32_t, std::string> classHandleMap_;
    std::unordered_map<uint32_t, GuestJniMethodInfo> methodHandleMap_;
    std::unordered_map<uint32_t, GuestJniFieldInfo> fieldHandleMap_;
    std::unordered_map<uint32_t, std::string> jstringMap_;
    std::unordered_map<uint32_t, uint32_t> jstringGuestPtrMap_;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> arrayHandleMap_; // handle -> (guestBufPtr, lengthElements)

    std::vector<std::shared_ptr<LoadedLibrary>> loadedLibraries_;
    std::unordered_map<uint32_t, std::shared_ptr<LoadedLibrary>> dlopenHandleMap_;
    uint32_t nextDlHandle_{0x50000010};

    std::string packageName_{"com.turner.cardwars"};
    std::string apkPath_;
    std::string obbDir_;
    std::string nativeLibDir_;
    std::string filesDir_{"/data/user/0/com.emux.translator/files"};

    std::atomic<int> lastTouchAction_{0};
    std::atomic<float> lastTouchX_{0.0f};
    std::atomic<float> lastTouchY_{0.0f};
    std::atomic<int> lastTouchPointerId_{0};
    std::atomic<bool> swappedThisFrame_{false};

    std::vector<RegisteredNativeMethod> registeredMethods_;
    EGLDisplay eglDisplay_{EGL_NO_DISPLAY};
    EGLSurface eglSurface_{EGL_NO_SURFACE};
    EGLContext eglContext_{EGL_NO_CONTEXT};
    PerformanceGovernor* governor_{nullptr};
};

} // namespace emux
