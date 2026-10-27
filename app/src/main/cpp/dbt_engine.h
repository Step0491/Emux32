#pragma once

#include "elf_loader.h"
#include "unicorn/unicorn.h"
#include "unicorn/arm.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/native_window.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <time.h>

struct ANativeWindow;

namespace emux {

struct TouchEventItem {
    int action{0};
    float x{0.0f};
    float y{0.0f};
    int pointerId{0};
};

struct PerformanceGovernor {
    uint32_t targetFps{60};
    uint64_t frameTimeTargetNs{16666666ULL};
    bool throttleGuardEnabled{true};
    std::atomic<bool> isRunning{false};

    void configure(uint32_t fps, bool throttleGuard) {
        targetFps = fps > 0 ? fps : 60;
        frameTimeTargetNs = 1000000000ULL / targetFps;
        throttleGuardEnabled = throttleGuard;
    }

    inline void syncFrame(std::chrono::steady_clock::time_point frameStart) {
        if (!throttleGuardEnabled) return;
        auto now = std::chrono::steady_clock::now();
        auto elapsedNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - frameStart).count()
        );
        if (elapsedNs < frameTimeTargetNs) {
            uint64_t sleepNs = frameTimeTargetNs - elapsedNs;
            struct timespec req{};
            req.tv_sec = static_cast<time_t>(sleepNs / 1000000000ULL);
            req.tv_nsec = static_cast<long>(sleepNs % 1000000000ULL);
            nanosleep(&req, nullptr);
        }
    }
};

class DbtEngine {
public:
    DbtEngine();
    ~DbtEngine();

    bool initialize(size_t guestRamSize = 1024 * 1024 * 512);
    void configurePerformance(uint32_t targetFps, bool enableFhd, bool throttleGuard);
    bool loadLibrary(const std::string& libraryPath);
    bool startEmulation();
    bool executeEntryPoint() { return startEmulation(); }
    void requestStop();

    void onInterrupt(uint32_t intno);
    void setNativeSurface(ANativeWindow* window);
    bool injectTouchEvent(int action, float x, float y, int pointerId);

    bool initEgl();
    void destroyEgl();
    void swapBuffers();

    PerformanceGovernor& getGovernor() { return governor_; }
    uc_engine* getUnicorn() const { return uc_; }

private:
    void setupGuestStack();
    bool ensureWorkerEgl(bool waitIfNull);
    void processPendingTouchEvents();
    static void hookInterruptCallback(uc_engine* uc, uint32_t intno, void* userData);

    uc_engine* uc_{nullptr};
    uc_hook intrHook_{0};
    std::shared_ptr<LoadedLibrary> currentLib_{nullptr};
    std::vector<std::shared_ptr<LoadedLibrary>> loadedLibs_;
    Elf32Loader loader_;
    PerformanceGovernor governor_;

    std::mutex surfaceMutex_;
    std::condition_variable surfaceCv_;
    ANativeWindow* nativeWindow_{nullptr};
    ANativeWindow* boundEglWindow_{nullptr};
    std::atomic<bool> surfaceDirty_{false};

    std::mutex touchMutex_;
    std::deque<TouchEventItem> touchQueue_;

    EGLDisplay eglDisplay_{EGL_NO_DISPLAY};
    EGLSurface eglSurface_{EGL_NO_SURFACE};
    EGLContext eglContext_{EGL_NO_CONTEXT};
    EGLConfig  eglConfig_{nullptr};

    uint32_t guestStackBase_{0x7E000000};
    uint32_t guestStackSize_{2 * 1024 * 1024}; // 2MB Stack dedicato al processo ospite
    uint32_t guestStackPointer_{0};
    bool isInitialized_{false};
    std::atomic<bool> stopRequested_{false};
};

} // namespace emux
