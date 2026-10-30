#include "linker_hook.h"
#include "elf_loader.h"
#include "core/syscall_thunker.h"
#include "core/audio_thunker.h"
#include "core/guest_thread.h"
#include "core/guest_memory.h"
#include "core/gles_virtualizer.h"
#include "core/guest_signal.h"
#include "core/guest_proc_fs.h"
#include "unicorn/arm.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

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
#include <dirent.h>
#include <sys/mman.h>
#include <zlib.h>
#include "core/vfs_zip.h"
#include <cstring>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <thread>
#include <sched.h>

#define TAG "EmuxLinkerHook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace emux {

static std::string sCustomStoragePath = "/sdcard/Emux32";

static std::unordered_map<uint32_t, FILE*> sFileTable;
static uint32_t sNextFileHandle = 0x30000001;
static std::mutex sFileMutex;

static std::unordered_map<uint32_t, z_stream*> sZlibStreamMap;
static uint32_t sNextZlibHandle = 0x60000001;

static std::mutex sSyncTableMutex;
static std::unordered_map<uint32_t, std::shared_ptr<std::recursive_mutex>> sHostPthreadMutexes;
static std::unordered_map<uint32_t, std::shared_ptr<std::condition_variable_any>> sHostPthreadConds;

static std::shared_ptr<std::recursive_mutex> getOrCreateGuestMutex(uint32_t addr) {
    if (addr == 0) return nullptr;
    std::lock_guard<std::mutex> lock(sSyncTableMutex);
    auto it = sHostPthreadMutexes.find(addr);
    if (it != sHostPthreadMutexes.end()) return it->second;
    auto m = std::make_shared<std::recursive_mutex>();
    sHostPthreadMutexes[addr] = m;
    return m;
}

static std::shared_ptr<std::condition_variable_any> getOrCreateGuestCond(uint32_t addr) {
    if (addr == 0) return nullptr;
    std::lock_guard<std::mutex> lock(sSyncTableMutex);
    auto it = sHostPthreadConds.find(addr);
    if (it != sHostPthreadConds.end()) return it->second;
    auto c = std::make_shared<std::condition_variable_any>();
    sHostPthreadConds[addr] = c;
    return c;
}

static inline std::string readGuestCString(uc_engine* uc, uint32_t addr, size_t maxLen = 512) {
    if (!uc || addr == 0) return "";
    std::string out;
    out.reserve(64);
    char chunk[64];
    size_t readTotal = 0;
    while (readTotal < maxLen) {
        size_t toRead = std::min<size_t>(sizeof(chunk), maxLen - readTotal);
        if (uc_mem_read(uc, addr + readTotal, chunk, toRead) != UC_ERR_OK) break;
        for (size_t i = 0; i < toRead; ++i) {
            if (chunk[i] == '\0') return out;
            out.push_back(chunk[i]);
        }
        readTotal += toRead;
    }
    return out;
}

#pragma pack(push, 4)
struct GuestHeapHeader {
    uint32_t magic;    // 0x48454150 ("HEAP")
    uint32_t size;     // Dimensione richiesta dal chiamante
    uint32_t capacity; // Capacità utile del blocco allocato
    uint32_t is_free;  // 1 se libero, 0 se in uso
};
#pragma pack(pop)

struct GuestFreeBlock {
    uint32_t payloadAddr;
    uint32_t capacity;
};

static std::vector<GuestFreeBlock> sHeapFreeList;
static std::mutex sHeapMutex;

static inline uint32_t allocGuestHeap(uc_engine* uc, uint32_t size, uint32_t align = 16) {
    std::lock_guard<std::mutex> lock(sHeapMutex);
    if (size == 0) size = 16;
    if (align < 16) align = 16;
    uint32_t neededCap = (size + 15U) & ~15U;

    // 1. Ricerca nella free list dei blocchi riutilizzabili (first-fit)
    for (auto it = sHeapFreeList.begin(); it != sHeapFreeList.end(); ++it) {
        if (it->capacity >= neededCap && (it->payloadAddr % align == 0)) {
            uint32_t foundAddr = it->payloadAddr;
            uint32_t foundCap = it->capacity;
            sHeapFreeList.erase(it);

            uint32_t hdrAddr = foundAddr - sizeof(GuestHeapHeader);
            GuestHeapHeader hdr{0x48454150U, size, foundCap, 0};
            if (uc) uc_mem_write(uc, hdrAddr, &hdr, sizeof(hdr));

            return foundAddr;
        }
    }

    // 2. Allocazione tramite il gestore VMA di GuestMemory
    uint32_t totalAlloc = sizeof(GuestHeapHeader) + neededCap + (align - 1U);
    uint32_t raw = GuestMemory::getSharedInstance().allocateHeap(totalAlloc, 16, uc);
    if (raw == 0) return 0;

    uint32_t payloadAddr = (raw + sizeof(GuestHeapHeader) + (align - 1U)) & ~(align - 1U);
    uint32_t headerAddr = payloadAddr - sizeof(GuestHeapHeader);

    GuestHeapHeader hdr{0x48454150U, size, neededCap, 0};
    if (uc) {
        uc_mem_write(uc, headerAddr, &hdr, sizeof(hdr));
    }
    return payloadAddr;
}

static inline void freeGuestHeap(uint32_t ptr, uc_engine* uc) {
    if (ptr < 0x60000000U) return;
    std::lock_guard<std::mutex> lock(sHeapMutex);
    uint32_t hdrAddr = ptr - sizeof(GuestHeapHeader);
    GuestHeapHeader hdr{};
    if (uc && uc_mem_read(uc, hdrAddr, &hdr, sizeof(hdr)) == UC_ERR_OK) {
        if (hdr.magic == 0x48454150U && hdr.is_free == 0) {
            hdr.is_free = 1;
            uc_mem_write(uc, hdrAddr, &hdr, sizeof(hdr));
            sHeapFreeList.push_back({ptr, hdr.capacity});
        }
    }
}

static inline uint32_t reallocGuestHeap(uc_engine* uc, uint32_t oldPtr, uint32_t newSize) {
    if (oldPtr == 0) return allocGuestHeap(uc, newSize, 16);
    if (newSize == 0) {
        freeGuestHeap(oldPtr, uc);
        return 0;
    }

    std::unique_lock<std::mutex> lock(sHeapMutex);
    uint32_t hdrAddr = oldPtr - sizeof(GuestHeapHeader);
    GuestHeapHeader hdr{};
    bool validHdr = false;
    if (uc && uc_mem_read(uc, hdrAddr, &hdr, sizeof(hdr)) == UC_ERR_OK && hdr.magic == 0x48454150U) {
        validHdr = true;
        if (hdr.capacity >= newSize) {
            hdr.size = newSize;
            uc_mem_write(uc, hdrAddr, &hdr, sizeof(hdr));
            return oldPtr;
        }
    }
    lock.unlock();

    uint32_t newPtr = allocGuestHeap(uc, newSize, 16);
    if (newPtr != 0 && oldPtr != 0) {
        size_t copyBytes = validHdr ? std::min<size_t>(hdr.size, newSize) : newSize;
        if (copyBytes > 0 && copyBytes < 0x04000000U) {
            std::vector<uint8_t> buf(copyBytes, 0);
            uc_mem_read(uc, oldPtr, buf.data(), copyBytes);
            uc_mem_write(uc, newPtr, buf.data(), copyBytes);
        }
        freeGuestHeap(oldPtr, uc);
    }
    return newPtr;
}

// Strutture per tracciamento directory POSIX guest
struct GuestDirEntry {
    DIR* hostDir{nullptr};
    uint32_t direntBufAddr{0};
};
static std::unordered_map<uint32_t, GuestDirEntry> sGuestDirMap;
static uint32_t sNextDirHandle = 0x50000001U;
static std::mutex sDirMutex;

#pragma pack(push, 4)
struct guest_dirent32 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[256];
};
#pragma pack(pop)

void SyscallDispatcher::setStoragePath(const std::string& path) {
    sCustomStoragePath = path;
    LOGI("SyscallDispatcher: Percorso storage configurato a '%s'", path.c_str());
}

static void ensureParentDirs(const std::string& filePath) {
    size_t pos = 1;
    while ((pos = filePath.find('/', pos)) != std::string::npos) {
        std::string dir = filePath.substr(0, pos);
        mkdir(dir.c_str(), 0775);
        pos++;
    }
}

void GuestRuntimeEnvironment::swapBuffers() {
    if (eglDisplay_ != EGL_NO_DISPLAY) {
        EGLSurface curDraw = ::eglGetCurrentSurface(EGL_DRAW);
        EGLSurface target = (curDraw != EGL_NO_SURFACE) ? curDraw : eglSurface_;
        if (target != EGL_NO_SURFACE) {
            ::eglSwapBuffers(eglDisplay_, target);
            swappedThisFrame_.store(true, std::memory_order_relaxed);
        }

        // Se un altro thread host ha richiesto il possesso della window surface,
        // cediamo la surface al confine del frame VSYNC
        if (handoverRequested_.load(std::memory_order_relaxed) &&
            windowSurfaceOwnerThread_.load() == pthread_self()) {
            std::lock_guard<std::mutex> lock(eglMutex_);
            ::eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            windowSurfaceOwnerThread_.store(0);
            windowSurfaceOwnerTid_.store(0);
            handoverRequested_.store(false);
            eglHandoverCv_.notify_all();
            LOGI("EGL Handover: Window Surface ceduta con successo su confine di frame (VSYNC swap)");
        }
    }
}

bool GuestRuntimeEnvironment::ensureThreadEglContext() {
    if (eglDisplay_ == EGL_NO_DISPLAY || eglContext_ == EGL_NO_CONTEXT) {
        return false;
    }
    EGLContext current = ::eglGetCurrentContext();
    if (current != EGL_NO_CONTEXT) {
        return true;
    }

    pthread_t self = pthread_self();
    uint32_t currentTid = GuestThreadScheduler::getInstance().getCurrentTid();
    std::lock_guard<std::mutex> lock(eglMutex_);

    // 1. Se la Window Surface (ANativeWindow) non è posseduta da nessun thread,
    // questo thread assume la leadership e diventa il Primary Render Thread
    if (eglSurface_ != EGL_NO_SURFACE && windowSurfaceOwnerThread_.load() == 0) {
        if (::eglMakeCurrent(eglDisplay_, eglSurface_, eglSurface_, eglContext_) == EGL_TRUE) {
            windowSurfaceOwnerThread_.store(self);
            windowSurfaceOwnerTid_.store(currentTid);
            threadCurrentContextHandleMap_[self] = 1;
            threadCurrentSurfaceHandleMap_[self] = 1;
            LOGI("ensureThreadEglContext: Thread %p (TID=%u) ha assunto la proprietà della Window Surface!", (void*)self, currentTid);
            return true;
        }
    }

    // 2. Se la Window Surface è già posseduta da un altro thread attivo (es. Render Thread primario),
    // questo thread è un worker secondario (texture upload / background):
    // crea o riusa un contesto condiviso dedicato con superficie Pbuffer
    EGLContext sharedCtx = EGL_NO_CONTEXT;
    auto scit = threadSharedContextMap_.find(self);
    if (scit != threadSharedContextMap_.end() && scit->second != EGL_NO_CONTEXT) {
        sharedCtx = scit->second;
    } else if (eglConfig_ != nullptr) {
        const EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
        sharedCtx = ::eglCreateContext(eglDisplay_, eglConfig_, eglContext_, ctxAttribs);
        if (sharedCtx != EGL_NO_CONTEXT) {
            threadSharedContextMap_[self] = sharedCtx;
        }
    }

    EGLSurface pbufSurf = EGL_NO_SURFACE;
    auto spit = threadPbufferSurfaceMap_.find(self);
    if (spit != threadPbufferSurfaceMap_.end() && spit->second != EGL_NO_SURFACE) {
        pbufSurf = spit->second;
    } else if (eglConfig_ != nullptr) {
        const EGLint pbufAttribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
        pbufSurf = ::eglCreatePbufferSurface(eglDisplay_, eglConfig_, pbufAttribs);
        if (pbufSurf != EGL_NO_SURFACE) {
            threadPbufferSurfaceMap_[self] = pbufSurf;
        }
    }

    if (sharedCtx != EGL_NO_CONTEXT && pbufSurf != EGL_NO_SURFACE) {
        if (::eglMakeCurrent(eglDisplay_, pbufSurf, pbufSurf, sharedCtx) == EGL_TRUE) {
            threadCurrentContextHandleMap_[self] = 2;
            threadCurrentSurfaceHandleMap_[self] = 2;
            return true;
        }
    }
    return false;
}

uint32_t GuestRuntimeEnvironment::getCurrentThreadEglContextHandle() const {
    pthread_t self = pthread_self();
    auto it = threadCurrentContextHandleMap_.find(self);
    return (it != threadCurrentContextHandleMap_.end()) ? it->second : 0U;
}

uint32_t GuestRuntimeEnvironment::getCurrentThreadEglSurfaceHandle() const {
    pthread_t self = pthread_self();
    auto it = threadCurrentSurfaceHandleMap_.find(self);
    return (it != threadCurrentSurfaceHandleMap_.end()) ? it->second : 0U;
}

bool GuestRuntimeEnvironment::handleEglMakeCurrent(uint32_t drawHandle, uint32_t readHandle, uint32_t ctxHandle, uc_engine* /* uc */) {
    std::unique_lock<std::mutex> lock(eglMutex_);
    if (eglDisplay_ == EGL_NO_DISPLAY) {
        return false;
    }

    pthread_t self = pthread_self();
    uint32_t currentTid = GuestThreadScheduler::getInstance().getCurrentTid();

    // 1. Unbinding esplicito del thread corrente: SOLO SE ctxHandle == 0
    if (ctxHandle == 0) {
        EGLBoolean ok = ::eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (windowSurfaceOwnerThread_.load() == self) {
            windowSurfaceOwnerThread_.store(0);
            windowSurfaceOwnerTid_.store(0);
            handoverRequested_.store(false);
            eglHandoverCv_.notify_all();
            LOGI("EGL Handover: Window Surface (ANativeWindow) rilasciata esplicitamente da host thread %p (TID=%u)",
                 (void*)self, currentTid);
        }
        threadCurrentContextHandleMap_.erase(self);
        threadCurrentSurfaceHandleMap_.erase(self);
        return ok == EGL_TRUE;
    }

    // 2. Risoluzione contesto EGL target
    EGLContext targetCtx = eglContext_;
    auto cit = guestEglContextMap_.find(ctxHandle);
    if (cit != guestEglContextMap_.end() && cit->second != EGL_NO_CONTEXT) {
        targetCtx = cit->second;
    }

    // 3. Risoluzione superficie EGL target
    EGLSurface targetSurf = EGL_NO_SURFACE;
    if (drawHandle != 0) {
        auto sit = guestEglSurfaceMap_.find(drawHandle);
        if (sit != guestEglSurfaceMap_.end() && sit->second != EGL_NO_SURFACE) {
            targetSurf = sit->second;
        } else if (drawHandle == 1 && eglSurface_ != EGL_NO_SURFACE) {
            targetSurf = eglSurface_;
        }
    }

    // 4. Gestione Surfaceless Context per Worker Threads (drawHandle == 0)
    if (drawHandle == 0) {
        // Tentativo 1: Surfaceless nativo (EGL_KHR_surfaceless_context)
        if (::eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, targetCtx) == EGL_TRUE) {
            threadCurrentContextHandleMap_[self] = ctxHandle;
            threadCurrentSurfaceHandleMap_[self] = 0;
            LOGI("EGL: Surfaceless eglMakeCurrent SUCCESSO su worker thread %p (TID=%u, Ctx=0x%x [%p])",
                 (void*)self, currentTid, ctxHandle, targetCtx);
            return true;
        }

        // Tentativo 2: Fallback Pbuffer dedicato per driver che esigono superficie valida
        auto spit = threadPbufferSurfaceMap_.find(self);
        if (spit != threadPbufferSurfaceMap_.end() && spit->second != EGL_NO_SURFACE) {
            targetSurf = spit->second;
        } else if (eglConfig_ != nullptr) {
            const EGLint pbufAttribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
            targetSurf = ::eglCreatePbufferSurface(eglDisplay_, eglConfig_, pbufAttribs);
            if (targetSurf != EGL_NO_SURFACE) {
                threadPbufferSurfaceMap_[self] = targetSurf;
            }
        }
    }

    // Se la superficie target (drawHandle != 0) non esiste, creiamola come pbuffer
    if (targetSurf == EGL_NO_SURFACE && eglConfig_ != nullptr) {
        const EGLint pbufAttribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
        targetSurf = ::eglCreatePbufferSurface(eglDisplay_, eglConfig_, pbufAttribs);
        if (targetSurf != EGL_NO_SURFACE) {
            guestEglSurfaceMap_[drawHandle] = targetSurf;
        }
    }

    bool isWindowSurface = (targetSurf == eglSurface_ && eglSurface_ != EGL_NO_SURFACE);

    // 5. Protocollo di Handover Cooperativo per Window Surface (ANativeWindow)
    if (isWindowSurface) {
        pthread_t owner = windowSurfaceOwnerThread_.load();
        if (owner != 0 && owner != self) {
            LOGI("EGL Handover: Thread %p (TID=%u) richiede Window Surface posseduta da %p. In attesa rilascio...",
                 (void*)self, currentTid, (void*)owner);
            handoverRequested_.store(true);
            eglHandoverCv_.wait_for(lock, std::chrono::milliseconds(100), [&]() {
                return windowSurfaceOwnerThread_.load() == 0;
            });
        }
    }

    // 6. Tentativo di binding primario
    EGLBoolean ok = ::eglMakeCurrent(eglDisplay_, targetSurf, targetSurf, targetCtx);
    if (ok == EGL_TRUE) {
        if (isWindowSurface) {
            windowSurfaceOwnerThread_.store(self);
            windowSurfaceOwnerTid_.store(currentTid);
            handoverRequested_.store(false);
        }
        threadCurrentContextHandleMap_[self] = ctxHandle;
        threadCurrentSurfaceHandleMap_[self] = drawHandle;
        LOGI("EGL Handover: eglMakeCurrent SUCCESSO su thread %p (TID=%u, Ctx=0x%x [%p], Surf=0x%x [%p], Window=%d)",
             (void*)self, currentTid, ctxHandle, targetCtx, drawHandle, targetSurf, isWindowSurface);
        return true;
    }

    EGLint err = ::eglGetError();
    LOGW("EGL Handover: eglMakeCurrent primario fallito (err=0x%x) su thread %p (TID=%u)", err, (void*)self, currentTid);

    // 7. Strategia di Resilienza per EGL_BAD_ACCESS su Worker Threads
    if (err == EGL_BAD_ACCESS && !isWindowSurface) {
        EGLContext sharedCtx = EGL_NO_CONTEXT;
        auto scit = threadSharedContextMap_.find(self);
        if (scit != threadSharedContextMap_.end() && scit->second != EGL_NO_CONTEXT) {
            sharedCtx = scit->second;
        } else if (eglConfig_ != nullptr) {
            const EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
            sharedCtx = ::eglCreateContext(eglDisplay_, eglConfig_, targetCtx, ctxAttribs);
            if (sharedCtx != EGL_NO_CONTEXT) {
                threadSharedContextMap_[self] = sharedCtx;
            }
        }

        EGLSurface pbufSurf = EGL_NO_SURFACE;
        auto spit = threadPbufferSurfaceMap_.find(self);
        if (spit != threadPbufferSurfaceMap_.end() && spit->second != EGL_NO_SURFACE) {
            pbufSurf = spit->second;
        } else if (eglConfig_ != nullptr) {
            const EGLint pbufAttribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
            pbufSurf = ::eglCreatePbufferSurface(eglDisplay_, eglConfig_, pbufAttribs);
            if (pbufSurf != EGL_NO_SURFACE) {
                threadPbufferSurfaceMap_[self] = pbufSurf;
            }
        }

        if (sharedCtx != EGL_NO_CONTEXT && pbufSurf != EGL_NO_SURFACE) {
            if (::eglMakeCurrent(eglDisplay_, pbufSurf, pbufSurf, sharedCtx) == EGL_TRUE) {
                threadCurrentContextHandleMap_[self] = ctxHandle;
                threadCurrentSurfaceHandleMap_[self] = drawHandle;
                LOGI("EGL Handover: Worker Thread %p associato a Contesto Condiviso dedicato %p con Pbuffer %p",
                     (void*)self, sharedCtx, pbufSurf);
                return true;
            }
        }
    }

    return false;
}

std::string GuestRuntimeEnvironment::resolveAssetPath(const std::string& relativePath) {
    if (relativePath.empty()) return "";
    std::string clean = relativePath;
    if (clean.rfind("assets/", 0) == 0) {
        clean = clean.substr(7);
    }
    const std::vector<std::string> tries = {
        filesDir_ + "/assets/" + packageName_ + "/" + clean,
        filesDir_ + "/assets/" + clean,
        filesDir_ + "/assets/" + packageName_ + "/" + relativePath,
        filesDir_ + "/assets/" + relativePath,
        obbDir_ + "/" + clean,
        obbDir_ + "/" + relativePath
    };
    for (const auto& p : tries) {
        if (access(p.c_str(), F_OK) == 0) {
            return p;
        }
    }
    size_t lastSlash = clean.find_last_of('/');
    std::string fname = (lastSlash != std::string::npos) ? clean.substr(lastSlash + 1) : clean;
    std::string fallback = filesDir_ + "/assets/" + packageName_ + "/" + fname;
    if (access(fallback.c_str(), F_OK) == 0) {
        return fallback;
    }
    return "";
}

std::string SyscallDispatcher::resolveFilePath(const char* guestPath) {
    if (!guestPath || guestPath[0] == '\0') return "";
    std::string pathStr(guestPath);
    auto& env = GuestRuntimeEnvironment::getInstance();
    // 0. Intercettazione filesystem virtuale di sistema (/proc e /sys) per ambiente ARMv7-A
    std::string procCand = GuestProcFs::getInstance().resolveProcPath(pathStr);
    if (!procCand.empty()) {
        return procCand;
    }

    const std::string& pkg = env.getPackageName();
    const std::string& fDir = env.getFilesDir();

    // 1. Redirezione sandbox assoluta Android SDK 34-36 per percorsi dati privati (/data/data/<pkg> o /data/user/0/<pkg>)
    if (!pkg.empty() && !fDir.empty()) {
        std::string p1 = "/data/data/" + pkg;
        std::string p2 = "/data/user/0/" + pkg;
        if (pathStr.rfind(p1, 0) == 0) {
            std::string redirected = fDir + "/guest_data/" + pkg + pathStr.substr(p1.size());
            ensureParentDirs(redirected);
            return redirected;
        }
        if (pathStr.rfind(p2, 0) == 0) {
            std::string redirected = fDir + "/guest_data/" + pkg + pathStr.substr(p2.size());
            ensureParentDirs(redirected);
            return redirected;
        }
    }

    // 2. Risoluzione prioritaria assoluta per cache Unity (/unitycache/)
    size_t ucPos = pathStr.find("/unitycache/");
    if (ucPos != std::string::npos) {
        std::string subUc = pathStr.substr(ucPos);
        std::vector<std::string> ucCandidates = {
            sCustomStoragePath + "/obb/" + pkg + subUc,
            sCustomStoragePath + "/data/" + pkg + subUc,
            sCustomStoragePath + "/obb/" + pkg + "/cache" + subUc,
            sCustomStoragePath + "/data/" + pkg + "/cache" + subUc,
            sCustomStoragePath + "/obb/com.glu.gladiator" + subUc,
            sCustomStoragePath + "/data/com.glu.gladiator" + subUc,
            fDir + "/guest_data/" + pkg + subUc,
            fDir + "/guest_data/" + pkg + "/cache" + subUc,
            "/storage/emulated/0/Android/data/" + pkg + subUc,
            "/storage/emulated/0/Android/data/" + pkg + "/cache" + subUc,
            "/storage/emulated/0/Android/data/com.glu.gladiator" + subUc,
            "/storage/emulated/0/Emux32/obb/" + pkg + subUc,
            "/storage/emulated/0/Emux32/data/" + pkg + subUc
        };
        for (const auto& cand : ucCandidates) {
            if (access(cand.c_str(), F_OK) == 0) {
                LOGI("Risoluzione unitycache (esistente): '%s' -> '%s'", guestPath, cand.c_str());
                return cand;
            }
        }
        // Redirezione assoluta per Scoped Storage: scrittura e creazione garantita sotto sandbox
        std::string targetUc = fDir + "/guest_data/" + (pkg.empty() ? "common" : pkg) + "/cache" + subUc;
        ensureParentDirs(targetUc);
        LOGI("Redirezione assoluta unitycache (nuovo): '%s' -> '%s'", guestPath, targetUc.c_str());
        return targetUc;
    }

    // 3. Risoluzione prioritaria assoluta per Android/data/
    size_t dataPos = pathStr.find("/Android/data/");
    if (dataPos != std::string::npos && !pkg.empty()) {
        std::string afterData = pathStr.substr(dataPos + 14);
        size_t nextSlash = afterData.find('/');
        std::string relPath = (nextSlash != std::string::npos) ? afterData.substr(nextSlash) : "";
        if (!relPath.empty()) {
            std::vector<std::string> dataCandidates = {
                sCustomStoragePath + "/obb/" + pkg + relPath,
                sCustomStoragePath + "/data/" + pkg + relPath,
                sCustomStoragePath + "/obb/com.glu.gladiator" + relPath,
                sCustomStoragePath + "/data/com.glu.gladiator" + relPath,
                fDir + "/guest_data/" + pkg + relPath,
                fDir + "/guest_data/" + pkg + "/cache" + relPath,
                "/storage/emulated/0/Emux32/obb/" + pkg + relPath,
                "/storage/emulated/0/Emux32/data/" + pkg + relPath
            };
            if (relPath.rfind("/cache/", 0) == 0) {
                std::string noCache = relPath.substr(6);
                dataCandidates.push_back(sCustomStoragePath + "/obb/" + pkg + noCache);
                dataCandidates.push_back(sCustomStoragePath + "/data/" + pkg + noCache);
                dataCandidates.push_back(fDir + "/guest_data/" + pkg + noCache);
            }
            for (const auto& cand : dataCandidates) {
                if (access(cand.c_str(), F_OK) == 0) {
                    LOGI("Risoluzione Android/data (esistente): '%s' -> '%s'", guestPath, cand.c_str());
                    return cand;
                }
            }
            // Scrittura/Creazione file assoluta sotto Scoped Storage sandbox
            std::string targetData = fDir + "/guest_data/" + pkg + relPath;
            ensureParentDirs(targetData);
            LOGI("Redirezione assoluta Android/data (nuovo): '%s' -> '%s'", guestPath, targetData.c_str());
            return targetData;
        }
    }

    // 4. Redirezione assoluta percorsi storage arbitrari (/sdcard/, /mnt/sdcard/, /storage/emulated/0/)
    std::string prefix;
    if (pathStr.rfind("/sdcard/", 0) == 0) prefix = "/sdcard/";
    else if (pathStr.rfind("/mnt/sdcard/", 0) == 0) prefix = "/mnt/sdcard/";
    else if (pathStr.rfind("/storage/emulated/0/", 0) == 0) prefix = "/storage/emulated/0/";

    if (!prefix.empty()) {
        std::string sub = pathStr.substr(prefix.size());
        if (sub.rfind("Android/obb", 0) != 0 && sub.rfind("Android/data", 0) != 0 &&
            sub.rfind("Emux32", 0) != 0 && sub.rfind("Emulators/Emux32", 0) != 0) {
            std::string customCand = sCustomStoragePath + "/" + sub;
            if (access(customCand.c_str(), F_OK) == 0) {
                return customCand;
            }
            std::string redirected = fDir + "/guest_data/" + (pkg.empty() ? "common" : pkg) + "/sdcard/" + sub;
            if (access(redirected.c_str(), F_OK) == 0) {
                return redirected;
            }
            ensureParentDirs(redirected);
            LOGI("Redirezione assoluta storage/save legacy: '%s' -> '%s'", guestPath, redirected.c_str());
            return redirected;
        }
    }

    if (access(guestPath, F_OK) == 0) {
        return pathStr;
    }

    // 5. Risoluzione prioritaria tramite la cartella assets estratta dall'APK
    std::string assetCand = env.resolveAssetPath(pathStr);
    if (!assetCand.empty()) {
        LOGI("Risoluzione asset guest via SyscallDispatcher: '%s' -> '%s'", guestPath, assetCand.c_str());
        return assetCand;
    }

    // 6. Ricerca file fra cartelle OBB, Custom Storage e APK
    size_t lastSlash = pathStr.find_last_of('/');
    std::string filename = (lastSlash != std::string::npos) ? pathStr.substr(lastSlash + 1) : pathStr;

    std::vector<std::string> candidates;
    if (!env.getObbDir().empty()) {
        candidates.push_back(env.getObbDir() + "/" + filename);
    }
    if (!fDir.empty() && !pkg.empty()) {
        candidates.push_back(fDir + "/obb/" + pkg + "/" + filename);
        candidates.push_back(fDir + "/data/" + pkg + "/" + filename);
        candidates.push_back(fDir + "/assets/" + pkg + "/" + filename);
        candidates.push_back(fDir + "/assets/" + filename);
    }
    if (!env.getNativeLibDir().empty()) {
        candidates.push_back(env.getNativeLibDir() + "/" + filename);
    }
    candidates.push_back(sCustomStoragePath + "/obb/" + pkg + "/" + filename);
    candidates.push_back(sCustomStoragePath + "/data/" + pkg + "/" + filename);
    candidates.push_back(sCustomStoragePath + "/obb/" + filename);
    candidates.push_back(sCustomStoragePath + "/data/" + filename);
    candidates.push_back(sCustomStoragePath + "/apk/" + filename);
    candidates.push_back(sCustomStoragePath + "/" + filename);
    candidates.push_back("/storage/emulated/0/Emux32/obb/" + pkg + "/" + filename);
    candidates.push_back("/storage/emulated/0/Emux32/data/" + pkg + "/" + filename);
    candidates.push_back("/storage/emulated/0/Emux32/obb/" + filename);
    candidates.push_back("/storage/emulated/0/Emux32/apk/" + filename);
    candidates.push_back("/storage/emulated/0/Android/obb/" + pkg + "/" + filename);
    candidates.push_back("/storage/emulated/0/Android/data/" + pkg + "/" + filename);
    candidates.push_back("/sdcard/Android/obb/" + pkg + "/" + filename);
    candidates.push_back("/sdcard/Android/data/" + pkg + "/" + filename);

    for (const auto& cand : candidates) {
        if (access(cand.c_str(), F_OK) == 0) {
            LOGI("Risoluzione file guest: '%s' -> '%s'", guestPath, cand.c_str());
            return cand;
        }
    }

    // 7. Redirezione di sicurezza assoluta per percorsi non-root /sdcard/ o /storage/
    if (!prefix.empty() && !fDir.empty()) {
        std::string sub = pathStr.substr(prefix.size());
        std::string finalRedirect = fDir + "/guest_data/" + (pkg.empty() ? "common" : pkg) + "/sdcard/" + sub;
        ensureParentDirs(finalRedirect);
        LOGI("Redirezione assoluta finale Scoped Storage: '%s' -> '%s'", guestPath, finalRedirect.c_str());
        return finalRedirect;
    }

    return pathStr;
}

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

constexpr uint32_t ARM_NR_exit          = 1;
constexpr uint32_t ARM_NR_read          = 3;
constexpr uint32_t ARM_NR_write         = 4;
constexpr uint32_t ARM_NR_open          = 5;
constexpr uint32_t ARM_NR_close         = 6;
constexpr uint32_t ARM_NR_unlink        = 10;
constexpr uint32_t ARM_NR_lseek         = 19;
constexpr uint32_t ARM_NR_getpid        = 20;
constexpr uint32_t ARM_NR_getuid        = 24;
constexpr uint32_t ARM_NR_access        = 33;
constexpr uint32_t ARM_NR_mkdir         = 39;
constexpr uint32_t ARM_NR_brk           = 45;
constexpr uint32_t ARM_NR_getgid        = 47;
constexpr uint32_t ARM_NR_geteuid       = 49;
constexpr uint32_t ARM_NR_getegid       = 50;
constexpr uint32_t ARM_NR_ioctl         = 54;
constexpr uint32_t ARM_NR_fcntl         = 55;
constexpr uint32_t ARM_NR_gettimeofday  = 78;
constexpr uint32_t ARM_NR_munmap        = 91;
constexpr uint32_t ARM_NR_mprotect      = 125;
constexpr uint32_t ARM_NR__llseek       = 140;
constexpr uint32_t ARM_NR_sched_yield   = 158;
constexpr uint32_t ARM_NR_nanosleep     = 162;
constexpr uint32_t ARM_NR_prctl         = 172;
constexpr uint32_t ARM_NR_mmap2         = 192;
constexpr uint32_t ARM_NR_stat64        = 195;
constexpr uint32_t ARM_NR_lstat64       = 196;
constexpr uint32_t ARM_NR_fstat64       = 197;
constexpr uint32_t ARM_NR_getuid32      = 199;
constexpr uint32_t ARM_NR_getgid32      = 200;
constexpr uint32_t ARM_NR_geteuid32     = 201;
constexpr uint32_t ARM_NR_getegid32     = 202;
constexpr uint32_t ARM_NR_fcntl64       = 221;
constexpr uint32_t ARM_NR_gettid        = 224;
constexpr uint32_t ARM_NR_futex         = 240;
constexpr uint32_t ARM_NR_exit_group    = 248;
constexpr uint32_t ARM_NR_clock_gettime = 263;
constexpr uint32_t ARM_NR_openat        = 322;
constexpr uint32_t ARM_NR_mkdirat       = 323;
constexpr uint32_t ARM_NR_fstatat64     = 327;
constexpr uint32_t ARM_NR_unlinkat      = 328;
constexpr uint32_t ARM_NR_faccessat     = 334;
constexpr uint32_t ARM_NR_set_tls       = 0xF0005;

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
        case ARM_NR_set_tls: {
            if (uc) {
                uc_reg_write(uc, UC_ARM_REG_C13_C0_3, &r0);
                uc_reg_write(uc, UC_ARM_REG_C13_C0_2, &r0);
            }
            return 0;
        }
        case ARM_NR_write: {
            int fd = static_cast<int>(r0);
            if (r1 == 0 || r2 == 0) return 0;
            std::vector<char> tempBuf(r2);
            if (uc && uc_mem_read(uc, r1, tempBuf.data(), r2) == UC_ERR_OK) {
                return static_cast<int32_t>(write(fd, tempBuf.data(), r2));
            }
            return -EFAULT;
        }
        case ARM_NR_read: {
            int fd = static_cast<int>(r0);
            if (r1 == 0 || r2 == 0) return 0;
            std::vector<char> tempBuf(r2);
            ssize_t bytesRead = read(fd, tempBuf.data(), r2);
            if (bytesRead > 0 && uc) {
                uc_mem_write(uc, r1, tempBuf.data(), static_cast<size_t>(bytesRead));
            }
            return (bytesRead >= 0) ? static_cast<int32_t>(bytesRead) : -errno;
        }
        case ARM_NR_open: {
            std::string path = readGuestCString(uc, r0);
            if (path.empty()) return -EFAULT;
            std::string resolved = resolveFilePath(path.c_str());
            int fd = open(resolved.c_str(), static_cast<int>(r1), static_cast<mode_t>(r2));
            return (fd >= 0) ? fd : -errno;
        }
        case ARM_NR_openat: {
            std::string path = readGuestCString(uc, r1);
            if (path.empty()) return -EFAULT;
            std::string resolved = resolveFilePath(path.c_str());
            int fd = openat(static_cast<int>(r0), resolved.c_str(), static_cast<int>(r2), static_cast<mode_t>(r3));
            return (fd >= 0) ? fd : -errno;
        }
        case ARM_NR_mkdir: {
            std::string path = readGuestCString(uc, r0);
            if (path.empty()) return -EFAULT;
            std::string resolved = resolveFilePath(path.c_str());
            int ret = mkdir(resolved.c_str(), static_cast<mode_t>(r1));
            return (ret == 0 || errno == EEXIST) ? 0 : -errno;
        }
        case ARM_NR_mkdirat: {
            std::string path = readGuestCString(uc, r1);
            if (path.empty()) return -EFAULT;
            std::string resolved = resolveFilePath(path.c_str());
            int ret = mkdir(resolved.c_str(), static_cast<mode_t>(r2));
            return (ret == 0 || errno == EEXIST) ? 0 : -errno;
        }
        case ARM_NR_unlink: {
            std::string path = readGuestCString(uc, r0);
            if (path.empty()) return -EFAULT;
            std::string resolved = resolveFilePath(path.c_str());
            return (unlink(resolved.c_str()) == 0) ? 0 : -errno;
        }
        case ARM_NR_unlinkat: {
            std::string path = readGuestCString(uc, r1);
            if (path.empty()) return -EFAULT;
            std::string resolved = resolveFilePath(path.c_str());
            return (unlink(resolved.c_str()) == 0) ? 0 : -errno;
        }
        case ARM_NR_close: {
            int fd = static_cast<int>(r0);
            if (fd <= 2) return 0;
            return (close(fd) == 0) ? 0 : -errno;
        }
        case ARM_NR_lseek: {
            off_t res = lseek(static_cast<int>(r0), static_cast<off_t>(static_cast<int32_t>(r1)), static_cast<int>(r2));
            return (res != (off_t)-1) ? static_cast<int32_t>(res) : -errno;
        }
        case ARM_NR__llseek: {
            int64_t offset = (static_cast<int64_t>(r1) << 32) | static_cast<int64_t>(r2);
            off_t newOff = lseek(static_cast<int>(r0), static_cast<off_t>(offset), static_cast<int>(r4));
            if (newOff == (off_t)-1) return -errno;
            if (r3 != 0 && uc) {
                int64_t res64 = static_cast<int64_t>(newOff);
                uc_mem_write(uc, r3, &res64, sizeof(res64));
            }
            return 0;
        }
        case ARM_NR_access: {
            std::string path = readGuestCString(uc, r0);
            if (path.empty()) return -EFAULT;
            std::string resolved = resolveFilePath(path.c_str());
            return (access(resolved.c_str(), static_cast<int>(r1)) == 0) ? 0 : -errno;
        }
        case ARM_NR_faccessat: {
            std::string path = readGuestCString(uc, r1);
            if (path.empty()) return -EFAULT;
            std::string resolved = resolveFilePath(path.c_str());
            return (access(resolved.c_str(), static_cast<int>(r2)) == 0) ? 0 : -errno;
        }
        case ARM_NR_stat64:
        case ARM_NR_lstat64: {
            std::string path = readGuestCString(uc, r0);
            if (path.empty() || r1 == 0) return -EFAULT;
            std::string resolved = resolveFilePath(path.c_str());
            struct stat hostSt{};
            if (stat(resolved.c_str(), &hostSt) == 0) {
                kernel_stat64_arm32 st32{};
                fillStat64(hostSt, st32);
                uc_mem_write(uc, r1, &st32, sizeof(st32));
                return 0;
            }
            return -errno;
        }
        case ARM_NR_fstat64: {
            if (r1 == 0) return -EFAULT;
            struct stat hostSt{};
            if (fstat(static_cast<int>(r0), &hostSt) == 0 && uc) {
                kernel_stat64_arm32 st32{};
                fillStat64(hostSt, st32);
                uc_mem_write(uc, r1, &st32, sizeof(st32));
                return 0;
            }
            return -errno;
        }
        case ARM_NR_fstatat64: {
            std::string path = readGuestCString(uc, r1);
            if (path.empty() || r2 == 0) return -EFAULT;
            std::string resolved = resolveFilePath(path.c_str());
            struct stat hostSt{};
            if (stat(resolved.c_str(), &hostSt) == 0) {
                kernel_stat64_arm32 st32{};
                fillStat64(hostSt, st32);
                uc_mem_write(uc, r2, &st32, sizeof(st32));
                return 0;
            }
            return -errno;
        }
        case ARM_NR_getpid:    return static_cast<int32_t>(getpid());
        case ARM_NR_gettid:    return static_cast<int32_t>(GuestThreadScheduler::getInstance().getCurrentTid());
        case ARM_NR_getuid:
        case ARM_NR_getuid32:  return static_cast<int32_t>(getuid());
        case ARM_NR_getgid:
        case ARM_NR_getgid32:  return static_cast<int32_t>(getgid());
        case ARM_NR_geteuid:
        case ARM_NR_geteuid32: return static_cast<int32_t>(geteuid());
        case ARM_NR_getegid:
        case ARM_NR_getegid32: return static_cast<int32_t>(getegid());
        case ARM_NR_ioctl:     return ioctl(static_cast<int>(r0), static_cast<int>(r1), nullptr);
        case ARM_NR_fcntl:
        case ARM_NR_fcntl64:   return 0;
        case ARM_NR_brk: {
            uint32_t res = GuestMemory::getSharedInstance().handleBrk(r0, uc);
            return static_cast<int32_t>(res);
        }
        case ARM_NR_mmap2: {
            uint64_t offset = static_cast<uint64_t>(r5) * 4096ULL;
            uint32_t res = GuestMemory::getSharedInstance().handleMmap(r0, r1, static_cast<int>(r2), static_cast<int>(r3), static_cast<int>(r4), offset, uc);
            return static_cast<int32_t>(res);
        }
        case ARM_NR_munmap: {
            return GuestMemory::getSharedInstance().handleMunmap(r0, r1, uc);
        }
        case ARM_NR_mprotect: {
            return GuestMemory::getSharedInstance().handleMprotect(r0, r1, static_cast<int>(r2), uc);
        }
        case ARM_NR_prctl:
            return 0;
        case 173: // rt_sigreturn
            return GuestSignalManager::getInstance().handleSigReturn(uc);
        case 174: // rt_sigaction
            return GuestSignalManager::getInstance().handleSigAction(r0, r1, r2, r3, uc);
        case 175: // rt_sigprocmask
            return GuestSignalManager::getInstance().handleSigProcMask(static_cast<int>(r0), r1, r2, r3, uc);
        case 186: // sigaltstack
            return GuestSignalManager::getInstance().handleSigAltStack(r0, r1, uc);
        case 238: // tkill
            return GuestSignalManager::getInstance().deliverSignal(uc, static_cast<int>(r1)) ? 0 : -EINVAL;
        case 268: // tgkill
            return GuestSignalManager::getInstance().deliverSignal(uc, static_cast<int>(r2)) ? 0 : -EINVAL;
        case 37:  // kill
            return GuestSignalManager::getInstance().deliverSignal(uc, static_cast<int>(r1)) ? 0 : -EINVAL;
        case ARM_NR_nanosleep:
        case ARM_NR_futex:
        case 145: // readv
        case 146: // writev
            return SyscallThunker::dispatch(uc, syscallNum, r0, r1, r2, r3, r4, r5);
        case ARM_NR_sched_yield:
            sched_yield();
            return 0;
        case ARM_NR_clock_gettime: {
            if (r1 == 0) return -EFAULT;
            struct timespec ts{};
            int ret = clock_gettime(static_cast<clockid_t>(r0), &ts);
            if (ret == 0 && uc) {
                uint32_t tp32[2] = { static_cast<uint32_t>(ts.tv_sec), static_cast<uint32_t>(ts.tv_nsec) };
                uc_mem_write(uc, r1, tp32, sizeof(tp32));
            }
            return ret;
        }
        case ARM_NR_gettimeofday: {
            if (r0 == 0) return -EFAULT;
            struct timeval tv{};
            int ret = gettimeofday(&tv, nullptr);
            if (ret == 0 && uc) {
                uint32_t tv32[2] = { static_cast<uint32_t>(tv.tv_sec), static_cast<uint32_t>(tv.tv_usec) };
                uc_mem_write(uc, r0, tv32, sizeof(tv32));
            }
            return ret;
        }
        case ARM_NR_exit:
        case ARM_NR_exit_group: {
            LOGI("Guest process invocato exit(%u)", r0);
            if (uc) uc_emu_stop(uc);
            return 0;
        }
        default:
            return 0;
    }
}

static void* sGlesHandle = nullptr;

void* SymbolResolver::resolveHostSymbol(const char* symbolName) {
    if (!symbolName) return nullptr;
    if (!sGlesHandle) {
        sGlesHandle = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    }
    if (sGlesHandle) {
        void* sym = dlsym(sGlesHandle, symbolName);
        if (sym) return sym;
    }
    return dlsym(RTLD_DEFAULT, symbolName);
}

GuestRuntimeEnvironment& GuestRuntimeEnvironment::getInstance() {
    static GuestRuntimeEnvironment instance;
    return instance;
}

GuestRuntimeEnvironment::GuestRuntimeEnvironment() = default;

void GuestRuntimeEnvironment::setGameContext(
    const std::string& pkg,
    const std::string& apkPath,
    const std::string& obbDir,
    const std::string& nativeLibDir,
    const std::string& filesDir
) {
    if (!pkg.empty()) packageName_ = pkg;
    if (!apkPath.empty()) apkPath_ = apkPath;
    if (!obbDir.empty()) obbDir_ = obbDir;
    if (!nativeLibDir.empty()) nativeLibDir_ = nativeLibDir;
    if (!filesDir.empty()) filesDir_ = filesDir;
    LOGI("GameContext configurato: pkg='%s', apk='%s', obbDir='%s', libDir='%s'",
         packageName_.c_str(), apkPath_.c_str(), obbDir_.c_str(), nativeLibDir_.c_str());

    // Inizializzazione e montaggio automatico dei container VFS (APK e archivi OBB)
    auto& vfs = ZipArchiveVfs::getInstance();
    vfs.reset();
    if (!apkPath_.empty()) {
        vfs.mountArchive(apkPath_);
    }
    if (!obbDir_.empty()) {
        vfs.mountObbDirectory(obbDir_);
    }
    if (!packageName_.empty()) {
        vfs.mountObbDirectory("/storage/emulated/0/Android/obb/" + packageName_);
        vfs.mountObbDirectory("/sdcard/Android/obb/" + packageName_);
        if (!filesDir_.empty()) {
            vfs.mountObbDirectory(filesDir_ + "/obb/" + packageName_);
        }
        if (!sCustomStoragePath.empty()) {
            vfs.mountObbDirectory(sCustomStoragePath + "/obb/" + packageName_);
        }
    }
    LOGI("ZipArchiveVfs: Montaggio completato (Totale file indicizzati: %zu)", vfs.getTotalIndexedFiles());
    GuestProcFs::getInstance().initialize(filesDir_, packageName_);
}

void GuestRuntimeEnvironment::setGameClassLoader(JNIEnv* env, jobject classLoader) {
    if (!env) return;
    env->GetJavaVM(&hostJavaVm_);
    if (hostClassLoader_) {
        env->DeleteGlobalRef(hostClassLoader_);
        hostClassLoader_ = nullptr;
    }
    for (auto& pair : hostClassMap_) {
        if (pair.second) env->DeleteGlobalRef(pair.second);
    }
    hostClassMap_.clear();
    hostMethodMap_.clear();

    if (classLoader) {
        hostClassLoader_ = env->NewGlobalRef(classLoader);
        jclass clClass = env->GetObjectClass(hostClassLoader_);
        loadClassMethod_ = env->GetMethodID(clClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
        env->DeleteLocalRef(clClass);
        LOGI("GuestRuntimeEnvironment: DexClassLoader host ART collegato con successo (JavaVM=%p)", hostJavaVm_);
    }
}

bool GuestRuntimeEnvironment::initialize(uc_engine* uc) {
    if (!uc) return false;

    // Reset completo dello stato runtime per evitare puntatori di istanze uc_ precedenti
    GuestSignalManager::getInstance().reset();
    GuestMemory::getSharedInstance().resetAllocators();
    nextTrampolineId_ = 1;
    symbolToTrampolineMap_.clear();
    trampolineToSymbolMap_.clear();
    registeredMethods_.clear();
    loadedLibraries_.clear();
    dlopenHandleMap_.clear();
    nextDlHandle_ = 0x50000010;

    nextMockHandle_ = 0x2000;
    classHandleMap_.clear();
    methodHandleMap_.clear();
    fieldHandleMap_.clear();
    jstringMap_.clear();
    jstringGuestPtrMap_.clear();
    arrayHandleMap_.clear();
    hostClassMap_.clear();
    hostMethodMap_.clear();

    GuestThreadScheduler::getInstance().shutdown();

    {
        std::lock_guard<std::mutex> lock(sFileMutex);
        for (auto& pair : sFileTable) {
            if (pair.second) fclose(pair.second);
        }
        sFileTable.clear();
        sNextFileHandle = 0x30000001;
    }
    for (auto& zp : sZlibStreamMap) {
        if (zp.second) {
            inflateEnd(zp.second);
            delete zp.second;
        }
    }
    sZlibStreamMap.clear();
    for (auto& ap : guestAssetMap_) {
        if (ap.second.fp) fclose(ap.second.fp);
    }
    guestAssetMap_.clear();
    guestAssetDirMap_.clear();
    for (auto& ip : guestInputStreamMap_) {
        if (ip.second.fp) fclose(ip.second.fp);
    }
    guestInputStreamMap_.clear();

    // 1. Mappa regione JNI + Bionic Globals (0x7D000000, 64KB, UC_PROT_ALL per stub eseguibili)
    uc_mem_map(uc, guestJniBase_, 0x10000, UC_PROT_ALL);

    // 2. Mappa regione TLS Bionic (0x7D020000, 16KB) e imposta registro CP15 TPIDRURO
    uc_mem_map(uc, guestTlsBase_, 0x4000, UC_PROT_READ | UC_PROT_WRITE);
    uint32_t tlsSlots[64] = {0};
    tlsSlots[0] = guestTlsBase_;         // TLS_SLOT_SELF
    tlsSlots[1] = 1;                     // TLS_SLOT_THREAD_ID (Main Thread TID=1)
    tlsSlots[2] = guestTlsBase_ + 0x100; // TLS_SLOT_ERRNO (allocato a offset +256)
    tlsSlots[3] = 0;                     // TLS_SLOT_OPENGL
    tlsSlots[5] = 0xDEADBEE0U;           // TLS_SLOT_STACK_GUARD (Bionic ARM32)
    tlsSlots[6] = 0xDEADBEE0U;           // Stack guard backup
    uc_mem_write(uc, guestTlsBase_, tlsSlots, sizeof(tlsSlots));
    uint32_t zeroErrno = 0;
    uc_mem_write(uc, guestTlsBase_ + 0x100, &zeroErrno, 4);
    uc_reg_write(uc, UC_ARM_REG_C13_C0_3, &guestTlsBase_);
    uc_reg_write(uc, UC_ARM_REG_C13_C0_2, &guestTlsBase_);

    // 3. Mappa regione Trampoline (0x70000000, 1MB -> fino a 131.072 simboli)
    uc_mem_map(uc, guestTrampolineBase_, 0x100000, UC_PROT_ALL);

    // Inizializza il Gestore dei Segnali e installa il restorer trampoline rt_sigreturn
    GuestSignalManager::getInstance().initialize(uc, guestTrampolineBase_ + 0x10U);

    // 3b. Mappa pool stack dei thread worker (0x7A000000, 4MB)
    uc_mem_map(uc, guestWorkerStackBase_, 0x00400000, UC_PROT_READ | UC_PROT_WRITE);

    // 4. Mappa Heap Guest iniziale a 0x58000000 (128MB, demand-paged)
    uc_mem_map(uc, 0x58000000, 0x08000000, UC_PROT_READ | UC_PROT_WRITE);

    // 5. Inizializza tabella _ctype_ Bionic a 0x7D002400
    uint8_t ctypeTable[257] = {0};
    for (int c = 0; c < 128; ++c) {
        uint8_t flags = 0;
        if (c >= 'A' && c <= 'Z') flags |= 0x01; // _U
        if (c >= 'a' && c <= 'z') flags |= 0x02; // _L
        if (c >= '0' && c <= '9') flags |= 0x04; // _N
        if (c == ' ' || (c >= '\t' && c <= '\r')) flags |= 0x08; // _S
        if ((c >= 0 && c < 32) || c == 127) flags |= 0x20; // _C
        if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) flags |= 0x40; // _X
        if (c == ' ') flags |= 0x80; // _B
        if (c > 32 && c < 127 && !flags) flags |= 0x10; // _P
        ctypeTable[c + 1] = flags;
    }
    uc_mem_write(uc, getCtypePointer(), ctypeTable, sizeof(ctypeTable));
    uint32_t stackGuard = 0xDEADBEE0U;
    uc_mem_write(uc, getStackChkGuardPointer(), &stackGuard, 4);

    // 6. Costruisci JavaVM32 e tutti gli 8 slot di JNIInvokeInterface32
    JavaVM32 vm32{};
    vm32.functions = guestJniBase_ + 0x10U;
    uc_mem_write(uc, guestJavaVMAddr_, &vm32, sizeof(vm32));

    JNIInvokeInterface32 vmVtable{};
    for (uint32_t i = 0; i < 8; ++i) {
        uint32_t stubAddr = guestJniBase_ + 0x100U + (i * 8U);
        vmVtable.slots[i] = stubAddr;
        uint32_t stubInsn[2] = { 0xEF000000U | (0x3000U + i), 0xE12FFF1EU }; // SVC #(0x3000 + i); BX LR
        uc_mem_write(uc, stubAddr, stubInsn, sizeof(stubInsn));
    }
    uc_mem_write(uc, vm32.functions, &vmVtable, sizeof(vmVtable));

    // 7. Costruisci JNIEnv32 e tutti i 256 slot standard di JNINativeInterface32
    JNIEnv32 env32{};
    env32.functions = guestJniBase_ + 0x220U;
    uc_mem_write(uc, guestJNIEnvAddr_, &env32, sizeof(env32));

    JNINativeInterface32 envVtable{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t stubAddr = guestJniBase_ + 0x1000U + (i * 8U);
        envVtable.slots[i] = stubAddr;
        uint32_t stubInsn[2] = { 0xEF000000U | (0x4000U + i), 0xE12FFF1EU }; // SVC #(0x4000 + i); BX LR
        uc_mem_write(uc, stubAddr, stubInsn, sizeof(stubInsn));
    }
    uc_mem_write(uc, env32.functions, &envVtable, sizeof(envVtable));

    // Inizializza mock ANativeWindow a 0x7D002800 con stub safe MOV R0, #0; BX LR a 0x7D002900
    uint32_t winStub[2] = { 0xE3A00000U, 0xE12FFF1EU };
    uint32_t winStubAddr = guestJniBase_ + 0x2900U;
    uc_mem_write(uc, winStubAddr, winStub, sizeof(winStub));

    uint32_t fakeNativeWindow[32];
    for (size_t w = 0; w < 32; ++w) fakeNativeWindow[w] = winStubAddr;
    fakeNativeWindow[1] = 1920; // width
    fakeNativeWindow[2] = 1080; // height
    fakeNativeWindow[3] = 1;    // format WINDOW_FORMAT_RGBA_8888
    uc_mem_write(uc, guestJniBase_ + 0x2800U, fakeNativeWindow, sizeof(fakeNativeWindow));

    classHandleMap_[0x1001] = "android/content/Context";
    classHandleMap_[0x1002] = "com/unity3d/player/UnityPlayer";
    classHandleMap_[0x1003] = "android/view/Surface";
    classHandleMap_[0x1050] = "android/view/MotionEvent";

    // Inizializza modulo Audio OpenSL ES simulato per i giochi guest
    AudioThunker::getInstance().initialize(uc);

    LOGI("GuestRuntimeEnvironment inizializzato: JavaVM32=0x%08x, JNIEnv32=0x%08x (256 slot ABI), TLS=0x%08x, Heap=0x58000000",
         guestJavaVMAddr_, guestJNIEnvAddr_, guestTlsBase_);
    return true;
}

uint32_t GuestRuntimeEnvironment::createGuestString(uc_engine* uc, const std::string& str) {
    std::lock_guard<std::recursive_mutex> lock(runtimeMutex_);
    uint32_t addr = allocGuestHeap(uc, static_cast<uint32_t>(str.size() + 1), 8);
    if (uc) {
        uc_mem_write(uc, addr, str.c_str(), str.size() + 1);
    }
    return addr;
}

uint32_t GuestRuntimeEnvironment::createGuestJString(uc_engine* uc, const std::string& str) {
    std::lock_guard<std::recursive_mutex> lock(runtimeMutex_);
    uint32_t handle = nextMockHandle_++;
    jstringMap_[handle] = str;
    jstringGuestPtrMap_[handle] = createGuestString(uc, str);
    return handle;
}

void GuestRuntimeEnvironment::registerLoadedLibrary(const std::shared_ptr<LoadedLibrary>& lib) {
    if (!lib) return;
    loadedLibraries_.push_back(lib);
}

uint32_t GuestRuntimeEnvironment::findExportedSymbol(const std::string& symbolName) const {
    if (symbolName.empty()) return 0;
    for (const auto& lib : loadedLibraries_) {
        if (!lib) continue;
        for (const auto& sym : lib->symbols) {
            if (sym.name == symbolName && sym.vaddr != 0) {
                return lib->load_bias + sym.vaddr;
            }
        }
    }
    return 0;
}

static bool isSystemHookSymbol(const std::string& name) {
    if (name.find("pthread_") == 0 || name.find("__android_log_") == 0 ||
        name.find("gl") == 0 || name.find("egl") == 0 || name.find("dl") == 0 ||
        name.find("ANativeWindow_") == 0 || name.find("__aeabi_") == 0 ||
        name.find("sl") == 0 || name.find("SL_IID_") == 0 ||
        name == "malloc" || name == "free" || name == "calloc" || name == "realloc" ||
        name == "memalign" || name == "posix_memalign" || name == "valloc" ||
        name == "mmap" || name == "munmap" || name == "mprotect" || name == "sysconf" ||
        name == "getauxval" ||
        name == "__errno" || name == "abort" || name == "raise" || name == "signal" ||
        name == "sigaction" || name == "__sigaction" || name == "clock_gettime" ||
        name == "gettimeofday" || name == "time" || name == "nanosleep" || name == "usleep" ||
        name == "open" || name == "open64" || name == "close" || name == "read" || name == "write" ||
        name == "ioctl" || name == "lseek" || name == "lseek64" || name == "stat" || name == "stat64" || name == "fstat" || name == "fstat64" ||
        name == "access" || name == "opendir" || name == "closedir" || name == "readdir" ||
        name == "memcpy" || name == "memset" || name == "memmove" || name == "memcmp" ||
        name == "strlen" || name == "strcpy" || name == "strncpy" || name == "strcmp" ||
        name == "strncmp" || name == "strcat" || name == "strncat" || name == "strchr" ||
        name == "strrchr" || name == "strstr" || name == "snprintf" || name == "sprintf" ||
        name == "printf" || name == "vsnprintf" || name == "vsprintf" || name == "sscanf" ||
        name == "fopen" || name == "fopen64" || name == "fread" || name == "fwrite" ||
        name == "fseek" || name == "fseeko" || name == "fseeko64" || name == "ftell" ||
        name == "ftello" || name == "ftello64" || name == "fclose" || name == "fflush" ||
        name == "getenv" || name == "setenv" || name == "system" || name == "atexit" ||
        name == "__cxa_atexit" || name == "__cxa_finalize") {
        return true;
    }
    return false;
}

uint32_t GuestRuntimeEnvironment::resolveOrRegisterSymbol(const std::string& symbolName, uc_engine* uc) {
    if (symbolName == "_ctype_") return getCtypePointer();
    if (symbolName == "__sF") return getSFilePointer();
    if (symbolName == "__stack_chk_guard") return getStackChkGuardPointer();

    if (symbolName.rfind("SL_IID_", 0) == 0) {
        uint32_t iidAddr = AudioThunker::getInstance().resolveInterfaceId(symbolName, uc);
        if (iidAddr != 0) return iidAddr;
    }

    // Se non e' un simbolo di sistema gestito dai nostri hook,
    // verifica se una qualsiasi libreria guest gia' caricata (es. libc++_shared, libmono, libfmod, libunity)
    // esporta questo simbolo: in tal caso usa direttamente l'indirizzo reale guest!
    if (!isSystemHookSymbol(symbolName)) {
        uint32_t exported = findExportedSymbol(symbolName);
        if (exported != 0) return exported;
    }

    return registerSymbolTrampoline(symbolName, uc);
}

uint32_t GuestRuntimeEnvironment::registerSymbolTrampoline(const std::string& symbolName, uc_engine* uc) {
    auto it = symbolToTrampolineMap_.find(symbolName);
    if (it != symbolToTrampolineMap_.end()) {
        return it->second;
    }

    uint32_t trampId = nextTrampolineId_++;
    uint32_t trampAddr = guestTrampolineBase_ + (trampId * 8U);

    symbolToTrampolineMap_[symbolName] = trampAddr;
    trampolineToSymbolMap_[trampId] = symbolName;

    uint32_t stubInsn[2] = {
        0xEF000000U | (0x2000U + trampId),
        0xE12FFF1EU
    };

    if (uc) {
        uc_mem_write(uc, trampAddr, stubInsn, sizeof(stubInsn));
    }

    return trampAddr;
}

void GuestRuntimeEnvironment::handleJavaVmCall(uint32_t slotIndex, uc_engine* uc) {
    // Slots: 3=DestroyJavaVM, 4=AttachCurrentThread, 5=DetachCurrentThread, 6=GetEnv, 7=AttachCurrentThreadAsDaemon
    if (slotIndex == 4 || slotIndex == 6 || slotIndex == 7) {
        uint32_t envPtrAddr = 0;
        uc_reg_read(uc, UC_ARM_REG_R1, &envPtrAddr);
        if (envPtrAddr != 0) {
            uint32_t envVal = guestJNIEnvAddr_;
            uc_mem_write(uc, envPtrAddr, &envVal, sizeof(envVal));
        }
    }
    uint32_t retOk = 0; // JNI_OK
    uc_reg_write(uc, UC_ARM_REG_R0, &retOk);
}

jobject GuestRuntimeEnvironment::resolveHostObject(uint32_t guestHandle, JNIEnv* hostEnv, uc_engine* uc) {
    if (!hostEnv || guestHandle == 0) return nullptr;

    auto oit = hostObjectMap_.find(guestHandle);
    if (oit != hostObjectMap_.end()) {
        return oit->second;
    }

    auto cit = hostClassMap_.find(guestHandle);
    if (cit != hostClassMap_.end()) {
        return cit->second;
    }

    auto sit = jstringMap_.find(guestHandle);
    if (sit != jstringMap_.end()) {
        return hostEnv->NewStringUTF(sit->second.c_str());
    }

    if (uc && guestHandle >= 0x10000 && guestHandle < 0xFF000000) {
        std::string rawStr = readGuestCString(uc, guestHandle, 256);
        if (!rawStr.empty()) {
            return hostEnv->NewStringUTF(rawStr.c_str());
        }
    }

    return nullptr;
}

struct JniArgUnpackResult {
    std::vector<jvalue> args;
    std::vector<jobject> localRefsToDelete;
};

static JniArgUnpackResult unpackJniArguments(
    uc_engine* uc,
    const std::string& signature,
    uint32_t r3,
    uint32_t slotIndex,
    JNIEnv* hostEnv,
    GuestRuntimeEnvironment& env
) {
    JniArgUnpackResult result;
    size_t openParen = signature.find('(');
    size_t closeParen = signature.find(')');
    if (openParen == std::string::npos || closeParen == std::string::npos) {
        return result;
    }

    uint32_t sp = 0;
    uc_reg_read(uc, UC_ARM_REG_SP, &sp);

    int callMode = 0; // 0=variadic, 1=va_list, 2=array
    if (slotIndex >= 28 && slotIndex <= 30) {
        callMode = (slotIndex - 28);
    } else if (slotIndex >= 34 && slotIndex <= 93) {
        callMode = (slotIndex - 34) % 3;
    } else if (slotIndex >= 114 && slotIndex <= 143) {
        callMode = (slotIndex - 114) % 3;
    }

    size_t sigIdx = openParen + 1;
    int regIdx = 3;
    uint32_t stackOff = 0;
    uint32_t vaListPtr = r3;
    uint32_t jvaluePtr = r3;
    size_t argIndex = 0;

    if (slotIndex >= 64 && slotIndex <= 93) {
        regIdx = 4;
        if (callMode == 1 || callMode == 2) {
            uc_mem_read(uc, sp, &vaListPtr, 4);
            jvaluePtr = vaListPtr;
            stackOff = 4;
        }
    }

    auto readNextWord = [&]() -> uint32_t {
        if (callMode == 1) { // va_list
            uint32_t w = 0;
            uc_mem_read(uc, vaListPtr, &w, 4);
            vaListPtr += 4;
            return w;
        } else { // variadic
            if (regIdx == 3) {
                regIdx = 4;
                return r3;
            }
            uint32_t w = 0;
            uc_mem_read(uc, sp + stackOff, &w, 4);
            stackOff += 4;
            return w;
        }
    };

    while (sigIdx < closeParen) {
        char type = signature[sigIdx++];
        jvalue val{};

        if (callMode == 2) { // Array jvalue*
            uint64_t rawVal = 0;
            uc_mem_read(uc, jvaluePtr + (argIndex * 8), &rawVal, 8);
            argIndex++;

            switch (type) {
                case 'Z': val.z = static_cast<jboolean>(rawVal & 0xFF); break;
                case 'B': val.b = static_cast<jbyte>(rawVal & 0xFF); break;
                case 'C': val.c = static_cast<jchar>(rawVal & 0xFFFF); break;
                case 'S': val.s = static_cast<jshort>(rawVal & 0xFFFF); break;
                case 'I': val.i = static_cast<jint>(rawVal & 0xFFFFFFFFU); break;
                case 'J': val.j = static_cast<jlong>(rawVal); break;
                case 'F': {
                    uint32_t rawF = static_cast<uint32_t>(rawVal);
                    std::memcpy(&val.f, &rawF, 4);
                    break;
                }
                case 'D': {
                    std::memcpy(&val.d, &rawVal, 8);
                    break;
                }
                case 'L': {
                    while (sigIdx < closeParen && signature[sigIdx++] != ';') {}
                    uint32_t h = static_cast<uint32_t>(rawVal & 0xFFFFFFFFU);
                    jobject obj = env.resolveHostObject(h, hostEnv, uc);
                    if (obj) result.localRefsToDelete.push_back(obj);
                    val.l = obj;
                    break;
                }
                case '[': {
                    while (sigIdx < closeParen && signature[sigIdx] == '[') sigIdx++;
                    if (sigIdx < closeParen && signature[sigIdx] == 'L') {
                        while (sigIdx < closeParen && signature[sigIdx++] != ';') {}
                    } else if (sigIdx < closeParen) {
                        sigIdx++;
                    }
                    uint32_t h = static_cast<uint32_t>(rawVal & 0xFFFFFFFFU);
                    jobject obj = env.resolveHostObject(h, hostEnv, uc);
                    if (obj) result.localRefsToDelete.push_back(obj);
                    val.l = obj;
                    break;
                }
            }
            result.args.push_back(val);
            continue;
        }

        switch (type) {
            case 'Z': val.z = static_cast<jboolean>(readNextWord() != 0); break;
            case 'B': val.b = static_cast<jbyte>(readNextWord()); break;
            case 'C': val.c = static_cast<jchar>(readNextWord()); break;
            case 'S': val.s = static_cast<jshort>(readNextWord()); break;
            case 'I': val.i = static_cast<jint>(readNextWord()); break;
            case 'F': {
                uint32_t raw = readNextWord();
                std::memcpy(&val.f, &raw, 4);
                break;
            }
            case 'J':
            case 'D': {
                uint32_t lo = 0, hi = 0;
                if (callMode == 1) { // va_list
                    if (vaListPtr % 8 != 0) vaListPtr = (vaListPtr + 7) & ~7U;
                    uc_mem_read(uc, vaListPtr, &lo, 4);
                    uc_mem_read(uc, vaListPtr + 4, &hi, 4);
                    vaListPtr += 8;
                } else { // variadic AAPCS
                    if (regIdx == 3) {
                        regIdx = 4;
                    }
                    if (stackOff % 8 != 0) stackOff = (stackOff + 7) & ~7U;
                    uc_mem_read(uc, sp + stackOff, &lo, 4);
                    uc_mem_read(uc, sp + stackOff + 4, &hi, 4);
                    stackOff += 8;
                }
                uint64_t raw64 = (static_cast<uint64_t>(hi) << 32) | lo;
                if (type == 'J') val.j = static_cast<jlong>(raw64);
                else std::memcpy(&val.d, &raw64, 8);
                break;
            }
            case 'L': {
                while (sigIdx < closeParen && signature[sigIdx++] != ';') {}
                uint32_t h = readNextWord();
                jobject obj = env.resolveHostObject(h, hostEnv, uc);
                if (obj) result.localRefsToDelete.push_back(obj);
                val.l = obj;
                break;
            }
            case '[': {
                while (sigIdx < closeParen && signature[sigIdx] == '[') sigIdx++;
                if (sigIdx < closeParen && signature[sigIdx] == 'L') {
                    while (sigIdx < closeParen && signature[sigIdx++] != ';') {}
                } else if (sigIdx < closeParen) {
                    sigIdx++;
                }
                uint32_t h = readNextWord();
                jobject obj = env.resolveHostObject(h, hostEnv, uc);
                if (obj) result.localRefsToDelete.push_back(obj);
                val.l = obj;
                break;
            }
        }
        result.args.push_back(val);
    }

    return result;
}

void GuestRuntimeEnvironment::handleJniCall(uint32_t slotIndex, uc_engine* uc) {
    std::lock_guard<std::recursive_mutex> lock(runtimeMutex_);
    uint32_t r1 = 0, r2 = 0, r3 = 0;
    uc_reg_read(uc, UC_ARM_REG_R1, &r1);
    uc_reg_read(uc, UC_ARM_REG_R2, &r2);
    uc_reg_read(uc, UC_ARM_REG_R3, &r3);

    auto setReturnU32 = [&](uint32_t val) {
        uc_reg_write(uc, UC_ARM_REG_R0, &val);
    };

    switch (slotIndex) {
        case 4: // GetVersion
            setReturnU32(0x00010006U); // JNI_VERSION_1_6
            return;

        case 5: // DefineClass
        case 6: { // FindClass(env, const char* name)
            std::string clsName = readGuestCString(uc, r1);
            uint32_t handle = nextMockHandle_++;
            classHandleMap_[handle] = clsName;

            if (hostJavaVm_ && hostClassLoader_ && loadClassMethod_) {
                JNIEnv* hostEnv = nullptr;
                if (hostJavaVm_->GetEnv(reinterpret_cast<void**>(&hostEnv), JNI_VERSION_1_6) == JNI_OK && hostEnv) {
                    std::string dotted = clsName;
                    std::replace(dotted.begin(), dotted.end(), '/', '.');
                    jstring jDotted = hostEnv->NewStringUTF(dotted.c_str());
                    jclass foundCls = static_cast<jclass>(hostEnv->CallObjectMethod(hostClassLoader_, loadClassMethod_, jDotted));
                    if (hostEnv->ExceptionCheck()) {
                        hostEnv->ExceptionClear();
                        foundCls = hostEnv->FindClass(clsName.c_str());
                        if (hostEnv->ExceptionCheck()) hostEnv->ExceptionClear();
                    }
                    hostEnv->DeleteLocalRef(jDotted);
                    if (foundCls) {
                        hostClassMap_[handle] = static_cast<jclass>(hostEnv->NewGlobalRef(foundCls));
                        hostEnv->DeleteLocalRef(foundCls);
                        LOGI("JNI FindClass('%s') -> CLASSE REALE HOST ART CARICATA (handle=0x%x)", clsName.c_str(), handle);
                    }
                }
            }

            LOGI("JNI FindClass('%s') -> handle=0x%x", clsName.c_str(), handle);
            setReturnU32(handle);
            return;
        }

        case 10: // GetSuperclass
        case 31: { // GetObjectClass(env, jobject obj)
            if (slotIndex == 31 && hostJavaVm_ && r1 != 0) {
                auto hit = hostObjectMap_.find(r1);
                if (hit != hostObjectMap_.end()) {
                    JNIEnv* hostEnv = nullptr;
                    if (hostJavaVm_->GetEnv(reinterpret_cast<void**>(&hostEnv), JNI_VERSION_1_6) == JNI_OK && hostEnv) {
                        jclass objCls = hostEnv->GetObjectClass(hit->second);
                        if (hostEnv->ExceptionCheck()) hostEnv->ExceptionClear();
                        if (objCls) {
                            uint32_t handle = nextMockHandle_++;
                            hostClassMap_[handle] = static_cast<jclass>(hostEnv->NewGlobalRef(objCls));
                            hostEnv->DeleteLocalRef(objCls);
                            setReturnU32(handle);
                            return;
                        }
                    }
                }
            }
            auto it = classHandleMap_.find(r1);
            if (it != classHandleMap_.end()) {
                setReturnU32(r1);
            } else {
                uint32_t handle = nextMockHandle_++;
                classHandleMap_[handle] = "java/lang/Object";
                setReturnU32(handle);
            }
            return;
        }

        case 11: // IsAssignableFrom
        case 24: // IsSameObject
        case 32: // IsInstanceOf
            setReturnU32(1); // JNI_TRUE
            return;

        case 13: // Throw
        case 14: // ThrowNew
        case 15: // ExceptionOccurred
        case 16: // ExceptionDescribe
        case 17: // ExceptionClear
        case 228: // ExceptionCheck
            setReturnU32(0); // Nessuna eccezione Java pendente
            return;

        case 19: // PushLocalFrame
        case 26: // EnsureLocalCapacity
        case 216: // UnregisterNatives
        case 217: // MonitorEnter
        case 218: // MonitorExit
            setReturnU32(0); // JNI_OK
            return;

        case 20: // PopLocalFrame(env, jobject result)
        case 21: // NewGlobalRef(env, jobject obj)
        case 25: // NewLocalRef(env, jobject obj)
        case 226: // NewWeakGlobalRef
            setReturnU32(r1 != 0 ? r1 : nextMockHandle_++);
            return;

        case 22: // DeleteGlobalRef
        case 23: // DeleteLocalRef
        case 227: // DeleteWeakGlobalRef
            if (hostJavaVm_ && r1 != 0) {
                auto hit = hostObjectMap_.find(r1);
                if (hit != hostObjectMap_.end()) {
                    JNIEnv* hostEnv = nullptr;
                    if (hostJavaVm_->GetEnv(reinterpret_cast<void**>(&hostEnv), JNI_VERSION_1_6) == JNI_OK && hostEnv) {
                        hostEnv->DeleteGlobalRef(hit->second);
                    }
                    hostObjectMap_.erase(hit);
                }
            }
            setReturnU32(0);
            return;

        case 27: // AllocObject
        case 28: // NewObject
        case 29: // NewObjectV
        case 30: { // NewObjectA
            uint32_t handle = nextMockHandle_++;
            if (classHandleMap_.count(r1)) {
                classHandleMap_[handle] = classHandleMap_[r1];
            }

            if (hostClassMap_.count(r1) && hostJavaVm_) {
                JNIEnv* hostEnv = nullptr;
                if (hostJavaVm_->GetEnv(reinterpret_cast<void**>(&hostEnv), JNI_VERSION_1_6) == JNI_OK && hostEnv) {
                    jclass realCls = hostClassMap_[r1];
                    jobject realObj = nullptr;
                    if (slotIndex == 27) {
                        realObj = hostEnv->AllocObject(realCls);
                    } else if (hostMethodMap_.count(r2)) {
                        jmethodID initMethod = hostMethodMap_[r2];
                        std::string sig = methodHandleMap_.count(r2) ? methodHandleMap_[r2].signature : "()V";
                        auto unpacked = unpackJniArguments(uc, sig, r3, slotIndex, hostEnv, *this);
                        realObj = hostEnv->NewObjectA(realCls, initMethod, unpacked.args.data());
                        for (auto ref : unpacked.localRefsToDelete) {
                            hostEnv->DeleteLocalRef(ref);
                        }
                    }
                    if (hostEnv->ExceptionCheck()) hostEnv->ExceptionClear();
                    if (realObj) {
                        hostObjectMap_[handle] = hostEnv->NewGlobalRef(realObj);
                        hostEnv->DeleteLocalRef(realObj);
                        LOGI("JNI NewObject instanziato vero oggetto host ART per %s (handle=0x%x)",
                             classHandleMap_[r1].c_str(), handle);
                    }
                }
            }

            setReturnU32(handle);
            return;
        }

        case 33: // GetMethodID(env, jclass clazz, const char* name, const char* sig)
        case 113: { // GetStaticMethodID
            std::string mName = readGuestCString(uc, r2);
            std::string mSig = readGuestCString(uc, r3);
            std::string cName = classHandleMap_.count(r1) ? classHandleMap_[r1] : "";
            uint32_t handle = nextMockHandle_++;
            methodHandleMap_[handle] = {cName, mName, mSig, slotIndex == 113};

            if (hostClassMap_.count(r1) && hostJavaVm_) {
                JNIEnv* hostEnv = nullptr;
                if (hostJavaVm_->GetEnv(reinterpret_cast<void**>(&hostEnv), JNI_VERSION_1_6) == JNI_OK && hostEnv) {
                    jclass realCls = hostClassMap_[r1];
                    jmethodID mId = (slotIndex == 113)
                        ? hostEnv->GetStaticMethodID(realCls, mName.c_str(), mSig.c_str())
                        : hostEnv->GetMethodID(realCls, mName.c_str(), mSig.c_str());
                    if (hostEnv->ExceptionCheck()) hostEnv->ExceptionClear();
                    if (mId) {
                        hostMethodMap_[handle] = mId;
                        LOGI("JNI %s -> METODO REALE HOST TROVATO: %s::%s%s (handle=0x%x)",
                             (slotIndex == 113 ? "GetStaticMethodID" : "GetMethodID"),
                             cName.c_str(), mName.c_str(), mSig.c_str(), handle);
                    }
                }
            }

            LOGI("JNI %s('%s', '%s', '%s') -> 0x%x",
                 (slotIndex == 113 ? "GetStaticMethodID" : "GetMethodID"),
                 cName.c_str(), mName.c_str(), mSig.c_str(), handle);
            setReturnU32(handle);
            return;
        }

        case 94: // GetFieldID(env, jclass clazz, const char* name, const char* sig)
        case 144: { // GetStaticFieldID
            std::string fName = readGuestCString(uc, r2);
            std::string fSig = readGuestCString(uc, r3);
            std::string cName = classHandleMap_.count(r1) ? classHandleMap_[r1] : "";
            uint32_t handle = nextMockHandle_++;
            fieldHandleMap_[handle] = {cName, fName, fSig, slotIndex == 144};
            setReturnU32(handle);
            return;
        }

        // CallObjectMethod / CallNonvirtualObjectMethod / CallStaticObjectMethod
        case 34: case 35: case 36:
        case 64: case 65: case 66:
        case 114: case 115: case 116: {
            auto hit = hostMethodMap_.find(r2);
            if (hit != hostMethodMap_.end() && hostJavaVm_) {
                JNIEnv* hostEnv = nullptr;
                if (hostJavaVm_->GetEnv(reinterpret_cast<void**>(&hostEnv), JNI_VERSION_1_6) == JNI_OK && hostEnv) {
                    bool isStatic = (slotIndex >= 114);
                    jclass realCls = hostClassMap_.count(r1) ? hostClassMap_[r1] : nullptr;
                    jobject targetObj = isStatic ? nullptr : resolveHostObject(r1, hostEnv, uc);
                    if ((isStatic && realCls) || (!isStatic && targetObj)) {
                        std::string sig = methodHandleMap_.count(r2) ? methodHandleMap_[r2].signature : "()Ljava/lang/Object;";
                        auto unpacked = unpackJniArguments(uc, sig, r3, slotIndex, hostEnv, *this);
                        jobject resObj = isStatic
                            ? hostEnv->CallStaticObjectMethodA(realCls, hit->second, unpacked.args.data())
                            : hostEnv->CallObjectMethodA(targetObj, hit->second, unpacked.args.data());
                        for (auto ref : unpacked.localRefsToDelete) {
                            hostEnv->DeleteLocalRef(ref);
                        }
                        if (hostEnv->ExceptionCheck()) hostEnv->ExceptionClear();
                        if (resObj) {
                            jclass strCls = hostEnv->FindClass("java/lang/String");
                            if (strCls && hostEnv->IsInstanceOf(resObj, strCls)) {
                                const char* utf = hostEnv->GetStringUTFChars(static_cast<jstring>(resObj), nullptr);
                                uint32_t guestStr = createGuestJString(uc, utf ? utf : "");
                                if (utf) hostEnv->ReleaseStringUTFChars(static_cast<jstring>(resObj), utf);
                                hostEnv->DeleteLocalRef(strCls);
                                hostEnv->DeleteLocalRef(resObj);
                                setReturnU32(guestStr);
                                return;
                            }
                            if (strCls) hostEnv->DeleteLocalRef(strCls);

                            uint32_t objH = nextMockHandle_++;
                            jclass objCls = hostEnv->GetObjectClass(resObj);
                            hostClassMap_[objH] = static_cast<jclass>(hostEnv->NewGlobalRef(objCls));
                            hostObjectMap_[objH] = hostEnv->NewGlobalRef(resObj);
                            hostEnv->DeleteLocalRef(objCls);
                            hostEnv->DeleteLocalRef(resObj);
                            setReturnU32(objH);
                            return;
                        } else {
                            setReturnU32(0);
                            return;
                        }
                    }
                }
            }

            auto mit = methodHandleMap_.find(r2);
            if (mit != methodHandleMap_.end()) {
                const auto& info = mit->second;
                if (info.name == "getPackageName") {
                    setReturnU32(createGuestJString(uc, packageName_));
                    return;
                }
                if (info.name == "getPackageCodePath" || info.name == "getResourcePath" || info.name == "getApkPath") {
                    setReturnU32(createGuestJString(uc, apkPath_));
                    return;
                }
                if (info.name == "getObbDir") {
                    uint32_t dirObj = nextMockHandle_++;
                    jstringMap_[dirObj] = obbDir_.empty() ? (sCustomStoragePath + "/obb") : obbDir_;
                    setReturnU32(dirObj);
                    return;
                }
                if (info.name == "getExternalStorageDirectory") {
                    uint32_t dirObj = nextMockHandle_++;
                    jstringMap_[dirObj] = sCustomStoragePath.empty() ? "/storage/emulated/0" : sCustomStoragePath;
                    setReturnU32(dirObj);
                    return;
                }
                if (info.name == "getFilesDir" || info.name == "getCacheDir" || info.name == "getExternalFilesDir") {
                    uint32_t dirObj = nextMockHandle_++;
                    jstringMap_[dirObj] = filesDir_;
                    setReturnU32(dirObj);
                    return;
                }
                if (info.name == "newInterfaceProxy") {
                    uint32_t proxyH = nextMockHandle_++;
                    LOGI("JNI bitter/jnibridge/JNIBridge.newInterfaceProxy generato proxy 0x%x", proxyH);
                    setReturnU32(proxyH);
                    return;
                }
                if (info.name == "getAbsolutePath" || info.name == "getPath" || info.name == "toString") {
                    std::string pathVal = jstringMap_.count(r1) ? jstringMap_[r1] : (obbDir_.empty() ? filesDir_ : obbDir_);
                    setReturnU32(createGuestJString(uc, pathVal));
                    return;
                }
                if (info.name == "getAssets") {
                    uint32_t assetMgrHandle = 0x7D005000U;
                    classHandleMap_[assetMgrHandle] = "android/content/res/AssetManager";
                    setReturnU32(assetMgrHandle);
                    return;
                }
                if (info.name == "getResources") {
                    uint32_t resHandle = 0x7D005100U;
                    classHandleMap_[resHandle] = "android/content/res/Resources";
                    setReturnU32(resHandle);
                    return;
                }
                if (info.name == "getDisplayMetrics") {
                    uint32_t dmHandle = 0x7D005200U;
                    classHandleMap_[dmHandle] = "android/util/DisplayMetrics";
                    setReturnU32(dmHandle);
                    return;
                }
                if (info.name == "getWindowManager") {
                    uint32_t wmHandle = 0x7D005300U;
                    classHandleMap_[wmHandle] = "android/view/WindowManager";
                    setReturnU32(wmHandle);
                    return;
                }
                if (info.name == "getDefaultDisplay") {
                    uint32_t dispHandle = 0x7D005400U;
                    classHandleMap_[dispHandle] = "android/view/Display";
                    setReturnU32(dispHandle);
                    return;
                }
                if (info.name == "open") {
                    // AssetManager.open(String filename)
                    std::string assetPath = jstringMap_.count(r3) ? jstringMap_[r3] : readGuestCString(uc, r3, 256);
                    std::string resolved = resolveAssetPath(assetPath);
                    if (!resolved.empty()) {
                        FILE* fp = fopen(resolved.c_str(), "rb");
                        if (fp) {
                            fseek(fp, 0, SEEK_END);
                            size_t sz = static_cast<size_t>(ftell(fp));
                            fseek(fp, 0, SEEK_SET);
                            uint32_t sHandle = nextInputStreamHandle_++;
                            guestInputStreamMap_[sHandle] = {sHandle, fp, sz};
                            classHandleMap_[sHandle] = "java/io/InputStream";
                            LOGI("JNI AssetManager.open('%s') -> handle 0x%08x (dim=%zu)", assetPath.c_str(), sHandle, sz);
                            setReturnU32(sHandle);
                            return;
                        }
                    }
                    LOGW("JNI AssetManager.open('%s') fallito: asset non trovato!", assetPath.c_str());
                    setReturnU32(0);
                    return;
                }
                if (info.name == "openFd") {
                    uint32_t fdObj = nextMockHandle_++;
                    classHandleMap_[fdObj] = "android/content/res/AssetFileDescriptor";
                    setReturnU32(fdObj);
                    return;
                }
                if (info.name == "getFileData" || info.name == "getFileDataFromZip") {
                    std::string assetPath;
                    if (info.name == "getFileData") {
                        assetPath = jstringMap_.count(r3) ? jstringMap_[r3] : readGuestCString(uc, r3, 256);
                    } else {
                        uint32_t sp = 0, p1 = 0;
                        uc_reg_read(uc, UC_ARM_REG_SP, &sp);
                        uc_mem_read(uc, sp, &p1, 4);
                        assetPath = jstringMap_.count(p1) ? jstringMap_[p1] : readGuestCString(uc, p1, 256);
                        if (assetPath.empty()) {
                            assetPath = jstringMap_.count(r3) ? jstringMap_[r3] : readGuestCString(uc, r3, 256);
                        }
                    }
                    std::string resolved = resolveAssetPath(assetPath);
                    if (!resolved.empty()) {
                        FILE* fp = fopen(resolved.c_str(), "rb");
                        if (fp) {
                            fseek(fp, 0, SEEK_END);
                            long fsz = ftell(fp);
                            fseek(fp, 0, SEEK_SET);
                            if (fsz >= 0) {
                                size_t sz = static_cast<size_t>(fsz);
                                std::vector<uint8_t> bytes(sz);
                                if (sz > 0) {
                                    fread(bytes.data(), 1, sz, fp);
                                }
                                fclose(fp);

                                uint32_t arrH = nextMockHandle_++;
                                uint32_t buf = allocGuestHeap(uc, static_cast<uint32_t>(sz + 16U), 16);
                                if (sz > 0) {
                                    uc_mem_write(uc, buf, bytes.data(), sz);
                                }
                                arrayHandleMap_[arrH] = {buf, static_cast<uint32_t>(sz)};
                                LOGI("JNI %s('%s') -> byte[%zu] @ 0x%08x (handle=0x%x)",
                                     info.name.c_str(), assetPath.c_str(), sz, buf, arrH);
                                setReturnU32(arrH);
                                return;
                            }
                            fclose(fp);
                        }
                    }
                    LOGW("JNI %s('%s') fallito: file non trovato!", info.name.c_str(), assetPath.c_str());
                    setReturnU32(0);
                    return;
                }
                if (info.signature.find(")Ljava/lang/String;") != std::string::npos) {
                    setReturnU32(createGuestJString(uc, ""));
                    return;
                }
                if (info.signature.find(")[") != std::string::npos) {
                    uint32_t arrH = nextMockHandle_++;
                    uint32_t buf = allocGuestHeap(uc, 64, 8);
                    arrayHandleMap_[arrH] = {buf, 0};
                    setReturnU32(arrH);
                    return;
                }
            }
            setReturnU32(nextMockHandle_++);
            return;
        }

        // CallBooleanMethod / CallStaticBooleanMethod
        case 37: case 38: case 39:
        case 67: case 68: case 69:
        case 117: case 118: case 119: {
            auto hit = hostMethodMap_.find(r2);
            if (hit != hostMethodMap_.end() && hostJavaVm_) {
                JNIEnv* hostEnv = nullptr;
                if (hostJavaVm_->GetEnv(reinterpret_cast<void**>(&hostEnv), JNI_VERSION_1_6) == JNI_OK && hostEnv) {
                    bool isStatic = (slotIndex >= 117);
                    jclass realCls = hostClassMap_.count(r1) ? hostClassMap_[r1] : nullptr;
                    jobject targetObj = isStatic ? nullptr : resolveHostObject(r1, hostEnv, uc);
                    if ((isStatic && realCls) || (!isStatic && targetObj)) {
                        std::string sig = methodHandleMap_.count(r2) ? methodHandleMap_[r2].signature : "()Z";
                        auto unpacked = unpackJniArguments(uc, sig, r3, slotIndex, hostEnv, *this);
                        jboolean bRes = isStatic
                            ? hostEnv->CallStaticBooleanMethodA(realCls, hit->second, unpacked.args.data())
                            : hostEnv->CallBooleanMethodA(targetObj, hit->second, unpacked.args.data());
                        for (auto ref : unpacked.localRefsToDelete) {
                            hostEnv->DeleteLocalRef(ref);
                        }
                        if (hostEnv->ExceptionCheck()) hostEnv->ExceptionClear();
                        setReturnU32(bRes ? 1U : 0U);
                        return;
                    }
                }
            }
            setReturnU32(1);
            return;
        }

        // CallIntMethod / CallStaticIntMethod (inclusi metodi MotionEvent e InputStream)
        case 49: case 50: case 51:
        case 79: case 80: case 81:
        case 129: case 130: case 131: {
            auto hit = hostMethodMap_.find(r2);
            if (hit != hostMethodMap_.end() && hostJavaVm_) {
                JNIEnv* hostEnv = nullptr;
                if (hostJavaVm_->GetEnv(reinterpret_cast<void**>(&hostEnv), JNI_VERSION_1_6) == JNI_OK && hostEnv) {
                    bool isStatic = (slotIndex >= 129);
                    jclass realCls = hostClassMap_.count(r1) ? hostClassMap_[r1] : nullptr;
                    jobject targetObj = isStatic ? nullptr : resolveHostObject(r1, hostEnv, uc);
                    if ((isStatic && realCls) || (!isStatic && targetObj)) {
                        std::string sig = methodHandleMap_.count(r2) ? methodHandleMap_[r2].signature : "()I";
                        auto unpacked = unpackJniArguments(uc, sig, r3, slotIndex, hostEnv, *this);
                        jint iRes = isStatic
                            ? hostEnv->CallStaticIntMethodA(realCls, hit->second, unpacked.args.data())
                            : hostEnv->CallIntMethodA(targetObj, hit->second, unpacked.args.data());
                        for (auto ref : unpacked.localRefsToDelete) {
                            hostEnv->DeleteLocalRef(ref);
                        }
                        if (hostEnv->ExceptionCheck()) hostEnv->ExceptionClear();
                        setReturnU32(static_cast<uint32_t>(iRes));
                        return;
                    }
                }
            }
            auto mit = methodHandleMap_.find(r2);
            if (mit != methodHandleMap_.end()) {
                const std::string& mName = mit->second.name;
                if (mName == "read") {
                    // InputStream.read(byte[] b) o read(byte[] b, int off, int len)
                    auto sit = guestInputStreamMap_.find(r1);
                    if (sit != guestInputStreamMap_.end() && sit->second.fp) {
                        auto ait = arrayHandleMap_.find(r3);
                        if (ait != arrayHandleMap_.end()) {
                            uint32_t off = 0, len = ait->second.second;
                            if (mit->second.signature.find("([BII)") != std::string::npos) {
                                uint32_t sp = 0, pOff = 0, pLen = 0;
                                uc_reg_read(uc, UC_ARM_REG_SP, &sp);
                                uc_mem_read(uc, sp, &pOff, 4);
                                uc_mem_read(uc, sp + 4, &pLen, 4);
                                off = pOff;
                                if (pLen < len) len = pLen;
                            }
                            if (len > 0 && off < ait->second.second) {
                                uint32_t maxRead = std::min(len, ait->second.second - off);
                                std::vector<uint8_t> tmp(maxRead);
                                size_t nRead = fread(tmp.data(), 1, maxRead, sit->second.fp);
                                if (nRead > 0) {
                                    uc_mem_write(uc, ait->second.first + off, tmp.data(), nRead);
                                    setReturnU32(static_cast<uint32_t>(nRead));
                                    return;
                                }
                            }
                            if (feof(sit->second.fp)) {
                                setReturnU32(0xFFFFFFFFU); // -1 EOF
                                return;
                            }
                        }
                    }
                    setReturnU32(0xFFFFFFFFU);
                    return;
                }
                if (mName == "available") {
                    auto sit = guestInputStreamMap_.find(r1);
                    if (sit != guestInputStreamMap_.end() && sit->second.fp) {
                        long cur = ftell(sit->second.fp);
                        long rem = (cur >= 0 && static_cast<size_t>(cur) < sit->second.length)
                            ? (sit->second.length - cur) : 0;
                        setReturnU32(static_cast<uint32_t>(rem));
                        return;
                    }
                    setReturnU32(0);
                    return;
                }
                if (mName == "getAction" || mName == "getActionMasked") {
                    setReturnU32(static_cast<uint32_t>(lastTouchAction_.load()));
                    return;
                }
                if (mName == "getPointerCount") {
                    setReturnU32(1);
                    return;
                }
                if (mName == "getPointerId") {
                    setReturnU32(static_cast<uint32_t>(lastTouchPointerId_.load()));
                    return;
                }
                if (mName == "getSource") {
                    setReturnU32(0x1002U); // SOURCE_TOUCHSCREEN
                    return;
                }
                if (mName == "getWidth") { setReturnU32(1920); return; }
                if (mName == "getHeight") { setReturnU32(1080); return; }
                if (mName == "getRotation") { setReturnU32(1); return; }
                if (mName == "getSplashMode") { setReturnU32(0); return; }
            }
            setReturnU32(0);
            return;
        }

        // CallLongMethod / CallStaticLongMethod
        case 52: case 53: case 54:
        case 82: case 83: case 84:
        case 132: case 133: case 134: {
            auto hit = hostMethodMap_.find(r2);
            if (hit != hostMethodMap_.end() && hostJavaVm_) {
                JNIEnv* hostEnv = nullptr;
                if (hostJavaVm_->GetEnv(reinterpret_cast<void**>(&hostEnv), JNI_VERSION_1_6) == JNI_OK && hostEnv) {
                    bool isStatic = (slotIndex >= 132);
                    jclass realCls = hostClassMap_.count(r1) ? hostClassMap_[r1] : nullptr;
                    jobject targetObj = isStatic ? nullptr : resolveHostObject(r1, hostEnv, uc);
                    if ((isStatic && realCls) || (!isStatic && targetObj)) {
                        std::string sig = methodHandleMap_.count(r2) ? methodHandleMap_[r2].signature : "()J";
                        auto unpacked = unpackJniArguments(uc, sig, r3, slotIndex, hostEnv, *this);
                        jlong jRes = isStatic
                            ? hostEnv->CallStaticLongMethodA(realCls, hit->second, unpacked.args.data())
                            : hostEnv->CallLongMethodA(targetObj, hit->second, unpacked.args.data());
                        for (auto ref : unpacked.localRefsToDelete) {
                            hostEnv->DeleteLocalRef(ref);
                        }
                        if (hostEnv->ExceptionCheck()) hostEnv->ExceptionClear();
                        uint32_t lo = static_cast<uint32_t>(jRes & 0xFFFFFFFFU);
                        uint32_t hi = static_cast<uint32_t>((jRes >> 32) & 0xFFFFFFFFU);
                        uc_reg_write(uc, UC_ARM_REG_R0, &lo);
                        uc_reg_write(uc, UC_ARM_REG_R1, &hi);
                        return;
                    }
                }
            }
            uint32_t zero = 0;
            uc_reg_write(uc, UC_ARM_REG_R0, &zero);
            uc_reg_write(uc, UC_ARM_REG_R1, &zero);
            return;
        }

        // CallDoubleMethod / CallStaticDoubleMethod
        case 58: case 59: case 60:
        case 88: case 89: case 90:
        case 138: case 139: case 140: {
            auto hit = hostMethodMap_.find(r2);
            if (hit != hostMethodMap_.end() && hostJavaVm_) {
                JNIEnv* hostEnv = nullptr;
                if (hostJavaVm_->GetEnv(reinterpret_cast<void**>(&hostEnv), JNI_VERSION_1_6) == JNI_OK && hostEnv) {
                    bool isStatic = (slotIndex >= 138);
                    jclass realCls = hostClassMap_.count(r1) ? hostClassMap_[r1] : nullptr;
                    jobject targetObj = isStatic ? nullptr : resolveHostObject(r1, hostEnv, uc);
                    if ((isStatic && realCls) || (!isStatic && targetObj)) {
                        std::string sig = methodHandleMap_.count(r2) ? methodHandleMap_[r2].signature : "()D";
                        auto unpacked = unpackJniArguments(uc, sig, r3, slotIndex, hostEnv, *this);
                        jdouble dRes = isStatic
                            ? hostEnv->CallStaticDoubleMethodA(realCls, hit->second, unpacked.args.data())
                            : hostEnv->CallDoubleMethodA(targetObj, hit->second, unpacked.args.data());
                        for (auto ref : unpacked.localRefsToDelete) {
                            hostEnv->DeleteLocalRef(ref);
                        }
                        if (hostEnv->ExceptionCheck()) hostEnv->ExceptionClear();
                        uint64_t raw64 = 0;
                        std::memcpy(&raw64, &dRes, 8);
                        uint32_t lo = static_cast<uint32_t>(raw64 & 0xFFFFFFFFU);
                        uint32_t hi = static_cast<uint32_t>((raw64 >> 32) & 0xFFFFFFFFU);
                        uc_reg_write(uc, UC_ARM_REG_R0, &lo);
                        uc_reg_write(uc, UC_ARM_REG_R1, &hi);
                        uc_reg_write(uc, UC_ARM_REG_D0, &raw64);
                        return;
                    }
                }
            }
            uint32_t zero = 0;
            uc_reg_write(uc, UC_ARM_REG_R0, &zero);
            uc_reg_write(uc, UC_ARM_REG_R1, &zero);
            return;
        }

        // CallVoidMethod / CallNonvirtualVoidMethod / CallStaticVoidMethod
        case 61: case 62: case 63:
        case 91: case 92: case 93:
        case 141: case 142: case 143: {
            auto hit = hostMethodMap_.find(r2);
            if (hit != hostMethodMap_.end() && hostJavaVm_) {
                JNIEnv* hostEnv = nullptr;
                if (hostJavaVm_->GetEnv(reinterpret_cast<void**>(&hostEnv), JNI_VERSION_1_6) == JNI_OK && hostEnv) {
                    bool isStatic = (slotIndex >= 141);
                    jclass realCls = hostClassMap_.count(r1) ? hostClassMap_[r1] : nullptr;
                    jobject targetObj = isStatic ? nullptr : resolveHostObject(r1, hostEnv, uc);
                    if ((isStatic && realCls) || (!isStatic && targetObj)) {
                        std::string sig = methodHandleMap_.count(r2) ? methodHandleMap_[r2].signature : "()V";
                        auto unpacked = unpackJniArguments(uc, sig, r3, slotIndex, hostEnv, *this);
                        if (isStatic) {
                            hostEnv->CallStaticVoidMethodA(realCls, hit->second, unpacked.args.data());
                        } else {
                            hostEnv->CallVoidMethodA(targetObj, hit->second, unpacked.args.data());
                        }
                        for (auto ref : unpacked.localRefsToDelete) {
                            hostEnv->DeleteLocalRef(ref);
                        }
                        if (hostEnv->ExceptionCheck()) hostEnv->ExceptionClear();
                        setReturnU32(0);
                        return;
                    }
                }
            }
            auto mit = methodHandleMap_.find(r2);
            if (mit != methodHandleMap_.end()) {
                const auto& info = mit->second;
                if (info.name == "runOnUiThread") {
                    LOGI("JNI Context.runOnUiThread invocato con Runnable=0x%x", r3);
                } else if (info.name == "executeGLThreadJobs") {
                    // Poll periodico da Unity per i job di rendering
                } else if (info.name == "close") {
                    auto sit = guestInputStreamMap_.find(r1);
                    if (sit != guestInputStreamMap_.end()) {
                        if (sit->second.fp) fclose(sit->second.fp);
                        guestInputStreamMap_.erase(sit);
                    }
                }
            }
            setReturnU32(0);
            return;
        }

        // CallFloatMethod / CallStaticFloatMethod (MotionEvent.getX/getY, DisplayMetrics.density)
        case 55: case 56: case 57:
        case 85: case 86: case 87:
        case 135: case 136: case 137: {
            auto hit = hostMethodMap_.find(r2);
            if (hit != hostMethodMap_.end() && hostJavaVm_) {
                JNIEnv* hostEnv = nullptr;
                if (hostJavaVm_->GetEnv(reinterpret_cast<void**>(&hostEnv), JNI_VERSION_1_6) == JNI_OK && hostEnv) {
                    bool isStatic = (slotIndex >= 135);
                    jclass realCls = hostClassMap_.count(r1) ? hostClassMap_[r1] : nullptr;
                    jobject targetObj = isStatic ? nullptr : resolveHostObject(r1, hostEnv, uc);
                    if ((isStatic && realCls) || (!isStatic && targetObj)) {
                        std::string sig = methodHandleMap_.count(r2) ? methodHandleMap_[r2].signature : "()F";
                        auto unpacked = unpackJniArguments(uc, sig, r3, slotIndex, hostEnv, *this);
                        jfloat fRes = isStatic
                            ? hostEnv->CallStaticFloatMethodA(realCls, hit->second, unpacked.args.data())
                            : hostEnv->CallFloatMethodA(targetObj, hit->second, unpacked.args.data());
                        for (auto ref : unpacked.localRefsToDelete) {
                            hostEnv->DeleteLocalRef(ref);
                        }
                        if (hostEnv->ExceptionCheck()) hostEnv->ExceptionClear();
                        uint32_t raw = 0;
                        std::memcpy(&raw, &fRes, 4);
                        setReturnU32(raw);
                        uint64_t d0Val = static_cast<uint64_t>(raw);
                        uc_reg_write(uc, UC_ARM_REG_D0, &d0Val);
                        return;
                    }
                }
            }
            float fVal = 0.0f;
            auto mit = methodHandleMap_.find(r2);
            if (mit != methodHandleMap_.end()) {
                const std::string& mName = mit->second.name;
                if (mName == "getX") fVal = lastTouchX_.load();
                else if (mName == "getY") fVal = lastTouchY_.load();
                else if (mName == "getPressure" || mName == "getSize") fVal = 1.0f;
                else if (mName == "getRefreshRate") fVal = 60.0f;
                else if (mName == "getDensity" || mName == "getScaledDensity") fVal = 2.0f;
                else if (mName == "getXdpi" || mName == "getYdpi") fVal = 320.0f;
            }
            uint32_t raw = 0;
            std::memcpy(&raw, &fVal, 4);
            setReturnU32(raw);
            uint64_t d0Val = static_cast<uint64_t>(raw);
            uc_reg_write(uc, UC_ARM_REG_D0, &d0Val);
            return;
        }

        // GetObjectField / GetStaticObjectField
        case 95:
        case 145: {
            auto fit = fieldHandleMap_.find(r2);
            if (fit != fieldHandleMap_.end()) {
                const auto& f = fit->second;
                if (f.name == "sourceDir" || f.name == "publicSourceDir") {
                    setReturnU32(createGuestJString(uc, apkPath_));
                    return;
                }
                if (f.name == "dataDir") {
                    setReturnU32(createGuestJString(uc, filesDir_));
                    return;
                }
                if (f.name == "nativeLibraryDir") {
                    setReturnU32(createGuestJString(uc, nativeLibDir_));
                    return;
                }
                if (f.name == "packageName") {
                    setReturnU32(createGuestJString(uc, packageName_));
                    return;
                }
                if (f.signature == "Ljava/lang/String;") {
                    setReturnU32(createGuestJString(uc, ""));
                    return;
                }
            }
            setReturnU32(nextMockHandle_++);
            return;
        }

        // GetBooleanField / GetStaticBooleanField
        case 96:
        case 146:
            setReturnU32(1);
            return;

        // GetIntField / GetStaticIntField (es. Build$VERSION.SDK_INT, DisplayMetrics.widthPixels)
        case 100:
        case 150: {
            auto fit = fieldHandleMap_.find(r2);
            if (fit != fieldHandleMap_.end()) {
                const auto& fName = fit->second.name;
                if (fName == "SDK_INT") { setReturnU32(27); return; }
                if (fName == "widthPixels") { setReturnU32(1920); return; }
                if (fName == "heightPixels") { setReturnU32(1080); return; }
                if (fName == "densityDpi") { setReturnU32(320); return; }
            }
            setReturnU32(0);
            return;
        }

        // GetFloatField / GetStaticFloatField (DisplayMetrics.density, scaledDensity, xdpi, ydpi)
        case 102:
        case 152: {
            float fVal = 1.0f;
            auto fit = fieldHandleMap_.find(r2);
            if (fit != fieldHandleMap_.end()) {
                const auto& fName = fit->second.name;
                if (fName == "density" || fName == "scaledDensity") {
                    fVal = 2.0f;
                } else if (fName == "xdpi" || fName == "ydpi") {
                    fVal = 320.0f;
                }
            }
            uint32_t raw = 0;
            std::memcpy(&raw, &fVal, 4);
            setReturnU32(raw);
            uint64_t d0Val = static_cast<uint64_t>(raw);
            uc_reg_write(uc, UC_ARM_REG_D0, &d0Val);
            return;
        }

        // String Operations (163..170)
        case 163: // NewString(env, const jchar*, jsize)
        case 167: { // NewStringUTF(env, const char* bytes)
            std::string s = (slotIndex == 167) ? readGuestCString(uc, r1) : "";
            setReturnU32(createGuestJString(uc, s));
            return;
        }
        case 164: // GetStringLength
        case 168: { // GetStringUTFLength
            auto it = jstringMap_.find(r1);
            setReturnU32(it != jstringMap_.end() ? static_cast<uint32_t>(it->second.size()) : 0U);
            return;
        }
        case 165: // GetStringChars
        case 169: // GetStringUTFChars(env, jstring, jboolean* isCopy)
        case 224: { // GetStringCritical
            if (r2 != 0) {
                uint8_t zero = 0;
                uc_mem_write(uc, r2, &zero, 1);
            }
            auto it = jstringGuestPtrMap_.find(r1);
            if (it != jstringGuestPtrMap_.end()) {
                setReturnU32(it->second);
            } else {
                setReturnU32(createGuestString(uc, ""));
            }
            return;
        }
        case 166: // ReleaseStringChars
        case 170: // ReleaseStringUTFChars
        case 225: // ReleaseStringCritical
            setReturnU32(0);
            return;

        // Array Operations (171..214)
        case 171: { // GetArrayLength
            auto it = arrayHandleMap_.find(r1);
            setReturnU32(it != arrayHandleMap_.end() ? it->second.second : 0U);
            return;
        }
        case 172: // NewObjectArray
        case 175: case 176: case 177: case 178:
        case 179: case 180: case 181: case 182: { // New<Primitive>Array(env, jsize len)
            uint32_t len = (r1 < 0x100000U) ? r1 : 0U;
            uint32_t arrH = nextMockHandle_++;
            uint32_t buf = allocGuestHeap(uc, len * 8U + 16U, 16);
            arrayHandleMap_[arrH] = {buf, len};
            setReturnU32(arrH);
            return;
        }
        case 173: { // GetObjectArrayElement(env, jobjectArray, jsize index)
            uint32_t val = 0;
            auto it = arrayHandleMap_.find(r1);
            if (it != arrayHandleMap_.end() && r2 < it->second.second) {
                uc_mem_read(uc, it->second.first + (r2 * 4U), &val, 4);
            }
            setReturnU32(val);
            return;
        }
        case 174: { // SetObjectArrayElement(env, jobjectArray, jsize index, jobject val)
            auto it = arrayHandleMap_.find(r1);
            if (it != arrayHandleMap_.end() && r2 < it->second.second) {
                uc_mem_write(uc, it->second.first + (r2 * 4U), &r3, 4);
            }
            setReturnU32(0);
            return;
        }
        case 183: case 184: case 185: case 186:
        case 187: case 188: case 189: case 190:
        case 222: { // Get<Primitive>ArrayElements / GetPrimitiveArrayCritical
            if (r2 != 0) {
                uint8_t zero = 0;
                uc_mem_write(uc, r2, &zero, 1);
            }
            auto it = arrayHandleMap_.find(r1);
            if (it != arrayHandleMap_.end()) {
                setReturnU32(it->second.first);
            } else {
                setReturnU32(allocGuestHeap(uc, 64, 16));
            }
            return;
        }
        case 191: case 192: case 193: case 194:
        case 195: case 196: case 197: case 198:
        case 223: // Release<Primitive>ArrayElements
            setReturnU32(0);
            return;

        case 199: case 200: case 201: case 202:
        case 203: case 204: case 205: case 206: { // Get<Primitive>ArrayRegion(env, array, start, len, buf)
            uint32_t sp = 0, dstBuf = 0;
            uc_reg_read(uc, UC_ARM_REG_SP, &sp);
            uc_mem_read(uc, sp, &dstBuf, 4);
            auto it = arrayHandleMap_.find(r1);
            if (it != arrayHandleMap_.end() && dstBuf != 0 && r3 > 0 && r3 < 0x100000U) {
                size_t elemSz = (slotIndex == 199 || slotIndex == 200) ? 1 : ((slotIndex == 201 || slotIndex == 202) ? 2 : ((slotIndex == 204 || slotIndex == 206) ? 8 : 4));
                std::vector<uint8_t> tmp(r3 * elemSz, 0);
                uc_mem_read(uc, it->second.first + (r2 * elemSz), tmp.data(), tmp.size());
                uc_mem_write(uc, dstBuf, tmp.data(), tmp.size());
            }
            setReturnU32(0);
            return;
        }

        case 207: case 208: case 209: case 210:
        case 211: case 212: case 213: case 214: { // Set<Primitive>ArrayRegion(env, array, start, len, buf)
            uint32_t sp = 0, srcBuf = 0;
            uc_reg_read(uc, UC_ARM_REG_SP, &sp);
            uc_mem_read(uc, sp, &srcBuf, 4);
            auto it = arrayHandleMap_.find(r1);
            if (it != arrayHandleMap_.end() && srcBuf != 0 && r3 > 0 && r3 < 0x100000U) {
                size_t elemSz = (slotIndex == 207 || slotIndex == 208) ? 1 : ((slotIndex == 209 || slotIndex == 210) ? 2 : ((slotIndex == 212 || slotIndex == 214) ? 8 : 4));
                std::vector<uint8_t> tmp(r3 * elemSz, 0);
                uc_mem_read(uc, srcBuf, tmp.data(), tmp.size());
                uc_mem_write(uc, it->second.first + (r2 * elemSz), tmp.data(), tmp.size());
            }
            setReturnU32(0);
            return;
        }

        // SLOT 215 (offset 0x35C): RegisterNatives(JNIEnv* env, jclass clazz, const JNINativeMethod* methods, jint nMethods)
        case 215: {
            uint32_t methodsAddr = r2;
            uint32_t nMethods = r3;
            std::string clsName = classHandleMap_.count(r1) ? classHandleMap_[r1] : "UnknownClass";
            LOGI("JNI RegisterNatives invocato per classe '%s': %u metodi (methodsAddr=0x%08x)",
                 clsName.c_str(), nMethods, methodsAddr);

            for (uint32_t i = 0; i < nMethods && i < 128; ++i) {
                uint32_t entryAddr = methodsAddr + (i * 12U);
                uint32_t namePtr = 0, sigPtr = 0, fnPtr = 0;
                uc_mem_read(uc, entryAddr, &namePtr, 4);
                uc_mem_read(uc, entryAddr + 4, &sigPtr, 4);
                uc_mem_read(uc, entryAddr + 8, &fnPtr, 4);

                std::string mName = readGuestCString(uc, namePtr, 128);
                std::string mSig = readGuestCString(uc, sigPtr, 128);
                LOGI("   -> Registrato JNINativeMethod[%u]: %s.%s %s -> Entry: 0x%08x",
                     i, clsName.c_str(), mName.c_str(), mSig.c_str(), fnPtr);
                addRegisteredMethod(clsName, mName, mSig, fnPtr);
            }
            setReturnU32(0); // JNI_OK
            return;
        }

        case 219: { // GetJavaVM(JNIEnv* env, JavaVM** vm)
            if (r1 != 0) {
                uc_mem_write(uc, r1, &guestJavaVMAddr_, 4);
            }
            setReturnU32(0); // JNI_OK
            return;
        }

        case 229: { // NewDirectByteBuffer(env, void* address, jlong capacity)
            uint32_t h = nextMockHandle_++;
            arrayHandleMap_[h] = {r1, r2};
            setReturnU32(h);
            return;
        }
        case 230: { // GetDirectBufferAddress
            auto it = arrayHandleMap_.find(r1);
            setReturnU32(it != arrayHandleMap_.end() ? it->second.first : 0U);
            return;
        }
        case 231: { // GetDirectBufferCapacity
            auto it = arrayHandleMap_.find(r1);
            uint32_t cap = it != arrayHandleMap_.end() ? it->second.second : 0U;
            uint32_t zero = 0;
            uc_reg_write(uc, UC_ARM_REG_R0, &cap);
            uc_reg_write(uc, UC_ARM_REG_R1, &zero);
            return;
        }

        default:
            setReturnU32(0);
            return;
    }
}

static std::string formatGuestPrintf(uc_engine* uc, const std::string& fmt, uint32_t firstArgRegIdx, uint32_t vaListGuestAddr) {
    uint32_t rArgs[4] = {0};
    uc_reg_read(uc, UC_ARM_REG_R0, &rArgs[0]);
    uc_reg_read(uc, UC_ARM_REG_R1, &rArgs[1]);
    uc_reg_read(uc, UC_ARM_REG_R2, &rArgs[2]);
    uc_reg_read(uc, UC_ARM_REG_R3, &rArgs[3]);
    uint32_t sp = 0;
    uc_reg_read(uc, UC_ARM_REG_SP, &sp);

    uint32_t regCursor = firstArgRegIdx;
    uint32_t memCursor = (vaListGuestAddr != 0) ? vaListGuestAddr : sp;
    bool useVaPtr = (vaListGuestAddr != 0);

    auto nextU32 = [&]() -> uint32_t {
        if (!useVaPtr && regCursor < 4) {
            return rArgs[regCursor++];
        }
        uint32_t val = 0;
        uc_mem_read(uc, memCursor, &val, 4);
        memCursor += 4;
        return val;
    };

    auto nextU64 = [&]() -> uint64_t {
        if (!useVaPtr) {
            if (regCursor == 1 || regCursor == 3) {
                regCursor++;
            }
            if (regCursor == 0 || regCursor == 2) {
                uint32_t lo = rArgs[regCursor++];
                uint32_t hi = rArgs[regCursor++];
                return (static_cast<uint64_t>(hi) << 32) | lo;
            }
        }
        memCursor = (memCursor + 7U) & ~7U;
        uint32_t lo = 0, hi = 0;
        uc_mem_read(uc, memCursor, &lo, 4);
        uc_mem_read(uc, memCursor + 4, &hi, 4);
        memCursor += 8;
        return (static_cast<uint64_t>(hi) << 32) | lo;
    };

    std::string out;
    out.reserve(fmt.size() + 64);

    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%') {
            out.push_back(fmt[i]);
            continue;
        }
        ++i;
        if (i >= fmt.size()) break;
        if (fmt[i] == '%') {
            out.push_back('%');
            continue;
        }

        // Salta flag, width, precision e length modifiers
        bool isLongLong = false;
        while (i < fmt.size() && (fmt[i] == '-' || fmt[i] == '+' || fmt[i] == ' ' || fmt[i] == '#' || fmt[i] == '0' ||
               (fmt[i] >= '1' && fmt[i] <= '9') || fmt[i] == '.' || fmt[i] == '*')) {
            if (fmt[i] == '*') (void)nextU32();
            ++i;
        }
        while (i < fmt.size() && (fmt[i] == 'l' || fmt[i] == 'h' || fmt[i] == 'z' || fmt[i] == 't')) {
            if (fmt[i] == 'l' && i + 1 < fmt.size() && fmt[i + 1] == 'l') {
                isLongLong = true;
                i += 2;
                break;
            }
            ++i;
        }
        if (i >= fmt.size()) break;

        char spec = fmt[i];
        char buf[64] = {0};
        switch (spec) {
            case 's': {
                uint32_t sPtr = nextU32();
                out += (sPtr != 0) ? readGuestCString(uc, sPtr, 512) : "(null)";
                break;
            }
            case 'd':
            case 'i': {
                if (isLongLong) {
                    long long v = static_cast<long long>(nextU64());
                    std::snprintf(buf, sizeof(buf), "%lld", v);
                } else {
                    std::snprintf(buf, sizeof(buf), "%d", static_cast<int32_t>(nextU32()));
                }
                out += buf;
                break;
            }
            case 'u': {
                if (isLongLong) {
                    unsigned long long v = nextU64();
                    std::snprintf(buf, sizeof(buf), "%llu", v);
                } else {
                    std::snprintf(buf, sizeof(buf), "%u", nextU32());
                }
                out += buf;
                break;
            }
            case 'x':
            case 'X':
            case 'p': {
                std::snprintf(buf, sizeof(buf), (spec == 'X') ? "%X" : "%x", nextU32());
                out += buf;
                break;
            }
            case 'c': {
                char c = static_cast<char>(nextU32() & 0xFFU);
                out.push_back(c);
                break;
            }
            case 'f':
            case 'g':
            case 'e': {
                uint64_t raw = nextU64();
                double d = 0.0;
                std::memcpy(&d, &raw, 8);
                std::snprintf(buf, sizeof(buf), "%g", d);
                out += buf;
                break;
            }
            default:
                break;
        }
    }
    return out;
}

bool GuestRuntimeEnvironment::disarmGuestTrapFlag(uc_engine* uc, uint32_t lr) {
    if (!uc || lr < 0x1000) return false;

    // 1. Diagnostic Dump: Registri CPU R0-R12, SP, LR, CPSR
    uint32_t regs[13] = {0};
    for (int i = 0; i < 13; ++i) {
        uc_reg_read(uc, UC_ARM_REG_R0 + i, &regs[i]);
    }
    uint32_t sp = 0, cpsr = 0;
    uc_reg_read(uc, UC_ARM_REG_SP, &sp);
    uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
    LOGW("[GuestLogCrashDiagnostic] Trap Dump -> R0=0x%08x R1=0x%08x R2=0x%08x R3=0x%08x",
         regs[0], regs[1], regs[2], regs[3]);
    LOGW("[GuestLogCrashDiagnostic] Trap Dump -> R4=0x%08x R5=0x%08x R6=0x%08x R7=0x%08x R8=0x%08x R9=0x%08x R10=0x%08x R11=0x%08x R12=0x%08x",
         regs[4], regs[5], regs[6], regs[7], regs[8], regs[9], regs[10], regs[11], regs[12]);
    LOGW("[GuestLogCrashDiagnostic] Trap Dump -> SP=0x%08x LR=0x%08x CPSR=0x%08x", sp, lr, cpsr);

    // 2. Diagnostic Dump: 16 parole dallo Stack (SP)
    if (sp >= 0x70000000U && sp <= 0x7FFFF000U) {
        uint32_t stackWords[16] = {0};
        if (uc_mem_read(uc, sp, stackWords, sizeof(stackWords)) == UC_ERR_OK) {
            LOGW("[GuestLogCrashDiagnostic] Stack Dump [SP..SP+64]: %08x %08x %08x %08x | %08x %08x %08x %08x",
                 stackWords[0], stackWords[1], stackWords[2], stackWords[3],
                 stackWords[4], stackWords[5], stackWords[6], stackWords[7]);
        }
    }

    // 3. Diagnostic Code Dump: 64 byte prima di LR e 16 byte dopo LR
    uint32_t startAddr = (lr >= 64) ? (lr - 64) : 0;
    uint32_t countBytes = 80;
    std::vector<uint8_t> codeBuf(countBytes);
    if (uc_mem_read(uc, startAddr, codeBuf.data(), countBytes) == UC_ERR_OK) {
        for (uint32_t off = 0; off + 4 <= countBytes; off += 16) {
            uint32_t pc = startAddr + off;
            uint32_t w0 = 0, w1 = 0, w2 = 0, w3 = 0;
            std::memcpy(&w0, codeBuf.data() + off, 4);
            if (off + 8 <= countBytes) std::memcpy(&w1, codeBuf.data() + off + 4, 4);
            if (off + 12 <= countBytes) std::memcpy(&w2, codeBuf.data() + off + 8, 4);
            if (off + 16 <= countBytes) std::memcpy(&w3, codeBuf.data() + off + 12, 4);
            LOGW("[GuestLogCrashDiagnostic] Code [%08x%s]: %08x %08x %08x %08x",
                 pc, (pc <= lr && lr < pc + 16 ? " <= LR" : "      "), w0, w1, w2, w3);
        }
    }

    bool disarmed = false;

    // 4. Memory Scan: scansione segmenti scrivibili (.data / .bss) di tutte le librerie per signum = 1060439283 (0x3F3504F3)
    const uint32_t targetSignum = 1060439283U; // 0x3F3504F3
    for (const auto& lib : loadedLibraries_) {
        if (!lib) continue;
        for (const auto& seg : lib->segments) {
            if ((seg.flags & 2) /* PF_W */ && seg.memsz > 0 && seg.memsz < 0x2000000U) {
                uint32_t segBase = lib->load_bias + seg.vaddr;
                size_t scanSize = std::min<size_t>(seg.memsz, 0x400000U); // fino a 4MB
                std::vector<uint8_t> segData(scanSize);
                if (uc_mem_read(uc, segBase, segData.data(), scanSize) == UC_ERR_OK) {
                    for (size_t off = 0; off + 4 <= scanSize; off += 4) {
                        uint32_t word = 0;
                        std::memcpy(&word, segData.data() + off, 4);
                        if (word == targetSignum) {
                            uint32_t foundAddr = segBase + off;
                            LOGI("DisarmTrap [MemoryScan]: Trovato signum 0x%08x a 0x%08x in '%s'! Neutralizzazione...",
                                 targetSignum, foundAddr, lib->path.c_str());
                            uint32_t zero = 0;
                            uc_mem_write(uc, foundAddr, &zero, 4);
                            // Neutralizza anche variabili booleane limitrofe [-32..+32] byte
                            for (int delta = -32; delta <= 32; delta += 4) {
                                if (delta == 0) continue;
                                uint32_t nAddr = foundAddr + delta;
                                uint32_t nVal = 0;
                                if (uc_mem_read(uc, nAddr, &nVal, 4) == UC_ERR_OK && nVal != 0 && nVal < 0x1000U) {
                                    uc_mem_write(uc, nAddr, &zero, 4);
                                    LOGI("DisarmTrap [MemoryScan]: Neutralizzato flag limitrofo a 0x%08x (era 0x%08x)", nAddr, nVal);
                                }
                            }
                            disarmed = true;
                        }
                    }
                }
            }
        }
    }

    // 5. Instruction Scan around LR: ARM LDR Rd, [PC, #+/-imm12]
    // Maschera corretta: (insn & 0x0F3F0000U) == 0x051F0000U
    for (uint32_t off = 0; off + 4 <= countBytes; off += 4) {
        uint32_t pc = startAddr + off;
        uint32_t insn = 0;
        std::memcpy(&insn, codeBuf.data() + off, 4);

        if ((insn & 0x0F3F0000U) == 0x051F0000U) {
            bool add = (insn & (1U << 23)) != 0;
            uint32_t imm12 = insn & 0xFFFU;
            uint32_t poolAddr = add ? (pc + 8 + imm12) : (pc + 8 - imm12);
            uint32_t targetVarAddr = 0;
            if (uc_mem_read(uc, poolAddr, &targetVarAddr, 4) == UC_ERR_OK) {
                if (targetVarAddr >= 0x40000000U && targetVarAddr < 0x60000000U) {
                    uint32_t currentVal = 0;
                    if (uc_mem_read(uc, targetVarAddr, &currentVal, 4) == UC_ERR_OK && currentVal != 0) {
                        uint32_t zero = 0;
                        uc_mem_write(uc, targetVarAddr, &zero, 4);
                        LOGI("DisarmTrap [ARM Direct]: Neutralizzato flag trappola guest a 0x%08x (era 0x%08x)",
                             targetVarAddr, currentVal);
                        disarmed = true;
                    }
                    // Supporto indirection GOT (puntatore a variabile)
                    if (currentVal >= 0x40000000U && currentVal < 0x60000000U) {
                        uint32_t indVal = 0;
                        if (uc_mem_read(uc, currentVal, &indVal, 4) == UC_ERR_OK && indVal != 0) {
                            uint32_t zero = 0;
                            uc_mem_write(uc, currentVal, &zero, 4);
                            LOGI("DisarmTrap [ARM GOT-Indirect]: Neutralizzato flag a 0x%08x (era 0x%08x)",
                                 currentVal, indVal);
                            disarmed = true;
                        }
                    }
                }
            }
        }
    }

    // 6. Thumb LDR Rd, [PC, #imm8*4]
    for (uint32_t off = 0; off + 2 <= countBytes; off += 2) {
        uint32_t pc = startAddr + off;
        uint16_t tInsn = 0;
        std::memcpy(&tInsn, codeBuf.data() + off, 2);
        if ((tInsn & 0xF800U) == 0x4800U) { // LDR Rd, [PC, #imm8*4]
            uint32_t imm = (tInsn & 0xFFU) * 4;
            uint32_t poolAddr = ((pc + 4) & ~3U) + imm;
            uint32_t targetVarAddr = 0;
            if (uc_mem_read(uc, poolAddr, &targetVarAddr, 4) == UC_ERR_OK) {
                if (targetVarAddr >= 0x40000000U && targetVarAddr < 0x60000000U) {
                    uint32_t currentVal = 0;
                    if (uc_mem_read(uc, targetVarAddr, &currentVal, 4) == UC_ERR_OK && currentVal != 0) {
                        uint32_t zero = 0;
                        uc_mem_write(uc, targetVarAddr, &zero, 4);
                        LOGI("DisarmTrap [Thumb]: Neutralizzato flag trappola guest a 0x%08x (era 0x%08x)",
                             targetVarAddr, currentVal);
                        disarmed = true;
                    }
                }
            }
        }
    }

    // 7. Register Pointer Scan: R4..R12
    for (int i = 4; i <= 12; ++i) {
        uint32_t regVal = regs[i];
        if (regVal >= 0x40000000U && regVal < 0x60000000U) {
            uint32_t valAtReg = 0;
            if (uc_mem_read(uc, regVal, &valAtReg, 4) == UC_ERR_OK && valAtReg != 0) {
                if (valAtReg == 1 || valAtReg == targetSignum || valAtReg < 0x1000U) {
                    uint32_t zero = 0;
                    uc_mem_write(uc, regVal, &zero, 4);
                    LOGI("DisarmTrap [Reg R%d]: Neutralizzato flag puntato da registro a 0x%08x (era 0x%08x)",
                         i, regVal, valAtReg);
                    disarmed = true;
                }
            }
        }
    }

    return disarmed;
}

#pragma pack(push, 4)
struct z_stream32 {
    uint32_t next_in;
    uint32_t avail_in;
    uint32_t total_in;
    uint32_t next_out;
    uint32_t avail_out;
    uint32_t total_out;
    uint32_t msg;
    uint32_t state;
    uint32_t zalloc;
    uint32_t zfree;
    uint32_t opaque;
    int32_t  data_type;
    uint32_t adler;
    uint32_t reserved;
};
#pragma pack(pop)

void GuestRuntimeEnvironment::handleTrampolineCall(uint32_t trampolineId, uc_engine* uc) {
    if (!uc) return;

    // 1. Intercettazione vtable JavaVM32 (0x3000 .. 0x3007)
    if (trampolineId >= 0x3000U && trampolineId < 0x3008U) {
        handleJavaVmCall(trampolineId - 0x3000U, uc);
        return;
    }

    // 2. Intercettazione vtable JNIEnv32 completa a 256 slot (0x4000 .. 0x40FF)
    if (trampolineId >= 0x4000U && trampolineId < 0x4100U) {
        handleJniCall(trampolineId - 0x4000U, uc);
        return;
    }

    // 2b. Intercettazione OpenSL ES Method Calls (0x5000 .. 0x50FF)
    if (trampolineId >= 0x5000U && trampolineId < 0x5100U) {
        AudioThunker::getInstance().handleMethodCall(trampolineId - 0x5000U, uc);
        return;
    }

    auto it = trampolineToSymbolMap_.find(trampolineId - 0x2000U);
    if (it == trampolineToSymbolMap_.end()) {
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }

    const std::string& name = it->second;

    // 1. Android Logcat
    if (name == "__android_log_print" || name == "__android_log_vprint") {
        uint32_t tagAddr = 0, fmtAddr = 0, vaAddr = 0;
        uc_reg_read(uc, UC_ARM_REG_R1, &tagAddr);
        uc_reg_read(uc, UC_ARM_REG_R2, &fmtAddr);
        uc_reg_read(uc, UC_ARM_REG_R3, &vaAddr);
        std::string tag = readGuestCString(uc, tagAddr, 64);
        std::string fmt = readGuestCString(uc, fmtAddr, 512);
        std::string formatted = (name == "__android_log_vprint")
            ? formatGuestPrintf(uc, fmt, 4, vaAddr)
            : formatGuestPrintf(uc, fmt, 3, 0);
        LOGI("[GuestLog][%s] %s", tag.c_str(), formatted.c_str());
        if (tag == "CRASH" || fmt.find("trapped") != std::string::npos) {
            uint32_t sp = 0, lr = 0, pc = 0;
            uc_reg_read(uc, UC_ARM_REG_SP, &sp);
            uc_reg_read(uc, UC_ARM_REG_LR, &lr);
            uc_reg_read(uc, UC_ARM_REG_PC, &pc);
            LOGW("[GuestLogCrashDiagnostic] Trap detect: tag='%s', fmt='%s', PC=0x%08x, LR=0x%08x, SP=0x%08x",
                 tag.c_str(), fmt.c_str(), pc, lr, sp);
            disarmGuestTrapFlag(uc, lr);
        }
        uint32_t ret = 1;
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }

    if (name == "__android_log_write" || name == "__android_log_assert") {
        uint32_t r1 = 0, r2 = 0;
        uc_reg_read(uc, UC_ARM_REG_R1, &r1);
        uc_reg_read(uc, UC_ARM_REG_R2, &r2);
        std::string tag = readGuestCString(uc, r1, 64);
        std::string msg = readGuestCString(uc, r2, 512);
        LOGI("[GuestLog][%s] %s", tag.c_str(), msg.c_str());
        uint32_t ret = 1;
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }

    // 2. Bionic C++ ABI & System Globals
    if (name == "__errno") {
        uint32_t errnoAddr = guestJniBase_ + 0x2000U;
        uc_reg_write(uc, UC_ARM_REG_R0, &errnoAddr);
        return;
    }
    if (name == "__aeabi_atexit" || name == "__cxa_atexit" || name == "atexit" || name == "__cxa_finalize") {
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }
    if (name == "__gnu_Unwind_Find_exidx") {
        uint32_t pcount = 0, zero = 0;
        uc_reg_read(uc, UC_ARM_REG_R1, &pcount);
        if (pcount) uc_mem_write(uc, pcount, &zero, 4);
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }
    if (name == "__system_property_get") {
        uint32_t nameAddr = 0, valAddr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &nameAddr);
        uc_reg_read(uc, UC_ARM_REG_R1, &valAddr);
        std::string propName = readGuestCString(uc, nameAddr, 128);
        std::string propVal;
        if (propName == "ro.build.version.sdk") propVal = "27";
        else if (propName == "ro.product.cpu.abi") propVal = "armeabi-v7a";
        else if (propName == "ro.product.model" || propName == "ro.product.device") propVal = "Emux32";
        else if (propName == "ro.opengles.version") propVal = "196608";
        else if (propName == "ro.debuggable") propVal = "0";
        if (valAddr != 0) {
            uc_mem_write(uc, valAddr, propVal.c_str(), propVal.size() + 1);
        }
        uint32_t len = static_cast<uint32_t>(propVal.size());
        uc_reg_write(uc, UC_ARM_REG_R0, &len);
        return;
    }
    if (name == "sysconf") {
        uint32_t scName = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &scName);
        uint32_t ret = (scName == 96 || scName == 97) ? 4U : 4096U;
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }
    if (name == "getenv") {
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }
    if (name == "getauxval") {
        uint32_t type = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &type);
        uint32_t ret = 0;
        switch (type) {
            case 16: ret = 0x000F7F96U; break; // AT_HWCAP (VFP, NEON, VFPv3, VFPv4, IDIVA, IDIVT, EDSP, THUMB, HALF, FAST_MULT, TLS)
            case 26: ret = 0x0000001FU; break; // AT_HWCAP2 (AES, PMULL, SHA1, SHA2, CRC32)
            case 6:  ret = 4096U; break;       // AT_PAGESZ
            case 17: ret = 100U; break;        // AT_CLKTCK
            case 11: ret = static_cast<uint32_t>(getuid()); break;
            case 12: ret = static_cast<uint32_t>(geteuid()); break;
            case 13: ret = static_cast<uint32_t>(getgid()); break;
            case 14: ret = static_cast<uint32_t>(getegid()); break;
            case 23: ret = 0U; break;          // AT_SECURE
            default: ret = 0U; break;
        }
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }
    if (name == "mmap" || name == "mmap64") {
        uint32_t addr = 0, len = 0, prot = 0, flags = 0, fd = 0, offset = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &addr);
        uc_reg_read(uc, UC_ARM_REG_R1, &len);
        uc_reg_read(uc, UC_ARM_REG_R2, &prot);
        uc_reg_read(uc, UC_ARM_REG_R3, &flags);
        uint32_t sp = 0;
        uc_reg_read(uc, UC_ARM_REG_SP, &sp);
        if (sp != 0) {
            uc_mem_read(uc, sp, &fd, 4);
            uc_mem_read(uc, sp + 4, &offset, 4);
        }
        uint32_t res = GuestMemory::getSharedInstance().handleMmap(addr, len, static_cast<int>(prot), static_cast<int>(flags), static_cast<int>(fd), static_cast<uint64_t>(offset), uc);
        uc_reg_write(uc, UC_ARM_REG_R0, &res);
        return;
    }
    if (name == "munmap") {
        uint32_t addr = 0, len = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &addr);
        uc_reg_read(uc, UC_ARM_REG_R1, &len);
        int res = GuestMemory::getSharedInstance().handleMunmap(addr, len, uc);
        uint32_t ret = static_cast<uint32_t>(res);
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }
    if (name == "mprotect") {
        uint32_t addr = 0, len = 0, prot = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &addr);
        uc_reg_read(uc, UC_ARM_REG_R1, &len);
        uc_reg_read(uc, UC_ARM_REG_R2, &prot);
        int res = GuestMemory::getSharedInstance().handleMprotect(addr, len, static_cast<int>(prot), uc);
        uint32_t ret = static_cast<uint32_t>(res);
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }

    // 3. Allocazione Memoria Heap (malloc, calloc, realloc, free, posix_memalign, memalign, operator new/delete)
    if (name == "malloc" || name == "_Znwj" || name == "_Znaj" || name == "_ZnwjRKSt9nothrow_t" || name == "_ZnajRKSt9nothrow_t") {
        uint32_t size = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &size);
        uint32_t allocAddr = allocGuestHeap(uc, size, 16);
        uc_reg_write(uc, UC_ARM_REG_R0, &allocAddr);
        return;
    }
    if (name == "calloc") {
        uint32_t num = 0, size = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &num);
        uc_reg_read(uc, UC_ARM_REG_R1, &size);
        uint32_t total = num * size;
        uint32_t allocAddr = allocGuestHeap(uc, total, 16);
        if (total > 0 && total < 0x04000000U) {
            std::vector<uint8_t> zeroes(total, 0);
            uc_mem_write(uc, allocAddr, zeroes.data(), total);
        }
        uc_reg_write(uc, UC_ARM_REG_R0, &allocAddr);
        return;
    }
    if (name == "realloc") {
        uint32_t ptr = 0, size = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &ptr);
        uc_reg_read(uc, UC_ARM_REG_R1, &size);
        uint32_t newAddr = reallocGuestHeap(uc, ptr, size);
        uc_reg_write(uc, UC_ARM_REG_R0, &newAddr);
        return;
    }
    if (name == "valloc") {
        uint32_t size = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &size);
        uint32_t allocAddr = allocGuestHeap(uc, size, 4096);
        uc_reg_write(uc, UC_ARM_REG_R0, &allocAddr);
        return;
    }
    if (name == "posix_memalign") {
        uint32_t memptrAddr = 0, alignment = 0, size = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &memptrAddr);
        uc_reg_read(uc, UC_ARM_REG_R1, &alignment);
        uc_reg_read(uc, UC_ARM_REG_R2, &size);
        uint32_t allocAddr = allocGuestHeap(uc, size, alignment > 16 ? alignment : 16);
        if (memptrAddr != 0) {
            uc_mem_write(uc, memptrAddr, &allocAddr, 4);
        }
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }
    if (name == "memalign") {
        uint32_t alignment = 0, size = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &alignment);
        uc_reg_read(uc, UC_ARM_REG_R1, &size);
        uint32_t allocAddr = allocGuestHeap(uc, size, alignment > 16 ? alignment : 16);
        uc_reg_write(uc, UC_ARM_REG_R0, &allocAddr);
        return;
    }
    if (name == "free" || name == "_ZdlPv" || name == "_ZdaPv") {
        uint32_t ptr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &ptr);
        if (ptr != 0) {
            freeGuestHeap(ptr, uc);
        }
        return;
    }

    // 3b. OpenSL ES Audio Thunking
    if (AudioThunker::getInstance().handleAudioCall(name, uc)) {
        return;
    }

    // 4. Operazioni di Memoria, Stringhe e Formattazione (AEABI & Libc)
    if (name == "memcpy" || name == "memmove" || name == "__aeabi_memcpy" || name == "__aeabi_memcpy4" ||
        name == "__aeabi_memcpy8" || name == "__aeabi_memmove" || name == "__aeabi_memmove4" || name == "__aeabi_memmove8") {
        uint32_t dst = 0, src = 0, count = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &dst);
        uc_reg_read(uc, UC_ARM_REG_R1, &src);
        uc_reg_read(uc, UC_ARM_REG_R2, &count);
        if (count > 0 && dst != 0 && src != 0 && count < 0x08000000U) {
            std::vector<uint8_t> tmp(count);
            if (uc_mem_read(uc, src, tmp.data(), count) == UC_ERR_OK) {
                uc_mem_write(uc, dst, tmp.data(), count);
            }
        }
        uc_reg_write(uc, UC_ARM_REG_R0, &dst);
        return;
    }
    if (name == "memset" || name == "__aeabi_memset" || name == "__aeabi_memset4" || name == "__aeabi_memset8" ||
        name == "__aeabi_memclr" || name == "__aeabi_memclr4" || name == "__aeabi_memclr8") {
        uint32_t dst = 0, a1 = 0, a2 = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &dst);
        uc_reg_read(uc, UC_ARM_REG_R1, &a1);
        uc_reg_read(uc, UC_ARM_REG_R2, &a2);
        uint32_t count = a2;
        uint8_t val = static_cast<uint8_t>(a1);
        if (name.find("__aeabi_memclr") == 0) {
            count = a1;
            val = 0;
        } else if (name.find("__aeabi_memset") == 0) {
            // In __aeabi_memset(void *dest, size_t n, int c): r1 = n, r2 = c
            count = a1;
            val = static_cast<uint8_t>(a2);
        }
        if (count > 0 && dst != 0 && count < 0x08000000U) {
            std::vector<uint8_t> tmp(count, val);
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
        if (count > 0 && s1 != 0 && s2 != 0 && count < 0x04000000U) {
            std::vector<uint8_t> b1(count), b2(count);
            uc_mem_read(uc, s1, b1.data(), count);
            uc_mem_read(uc, s2, b2.data(), count);
            res = std::memcmp(b1.data(), b2.data(), count);
        }
        uint32_t ret = static_cast<uint32_t>(res);
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }
    if (name == "memchr") {
        uint32_t sAddr = 0, cVal = 0, n = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &sAddr);
        uc_reg_read(uc, UC_ARM_REG_R1, &cVal);
        uc_reg_read(uc, UC_ARM_REG_R2, &n);
        uint32_t found = 0;
        if (sAddr != 0 && n > 0 && n < 0x01000000U) {
            std::vector<uint8_t> buf(n);
            if (uc_mem_read(uc, sAddr, buf.data(), n) == UC_ERR_OK) {
                const void* p = std::memchr(buf.data(), static_cast<int>(cVal & 0xFFU), n);
                if (p) found = sAddr + static_cast<uint32_t>(static_cast<const uint8_t*>(p) - buf.data());
            }
        }
        uc_reg_write(uc, UC_ARM_REG_R0, &found);
        return;
    }
    if (name == "strlen") {
        uint32_t sAddr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &sAddr);
        std::string s = readGuestCString(uc, sAddr, 65536);
        uint32_t len = static_cast<uint32_t>(s.size());
        uc_reg_write(uc, UC_ARM_REG_R0, &len);
        return;
    }
    if (name == "strcpy" || name == "strcat") {
        uint32_t dst = 0, src = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &dst);
        uc_reg_read(uc, UC_ARM_REG_R1, &src);
        if (dst != 0 && src != 0) {
            std::string sSrc = readGuestCString(uc, src, 16384);
            uint32_t writeOff = (name == "strcat") ? static_cast<uint32_t>(readGuestCString(uc, dst, 16384).size()) : 0U;
            uc_mem_write(uc, dst + writeOff, sSrc.c_str(), sSrc.size() + 1);
        }
        uc_reg_write(uc, UC_ARM_REG_R0, &dst);
        return;
    }
    if (name == "strncpy") {
        uint32_t dst = 0, src = 0, n = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &dst);
        uc_reg_read(uc, UC_ARM_REG_R1, &src);
        uc_reg_read(uc, UC_ARM_REG_R2, &n);
        if (dst != 0 && src != 0 && n > 0 && n < 65536) {
            std::string sSrc = readGuestCString(uc, src, n);
            std::vector<char> out(n, 0);
            std::memcpy(out.data(), sSrc.data(), std::min<size_t>(n, sSrc.size()));
            uc_mem_write(uc, dst, out.data(), n);
        }
        uc_reg_write(uc, UC_ARM_REG_R0, &dst);
        return;
    }
    if (name == "strcmp" || name == "strcasecmp") {
        uint32_t s1 = 0, s2 = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &s1);
        uc_reg_read(uc, UC_ARM_REG_R1, &s2);
        std::string a = readGuestCString(uc, s1, 4096);
        std::string b = readGuestCString(uc, s2, 4096);
        int res = (name == "strcasecmp") ? strcasecmp(a.c_str(), b.c_str()) : std::strcmp(a.c_str(), b.c_str());
        uint32_t ret = static_cast<uint32_t>(res);
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }
    if (name == "strncmp" || name == "strncasecmp") {
        uint32_t s1 = 0, s2 = 0, n = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &s1);
        uc_reg_read(uc, UC_ARM_REG_R1, &s2);
        uc_reg_read(uc, UC_ARM_REG_R2, &n);
        std::string a = readGuestCString(uc, s1, 4096);
        std::string b = readGuestCString(uc, s2, 4096);
        int res = (name == "strncasecmp") ? strncasecmp(a.c_str(), b.c_str(), n) : std::strncmp(a.c_str(), b.c_str(), n);
        uint32_t ret = static_cast<uint32_t>(res);
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }
    if (name == "strchr" || name == "strrchr") {
        uint32_t sAddr = 0, cVal = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &sAddr);
        uc_reg_read(uc, UC_ARM_REG_R1, &cVal);
        std::string s = readGuestCString(uc, sAddr, 8192);
        const char* p = (name == "strrchr") ? std::strrchr(s.c_str(), static_cast<int>(cVal & 0xFFU))
                                            : std::strchr(s.c_str(), static_cast<int>(cVal & 0xFFU));
        uint32_t res = p ? (sAddr + static_cast<uint32_t>(p - s.c_str())) : 0U;
        uc_reg_write(uc, UC_ARM_REG_R0, &res);
        return;
    }
    if (name == "strstr") {
        uint32_t hAddr = 0, nAddr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &hAddr);
        uc_reg_read(uc, UC_ARM_REG_R1, &nAddr);
        std::string h = readGuestCString(uc, hAddr, 8192);
        std::string n = readGuestCString(uc, nAddr, 1024);
        const char* p = std::strstr(h.c_str(), n.c_str());
        uint32_t res = p ? (hAddr + static_cast<uint32_t>(p - h.c_str())) : 0U;
        uc_reg_write(uc, UC_ARM_REG_R0, &res);
        return;
    }
    if (name == "strdup") {
        uint32_t sAddr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &sAddr);
        uint32_t newAddr = (sAddr != 0) ? createGuestString(uc, readGuestCString(uc, sAddr, 8192)) : 0U;
        uc_reg_write(uc, UC_ARM_REG_R0, &newAddr);
        return;
    }
    if (name == "snprintf" || name == "vsnprintf") {
        uint32_t dst = 0, size = 0, fmtAddr = 0, vaAddr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &dst);
        uc_reg_read(uc, UC_ARM_REG_R1, &size);
        uc_reg_read(uc, UC_ARM_REG_R2, &fmtAddr);
        uc_reg_read(uc, UC_ARM_REG_R3, &vaAddr);
        std::string fmt = readGuestCString(uc, fmtAddr, 2048);
        std::string res = (name == "vsnprintf") ? formatGuestPrintf(uc, fmt, 4, vaAddr) : formatGuestPrintf(uc, fmt, 3, 0);
        if (dst != 0 && size > 0) {
            size_t copyLen = std::min<size_t>(res.size(), size - 1);
            if (copyLen > 0) uc_mem_write(uc, dst, res.data(), copyLen);
            char z = 0;
            uc_mem_write(uc, dst + copyLen, &z, 1);
        }
        uint32_t len = static_cast<uint32_t>(res.size());
        uc_reg_write(uc, UC_ARM_REG_R0, &len);
        return;
    }
    if (name == "sprintf" || name == "vsprintf") {
        uint32_t dst = 0, fmtAddr = 0, vaAddr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &dst);
        uc_reg_read(uc, UC_ARM_REG_R1, &fmtAddr);
        uc_reg_read(uc, UC_ARM_REG_R2, &vaAddr);
        std::string fmt = readGuestCString(uc, fmtAddr, 2048);
        std::string res = (name == "vsprintf") ? formatGuestPrintf(uc, fmt, 4, vaAddr) : formatGuestPrintf(uc, fmt, 2, 0);
        if (dst != 0) {
            uc_mem_write(uc, dst, res.c_str(), res.size() + 1);
        }
        uint32_t len = static_cast<uint32_t>(res.size());
        uc_reg_write(uc, UC_ARM_REG_R0, &len);
        return;
    }
    if (name == "atoi" || name == "atol" || name == "strtol" || name == "strtoul") {
        uint32_t sAddr = 0, endPtr = 0, base = 10;
        uc_reg_read(uc, UC_ARM_REG_R0, &sAddr);
        uc_reg_read(uc, UC_ARM_REG_R1, &endPtr);
        uc_reg_read(uc, UC_ARM_REG_R2, &base);
        std::string s = readGuestCString(uc, sAddr, 128);
        char* hostEnd = nullptr;
        uint32_t val = (name == "strtoul")
            ? static_cast<uint32_t>(std::strtoul(s.c_str(), &hostEnd, static_cast<int>(base)))
            : static_cast<uint32_t>(std::strtol(s.c_str(), &hostEnd, (name == "strtol") ? static_cast<int>(base) : 10));
        if ((name == "strtol" || name == "strtoul") && endPtr != 0 && hostEnd != nullptr) {
            uint32_t guestEnd = sAddr + static_cast<uint32_t>(hostEnd - s.c_str());
            uc_mem_write(uc, endPtr, &guestEnd, 4);
        }
        uc_reg_write(uc, UC_ARM_REG_R0, &val);
        return;
    }

    // 5. File I/O Libc
    if (name == "fopen" || name == "fopen64") {
        uint32_t pathAddr = 0, modeAddr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &pathAddr);
        uc_reg_read(uc, UC_ARM_REG_R1, &modeAddr);
        uint32_t fileHandle = 0;
        if (pathAddr != 0 && modeAddr != 0) {
            std::string pathStr = readGuestCString(uc, pathAddr, 512);
            std::string modeStr = readGuestCString(uc, modeAddr, 32);
            std::string resolved = SyscallDispatcher::resolveFilePath(pathStr.c_str());
            FILE* fp = fopen(resolved.c_str(), modeStr.c_str());
            if (fp) {
                std::lock_guard<std::mutex> lock(sFileMutex);
                fileHandle = sNextFileHandle++;
                sFileTable[fileHandle] = fp;
                LOGI("fopen riuscito: '%s' (Handle: 0x%08x)", resolved.c_str(), fileHandle);
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
        if (fp && size > 0 && nmemb > 0 && ptr != 0 && (size * nmemb) < 0x08000000U) {
            std::vector<uint8_t> buf(size * nmemb);
            size_t r = fread(buf.data(), size, nmemb, fp);
            if (r > 0) uc_mem_write(uc, ptr, buf.data(), r * size);
            readCount = static_cast<uint32_t>(r);
        }
        uc_reg_write(uc, UC_ARM_REG_R0, &readCount);
        return;
    }
    if (name == "fwrite") {
        uint32_t ptr = 0, size = 0, nmemb = 0, handle = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &ptr);
        uc_reg_read(uc, UC_ARM_REG_R1, &size);
        uc_reg_read(uc, UC_ARM_REG_R2, &nmemb);
        uc_reg_read(uc, UC_ARM_REG_R3, &handle);
        uint32_t writtenCount = nmemb;
        FILE* fp = nullptr;
        {
            std::lock_guard<std::mutex> lock(sFileMutex);
            auto fit = sFileTable.find(handle);
            if (fit != sFileTable.end()) fp = fit->second;
        }
        if (fp && size > 0 && nmemb > 0 && ptr != 0 && (size * nmemb) < 0x04000000U) {
            std::vector<uint8_t> buf(size * nmemb);
            uc_mem_read(uc, ptr, buf.data(), buf.size());
            writtenCount = static_cast<uint32_t>(fwrite(buf.data(), size, nmemb, fp));
        }
        uc_reg_write(uc, UC_ARM_REG_R0, &writtenCount);
        return;
    }
    if (name == "fseek" || name == "fseeko") {
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
        if (fp) res = fseek(fp, static_cast<long>(static_cast<int32_t>(offset)), static_cast<int>(whence));
        uint32_t ret = static_cast<uint32_t>(res);
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }
    if (name == "fseeko64") {
        // AAPCS ARM32: R0=FILE*, R1=pad, R2=offset_lo, R3=offset_hi, [SP]=whence
        uint32_t handle = 0, offLo = 0, offHi = 0, sp = 0, whence = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &handle);
        uc_reg_read(uc, UC_ARM_REG_R2, &offLo);
        uc_reg_read(uc, UC_ARM_REG_R3, &offHi);
        uc_reg_read(uc, UC_ARM_REG_SP, &sp);
        if (sp != 0) uc_mem_read(uc, sp, &whence, 4);

        int64_t off64 = (static_cast<int64_t>(offHi) << 32) | offLo;
        int res = -1;
        FILE* fp = nullptr;
        {
            std::lock_guard<std::mutex> lock(sFileMutex);
            auto fit = sFileTable.find(handle);
            if (fit != sFileTable.end()) fp = fit->second;
        }
        if (fp) res = fseeko(fp, static_cast<off_t>(off64), static_cast<int>(whence));
        uint32_t ret = static_cast<uint32_t>(res);
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }
    if (name == "ftell" || name == "ftello") {
        uint32_t handle = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &handle);
        long pos = -1;
        FILE* fp = nullptr;
        {
            std::lock_guard<std::mutex> lock(sFileMutex);
            auto fit = sFileTable.find(handle);
            if (fit != sFileTable.end()) fp = fit->second;
        }
        if (fp) pos = ftell(fp);
        uint32_t ret = static_cast<uint32_t>(pos);
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }
    if (name == "ftello64") {
        // AAPCS ARM32: R0=FILE*, return 64-bit in R0 (lo) and R1 (hi)
        uint32_t handle = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &handle);
        off_t pos = -1;
        FILE* fp = nullptr;
        {
            std::lock_guard<std::mutex> lock(sFileMutex);
            auto fit = sFileTable.find(handle);
            if (fit != sFileTable.end()) fp = fit->second;
        }
        if (fp) pos = ftello(fp);
        uint32_t retLo = static_cast<uint32_t>(pos & 0xFFFFFFFFU);
        uint32_t retHi = static_cast<uint32_t>((static_cast<uint64_t>(pos) >> 32) & 0xFFFFFFFFU);
        uc_reg_write(uc, UC_ARM_REG_R0, &retLo);
        uc_reg_write(uc, UC_ARM_REG_R1, &retHi);
        return;
    }
    if (name == "fclose" || name == "fflush") {
        uint32_t handle = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &handle);
        std::lock_guard<std::mutex> lock(sFileMutex);
        auto fit = sFileTable.find(handle);
        if (fit != sFileTable.end() && fit->second) {
            if (name == "fclose") {
                fclose(fit->second);
                sFileTable.erase(fit);
            } else {
                fflush(fit->second);
            }
        }
        uint32_t ret = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }

    // 6. Dynamic Linker (dlopen con caricamento reale librerie guest come libmono.so, dlsym, dladdr)
    if (name == "dlopen") {
        uint32_t pathAddr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &pathAddr);
        if (pathAddr == 0) {
            uint32_t globalH = 0x50000001U;
            uc_reg_write(uc, UC_ARM_REG_R0, &globalH);
            return;
        }
        std::string reqPath = readGuestCString(uc, pathAddr, 256);
        size_t sl = reqPath.find_last_of('/');
        std::string baseName = (sl != std::string::npos) ? reqPath.substr(sl + 1) : reqPath;

        // Cerca se gia' caricata tra le librerie guest
        for (const auto& lib : loadedLibraries_) {
            if (lib && lib->path.find(baseName) != std::string::npos) {
                uint32_t h = nextDlHandle_++;
                dlopenHandleMap_[h] = lib;
                LOGI("dlopen('%s') -> libreria guest gia' caricata (Handle: 0x%x)", reqPath.c_str(), h);
                uc_reg_write(uc, UC_ARM_REG_R0, &h);
                return;
            }
        }

        // Prova caricamento dinamico da nativeLibDir_ (es. libmono.so, libgdvoucher.so)
        if (!nativeLibDir_.empty() && baseName != "libc.so" && baseName != "libm.so" &&
            baseName != "libdl.so" && baseName != "liblog.so" && baseName != "libGLESv2.so" &&
            baseName != "libGLESv1_CM.so" && baseName != "libGLESv3.so" && baseName != "libEGL.so" &&
            baseName != "libandroid.so" && baseName != "libOpenSLES.so" && baseName != "libz.so") {
            std::string fullCandidate = nativeLibDir_ + "/" + baseName;
            if (access(fullCandidate.c_str(), F_OK) == 0) {
                Elf32Loader dynLoader;
                auto dynLib = dynLoader.loadFromFile(fullCandidate);
                if (dynLib && dynLib->mapToUnicorn(uc)) {
                    registerLoadedLibrary(dynLib);
                    dynLib->applyRelocations(uc);
                    uint32_t h = nextDlHandle_++;
                    dlopenHandleMap_[h] = dynLib;
                    LOGI("dlopen('%s') -> caricata dinamicamente da '%s' (Bias=0x%08x, Handle=0x%x)",
                         reqPath.c_str(), fullCandidate.c_str(), dynLib->load_bias, h);
                    uc_reg_write(uc, UC_ARM_REG_R0, &h);
                    return;
                }
            }
        }

        uint32_t sysH = nextDlHandle_++;
        LOGI("dlopen('%s') -> handle di sistema 0x%x", reqPath.c_str(), sysH);
        uc_reg_write(uc, UC_ARM_REG_R0, &sysH);
        return;
    }

    if (name == "dlsym") {
        uint32_t handle = 0, symNameAddr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &handle);
        uc_reg_read(uc, UC_ARM_REG_R1, &symNameAddr);
        uint32_t resolvedAddr = 0;

        if (symNameAddr != 0) {
            std::string symName = readGuestCString(uc, symNameAddr, 128);
            auto hit = dlopenHandleMap_.find(handle);
            if (hit != dlopenHandleMap_.end() && hit->second) {
                for (const auto& s : hit->second->symbols) {
                    if (s.name == symName && s.vaddr != 0) {
                        resolvedAddr = hit->second->load_bias + s.vaddr;
                        break;
                    }
                }
            }
            if (resolvedAddr == 0 && symName.rfind("gl", 0) != 0 && symName.rfind("egl", 0) != 0) {
                resolvedAddr = findExportedSymbol(symName);
            }
            if (resolvedAddr == 0) {
                resolvedAddr = registerSymbolTrampoline(symName, uc);
            }
            LOGI("dlsym('%s') -> 0x%08x", symName.c_str(), resolvedAddr);
        }
        uc_reg_write(uc, UC_ARM_REG_R0, &resolvedAddr);
        return;
    }

    if (name == "dladdr") {
        uint32_t addr = 0, infoPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &addr);
        uc_reg_read(uc, UC_ARM_REG_R1, &infoPtr);
        if (infoPtr != 0) {
            std::string fpath = loadedLibraries_.empty() ? "/system/lib/libc.so" : loadedLibraries_.back()->path;
            uint32_t fbase = loadedLibraries_.empty() ? 0x40000000U : loadedLibraries_.back()->load_bias;
            for (const auto& lib : loadedLibraries_) {
                if (lib && addr >= lib->load_bias && addr < (lib->load_bias + lib->aligned_max_vaddr)) {
                    fpath = lib->path;
                    fbase = lib->load_bias;
                    break;
                }
            }
            uint32_t dlInfo32[4] = {
                createGuestString(uc, fpath),
                fbase,
                0,
                addr
            };
            uc_mem_write(uc, infoPtr, dlInfo32, sizeof(dlInfo32));
        }
        uint32_t one = 1;
        uc_reg_write(uc, UC_ARM_REG_R0, &one);
        return;
    }

    if (name == "dlclose" || name == "dlerror") {
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }

    // 7. Sottosistema Multi-Threading Guest: Pthread, TLS & Primitive di Sincronizzazione
    if (name == "pthread_key_create") {
        uint32_t keyAddr = 0, dtor = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &keyAddr);
        uc_reg_read(uc, UC_ARM_REG_R1, &dtor);
        uint32_t newKey = GuestThreadScheduler::getInstance().createTlsKey(dtor);
        if (keyAddr != 0) {
            uc_mem_write(uc, keyAddr, &newKey, 4);
        }
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }
    if (name == "pthread_setspecific") {
        uint32_t key = 0, val = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &key);
        uc_reg_read(uc, UC_ARM_REG_R1, &val);
        int32_t res = GuestThreadScheduler::getInstance().setTlsSpecific(key, val);
        uint32_t ret = (res == 0) ? 0U : static_cast<uint32_t>(-res);
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }
    if (name == "pthread_getspecific") {
        uint32_t key = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &key);
        uint32_t val = GuestThreadScheduler::getInstance().getTlsSpecific(key);
        uc_reg_write(uc, UC_ARM_REG_R0, &val);
        return;
    }
    if (name == "pthread_key_delete") {
        uint32_t key = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &key);
        int32_t res = GuestThreadScheduler::getInstance().deleteTlsKey(key);
        uint32_t ret = (res == 0) ? 0U : static_cast<uint32_t>(-res);
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }
    if (name == "pthread_self") {
        uint32_t tid = GuestThreadScheduler::getInstance().getCurrentTid();
        uc_reg_write(uc, UC_ARM_REG_R0, &tid);
        return;
    }
    if (name == "pthread_equal") {
        uint32_t t1 = 0, t2 = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &t1);
        uc_reg_read(uc, UC_ARM_REG_R1, &t2);
        uint32_t eq = (t1 == t2) ? 1U : 0U;
        uc_reg_write(uc, UC_ARM_REG_R0, &eq);
        return;
    }
    if (name == "sched_yield" || name == "pthread_yield") {
        sched_yield();
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }
    if (name == "pthread_join") {
        uint32_t targetTid = 0, retvalPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &targetTid);
        uc_reg_read(uc, UC_ARM_REG_R1, &retvalPtr);
        int32_t res = GuestThreadScheduler::getInstance().joinThread(targetTid, retvalPtr, uc);
        uint32_t ret = (res == 0) ? 0U : static_cast<uint32_t>(-res);
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }
    if (name == "pthread_detach") {
        uint32_t targetTid = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &targetTid);
        int32_t res = GuestThreadScheduler::getInstance().detachThread(targetTid);
        uint32_t ret = (res == 0) ? 0U : static_cast<uint32_t>(-res);
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }
    if (name == "pthread_exit") {
        uint32_t retval = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &retval);
        GuestThreadScheduler::getInstance().exitThread(retval, uc);
        return;
    }
    if (name == "pthread_mutex_init") {
        uint32_t mAddr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &mAddr);
        getOrCreateGuestMutex(mAddr);
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }
    if (name == "pthread_mutex_destroy") {
        uint32_t mAddr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &mAddr);
        if (mAddr != 0) {
            std::lock_guard<std::mutex> lock(sSyncTableMutex);
            sHostPthreadMutexes.erase(mAddr);
        }
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }
    if (name == "pthread_mutex_lock") {
        uint32_t mAddr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &mAddr);
        auto m = getOrCreateGuestMutex(mAddr);
        if (m) {
            m->lock();
        }
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }
    if (name == "pthread_mutex_unlock") {
        uint32_t mAddr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &mAddr);
        auto m = getOrCreateGuestMutex(mAddr);
        if (m) {
            m->unlock();
        }
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }
    if (name == "pthread_mutex_trylock") {
        uint32_t mAddr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &mAddr);
        auto m = getOrCreateGuestMutex(mAddr);
        uint32_t ret = (m && m->try_lock()) ? 0U : 16U; // 16 = EBUSY
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }
    if (name == "pthread_cond_init") {
        uint32_t cAddr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &cAddr);
        getOrCreateGuestCond(cAddr);
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }
    if (name == "pthread_cond_destroy") {
        uint32_t cAddr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &cAddr);
        if (cAddr != 0) {
            std::lock_guard<std::mutex> lock(sSyncTableMutex);
            sHostPthreadConds.erase(cAddr);
        }
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }
    if (name == "pthread_cond_signal") {
        uint32_t cAddr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &cAddr);
        auto c = getOrCreateGuestCond(cAddr);
        if (c) {
            c->notify_one();
        }
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }
    if (name == "pthread_cond_broadcast") {
        uint32_t cAddr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &cAddr);
        auto c = getOrCreateGuestCond(cAddr);
        if (c) {
            c->notify_all();
        }
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }
    if (name == "pthread_cond_wait") {
        uint32_t cAddr = 0, mAddr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &cAddr);
        uc_reg_read(uc, UC_ARM_REG_R1, &mAddr);
        auto c = getOrCreateGuestCond(cAddr);
        auto m = getOrCreateGuestMutex(mAddr);
        if (c && m) {
            c->wait(*m);
        }
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }
    if (name == "pthread_cond_timedwait") {
        uint32_t cAddr = 0, mAddr = 0, tsAddr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &cAddr);
        uc_reg_read(uc, UC_ARM_REG_R1, &mAddr);
        uc_reg_read(uc, UC_ARM_REG_R2, &tsAddr);
        auto c = getOrCreateGuestCond(cAddr);
        auto m = getOrCreateGuestMutex(mAddr);
        uint32_t ret = 0;
        if (c && m && tsAddr != 0) {
            struct { int32_t tv_sec; int32_t tv_nsec; } ts32{};
            uc_mem_read(uc, tsAddr, &ts32, sizeof(ts32));
            struct timespec now{};
            clock_gettime(CLOCK_REALTIME, &now);
            int64_t diffNs = (static_cast<int64_t>(ts32.tv_sec) - now.tv_sec) * 1000000000LL + (ts32.tv_nsec - now.tv_nsec);
            if (diffNs <= 0) {
                ret = 110U; // ETIMEDOUT
            } else {
                auto status = c->wait_for(*m, std::chrono::nanoseconds(diffNs));
                if (status == std::cv_status::timeout) {
                    ret = 110U; // ETIMEDOUT
                }
            }
        } else if (c && m) {
            c->wait(*m);
        }
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }
    if (name == "pthread_once") {
        uint32_t onceCtrl = 0, initRoutine = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &onceCtrl);
        uc_reg_read(uc, UC_ARM_REG_R1, &initRoutine);
        uint32_t val = 0;
        if (onceCtrl != 0) {
            uc_mem_read(uc, onceCtrl, &val, 4);
        }
        if (val == 0) {
            if (onceCtrl != 0) {
                uint32_t done = 1;
                uc_mem_write(uc, onceCtrl, &done, 4);
            }
            if (initRoutine != 0) {
                uint32_t savedSp = 0, savedLr = 0, savedCpsr = 0;
                uint32_t savedR0 = 0, savedR1 = 0, savedR2 = 0, savedR3 = 0;
                uc_reg_read(uc, UC_ARM_REG_SP, &savedSp);
                uc_reg_read(uc, UC_ARM_REG_LR, &savedLr);
                uc_reg_read(uc, UC_ARM_REG_CPSR, &savedCpsr);
                uc_reg_read(uc, UC_ARM_REG_R0, &savedR0);
                uc_reg_read(uc, UC_ARM_REG_R1, &savedR1);
                uc_reg_read(uc, UC_ARM_REG_R2, &savedR2);
                uc_reg_read(uc, UC_ARM_REG_R3, &savedR3);

                uint32_t lr = 0xDEADBEEFU;
                uint32_t cpsr = (initRoutine & 1U) ? (savedCpsr | 0x20U) : (savedCpsr & ~0x20U);
                uc_reg_write(uc, UC_ARM_REG_LR, &lr);
                uc_reg_write(uc, UC_ARM_REG_CPSR, &cpsr);

                LOGI("pthread_once: Esecuzione init_routine @ 0x%08x...", initRoutine);
                uc_emu_start(uc, initRoutine, lr, 0, 500000);

                uc_reg_write(uc, UC_ARM_REG_SP, &savedSp);
                uc_reg_write(uc, UC_ARM_REG_LR, &savedLr);
                uc_reg_write(uc, UC_ARM_REG_CPSR, &savedCpsr);
                uc_reg_write(uc, UC_ARM_REG_R1, &savedR1);
                uc_reg_write(uc, UC_ARM_REG_R2, &savedR2);
                uc_reg_write(uc, UC_ARM_REG_R3, &savedR3);
            }
        }
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }
    if (name == "pthread_getattr_np") {
        uint32_t attrPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R1, &attrPtr);
        if (attrPtr != 0) {
            uint8_t zeroAttr[64] = {0};
            uc_mem_write(uc, attrPtr, zeroAttr, sizeof(zeroAttr));
        }
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }
    if (name == "pthread_attr_getstack") {
        uint32_t stackAddrPtr = 0, stackSizePtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R1, &stackAddrPtr);
        uc_reg_read(uc, UC_ARM_REG_R2, &stackSizePtr);
        if (stackAddrPtr != 0) {
            uint32_t base = 0x7E000000U; // guestStackBase_
            uc_mem_write(uc, stackAddrPtr, &base, 4);
        }
        if (stackSizePtr != 0) {
            uint32_t size = 0x00200000U; // 2MB guestStackSize_
            uc_mem_write(uc, stackSizePtr, &size, 4);
        }
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }
    if (name == "pthread_attr_getstacksize") {
        uint32_t stackSizePtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R1, &stackSizePtr);
        if (stackSizePtr != 0) {
            uint32_t size = 0x00200000U; // 2MB
            uc_mem_write(uc, stackSizePtr, &size, 4);
        }
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }
    if (name == "pthread_attr_getguardsize") {
        uint32_t guardPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R1, &guardPtr);
        if (guardPtr != 0) {
            uint32_t guard = 0x1000U; // 4KB
            uc_mem_write(uc, guardPtr, &guard, 4);
        }
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }
    if (name.rfind("pthread_mutexattr", 0) == 0 || name.rfind("pthread_condattr", 0) == 0 ||
        name.rfind("pthread_attr", 0) == 0 || name.rfind("pthread_rwlock", 0) == 0 ||
        name == "pthread_setname_np") {
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }

    // 8. Android NDK NativeWindow & AssetManager
    if (name == "ANativeWindow_fromSurface") {
        uint32_t surfObj = 0;
        uc_reg_read(uc, UC_ARM_REG_R1, &surfObj);
        uint32_t winHandle = (surfObj != 0) ? (guestJniBase_ + 0x2800U) : 0U;
        uc_reg_write(uc, UC_ARM_REG_R0, &winHandle);
        return;
    }
    if (name == "ANativeWindow_getWidth") {
        uint32_t w = 1920;
        uc_reg_write(uc, UC_ARM_REG_R0, &w);
        return;
    }
    if (name == "ANativeWindow_getHeight") {
        uint32_t h = 1080;
        uc_reg_write(uc, UC_ARM_REG_R0, &h);
        return;
    }
    if (name == "ANativeWindow_getFormat") {
        uint32_t fmt = 1; // WINDOW_FORMAT_RGBA_8888
        uc_reg_write(uc, UC_ARM_REG_R0, &fmt);
        return;
    }
    if (name == "ANativeWindow_setBuffersGeometry" || name == "ANativeWindow_acquire" ||
        name == "ANativeWindow_release" || name == "ANativeWindow_lock" || name == "ANativeWindow_unlockAndPost") {
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }

    // =========================================================================
    // 8a. Sostituzione & Implementazione Completa Funzioni POSIX / Bionic Libc
    // =========================================================================

    // File Descriptor & IO: open, open64
    if (name == "open" || name == "open64") {
        uint32_t pathPtr = 0, flags = 0, mode = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &pathPtr);
        uc_reg_read(uc, UC_ARM_REG_R1, &flags);
        uc_reg_read(uc, UC_ARM_REG_R2, &mode);
        std::string gpath = readGuestCString(uc, pathPtr, 1024);
        if (gpath.empty()) {
            uint32_t negOne = 0xFFFFFFFFU;
            uc_reg_write(uc, UC_ARM_REG_R0, &negOne);
            return;
        }
        std::string resolved = SyscallDispatcher::resolveFilePath(gpath.c_str());
        ensureParentDirs(resolved);
        int fd = ::open(resolved.c_str(), static_cast<int>(flags), static_cast<mode_t>(mode));
        if (fd >= 0) {
            uint32_t ufd = static_cast<uint32_t>(fd);
            uc_reg_write(uc, UC_ARM_REG_R0, &ufd);
        } else {
            uint32_t negOne = 0xFFFFFFFFU;
            uc_reg_write(uc, UC_ARM_REG_R0, &negOne);
            uint32_t err = static_cast<uint32_t>(errno);
            uc_mem_write(uc, guestJniBase_ + 0x2000U, &err, 4);
            uc_mem_write(uc, guestTlsBase_ + 0x100U, &err, 4);
        }
        return;
    }

    // close
    if (name == "close") {
        uint32_t fd = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &fd);
        int res = 0;
        if (fd > 2) {
            res = ::close(static_cast<int>(fd));
        }
        uint32_t ret = (res == 0) ? 0 : 0xFFFFFFFFU;
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }

    // read
    if (name == "read") {
        uint32_t fd = 0, bufPtr = 0, count = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &fd);
        uc_reg_read(uc, UC_ARM_REG_R1, &bufPtr);
        uc_reg_read(uc, UC_ARM_REG_R2, &count);
        if (bufPtr == 0 || count == 0) {
            uint32_t zero = 0;
            uc_reg_write(uc, UC_ARM_REG_R0, &zero);
            return;
        }
        size_t toRead = std::min<size_t>(count, 0x1000000U);
        std::vector<uint8_t> tmp(toRead);
        ssize_t nRead = ::read(static_cast<int>(fd), tmp.data(), toRead);
        if (nRead > 0) {
            uc_mem_write(uc, bufPtr, tmp.data(), static_cast<size_t>(nRead));
        }
        uint32_t ret = (nRead >= 0) ? static_cast<uint32_t>(nRead) : 0xFFFFFFFFU;
        if (nRead < 0) {
            uint32_t err = static_cast<uint32_t>(errno);
            uc_mem_write(uc, guestJniBase_ + 0x2000U, &err, 4);
            uc_mem_write(uc, guestTlsBase_ + 0x100U, &err, 4);
        }
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }

    // write
    if (name == "write") {
        uint32_t fd = 0, bufPtr = 0, count = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &fd);
        uc_reg_read(uc, UC_ARM_REG_R1, &bufPtr);
        uc_reg_read(uc, UC_ARM_REG_R2, &count);
        if (bufPtr == 0 || count == 0) {
            uint32_t zero = 0;
            uc_reg_write(uc, UC_ARM_REG_R0, &zero);
            return;
        }
        size_t toWrite = std::min<size_t>(count, 0x1000000U);
        std::vector<uint8_t> tmp(toWrite);
        uc_mem_read(uc, bufPtr, tmp.data(), toWrite);
        if (fd == 1 || fd == 2) {
            std::string logMsg(reinterpret_cast<char*>(tmp.data()), toWrite);
            LOGI("GuestStdout/Stderr: %s", logMsg.c_str());
            uint32_t ret = static_cast<uint32_t>(toWrite);
            uc_reg_write(uc, UC_ARM_REG_R0, &ret);
            return;
        }
        ssize_t nWritten = ::write(static_cast<int>(fd), tmp.data(), toWrite);
        uint32_t ret = (nWritten >= 0) ? static_cast<uint32_t>(nWritten) : 0xFFFFFFFFU;
        if (nWritten < 0) {
            uint32_t err = static_cast<uint32_t>(errno);
            uc_mem_write(uc, guestJniBase_ + 0x2000U, &err, 4);
            uc_mem_write(uc, guestTlsBase_ + 0x100U, &err, 4);
        }
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }

    // lseek
    if (name == "lseek") {
        uint32_t fd = 0, off = 0, whence = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &fd);
        uc_reg_read(uc, UC_ARM_REG_R1, &off);
        uc_reg_read(uc, UC_ARM_REG_R2, &whence);
        off_t res = ::lseek(static_cast<int>(fd), static_cast<off_t>(static_cast<int32_t>(off)), static_cast<int>(whence));
        uint32_t ret = (res != (off_t)-1) ? static_cast<uint32_t>(res) : 0xFFFFFFFFU;
        if (res == (off_t)-1) {
            uint32_t err = static_cast<uint32_t>(errno);
            uc_mem_write(uc, guestJniBase_ + 0x2000U, &err, 4);
            uc_mem_write(uc, guestTlsBase_ + 0x100U, &err, 4);
        }
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }

    // lseek64 (AAPCS: R0=fd, R1=pad, R2:R3=off64, [SP]=whence -> R0:R1=ret64)
    if (name == "lseek64") {
        uint32_t fd = 0, offLo = 0, offHi = 0, sp = 0, whence = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &fd);
        uc_reg_read(uc, UC_ARM_REG_R2, &offLo);
        uc_reg_read(uc, UC_ARM_REG_R3, &offHi);
        uc_reg_read(uc, UC_ARM_REG_SP, &sp);
        if (sp != 0) uc_mem_read(uc, sp, &whence, 4);

        int64_t off64 = (static_cast<int64_t>(offHi) << 32) | offLo;
        off_t res = ::lseek(static_cast<int>(fd), static_cast<off_t>(off64), static_cast<int>(whence));
        if (res != (off_t)-1) {
            uint32_t retLo = static_cast<uint32_t>(res & 0xFFFFFFFFU);
            uint32_t retHi = static_cast<uint32_t>((static_cast<uint64_t>(res) >> 32) & 0xFFFFFFFFU);
            uc_reg_write(uc, UC_ARM_REG_R0, &retLo);
            uc_reg_write(uc, UC_ARM_REG_R1, &retHi);
        } else {
            uint32_t negOne = 0xFFFFFFFFU;
            uc_reg_write(uc, UC_ARM_REG_R0, &negOne);
            uc_reg_write(uc, UC_ARM_REG_R1, &negOne);
            uint32_t err = static_cast<uint32_t>(errno);
            uc_mem_write(uc, guestJniBase_ + 0x2000U, &err, 4);
            uc_mem_write(uc, guestTlsBase_ + 0x100U, &err, 4);
        }
        return;
    }

    // stat, stat64, lstat, lstat64
    if (name == "stat" || name == "stat64" || name == "lstat" || name == "lstat64") {
        uint32_t pathPtr = 0, statPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &pathPtr);
        uc_reg_read(uc, UC_ARM_REG_R1, &statPtr);
        std::string gpath = readGuestCString(uc, pathPtr, 1024);
        std::string resolved = SyscallDispatcher::resolveFilePath(gpath.c_str());
        struct stat hostSt{};
        int res = ::stat(resolved.c_str(), &hostSt);
        if (res == 0 && statPtr != 0) {
            kernel_stat64_arm32 st32{};
            fillStat64(hostSt, st32);
            uc_mem_write(uc, statPtr, &st32, sizeof(st32));
            uint32_t zero = 0;
            uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        } else {
            uint32_t negOne = 0xFFFFFFFFU;
            uc_reg_write(uc, UC_ARM_REG_R0, &negOne);
            uint32_t err = static_cast<uint32_t>(errno);
            uc_mem_write(uc, guestJniBase_ + 0x2000U, &err, 4);
            uc_mem_write(uc, guestTlsBase_ + 0x100U, &err, 4);
        }
        return;
    }

    // fstat, fstat64
    if (name == "fstat" || name == "fstat64") {
        uint32_t fd = 0, statPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &fd);
        uc_reg_read(uc, UC_ARM_REG_R1, &statPtr);
        struct stat hostSt{};
        int res = ::fstat(static_cast<int>(fd), &hostSt);
        if (res == 0 && statPtr != 0) {
            kernel_stat64_arm32 st32{};
            fillStat64(hostSt, st32);
            uc_mem_write(uc, statPtr, &st32, sizeof(st32));
            uint32_t zero = 0;
            uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        } else {
            uint32_t negOne = 0xFFFFFFFFU;
            uc_reg_write(uc, UC_ARM_REG_R0, &negOne);
            uint32_t err = static_cast<uint32_t>(errno);
            uc_mem_write(uc, guestJniBase_ + 0x2000U, &err, 4);
            uc_mem_write(uc, guestTlsBase_ + 0x100U, &err, 4);
        }
        return;
    }

    // access
    if (name == "access") {
        uint32_t pathPtr = 0, mode = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &pathPtr);
        uc_reg_read(uc, UC_ARM_REG_R1, &mode);
        std::string gpath = readGuestCString(uc, pathPtr, 1024);
        std::string resolved = SyscallDispatcher::resolveFilePath(gpath.c_str());
        int res = ::access(resolved.c_str(), static_cast<int>(mode));
        uint32_t ret = (res == 0) ? 0 : 0xFFFFFFFFU;
        if (res != 0) {
            uint32_t err = static_cast<uint32_t>(errno);
            uc_mem_write(uc, guestJniBase_ + 0x2000U, &err, 4);
            uc_mem_write(uc, guestTlsBase_ + 0x100U, &err, 4);
        }
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }

    // opendir, readdir, closedir
    if (name == "opendir") {
        uint32_t pathPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &pathPtr);
        std::string gpath = readGuestCString(uc, pathPtr, 1024);
        std::string resolved = SyscallDispatcher::resolveFilePath(gpath.c_str());
        DIR* d = ::opendir(resolved.c_str());
        if (d) {
            std::lock_guard<std::mutex> lk(sDirMutex);
            uint32_t h = sNextDirHandle++;
            uint32_t direntAddr = allocGuestHeap(uc, sizeof(guest_dirent32), 8);
            sGuestDirMap[h] = { d, direntAddr };
            uc_reg_write(uc, UC_ARM_REG_R0, &h);
        } else {
            uint32_t zero = 0;
            uc_reg_write(uc, UC_ARM_REG_R0, &zero);
            uint32_t err = static_cast<uint32_t>(errno);
            uc_mem_write(uc, guestJniBase_ + 0x2000U, &err, 4);
            uc_mem_write(uc, guestTlsBase_ + 0x100U, &err, 4);
        }
        return;
    }
    if (name == "readdir") {
        uint32_t dirH = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &dirH);
        std::lock_guard<std::mutex> lk(sDirMutex);
        auto it = sGuestDirMap.find(dirH);
        if (it != sGuestDirMap.end() && it->second.hostDir) {
            struct dirent* de = ::readdir(it->second.hostDir);
            if (de) {
                guest_dirent32 gd32{};
                gd32.d_ino = de->d_ino;
                gd32.d_off = de->d_off;
                gd32.d_reclen = sizeof(guest_dirent32);
                gd32.d_type = de->d_type;
                std::strncpy(gd32.d_name, de->d_name, sizeof(gd32.d_name) - 1);
                uc_mem_write(uc, it->second.direntBufAddr, &gd32, sizeof(gd32));
                uc_reg_write(uc, UC_ARM_REG_R0, &it->second.direntBufAddr);
                return;
            }
        }
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }
    if (name == "closedir") {
        uint32_t dirH = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &dirH);
        std::lock_guard<std::mutex> lk(sDirMutex);
        auto it = sGuestDirMap.find(dirH);
        if (it != sGuestDirMap.end()) {
            if (it->second.hostDir) ::closedir(it->second.hostDir);
            if (it->second.direntBufAddr) freeGuestHeap(it->second.direntBufAddr, uc);
            sGuestDirMap.erase(it);
        }
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }

    // clock_gettime, gettimeofday, time
    if (name == "clock_gettime") {
        uint32_t clkId = 0, tpPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &clkId);
        uc_reg_read(uc, UC_ARM_REG_R1, &tpPtr);
        struct timespec ts{};
        int res = ::clock_gettime(static_cast<clockid_t>(clkId), &ts);
        if (res == 0 && tpPtr != 0) {
            uint32_t buf[2] = { static_cast<uint32_t>(ts.tv_sec), static_cast<uint32_t>(ts.tv_nsec) };
            uc_mem_write(uc, tpPtr, buf, sizeof(buf));
        }
        uint32_t ret = static_cast<uint32_t>(res);
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }
    if (name == "gettimeofday") {
        uint32_t tvPtr = 0, tzPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &tvPtr);
        uc_reg_read(uc, UC_ARM_REG_R1, &tzPtr);
        struct timeval tv{};
        int res = ::gettimeofday(&tv, nullptr);
        if (res == 0 && tvPtr != 0) {
            uint32_t buf[2] = { static_cast<uint32_t>(tv.tv_sec), static_cast<uint32_t>(tv.tv_usec) };
            uc_mem_write(uc, tvPtr, buf, sizeof(buf));
        }
        uint32_t ret = static_cast<uint32_t>(res);
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }
    if (name == "time") {
        uint32_t tlocPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &tlocPtr);
        time_t cur = ::time(nullptr);
        uint32_t ucur = static_cast<uint32_t>(cur);
        if (tlocPtr != 0) {
            uc_mem_write(uc, tlocPtr, &ucur, 4);
        }
        uc_reg_write(uc, UC_ARM_REG_R0, &ucur);
        return;
    }

    // nanosleep, usleep
    if (name == "nanosleep") {
        uint32_t reqPtr = 0, remPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &reqPtr);
        uc_reg_read(uc, UC_ARM_REG_R1, &remPtr);
        if (reqPtr != 0) {
            uint32_t buf[2]{0};
            uc_mem_read(uc, reqPtr, buf, sizeof(buf));
            struct timespec req{ static_cast<time_t>(buf[0]), static_cast<long>(buf[1]) };
            ::nanosleep(&req, nullptr);
        }
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }
    if (name == "usleep") {
        uint32_t usec = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &usec);
        ::usleep(static_cast<useconds_t>(usec));
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }

    // mmap, munmap, mprotect
    if (name == "mmap") {
        uint32_t addr = 0, length = 0, prot = 0, flags = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &addr);
        uc_reg_read(uc, UC_ARM_REG_R1, &length);
        uc_reg_read(uc, UC_ARM_REG_R2, &prot);
        uc_reg_read(uc, UC_ARM_REG_R3, &flags);
        if (length == 0) length = 4096;
        uint32_t alignedSize = (length + 0xFFFU) & ~0xFFFU;
        uint32_t allocBase = (addr != 0) ? (addr & ~0xFFFU) : allocGuestHeap(uc, alignedSize, 4096);
        auto& gm = GuestMemory::getSharedInstance();
        if (!gm.isMappedRange(allocBase, alignedSize)) {
            gm.map(allocBase, alignedSize, GP_READ | GP_WRITE);
        }
        if (uc) {
            uc_mem_map(uc, allocBase, alignedSize, UC_PROT_ALL);
        }
        uc_reg_write(uc, UC_ARM_REG_R0, &allocBase);
        return;
    }
    if (name == "munmap") {
        uint32_t addr = 0, length = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &addr);
        uc_reg_read(uc, UC_ARM_REG_R1, &length);
        if (addr != 0 && length != 0) {
            uint32_t alignedBase = addr & ~0xFFFU;
            uint32_t alignedSize = (length + 0xFFFU) & ~0xFFFU;
            GuestMemory::getSharedInstance().unmap(alignedBase, alignedSize);
            if (uc) uc_mem_unmap(uc, alignedBase, alignedSize);
        }
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }
    if (name == "mprotect") {
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }

    // ioctl
    if (name == "ioctl") {
        uint32_t fd = 0, req = 0, arg = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &fd);
        uc_reg_read(uc, UC_ARM_REG_R1, &req);
        uc_reg_read(uc, UC_ARM_REG_R2, &arg);
        int res = ::ioctl(static_cast<int>(fd), static_cast<int>(req), nullptr);
        uint32_t ret = (res >= 0) ? static_cast<uint32_t>(res) : 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }

    // printf, sscanf, strncat, setenv, system
    if (name == "printf") {
        uint32_t fmtAddr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &fmtAddr);
        std::string fmt = readGuestCString(uc, fmtAddr, 2048);
        std::string formatted = formatGuestPrintf(uc, fmt, 1, 0);
        LOGI("GUEST_PRINTF: %s", formatted.c_str());
        uint32_t len = static_cast<uint32_t>(formatted.size());
        uc_reg_write(uc, UC_ARM_REG_R0, &len);
        return;
    }
    if (name == "sscanf") {
        uint32_t srcAddr = 0, fmtAddr = 0, out1 = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &srcAddr);
        uc_reg_read(uc, UC_ARM_REG_R1, &fmtAddr);
        uc_reg_read(uc, UC_ARM_REG_R2, &out1);
        std::string src = readGuestCString(uc, srcAddr, 1024);
        std::string fmt = readGuestCString(uc, fmtAddr, 256);
        int parsedVal = 0;
        int count = sscanf(src.c_str(), fmt.c_str(), &parsedVal);
        if (count == 1 && out1 != 0) {
            uint32_t uVal = static_cast<uint32_t>(parsedVal);
            uc_mem_write(uc, out1, &uVal, 4);
        }
        uint32_t ret = (count >= 0) ? static_cast<uint32_t>(count) : 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }
    if (name == "strncat") {
        uint32_t dst = 0, src = 0, n = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &dst);
        uc_reg_read(uc, UC_ARM_REG_R1, &src);
        uc_reg_read(uc, UC_ARM_REG_R2, &n);
        std::string sDst = readGuestCString(uc, dst, 8192);
        std::string sSrc = readGuestCString(uc, src, std::min<size_t>(n, 8192));
        std::string combined = sDst + sSrc.substr(0, n);
        uc_mem_write(uc, dst, combined.c_str(), combined.size() + 1);
        uc_reg_write(uc, UC_ARM_REG_R0, &dst);
        return;
    }
    if (name == "setenv") {
        uint32_t namePtr = 0, valPtr = 0, overwrite = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &namePtr);
        uc_reg_read(uc, UC_ARM_REG_R1, &valPtr);
        uc_reg_read(uc, UC_ARM_REG_R2, &overwrite);
        std::string envName = readGuestCString(uc, namePtr, 256);
        std::string envVal = readGuestCString(uc, valPtr, 1024);
        if (!envName.empty()) {
            ::setenv(envName.c_str(), envVal.c_str(), static_cast<int>(overwrite));
        }
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }
    if (name == "system") {
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }

    // 8b. Signal Handling Virtualizzato (GuestSignalManager)
    if (name == "sigaction" || name == "__sigaction") {
        uint32_t sig = 0, actPtr = 0, oldActPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &sig);
        uc_reg_read(uc, UC_ARM_REG_R1, &actPtr);
        uc_reg_read(uc, UC_ARM_REG_R2, &oldActPtr);
        int32_t ret = GuestSignalManager::getInstance().handleSigAction(sig, actPtr, oldActPtr, sizeof(GuestSigSet), uc);
        uint32_t retVal = static_cast<uint32_t>(ret);
        uc_reg_write(uc, UC_ARM_REG_R0, &retVal);
        return;
    }
    if (name == "signal" || name == "bsd_signal") {
        uint32_t sig = 0, handler = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &sig);
        uc_reg_read(uc, UC_ARM_REG_R1, &handler);
        uint32_t oldHandler = GuestSignalManager::getInstance().handleSignal(sig, handler, uc);
        uc_reg_write(uc, UC_ARM_REG_R0, &oldHandler);
        return;
    }
    if (name == "sigaltstack") {
        uint32_t ssPtr = 0, ossPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &ssPtr);
        uc_reg_read(uc, UC_ARM_REG_R1, &ossPtr);
        int32_t ret = GuestSignalManager::getInstance().handleSigAltStack(ssPtr, ossPtr, uc);
        uint32_t retVal = static_cast<uint32_t>(ret);
        uc_reg_write(uc, UC_ARM_REG_R0, &retVal);
        return;
    }
    if (name == "sigprocmask" || name == "pthread_sigmask") {
        uint32_t how = 0, setPtr = 0, oldSetPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &how);
        uc_reg_read(uc, UC_ARM_REG_R1, &setPtr);
        uc_reg_read(uc, UC_ARM_REG_R2, &oldSetPtr);
        int32_t ret = GuestSignalManager::getInstance().handleSigProcMask(static_cast<int>(how), setPtr, oldSetPtr, sizeof(GuestSigSet), uc);
        uint32_t retVal = static_cast<uint32_t>(ret);
        uc_reg_write(uc, UC_ARM_REG_R0, &retVal);
        return;
    }
    if (name == "raise") {
        uint32_t sig = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &sig);
        int32_t ret = GuestSignalManager::getInstance().deliverSignal(uc, static_cast<int>(sig)) ? 0 : -EINVAL;
        uint32_t retVal = static_cast<uint32_t>(ret);
        uc_reg_write(uc, UC_ARM_REG_R0, &retVal);
        return;
    }
    if (name == "kill" || name == "tkill" || name == "tgkill") {
        uint32_t r0 = 0, r1 = 0, r2 = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &r0);
        uc_reg_read(uc, UC_ARM_REG_R1, &r1);
        uc_reg_read(uc, UC_ARM_REG_R2, &r2);
        int sig = (name == "tgkill") ? static_cast<int>(r2) : static_cast<int>(r1);
        int32_t ret = GuestSignalManager::getInstance().deliverSignal(uc, sig) ? 0 : -EINVAL;
        uint32_t retVal = static_cast<uint32_t>(ret);
        uc_reg_write(uc, UC_ARM_REG_R0, &retVal);
        return;
    }
    if (name == "abort") {
        LOGE("Guest ha invocato abort() - neutralizzato per preservare il main loop");
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }
    if (name == "__assert" || name == "__assert2") {
        uint32_t fileAddr = 0, line = 0, msgAddr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &fileAddr);
        uc_reg_read(uc, UC_ARM_REG_R1, &line);
        uc_reg_read(uc, UC_ARM_REG_R2, &msgAddr);
        std::string file = readGuestCString(uc, fileAddr, 128);
        std::string msg = readGuestCString(uc, msgAddr, 256);
        LOGE("Guest ASSERTION FAILED: %s:%u: %s", file.c_str(), line, msg.c_str());
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }

    // 9. Zlib Native Asset Decompression (crc32, adler32, uncompress, inflateInit_, inflateInit2_, inflate, inflateEnd)
    if (name == "crc32" || name == "adler32") {
        uint32_t crc = 0, bufPtr = 0, len = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &crc);
        uc_reg_read(uc, UC_ARM_REG_R1, &bufPtr);
        uc_reg_read(uc, UC_ARM_REG_R2, &len);
        uLong res = (name == "crc32") ? ::crc32(crc, Z_NULL, 0) : ::adler32(crc, Z_NULL, 0);
        if (bufPtr != 0 && len > 0 && len < 0x04000000U) {
            std::vector<Bytef> tmp(len);
            uc_mem_read(uc, bufPtr, tmp.data(), len);
            res = (name == "crc32") ? ::crc32(crc, tmp.data(), len) : ::adler32(crc, tmp.data(), len);
        }
        uint32_t ret = static_cast<uint32_t>(res);
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }
    if (name == "uncompress") {
        uint32_t dstPtr = 0, dstLenPtr = 0, srcPtr = 0, srcLen = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &dstPtr);
        uc_reg_read(uc, UC_ARM_REG_R1, &dstLenPtr);
        uc_reg_read(uc, UC_ARM_REG_R2, &srcPtr);
        uc_reg_read(uc, UC_ARM_REG_R3, &srcLen);
        int zret = Z_DATA_ERROR;
        if (dstPtr != 0 && dstLenPtr != 0 && srcPtr != 0 && srcLen > 0) {
            uint32_t dstLen32 = 0;
            uc_mem_read(uc, dstLenPtr, &dstLen32, 4);
            if (dstLen32 > 0 && dstLen32 < 0x08000000U && srcLen < 0x04000000U) {
                std::vector<Bytef> srcBuf(srcLen), dstBuf(dstLen32);
                uc_mem_read(uc, srcPtr, srcBuf.data(), srcLen);
                uLongf hostDstLen = dstLen32;
                zret = ::uncompress(dstBuf.data(), &hostDstLen, srcBuf.data(), srcLen);
                if (zret == Z_OK) {
                    uc_mem_write(uc, dstPtr, dstBuf.data(), hostDstLen);
                    uint32_t outLen32 = static_cast<uint32_t>(hostDstLen);
                    uc_mem_write(uc, dstLenPtr, &outLen32, 4);
                }
            }
        }
        uint32_t ret = static_cast<uint32_t>(zret);
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }
    if (name == "inflateInit_" || name == "inflateInit2_") {
        uint32_t strmPtr = 0, r1 = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &strmPtr);
        uc_reg_read(uc, UC_ARM_REG_R1, &r1);
        int zret = Z_STREAM_ERROR;
        if (strmPtr != 0) {
            auto* hostStrm = new z_stream{};
            zret = (name == "inflateInit2_") ? inflateInit2(hostStrm, static_cast<int>(static_cast<int32_t>(r1))) : inflateInit(hostStrm);
            if (zret == Z_OK) {
                uint32_t h = sNextZlibHandle++;
                sZlibStreamMap[h] = hostStrm;
                z_stream32 gStrm{};
                uc_mem_read(uc, strmPtr, &gStrm, sizeof(gStrm));
                gStrm.state = h;
                gStrm.total_in = 0;
                gStrm.total_out = 0;
                uc_mem_write(uc, strmPtr, &gStrm, sizeof(gStrm));
            } else {
                delete hostStrm;
            }
        }
        uint32_t ret = static_cast<uint32_t>(zret);
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }
    if (name == "inflate") {
        uint32_t strmPtr = 0, flush = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &strmPtr);
        uc_reg_read(uc, UC_ARM_REG_R1, &flush);
        int zret = Z_STREAM_ERROR;
        if (strmPtr != 0) {
            z_stream32 gStrm{};
            uc_mem_read(uc, strmPtr, &gStrm, sizeof(gStrm));
            auto zit = sZlibStreamMap.find(gStrm.state);
            if (zit != sZlibStreamMap.end() && zit->second) {
                z_stream* hs = zit->second;
                std::vector<Bytef> inBuf(gStrm.avail_in), outBuf(gStrm.avail_out);
                if (gStrm.avail_in > 0 && gStrm.next_in != 0) {
                    uc_mem_read(uc, gStrm.next_in, inBuf.data(), gStrm.avail_in);
                }
                hs->next_in = inBuf.data();
                hs->avail_in = gStrm.avail_in;
                hs->next_out = outBuf.data();
                hs->avail_out = gStrm.avail_out;
                zret = ::inflate(hs, static_cast<int>(flush));
                uint32_t consumed = gStrm.avail_in - hs->avail_in;
                uint32_t produced = gStrm.avail_out - hs->avail_out;
                if (produced > 0 && gStrm.next_out != 0) {
                    uc_mem_write(uc, gStrm.next_out, outBuf.data(), produced);
                }
                gStrm.next_in += consumed;
                gStrm.avail_in = hs->avail_in;
                gStrm.total_in += consumed;
                gStrm.next_out += produced;
                gStrm.avail_out = hs->avail_out;
                gStrm.total_out += produced;
                gStrm.adler = static_cast<uint32_t>(hs->adler);
                uc_mem_write(uc, strmPtr, &gStrm, sizeof(gStrm));
            }
        }
        uint32_t ret = static_cast<uint32_t>(zret);
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }
    if (name == "inflateEnd" || name == "inflateReset") {
        uint32_t strmPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &strmPtr);
        if (strmPtr != 0) {
            z_stream32 gStrm{};
            uc_mem_read(uc, strmPtr, &gStrm, sizeof(gStrm));
            auto zit = sZlibStreamMap.find(gStrm.state);
            if (zit != sZlibStreamMap.end() && zit->second) {
                if (name == "inflateEnd") {
                    ::inflateEnd(zit->second);
                    delete zit->second;
                    sZlibStreamMap.erase(zit);
                } else {
                    ::inflateReset(zit->second);
                }
            }
        }
        uint32_t ret = Z_OK;
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }

    // 10. Libm Hardware Math (softfp ABI ARM32)
    if (name == "sinf" || name == "cosf" || name == "tanf" || name == "sqrtf" ||
        name == "expf" || name == "logf" || name == "floorf" || name == "ceilf" ||
        name == "acosf" || name == "asinf" || name == "atanf") {
        uint32_t r0 = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &r0);
        float x = 0.0f;
        std::memcpy(&x, &r0, 4);
        float res = 0.0f;
        if (name == "sinf") res = std::sin(x);
        else if (name == "cosf") res = std::cos(x);
        else if (name == "tanf") res = std::tan(x);
        else if (name == "sqrtf") res = x >= 0.0f ? std::sqrt(x) : 0.0f;
        else if (name == "expf") res = std::exp(x);
        else if (name == "logf") res = x > 0.0f ? std::log(x) : 0.0f;
        else if (name == "floorf") res = std::floor(x);
        else if (name == "ceilf") res = std::ceil(x);
        else if (name == "acosf") res = std::acos(std::clamp(x, -1.0f, 1.0f));
        else if (name == "asinf") res = std::asin(std::clamp(x, -1.0f, 1.0f));
        else res = std::atan(x);
        uint32_t out = 0;
        std::memcpy(&out, &res, 4);
        uc_reg_write(uc, UC_ARM_REG_R0, &out);
        return;
    }
    if (name == "atan2f" || name == "powf" || name == "fmodf") {
        uint32_t r0 = 0, r1 = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &r0);
        uc_reg_read(uc, UC_ARM_REG_R1, &r1);
        float a = 0.0f, b = 0.0f;
        std::memcpy(&a, &r0, 4);
        std::memcpy(&b, &r1, 4);
        float res = (name == "atan2f") ? std::atan2(a, b) : ((name == "powf") ? std::pow(a, b) : (b != 0.0f ? std::fmod(a, b) : 0.0f));
        uint32_t out = 0;
        std::memcpy(&out, &res, 4);
        uc_reg_write(uc, UC_ARM_REG_R0, &out);
        return;
    }

    // 11. OpenGL ES 2.0 & EGL Hardware Passthrough (64-bit Host GPU)
    if (name == "glViewport" || name == "glScissor") {
        uint32_t x = 0, y = 0, w = 0, h = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &x);
        uc_reg_read(uc, UC_ARM_REG_R1, &y);
        uc_reg_read(uc, UC_ARM_REG_R2, &w);
        uc_reg_read(uc, UC_ARM_REG_R3, &h);
        if (name == "glViewport") glViewport(static_cast<GLint>(x), static_cast<GLint>(y), static_cast<GLsizei>(w), static_cast<GLsizei>(h));
        else glScissor(static_cast<GLint>(x), static_cast<GLint>(y), static_cast<GLsizei>(w), static_cast<GLsizei>(h));
        return;
    }
    if (name == "glClearColor") {
        uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &r0);
        uc_reg_read(uc, UC_ARM_REG_R1, &r1);
        uc_reg_read(uc, UC_ARM_REG_R2, &r2);
        uc_reg_read(uc, UC_ARM_REG_R3, &r3);
        float red = 0.0f, green = 0.0f, blue = 0.0f, alpha = 1.0f;
        std::memcpy(&red, &r0, 4);
        std::memcpy(&green, &r1, 4);
        std::memcpy(&blue, &r2, 4);
        std::memcpy(&alpha, &r3, 4);
        glClearColor(red, green, blue, alpha);
        return;
    }
    if (name == "glClear") {
        uint32_t mask = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &mask);
        glClear(static_cast<GLbitfield>(mask));
        return;
    }
    if (name == "glClearDepthf") {
        uint32_t r0 = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &r0);
        float d = 1.0f;
        std::memcpy(&d, &r0, 4);
        glClearDepthf(d);
        return;
    }
    if (name == "glClearStencil") {
        uint32_t s = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &s);
        glClearStencil(static_cast<GLint>(s));
        return;
    }
    if (name == "glEnable" || name == "glDisable" || name == "glIsEnabled") {
        uint32_t cap = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &cap);
        if (name == "glIsEnabled") {
            uint32_t res = 0;
            if (GlesVirtualizer::getInstance().handleIsEnabled(static_cast<GLenum>(cap), res)) {
                uc_reg_write(uc, UC_ARM_REG_R0, &res);
            } else {
                uint32_t hostRes = glIsEnabled(static_cast<GLenum>(cap));
                uc_reg_write(uc, UC_ARM_REG_R0, &hostRes);
            }
        } else {
            if (!GlesVirtualizer::getInstance().handleEnableDisable(name, static_cast<GLenum>(cap))) {
                if (name == "glEnable") glEnable(static_cast<GLenum>(cap));
                else glDisable(static_cast<GLenum>(cap));
            }
        }
        return;
    }
    if (name == "glBlendFunc") {
        uint32_t s = 0, d = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &s);
        uc_reg_read(uc, UC_ARM_REG_R1, &d);
        glBlendFunc(static_cast<GLenum>(s), static_cast<GLenum>(d));
        return;
    }
    if (name == "glBlendFuncSeparate") {
        uint32_t sr = 0, dr = 0, sa = 0, da = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &sr);
        uc_reg_read(uc, UC_ARM_REG_R1, &dr);
        uc_reg_read(uc, UC_ARM_REG_R2, &sa);
        uc_reg_read(uc, UC_ARM_REG_R3, &da);
        glBlendFuncSeparate(sr, dr, sa, da);
        return;
    }
    if (name == "glBlendEquation") {
        uint32_t m = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &m);
        glBlendEquation(m);
        return;
    }
    if (name == "glBlendEquationSeparate") {
        uint32_t mr = 0, ma = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &mr);
        uc_reg_read(uc, UC_ARM_REG_R1, &ma);
        glBlendEquationSeparate(mr, ma);
        return;
    }
    if (name == "glDepthFunc" || name == "glCullFace" || name == "glFrontFace" || name == "glActiveTexture") {
        uint32_t v = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &v);
        if (name == "glDepthFunc") glDepthFunc(v);
        else if (name == "glCullFace") glCullFace(v);
        else if (name == "glFrontFace") glFrontFace(v);
        else glActiveTexture(v);
        return;
    }
    if (name == "glDepthMask") {
        uint32_t flag = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &flag);
        glDepthMask(static_cast<GLboolean>(flag));
        return;
    }
    if (name == "glColorMask") {
        uint32_t r = 0, g = 0, b = 0, a = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &r);
        uc_reg_read(uc, UC_ARM_REG_R1, &g);
        uc_reg_read(uc, UC_ARM_REG_R2, &b);
        uc_reg_read(uc, UC_ARM_REG_R3, &a);
        glColorMask(static_cast<GLboolean>(r), static_cast<GLboolean>(g), static_cast<GLboolean>(b), static_cast<GLboolean>(a));
        return;
    }
    if (name == "glBindTexture" || name == "glBindBuffer" || name == "glBindFramebuffer" || name == "glBindFramebufferOES" ||
        name == "glBindRenderbuffer" || name == "glBindRenderbufferOES") {
        uint32_t target = 0, id = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &target);
        uc_reg_read(uc, UC_ARM_REG_R1, &id);
        if (name == "glBindTexture") glBindTexture(target, id);
        else if (name == "glBindBuffer") {
            GlesVirtualizer::getInstance().notifyBindBuffer(target, id);
            glBindBuffer(target, id);
        }
        else if (name.rfind("glBindFramebuffer", 0) == 0) glBindFramebuffer(target, id);
        else glBindRenderbuffer(target, id);
        return;
    }
    if (name == "glGenTextures" || name == "glDeleteTextures" || name == "glGenBuffers" || name == "glDeleteBuffers" ||
        name == "glGenFramebuffers" || name == "glGenFramebuffersOES" || name == "glDeleteFramebuffers" || name == "glDeleteFramebuffersOES" ||
        name == "glGenRenderbuffers" || name == "glGenRenderbuffersOES" || name == "glDeleteRenderbuffers" || name == "glDeleteRenderbuffersOES") {
        uint32_t n = 0, idsPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &n);
        uc_reg_read(uc, UC_ARM_REG_R1, &idsPtr);
        if (n > 0 && n <= 4096 && idsPtr != 0) {
            std::vector<GLuint> ids(n, 0);
            if (name.find("Delete") != std::string::npos) {
                uc_mem_read(uc, idsPtr, ids.data(), n * sizeof(GLuint));
                if (name == "glDeleteTextures") glDeleteTextures(n, ids.data());
                else if (name == "glDeleteBuffers") glDeleteBuffers(n, ids.data());
                else if (name.rfind("glDeleteFramebuffers", 0) == 0) glDeleteFramebuffers(n, ids.data());
                else glDeleteRenderbuffers(n, ids.data());
            } else {
                if (name == "glGenTextures") glGenTextures(n, ids.data());
                else if (name == "glGenBuffers") glGenBuffers(n, ids.data());
                else if (name.rfind("glGenFramebuffers", 0) == 0) glGenFramebuffers(n, ids.data());
                else glGenRenderbuffers(n, ids.data());
                uc_mem_write(uc, idsPtr, ids.data(), n * sizeof(GLuint));
            }
        }
        return;
    }
    if (name == "glTexParameteri" || name == "glTexParameterf" || name == "glPixelStorei") {
        uint32_t r0 = 0, r1 = 0, r2 = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &r0);
        uc_reg_read(uc, UC_ARM_REG_R1, &r1);
        uc_reg_read(uc, UC_ARM_REG_R2, &r2);
        if (name == "glPixelStorei") {
            glPixelStorei(r0, static_cast<GLint>(r1));
        } else if (name == "glTexParameteri") {
            glTexParameteri(r0, r1, static_cast<GLint>(r2));
        } else {
            float param = 0.0f;
            std::memcpy(&param, &r2, 4);
            glTexParameterf(r0, r1, param);
        }
        return;
    }
    if (name == "glTexImage2D") {
        uint32_t target = 0, level = 0, internalFmt = 0, width = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &target);
        uc_reg_read(uc, UC_ARM_REG_R1, &level);
        uc_reg_read(uc, UC_ARM_REG_R2, &internalFmt);
        uc_reg_read(uc, UC_ARM_REG_R3, &width);
        uint32_t sp = 0, height = 0, border = 0, fmt = 0, type = 0, pixelsPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_SP, &sp);
        uc_mem_read(uc, sp + 0, &height, 4);
        uc_mem_read(uc, sp + 4, &border, 4);
        uc_mem_read(uc, sp + 8, &fmt, 4);
        uc_mem_read(uc, sp + 12, &type, 4);
        uc_mem_read(uc, sp + 16, &pixelsPtr, 4);

        std::vector<uint8_t> pixels;
        const void* hostPixels = nullptr;
        if (pixelsPtr != 0 && width > 0 && height > 0 && width <= 4096 && height <= 4096) {
            size_t bpp = (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1) ? 2 :
                         (fmt == GL_RGB ? 3 : (fmt == GL_ALPHA || fmt == GL_LUMINANCE ? 1 : 4));
            size_t totalBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * bpp;
            pixels.resize(totalBytes);
            if (uc_mem_read(uc, pixelsPtr, pixels.data(), totalBytes) == UC_ERR_OK) {
                hostPixels = pixels.data();
            }
        }
        glTexImage2D(target, static_cast<GLint>(level), static_cast<GLint>(internalFmt),
                     static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                     static_cast<GLint>(border), fmt, type, hostPixels);
        return;
    }
    if (name == "glTexSubImage2D") {
        uint32_t target = 0, level = 0, xoff = 0, yoff = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &target);
        uc_reg_read(uc, UC_ARM_REG_R1, &level);
        uc_reg_read(uc, UC_ARM_REG_R2, &xoff);
        uc_reg_read(uc, UC_ARM_REG_R3, &yoff);
        uint32_t sp = 0, width = 0, height = 0, fmt = 0, type = 0, pixelsPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_SP, &sp);
        uc_mem_read(uc, sp + 0, &width, 4);
        uc_mem_read(uc, sp + 4, &height, 4);
        uc_mem_read(uc, sp + 8, &fmt, 4);
        uc_mem_read(uc, sp + 12, &type, 4);
        uc_mem_read(uc, sp + 16, &pixelsPtr, 4);

        std::vector<uint8_t> pixels;
        if (pixelsPtr != 0 && width > 0 && height > 0 && width <= 4096 && height <= 4096) {
            size_t bpp = (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1) ? 2 :
                         (fmt == GL_RGB ? 3 : (fmt == GL_ALPHA || fmt == GL_LUMINANCE ? 1 : 4));
            size_t totalBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * bpp;
            pixels.resize(totalBytes);
            if (uc_mem_read(uc, pixelsPtr, pixels.data(), totalBytes) == UC_ERR_OK) {
                glTexSubImage2D(target, static_cast<GLint>(level), static_cast<GLint>(xoff), static_cast<GLint>(yoff),
                                static_cast<GLsizei>(width), static_cast<GLsizei>(height), fmt, type, pixels.data());
            }
        }
        return;
    }
    if (name == "glCompressedTexImage2D") {
        uint32_t target = 0, level = 0, internalFmt = 0, width = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &target);
        uc_reg_read(uc, UC_ARM_REG_R1, &level);
        uc_reg_read(uc, UC_ARM_REG_R2, &internalFmt);
        uc_reg_read(uc, UC_ARM_REG_R3, &width);
        uint32_t sp = 0, height = 0, border = 0, imgSize = 0, dataPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_SP, &sp);
        uc_mem_read(uc, sp + 0, &height, 4);
        uc_mem_read(uc, sp + 4, &border, 4);
        uc_mem_read(uc, sp + 8, &imgSize, 4);
        uc_mem_read(uc, sp + 12, &dataPtr, 4);
        if (dataPtr != 0 && imgSize > 0 && imgSize < 0x04000000U) {
            std::vector<uint8_t> data(imgSize);
            if (uc_mem_read(uc, dataPtr, data.data(), imgSize) == UC_ERR_OK) {
                glCompressedTexImage2D(target, static_cast<GLint>(level), internalFmt,
                                       static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                                       static_cast<GLint>(border), static_cast<GLsizei>(imgSize), data.data());
            }
        }
        return;
    }
    if (name == "glGenerateMipmap") {
        uint32_t target = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &target);
        glGenerateMipmap(target);
        return;
    }
    if (name == "glBufferData") {
        uint32_t target = 0, size = 0, dataPtr = 0, usage = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &target);
        uc_reg_read(uc, UC_ARM_REG_R1, &size);
        uc_reg_read(uc, UC_ARM_REG_R2, &dataPtr);
        uc_reg_read(uc, UC_ARM_REG_R3, &usage);
        if (size > 0 && size < 0x04000000U) {
            if (dataPtr != 0) {
                std::vector<uint8_t> buf(size);
                uc_mem_read(uc, dataPtr, buf.data(), size);
                glBufferData(target, size, buf.data(), usage);
            } else {
                glBufferData(target, size, nullptr, usage);
            }
        }
        return;
    }
    if (name == "glBufferSubData") {
        uint32_t target = 0, offset = 0, size = 0, dataPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &target);
        uc_reg_read(uc, UC_ARM_REG_R1, &offset);
        uc_reg_read(uc, UC_ARM_REG_R2, &size);
        uc_reg_read(uc, UC_ARM_REG_R3, &dataPtr);
        if (dataPtr != 0 && size > 0 && size < 0x04000000U) {
            std::vector<uint8_t> buf(size);
            uc_mem_read(uc, dataPtr, buf.data(), size);
            glBufferSubData(target, offset, size, buf.data());
        }
        return;
    }
    if (name == "glCreateShader") {
        uint32_t type = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &type);
        uint32_t s = glCreateShader(type);
        uc_reg_write(uc, UC_ARM_REG_R0, &s);
        return;
    }
    if (name == "glShaderSource") {
        uint32_t shader = 0, count = 0, stringsPtr = 0, lengthsPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &shader);
        uc_reg_read(uc, UC_ARM_REG_R1, &count);
        uc_reg_read(uc, UC_ARM_REG_R2, &stringsPtr);
        uc_reg_read(uc, UC_ARM_REG_R3, &lengthsPtr);
        if (count > 0 && count <= 64 && stringsPtr != 0) {
            std::string combined;
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t strAddr = 0;
                uc_mem_read(uc, stringsPtr + i * 4, &strAddr, 4);
                int32_t len = -1;
                if (lengthsPtr != 0) {
                    uc_mem_read(uc, lengthsPtr + i * 4, &len, 4);
                }
                if (strAddr != 0) {
                    if (len > 0 && len < 65536) {
                        std::vector<char> part(len);
                        uc_mem_read(uc, strAddr, part.data(), len);
                        combined.append(part.data(), len);
                    } else {
                        combined += readGuestCString(uc, strAddr, 65536);
                    }
                }
            }
            const char* src = combined.c_str();
            glShaderSource(shader, 1, &src, nullptr);
        }
        return;
    }
    if (name == "glCompileShader" || name == "glDeleteShader" || name == "glLinkProgram" || name == "glUseProgram" || name == "glDeleteProgram") {
        uint32_t id = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &id);
        if (name == "glCompileShader") glCompileShader(id);
        else if (name == "glDeleteShader") glDeleteShader(id);
        else if (name == "glLinkProgram") glLinkProgram(id);
        else if (name == "glUseProgram") glUseProgram(id);
        else glDeleteProgram(id);
        return;
    }
    if (name == "glGetShaderiv" || name == "glGetProgramiv") {
        uint32_t id = 0, pname = 0, paramsPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &id);
        uc_reg_read(uc, UC_ARM_REG_R1, &pname);
        uc_reg_read(uc, UC_ARM_REG_R2, &paramsPtr);
        GLint val = 1;
        if (name == "glGetShaderiv") glGetShaderiv(id, pname, &val);
        else glGetProgramiv(id, pname, &val);
        if (paramsPtr != 0) {
            uc_mem_write(uc, paramsPtr, &val, 4);
        }
        return;
    }
    if (name == "glGetShaderInfoLog" || name == "glGetProgramInfoLog") {
        uint32_t id = 0, maxLen = 0, lenPtr = 0, logPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &id);
        uc_reg_read(uc, UC_ARM_REG_R1, &maxLen);
        uc_reg_read(uc, UC_ARM_REG_R2, &lenPtr);
        uc_reg_read(uc, UC_ARM_REG_R3, &logPtr);
        if (lenPtr != 0) {
            uint32_t z = 0;
            uc_mem_write(uc, lenPtr, &z, 4);
        }
        if (logPtr != 0 && maxLen > 0) {
            char z = 0;
            uc_mem_write(uc, logPtr, &z, 1);
        }
        return;
    }
    if (name == "glCreateProgram") {
        uint32_t p = glCreateProgram();
        uc_reg_write(uc, UC_ARM_REG_R0, &p);
        return;
    }
    if (name == "glAttachShader") {
        uint32_t prog = 0, sh = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &prog);
        uc_reg_read(uc, UC_ARM_REG_R1, &sh);
        glAttachShader(prog, sh);
        return;
    }
    if (name == "glBindAttribLocation") {
        uint32_t prog = 0, idx = 0, namePtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &prog);
        uc_reg_read(uc, UC_ARM_REG_R1, &idx);
        uc_reg_read(uc, UC_ARM_REG_R2, &namePtr);
        std::string attr = readGuestCString(uc, namePtr, 128);
        glBindAttribLocation(prog, idx, attr.c_str());
        return;
    }
    if (name == "glGetUniformLocation" || name == "glGetAttribLocation") {
        uint32_t prog = 0, namePtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &prog);
        uc_reg_read(uc, UC_ARM_REG_R1, &namePtr);
        std::string sym = readGuestCString(uc, namePtr, 128);
        GLint loc = (name == "glGetUniformLocation") ? glGetUniformLocation(prog, sym.c_str())
                                                     : glGetAttribLocation(prog, sym.c_str());
        uint32_t ret = static_cast<uint32_t>(loc);
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }
    if (name == "glUniform1i") {
        uint32_t loc = 0, v0 = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &loc);
        uc_reg_read(uc, UC_ARM_REG_R1, &v0);
        glUniform1i(static_cast<GLint>(loc), static_cast<GLint>(v0));
        return;
    }
    if (name == "glUniform1f") {
        uint32_t loc = 0, r1 = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &loc);
        uc_reg_read(uc, UC_ARM_REG_R1, &r1);
        float v0 = 0.0f;
        std::memcpy(&v0, &r1, 4);
        glUniform1f(static_cast<GLint>(loc), v0);
        return;
    }
    if (name == "glUniform2f" || name == "glUniform3f" || name == "glUniform4f") {
        uint32_t loc = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, sp = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &loc);
        uc_reg_read(uc, UC_ARM_REG_R1, &r1);
        uc_reg_read(uc, UC_ARM_REG_R2, &r2);
        uc_reg_read(uc, UC_ARM_REG_R3, &r3);
        uc_reg_read(uc, UC_ARM_REG_SP, &sp);
        uc_mem_read(uc, sp, &r4, 4);
        float f0 = 0, f1 = 0, f2 = 0, f3 = 0;
        std::memcpy(&f0, &r1, 4);
        std::memcpy(&f1, &r2, 4);
        std::memcpy(&f2, &r3, 4);
        std::memcpy(&f3, &r4, 4);
        if (name == "glUniform2f") glUniform2f(static_cast<GLint>(loc), f0, f1);
        else if (name == "glUniform3f") glUniform3f(static_cast<GLint>(loc), f0, f1, f2);
        else glUniform4f(static_cast<GLint>(loc), f0, f1, f2, f3);
        return;
    }
    if (name == "glUniform1fv" || name == "glUniform2fv" || name == "glUniform3fv" || name == "glUniform4fv" ||
        name == "glUniform1iv" || name == "glUniform2iv" || name == "glUniform3iv" || name == "glUniform4iv") {
        uint32_t loc = 0, count = 0, ptr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &loc);
        uc_reg_read(uc, UC_ARM_REG_R1, &count);
        uc_reg_read(uc, UC_ARM_REG_R2, &ptr);
        int comp = (name[9] - '0');
        if (ptr != 0 && count > 0 && count <= 1024 && comp >= 1 && comp <= 4) {
            std::vector<uint32_t> vals(count * comp);
            uc_mem_read(uc, ptr, vals.data(), vals.size() * 4);
            if (name[10] == 'f') {
                const auto* fp = reinterpret_cast<const GLfloat*>(vals.data());
                if (comp == 1) glUniform1fv(static_cast<GLint>(loc), count, fp);
                else if (comp == 2) glUniform2fv(static_cast<GLint>(loc), count, fp);
                else if (comp == 3) glUniform3fv(static_cast<GLint>(loc), count, fp);
                else glUniform4fv(static_cast<GLint>(loc), count, fp);
            } else {
                const auto* ip = reinterpret_cast<const GLint*>(vals.data());
                if (comp == 1) glUniform1iv(static_cast<GLint>(loc), count, ip);
                else if (comp == 2) glUniform2iv(static_cast<GLint>(loc), count, ip);
                else if (comp == 3) glUniform3iv(static_cast<GLint>(loc), count, ip);
                else glUniform4iv(static_cast<GLint>(loc), count, ip);
            }
        }
        return;
    }
    if (name == "glUniformMatrix2fv" || name == "glUniformMatrix3fv" || name == "glUniformMatrix4fv") {
        uint32_t loc = 0, count = 0, transpose = 0, ptr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &loc);
        uc_reg_read(uc, UC_ARM_REG_R1, &count);
        uc_reg_read(uc, UC_ARM_REG_R2, &transpose);
        uc_reg_read(uc, UC_ARM_REG_R3, &ptr);
        int dim = (name[15] - '0');
        int elems = dim * dim;
        if (ptr != 0 && count > 0 && count <= 512) {
            std::vector<GLfloat> mat(count * elems);
            uc_mem_read(uc, ptr, mat.data(), mat.size() * sizeof(GLfloat));
            if (dim == 2) glUniformMatrix2fv(static_cast<GLint>(loc), count, static_cast<GLboolean>(transpose), mat.data());
            else if (dim == 3) glUniformMatrix3fv(static_cast<GLint>(loc), count, static_cast<GLboolean>(transpose), mat.data());
            else glUniformMatrix4fv(static_cast<GLint>(loc), count, static_cast<GLboolean>(transpose), mat.data());
        }
        return;
    }
    if (name == "glEnableVertexAttribArray" || name == "glDisableVertexAttribArray") {
        uint32_t idx = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &idx);
        if (name == "glEnableVertexAttribArray") {
            GlesVirtualizer::getInstance().enableVertexAttribArray(idx);
            glEnableVertexAttribArray(idx);
        } else {
            GlesVirtualizer::getInstance().disableVertexAttribArray(idx);
            glDisableVertexAttribArray(idx);
        }
        return;
    }
    if (name == "glVertexAttribPointer") {
        uint32_t idx = 0, size = 0, type = 0, norm = 0, stride = 0, ptr = 0, sp = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &idx);
        uc_reg_read(uc, UC_ARM_REG_R1, &size);
        uc_reg_read(uc, UC_ARM_REG_R2, &type);
        uc_reg_read(uc, UC_ARM_REG_R3, &norm);
        uc_reg_read(uc, UC_ARM_REG_SP, &sp);
        uc_mem_read(uc, sp + 0, &stride, 4);
        uc_mem_read(uc, sp + 4, &ptr, 4);

        GlesVirtualizer::getInstance().setVertexAttribPointer(idx, static_cast<GLint>(size), type,
                                                              static_cast<GLboolean>(norm),
                                                              static_cast<GLsizei>(stride), ptr);
        return;
    }
    if (name == "glDrawArrays") {
        GuestRuntimeEnvironment::getInstance().ensureThreadEglContext();
        static size_t sDrawArraysCount = 0;
        if (sDrawArraysCount++ < 5) {
            LOGI("GLES Passthrough: glDrawArrays #%zu eseguito su GPU host", sDrawArraysCount);
        }
        uint32_t mode = 0, first = 0, count = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &mode);
        uc_reg_read(uc, UC_ARM_REG_R1, &first);
        uc_reg_read(uc, UC_ARM_REG_R2, &count);
        GlesVirtualizer::getInstance().drawArrays(mode, static_cast<GLint>(first), static_cast<GLsizei>(count), uc);
        return;
    }
    if (name == "glDrawElements") {
        GuestRuntimeEnvironment::getInstance().ensureThreadEglContext();
        static size_t sDrawElementsCount = 0;
        if (sDrawElementsCount++ < 5) {
            LOGI("GLES Passthrough: glDrawElements #%zu eseguito su GPU host", sDrawElementsCount);
        }
        uint32_t mode = 0, count = 0, type = 0, indicesPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &mode);
        uc_reg_read(uc, UC_ARM_REG_R1, &count);
        uc_reg_read(uc, UC_ARM_REG_R2, &type);
        uc_reg_read(uc, UC_ARM_REG_R3, &indicesPtr);

        GlesVirtualizer::getInstance().drawElements(mode, static_cast<GLsizei>(count), type, indicesPtr, uc);
        return;
    }
    if (name == "glRenderbufferStorage" || name == "glRenderbufferStorageOES") {
        uint32_t target = 0, fmt = 0, w = 0, h = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &target);
        uc_reg_read(uc, UC_ARM_REG_R1, &fmt);
        uc_reg_read(uc, UC_ARM_REG_R2, &w);
        uc_reg_read(uc, UC_ARM_REG_R3, &h);
        glRenderbufferStorage(target, fmt, static_cast<GLsizei>(w), static_cast<GLsizei>(h));
        return;
    }
    if (name == "glFramebufferRenderbuffer" || name == "glFramebufferRenderbufferOES") {
        uint32_t target = 0, att = 0, rtarget = 0, rb = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &target);
        uc_reg_read(uc, UC_ARM_REG_R1, &att);
        uc_reg_read(uc, UC_ARM_REG_R2, &rtarget);
        uc_reg_read(uc, UC_ARM_REG_R3, &rb);
        glFramebufferRenderbuffer(target, att, rtarget, rb);
        return;
    }
    if (name == "glFramebufferTexture2D" || name == "glFramebufferTexture2DOES") {
        uint32_t target = 0, att = 0, textarget = 0, tex = 0, level = 0, sp = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &target);
        uc_reg_read(uc, UC_ARM_REG_R1, &att);
        uc_reg_read(uc, UC_ARM_REG_R2, &textarget);
        uc_reg_read(uc, UC_ARM_REG_R3, &tex);
        uc_reg_read(uc, UC_ARM_REG_SP, &sp);
        uc_mem_read(uc, sp, &level, 4);
        glFramebufferTexture2D(target, att, textarget, tex, static_cast<GLint>(level));
        return;
    }
    if (name == "glCheckFramebufferStatus" || name == "glCheckFramebufferStatusOES") {
        uint32_t target = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &target);
        uint32_t st = glCheckFramebufferStatus(target);
        if (st == 0) st = GL_FRAMEBUFFER_COMPLETE;
        uc_reg_write(uc, UC_ARM_REG_R0, &st);
        return;
    }
    if (name == "glFlush" || name == "glFinish") {
        glFlush();
        return;
    }
    if (name == "glGetError") {
        uint32_t err = 0; // GL_NO_ERROR
        uc_reg_write(uc, UC_ARM_REG_R0, &err);
        return;
    }
    if (name == "glGetIntegerv" || name == "glGetFloatv" || name == "glGetBooleanv") {
        uint32_t pname = 0, paramsPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &pname);
        uc_reg_read(uc, UC_ARM_REG_R1, &paramsPtr);
        if (paramsPtr != 0) {
            if (name == "glGetIntegerv") {
                if (pname == GL_VIEWPORT || pname == GL_SCISSOR_BOX) {
                    GLint box[4] = {0, 0, 1920, 1080};
                    uc_mem_write(uc, paramsPtr, box, 16);
                } else if (pname == GL_MAX_VIEWPORT_DIMS) {
                    GLint dims[2] = {4096, 4096};
                    uc_mem_write(uc, paramsPtr, dims, 8);
                } else {
                    GLint val = 4096;
                    if (pname == GL_NUM_COMPRESSED_TEXTURE_FORMATS) val = 10;
                    else if (pname == GL_MAX_TEXTURE_IMAGE_UNITS || pname == GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS) val = 16;
                    else if (pname == GL_MAX_VERTEX_ATTRIBS) val = 16;
                    else if (pname == GL_MAX_RENDERBUFFER_SIZE) val = 4096;
                    else glGetIntegerv(pname, &val);
                    uc_mem_write(uc, paramsPtr, &val, 4);
                }
            } else if (name == "glGetFloatv") {
                GLfloat vals[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                glGetFloatv(pname, vals);
                uc_mem_write(uc, paramsPtr, vals, 16);
            } else {
                GLboolean b = GL_TRUE;
                glGetBooleanv(pname, &b);
                uc_mem_write(uc, paramsPtr, &b, 1);
            }
        }
        return;
    }
    if (name == "glGetString") {
        uint32_t glEnum = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &glEnum);
        std::string strVal = "Emux32 Hardware GLES 2.0";
        if (glEnum == GL_VENDOR) strVal = "ARM / Qualcomm";
        else if (glEnum == GL_RENDERER) strVal = "Adreno / Mali Hardware Accelerated";
        else if (glEnum == GL_VERSION) strVal = "OpenGL ES 2.0 Emux32";
        else if (glEnum == GL_SHADING_LANGUAGE_VERSION) strVal = "OpenGL ES GLSL ES 1.00";
        else if (glEnum == GL_EXTENSIONS) strVal = "GL_OES_compressed_ETC1_RGB8_texture GL_OES_depth24 GL_OES_depth_texture GL_OES_texture_npot GL_OES_packed_depth_stencil GL_OES_rgb8_rgba8 GL_OES_vertex_half_float GL_EXT_texture_format_BGRA8888 GL_OES_framebuffer_object GL_OES_element_index_uint GL_OES_standard_derivatives GL_EXT_discard_framebuffer GL_OES_mapbuffer GL_OES_point_size_array GL_OES_matrix_palette GL_OES_fixed_point";
        uint32_t strGuestAddr = createGuestString(uc, strVal);
        uc_reg_write(uc, UC_ARM_REG_R0, &strGuestAddr);
        return;
    }

    // 11b. Dispatcher OpenGL ES 1.1 Fixed-Function (Matrix Stacks, Client States & Pointers)
    if (GlesVirtualizer::getInstance().handleGles1(name, uc)) {
        return;
    }

    // 12. EGL Guest Calls (Coordinate col Worker Thread EGL in DbtEngine)
    if (name == "eglSwapBuffers") {
        static size_t sGuestSwapCount = 0;
        if (sGuestSwapCount++ < 5) {
            LOGI("GLES Passthrough: eglSwapBuffers invocato da guest (#%zu)", sGuestSwapCount);
        }
        GuestRuntimeEnvironment::getInstance().swapBuffers();
        uint32_t one = 1;
        uc_reg_write(uc, UC_ARM_REG_R0, &one);
        return;
    }
    if (name == "eglCreateContext") {
        uint32_t dpy = 0, cfg = 0, shareCtx = 0, attribsPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &dpy);
        uc_reg_read(uc, UC_ARM_REG_R1, &cfg);
        uc_reg_read(uc, UC_ARM_REG_R2, &shareCtx);
        uc_reg_read(uc, UC_ARM_REG_R3, &attribsPtr);

        EGLDisplay hostDpy = (eglDisplay_ != EGL_NO_DISPLAY) ? eglDisplay_ : GuestRuntimeEnvironment::getInstance().getEglDisplay();
        EGLContext hostMainCtx = (eglContext_ != EGL_NO_CONTEXT) ? eglContext_ : GuestRuntimeEnvironment::getInstance().getEglContext();
        EGLConfig hostCfg = (eglConfig_ != nullptr) ? eglConfig_ : GuestRuntimeEnvironment::getInstance().getEglConfig();

        if (shareCtx != 0) {
            auto it = guestEglContextMap_.find(shareCtx);
            if (it != guestEglContextMap_.end() && it->second != EGL_NO_CONTEXT) {
                hostMainCtx = it->second;
            }
        }

        int clientVersion = 2;
        if (attribsPtr != 0) {
            uint32_t attrKey = 0, attrVal = 0;
            uint32_t currPtr = attribsPtr;
            for (int i = 0; i < 16; ++i) {
                if (uc_mem_read(uc, currPtr, &attrKey, 4) != UC_ERR_OK || attrKey == EGL_NONE) break;
                if (uc_mem_read(uc, currPtr + 4, &attrVal, 4) != UC_ERR_OK) break;
                if (attrKey == 0x3098 /* EGL_CONTEXT_CLIENT_VERSION */) {
                    clientVersion = static_cast<int>(attrVal);
                }
                currPtr += 8;
            }
        }

        uint32_t ctxHandle = nextMockHandle_++;
        if (hostDpy != EGL_NO_DISPLAY && hostCfg != nullptr) {
            const EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, clientVersion, EGL_NONE };
            // Condivisione contesti con fallback resiliente senza sharing se rifiutato dal driver host
            EGLContext hostCtx = ::eglCreateContext(hostDpy, hostCfg, hostMainCtx, ctxAttribs);
            if (hostCtx == EGL_NO_CONTEXT && hostMainCtx != EGL_NO_CONTEXT) {
                hostCtx = ::eglCreateContext(hostDpy, hostCfg, EGL_NO_CONTEXT, ctxAttribs);
            }

            if (hostCtx != EGL_NO_CONTEXT) {
                guestEglContextMap_[ctxHandle] = hostCtx;
                LOGI("EGL: eglCreateContext() creato contesto (GuestHandle: 0x%x, HostCtx: %p, Shared: %p, GLES ver=%d)",
                     ctxHandle, hostCtx, hostMainCtx, clientVersion);
            } else {
                LOGW("EGL: eglCreateContext() fallito (err=0x%x)", ::eglGetError());
            }
        }
        uc_reg_write(uc, UC_ARM_REG_R0, &ctxHandle);
        return;
    }

    if (name == "eglCreateWindowSurface" || name == "eglCreatePbufferSurface") {
        uint32_t dpy = 0, cfg = 0, win = 0, attribsPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &dpy);
        uc_reg_read(uc, UC_ARM_REG_R1, &cfg);
        uc_reg_read(uc, UC_ARM_REG_R2, &win);
        uc_reg_read(uc, UC_ARM_REG_R3, &attribsPtr);

        EGLDisplay hostDpy = (eglDisplay_ != EGL_NO_DISPLAY) ? eglDisplay_ : GuestRuntimeEnvironment::getInstance().getEglDisplay();
        EGLConfig hostCfg = (eglConfig_ != nullptr) ? eglConfig_ : GuestRuntimeEnvironment::getInstance().getEglConfig();
        uint32_t surfHandle = nextMockHandle_++;

        if (name == "eglCreatePbufferSurface" && hostDpy != EGL_NO_DISPLAY && hostCfg != nullptr) {
            EGLint pbufWidth = 1, pbufHeight = 1;
            if (attribsPtr != 0) {
                uint32_t attrKey = 0, attrVal = 0;
                uint32_t currPtr = attribsPtr;
                for (int i = 0; i < 16; ++i) {
                    if (uc_mem_read(uc, currPtr, &attrKey, 4) != UC_ERR_OK || attrKey == EGL_NONE) break;
                    if (uc_mem_read(uc, currPtr + 4, &attrVal, 4) != UC_ERR_OK) break;
                    if (attrKey == EGL_WIDTH) pbufWidth = static_cast<EGLint>(attrVal);
                    else if (attrKey == EGL_HEIGHT) pbufHeight = static_cast<EGLint>(attrVal);
                    currPtr += 8;
                }
            }
            const EGLint pbufAttribs[] = { EGL_WIDTH, pbufWidth, EGL_HEIGHT, pbufHeight, EGL_NONE };
            EGLSurface pbuf = ::eglCreatePbufferSurface(hostDpy, hostCfg, pbufAttribs);
            guestEglSurfaceMap_[surfHandle] = pbuf;
            LOGI("EGL: eglCreatePbufferSurface() creato pbuffer %p (%dx%d, GuestHandle: 0x%x)",
                 pbuf, pbufWidth, pbufHeight, surfHandle);
        } else {
            EGLSurface hostWindowSurf = (eglSurface_ != EGL_NO_SURFACE) ? eglSurface_ : GuestRuntimeEnvironment::getInstance().getEglSurface();
            guestEglSurfaceMap_[surfHandle] = hostWindowSurf;
            LOGI("EGL: eglCreateWindowSurface() associato Window Surface %p (GuestHandle: 0x%x)", hostWindowSurf, surfHandle);
        }
        uc_reg_write(uc, UC_ARM_REG_R0, &surfHandle);
        return;
    }

    if (name == "eglMakeCurrent") {
        uint32_t dpy = 0, draw = 0, read = 0, ctx = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &dpy);
        uc_reg_read(uc, UC_ARM_REG_R1, &draw);
        uc_reg_read(uc, UC_ARM_REG_R2, &read);
        uc_reg_read(uc, UC_ARM_REG_R3, &ctx);

        bool success = handleEglMakeCurrent(draw, read, ctx, uc);
        uint32_t retVal = success ? 1U : 0U;
        uc_reg_write(uc, UC_ARM_REG_R0, &retVal);
        return;
    }

    if (name == "eglDestroyContext") {
        uint32_t dpy = 0, ctx = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &dpy);
        uc_reg_read(uc, UC_ARM_REG_R1, &ctx);
        auto it = guestEglContextMap_.find(ctx);
        if (it != guestEglContextMap_.end() && it->second != EGL_NO_CONTEXT) {
            EGLDisplay hostDpy = (eglDisplay_ != EGL_NO_DISPLAY) ? eglDisplay_ : GuestRuntimeEnvironment::getInstance().getEglDisplay();
            if (::eglGetCurrentContext() == it->second) {
                ::eglMakeCurrent(hostDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            }
            ::eglDestroyContext(hostDpy, it->second);
            guestEglContextMap_.erase(it);
        }
        uint32_t one = 1;
        uc_reg_write(uc, UC_ARM_REG_R0, &one);
        return;
    }

    if (name == "eglDestroySurface") {
        uint32_t dpy = 0, surf = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &dpy);
        uc_reg_read(uc, UC_ARM_REG_R1, &surf);
        auto it = guestEglSurfaceMap_.find(surf);
        EGLSurface hostWindowSurf = (eglSurface_ != EGL_NO_SURFACE) ? eglSurface_ : GuestRuntimeEnvironment::getInstance().getEglSurface();
        if (it != guestEglSurfaceMap_.end() && it->second != EGL_NO_SURFACE &&
            it->second != hostWindowSurf) {
            EGLDisplay hostDpy = (eglDisplay_ != EGL_NO_DISPLAY) ? eglDisplay_ : GuestRuntimeEnvironment::getInstance().getEglDisplay();
            if (::eglGetCurrentSurface(EGL_DRAW) == it->second) {
                ::eglMakeCurrent(hostDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            }
            ::eglDestroySurface(hostDpy, it->second);
            guestEglSurfaceMap_.erase(it);
        }
        uint32_t one = 1;
        uc_reg_write(uc, UC_ARM_REG_R0, &one);
        return;
    }

    if (name == "eglGetCurrentContext") {
        uint32_t curCtxHandle = getCurrentThreadEglContextHandle();
        if (curCtxHandle == 0 && eglContext_ != EGL_NO_CONTEXT) {
            if (windowSurfaceOwnerThread_.load() == pthread_self() || ::eglGetCurrentContext() != EGL_NO_CONTEXT) {
                curCtxHandle = 1;
            }
        }
        uc_reg_write(uc, UC_ARM_REG_R0, &curCtxHandle);
        return;
    }

    if (name == "eglGetCurrentSurface") {
        uint32_t reType = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &reType);
        uint32_t curSurfHandle = getCurrentThreadEglSurfaceHandle();
        if (curSurfHandle == 0 && eglSurface_ != EGL_NO_SURFACE) {
            if (windowSurfaceOwnerThread_.load() == pthread_self() || ::eglGetCurrentSurface(EGL_DRAW) != EGL_NO_SURFACE) {
                curSurfHandle = 1;
            }
        }
        uc_reg_write(uc, UC_ARM_REG_R0, &curSurfHandle);
        return;
    }

    if (name == "eglGetCurrentDisplay" || name == "eglGetDisplay") {
        EGLDisplay hostDpy = (eglDisplay_ != EGL_NO_DISPLAY) ? eglDisplay_ : GuestRuntimeEnvironment::getInstance().getEglDisplay();
        uint32_t dpyHandle = (hostDpy != EGL_NO_DISPLAY) ? 1U : 0U;
        uc_reg_write(uc, UC_ARM_REG_R0, &dpyHandle);
        return;
    }

    if (name == "eglInitialize" || name == "eglChooseConfig") {
        if (name == "eglInitialize") {
            uint32_t majPtr = 0, minPtr = 0;
            uc_reg_read(uc, UC_ARM_REG_R1, &majPtr);
            uc_reg_read(uc, UC_ARM_REG_R2, &minPtr);
            uint32_t maj = 1, min = 4;
            if (majPtr) uc_mem_write(uc, majPtr, &maj, 4);
            if (minPtr) uc_mem_write(uc, minPtr, &min, 4);
        } else if (name == "eglChooseConfig") {
            uint32_t configsPtr = 0, numConfigPtr = 0;
            uc_reg_read(uc, UC_ARM_REG_R2, &configsPtr);
            uc_reg_read(uc, UC_ARM_REG_R4, &numConfigPtr);
            uint32_t sp = 0;
            uc_reg_read(uc, UC_ARM_REG_SP, &sp);
            uc_mem_read(uc, sp, &numConfigPtr, 4);
            uint32_t cfg = 1;
            if (configsPtr) uc_mem_write(uc, configsPtr, &cfg, 4);
            if (numConfigPtr) uc_mem_write(uc, numConfigPtr, &cfg, 4);
        }
        uint32_t one = 1;
        uc_reg_write(uc, UC_ARM_REG_R0, &one);
        return;
    }
    if (name == "eglGetConfigAttrib" || name == "eglQuerySurface") {
        uint32_t attr = 0, valPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R2, &attr);
        uc_reg_read(uc, UC_ARM_REG_R3, &valPtr);
        if (valPtr != 0) {
            uint32_t val = 0;
            switch (attr) {
                case 0x3057: val = 1920; break; // EGL_WIDTH
                case 0x3056: val = 1080; break; // EGL_HEIGHT
                case 0x3040: val = 4; break;    // EGL_RENDERABLE_TYPE (EGL_OPENGL_ES2_BIT)
                case 0x3033: val = 5; break;    // EGL_SURFACE_TYPE (WINDOW | PBUFFER)
                case 0x3025: val = 24; break;   // EGL_DEPTH_SIZE
                case 0x3026: val = 8; break;    // EGL_STENCIL_SIZE
                case 0x3024:                    // EGL_RED_SIZE
                case 0x3023:                    // EGL_GREEN_SIZE
                case 0x3022:                    // EGL_BLUE_SIZE
                case 0x3021: val = 8; break;    // EGL_ALPHA_SIZE
                case 0x3028: val = 1; break;    // EGL_CONFIG_ID
                default: val = 8; break;
            }
            uc_mem_write(uc, valPtr, &val, 4);
        }
        uint32_t one = 1;
        uc_reg_write(uc, UC_ARM_REG_R0, &one);
        return;
    }
    if (name == "eglGetError") {
        uint32_t ok = 0x3000U; // EGL_SUCCESS
        uc_reg_write(uc, UC_ARM_REG_R0, &ok);
        return;
    }
    if (name == "eglQueryString") {
        uint32_t dpy = 0, queryName = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &dpy);
        uc_reg_read(uc, UC_ARM_REG_R1, &queryName);
        EGLDisplay hostDpy = (eglDisplay_ != EGL_NO_DISPLAY) ? eglDisplay_ : GuestRuntimeEnvironment::getInstance().getEglDisplay();
        const char* str = nullptr;
        if (hostDpy != EGL_NO_DISPLAY) {
            str = ::eglQueryString(hostDpy, static_cast<EGLint>(queryName));
        }
        if (!str) {
            switch (queryName) {
                case 0x3053: str = "Android"; break; // EGL_VENDOR
                case 0x3054: str = "1.4 Android META-EGL"; break; // EGL_VERSION
                case 0x3055: str = "EGL_KHR_image EGL_KHR_image_base EGL_KHR_gl_texture_2D_image EGL_KHR_fence_sync EGL_ANDROID_image_native_buffer EGL_KHR_surfaceless_context"; break; // EGL_EXTENSIONS
                case 0x308D: str = "OpenGL_ES"; break; // EGL_CLIENT_APIS
                default: str = ""; break;
            }
        }
        uint32_t gStr = createGuestString(uc, str ? str : "");
        uc_reg_write(uc, UC_ARM_REG_R0, &gStr);
        return;
    }

    if (name == "eglCreateSyncKHR") {
        uint32_t dpy = 0, type = 0, attribsPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &dpy);
        uc_reg_read(uc, UC_ARM_REG_R1, &type);
        uc_reg_read(uc, UC_ARM_REG_R2, &attribsPtr);

        typedef void* (*PFN_eglCreateSyncKHR)(EGLDisplay, unsigned int, const EGLint*);
        static auto pfn = reinterpret_cast<PFN_eglCreateSyncKHR>(::eglGetProcAddress("eglCreateSyncKHR"));

        EGLDisplay hostDpy = (eglDisplay_ != EGL_NO_DISPLAY) ? eglDisplay_ : GuestRuntimeEnvironment::getInstance().getEglDisplay();
        void* hostSync = nullptr;
        if (pfn && hostDpy != EGL_NO_DISPLAY) {
            hostSync = pfn(hostDpy, static_cast<unsigned int>(type), nullptr);
        }
        uint32_t syncHandle = nextMockHandle_++;
        guestEglSyncMap_[syncHandle] = hostSync;
        LOGI("EGL: eglCreateSyncKHR() creato sync handle 0x%x (hostSync=%p, type=0x%x)", syncHandle, hostSync, type);
        uc_reg_write(uc, UC_ARM_REG_R0, &syncHandle);
        return;
    }

    if (name == "eglDestroySyncKHR") {
        uint32_t dpy = 0, syncHandle = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &dpy);
        uc_reg_read(uc, UC_ARM_REG_R1, &syncHandle);

        auto it = guestEglSyncMap_.find(syncHandle);
        if (it != guestEglSyncMap_.end()) {
            if (it->second != nullptr) {
                typedef EGLBoolean (*PFN_eglDestroySyncKHR)(EGLDisplay, void*);
                static auto pfn = reinterpret_cast<PFN_eglDestroySyncKHR>(::eglGetProcAddress("eglDestroySyncKHR"));
                EGLDisplay hostDpy = (eglDisplay_ != EGL_NO_DISPLAY) ? eglDisplay_ : GuestRuntimeEnvironment::getInstance().getEglDisplay();
                if (pfn && hostDpy != EGL_NO_DISPLAY) {
                    pfn(hostDpy, it->second);
                }
            }
            guestEglSyncMap_.erase(it);
        }
        uint32_t one = 1;
        uc_reg_write(uc, UC_ARM_REG_R0, &one);
        return;
    }

    if (name == "eglClientWaitSyncKHR") {
        uint32_t dpy = 0, syncHandle = 0, flags = 0;
        uint32_t timeoutLow = 0, timeoutHigh = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &dpy);
        uc_reg_read(uc, UC_ARM_REG_R1, &syncHandle);
        uc_reg_read(uc, UC_ARM_REG_R2, &flags);
        uc_reg_read(uc, UC_ARM_REG_R3, &timeoutLow);
        uint32_t sp = 0;
        uc_reg_read(uc, UC_ARM_REG_SP, &sp);
        uc_mem_read(uc, sp, &timeoutHigh, 4);
        uint64_t timeout = (static_cast<uint64_t>(timeoutHigh) << 32) | timeoutLow;

        auto it = guestEglSyncMap_.find(syncHandle);
        uint32_t res = 0x30BCU; // EGL_CONDITION_SATISFIED_KHR
        if (it != guestEglSyncMap_.end() && it->second != nullptr) {
            typedef EGLint (*PFN_eglClientWaitSyncKHR)(EGLDisplay, void*, EGLint, uint64_t);
            static auto pfn = reinterpret_cast<PFN_eglClientWaitSyncKHR>(::eglGetProcAddress("eglClientWaitSyncKHR"));
            EGLDisplay hostDpy = (eglDisplay_ != EGL_NO_DISPLAY) ? eglDisplay_ : GuestRuntimeEnvironment::getInstance().getEglDisplay();
            if (pfn && hostDpy != EGL_NO_DISPLAY) {
                res = static_cast<uint32_t>(pfn(hostDpy, it->second, static_cast<EGLint>(flags), timeout));
            }
        }
        uc_reg_write(uc, UC_ARM_REG_R0, &res);
        return;
    }

    if (name == "eglWaitSyncKHR") {
        uint32_t one = 1;
        uc_reg_write(uc, UC_ARM_REG_R0, &one);
        return;
    }

    if (name == "glFenceSync") {
        GuestRuntimeEnvironment::getInstance().ensureThreadEglContext();
        uint32_t condition = 0, flags = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &condition);
        uc_reg_read(uc, UC_ARM_REG_R1, &flags);

        typedef void* (*PFN_glFenceSync)(GLenum, GLbitfield);
        static auto pfn = reinterpret_cast<PFN_glFenceSync>(::eglGetProcAddress("glFenceSync"));
        void* syncObj = nullptr;
        if (pfn) {
            syncObj = pfn(condition, flags);
        }
        uint32_t syncH = nextMockHandle_++;
        guestGlSyncMap_[syncH] = syncObj;
        uc_reg_write(uc, UC_ARM_REG_R0, &syncH);
        return;
    }

    if (name == "glClientWaitSync") {
        GuestRuntimeEnvironment::getInstance().ensureThreadEglContext();
        uint32_t syncH = 0, flags = 0;
        uint32_t timeoutLow = 0, timeoutHigh = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &syncH);
        uc_reg_read(uc, UC_ARM_REG_R1, &flags);
        uc_reg_read(uc, UC_ARM_REG_R2, &timeoutLow);
        uc_reg_read(uc, UC_ARM_REG_R3, &timeoutHigh);
        uint64_t timeout = (static_cast<uint64_t>(timeoutHigh) << 32) | timeoutLow;

        uint32_t res = 0x911CU; // GL_ALREADY_SIGNALED (or GL_CONDITION_SATISFIED 0x911C)
        auto it = guestGlSyncMap_.find(syncH);
        if (it != guestGlSyncMap_.end() && it->second != nullptr) {
            typedef GLenum (*PFN_glClientWaitSync)(void*, GLbitfield, uint64_t);
            static auto pfn = reinterpret_cast<PFN_glClientWaitSync>(::eglGetProcAddress("glClientWaitSync"));
            if (pfn) {
                res = static_cast<uint32_t>(pfn(it->second, flags, timeout));
            }
        }
        uc_reg_write(uc, UC_ARM_REG_R0, &res);
        return;
    }

    if (name == "glWaitSync") {
        GuestRuntimeEnvironment::getInstance().ensureThreadEglContext();
        uint32_t syncH = 0, flags = 0;
        uint32_t timeoutLow = 0, timeoutHigh = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &syncH);
        uc_reg_read(uc, UC_ARM_REG_R1, &flags);
        uc_reg_read(uc, UC_ARM_REG_R2, &timeoutLow);
        uc_reg_read(uc, UC_ARM_REG_R3, &timeoutHigh);
        uint64_t timeout = (static_cast<uint64_t>(timeoutHigh) << 32) | timeoutLow;

        auto it = guestGlSyncMap_.find(syncH);
        if (it != guestGlSyncMap_.end() && it->second != nullptr) {
            typedef void (*PFN_glWaitSync)(void*, GLbitfield, uint64_t);
            static auto pfn = reinterpret_cast<PFN_glWaitSync>(::eglGetProcAddress("glWaitSync"));
            if (pfn) {
                pfn(it->second, flags, timeout);
            }
        }
        return;
    }

    if (name == "glDeleteSync") {
        GuestRuntimeEnvironment::getInstance().ensureThreadEglContext();
        uint32_t syncH = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &syncH);
        auto it = guestGlSyncMap_.find(syncH);
        if (it != guestGlSyncMap_.end()) {
            if (it->second != nullptr) {
                typedef void (*PFN_glDeleteSync)(void*);
                static auto pfn = reinterpret_cast<PFN_glDeleteSync>(::eglGetProcAddress("glDeleteSync"));
                if (pfn) pfn(it->second);
            }
            guestGlSyncMap_.erase(it);
        }
        return;
    }

    if (name == "glIsSync") {
        uint32_t syncH = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &syncH);
        uint32_t res = (guestGlSyncMap_.find(syncH) != guestGlSyncMap_.end()) ? 1U : 0U;
        uc_reg_write(uc, UC_ARM_REG_R0, &res);
        return;
    }

    if (name == "eglGetProcAddress") {
        uint32_t procNamePtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &procNamePtr);
        std::string procName = readGuestCString(uc, procNamePtr, 128);
        uint32_t tramp = registerSymbolTrampoline(procName, uc);
        uc_reg_write(uc, UC_ARM_REG_R0, &tramp);
        return;
    }

    // 12b. GLES 3.1 Compute Shaders & Extended Rendering
    if (name == "glDispatchCompute") {
        GuestRuntimeEnvironment::getInstance().ensureThreadEglContext();
        uint32_t gx = 0, gy = 0, gz = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &gx);
        uc_reg_read(uc, UC_ARM_REG_R1, &gy);
        uc_reg_read(uc, UC_ARM_REG_R2, &gz);
        typedef void (*PFN_glDispatchCompute)(GLuint, GLuint, GLuint);
        static auto pfn = reinterpret_cast<PFN_glDispatchCompute>(eglGetProcAddress("glDispatchCompute"));
        if (pfn) pfn(gx, gy, gz);
        return;
    }
    if (name == "glDispatchComputeIndirect") {
        GuestRuntimeEnvironment::getInstance().ensureThreadEglContext();
        uint32_t indirect = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &indirect);
        typedef void (*PFN_glDispatchComputeIndirect)(GLintptr);
        static auto pfn = reinterpret_cast<PFN_glDispatchComputeIndirect>(eglGetProcAddress("glDispatchComputeIndirect"));
        if (pfn) pfn(static_cast<GLintptr>(indirect));
        return;
    }
    if (name == "glMemoryBarrier" || name == "glMemoryBarrierByRegion") {
        GuestRuntimeEnvironment::getInstance().ensureThreadEglContext();
        uint32_t barriers = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &barriers);
        typedef void (*PFN_glMemoryBarrier)(GLbitfield);
        static auto pfn = reinterpret_cast<PFN_glMemoryBarrier>(eglGetProcAddress("glMemoryBarrier"));
        if (pfn) pfn(barriers);
        return;
    }
    if (name == "glBindImageTexture") {
        GuestRuntimeEnvironment::getInstance().ensureThreadEglContext();
        uint32_t unit = 0, tex = 0, level = 0, layered = 0, layer = 0, access = 0, format = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &unit);
        uc_reg_read(uc, UC_ARM_REG_R1, &tex);
        uc_reg_read(uc, UC_ARM_REG_R2, &level);
        uc_reg_read(uc, UC_ARM_REG_R3, &layered);
        uint32_t sp = 0;
        uc_reg_read(uc, UC_ARM_REG_SP, &sp);
        uc_mem_read(uc, sp, &layer, 4);
        uc_mem_read(uc, sp + 4, &access, 4);
        uc_mem_read(uc, sp + 8, &format, 4);
        typedef void (*PFN_glBindImageTexture)(GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum);
        static auto pfn = reinterpret_cast<PFN_glBindImageTexture>(eglGetProcAddress("glBindImageTexture"));
        if (pfn) pfn(unit, tex, static_cast<GLint>(level), static_cast<GLboolean>(layered), static_cast<GLint>(layer), access, format);
        return;
    }
    if (name == "glTexStorage2DMultisample") {
        GuestRuntimeEnvironment::getInstance().ensureThreadEglContext();
        uint32_t target = 0, samples = 0, internalformat = 0, w = 0, h = 0, fixedsamples = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &target);
        uc_reg_read(uc, UC_ARM_REG_R1, &samples);
        uc_reg_read(uc, UC_ARM_REG_R2, &internalformat);
        uc_reg_read(uc, UC_ARM_REG_R3, &w);
        uint32_t sp = 0;
        uc_reg_read(uc, UC_ARM_REG_SP, &sp);
        uc_mem_read(uc, sp, &h, 4);
        uc_mem_read(uc, sp + 4, &fixedsamples, 4);
        typedef void (*PFN_glTexStorage2DMultisample)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLboolean);
        static auto pfn = reinterpret_cast<PFN_glTexStorage2DMultisample>(eglGetProcAddress("glTexStorage2DMultisample"));
        if (pfn) pfn(target, samples, internalformat, w, h, fixedsamples != 0);
        return;
    }
    if (name == "glFramebufferParameteri") {
        GuestRuntimeEnvironment::getInstance().ensureThreadEglContext();
        uint32_t target = 0, pname = 0, param = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &target);
        uc_reg_read(uc, UC_ARM_REG_R1, &pname);
        uc_reg_read(uc, UC_ARM_REG_R2, &param);
        typedef void (*PFN_glFramebufferParameteri)(GLenum, GLenum, GLint);
        static auto pfn = reinterpret_cast<PFN_glFramebufferParameteri>(eglGetProcAddress("glFramebufferParameteri"));
        if (pfn) pfn(target, pname, param);
        return;
    }

    // 12c. Vulkan Loader & Hardware Fallback Safe Trapping
    if (name == "vkGetInstanceProcAddr") {
        uint32_t inst = 0, namePtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &inst);
        uc_reg_read(uc, UC_ARM_REG_R1, &namePtr);
        uint32_t retAddr = 0;
        if (namePtr != 0) {
            std::string vkFn = readGuestCString(uc, namePtr, 128);
            if (vkFn == "vkCreateInstance" || vkFn == "vkEnumerateInstanceExtensionProperties" ||
                vkFn == "vkEnumerateInstanceLayerProperties") {
                retAddr = registerSymbolTrampoline(vkFn, uc);
            }
            LOGI("Vulkan: vkGetInstanceProcAddr('%s') -> 0x%08x", vkFn.c_str(), retAddr);
        }
        uc_reg_write(uc, UC_ARM_REG_R0, &retAddr);
        return;
    }
    if (name == "vkGetDeviceProcAddr") {
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }
    if (name == "vkCreateInstance") {
        LOGI("Vulkan: vkCreateInstance segnala VK_ERROR_INCOMPATIBLE_DRIVER (-9) per fallback automatico su GLES");
        uint32_t errCode = 0xFFFFFFF7U; // VK_ERROR_INCOMPATIBLE_DRIVER (-9)
        uc_reg_write(uc, UC_ARM_REG_R0, &errCode);
        return;
    }
    if (name == "vkEnumerateInstanceExtensionProperties") {
        uint32_t layerPtr = 0, countPtr = 0, propsPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &layerPtr);
        uc_reg_read(uc, UC_ARM_REG_R1, &countPtr);
        uc_reg_read(uc, UC_ARM_REG_R2, &propsPtr);
        if (countPtr != 0) {
            uint32_t zero = 0;
            uc_mem_write(uc, countPtr, &zero, 4);
        }
        uint32_t ok = 0; // VK_SUCCESS
        uc_reg_write(uc, UC_ARM_REG_R0, &ok);
        return;
    }
    if (name == "vkEnumerateInstanceLayerProperties") {
        uint32_t countPtr = 0, propsPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &countPtr);
        uc_reg_read(uc, UC_ARM_REG_R1, &propsPtr);
        if (countPtr != 0) {
            uint32_t zero = 0;
            uc_mem_write(uc, countPtr, &zero, 4);
        }
        uint32_t ok = 0; // VK_SUCCESS
        uc_reg_write(uc, UC_ARM_REG_R0, &ok);
        return;
    }

    // 13. Pthread Create con virtualizzazione nativa del multi-threading guest
    if (name == "pthread_create") {
        uint32_t tidPtr = 0, attrPtr = 0, startRoutine = 0, arg = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &tidPtr);
        uc_reg_read(uc, UC_ARM_REG_R1, &attrPtr);
        uc_reg_read(uc, UC_ARM_REG_R2, &startRoutine);
        uc_reg_read(uc, UC_ARM_REG_R3, &arg);

        int32_t res = GuestThreadScheduler::getInstance().createThread(tidPtr, attrPtr, startRoutine, arg, uc);
        uint32_t ret = (res == 0) ? 0U : static_cast<uint32_t>(-res);
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }

    // 14. Android NDK AssetManager & Asset APIs
    if (name == "AAssetManager_fromJava") {
        uint32_t mockMgr = 0x7D005000U;
        uc_reg_write(uc, UC_ARM_REG_R0, &mockMgr);
        return;
    }

    if (name == "AAssetManager_open") {
        uint32_t mgr = 0, namePtr = 0, mode = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &mgr);
        uc_reg_read(uc, UC_ARM_REG_R1, &namePtr);
        uc_reg_read(uc, UC_ARM_REG_R2, &mode);
        std::string filename = readGuestCString(uc, namePtr, 256);

        // 1. Prova prima il filesystem reale (file estratti su disco)
        std::string resolved = resolveAssetPath(filename);
        if (resolved.empty()) {
            if (access(filename.c_str(), F_OK) == 0) {
                resolved = filename;
            } else {
                std::string pCustom = sCustomStoragePath + "/assets/" + filename;
                if (access(pCustom.c_str(), F_OK) == 0) resolved = pCustom;
            }
        }
        if (!resolved.empty()) {
            FILE* fp = fopen(resolved.c_str(), "rb");
            if (fp) {
                fseek(fp, 0, SEEK_END);
                size_t len = static_cast<size_t>(ftell(fp));
                fseek(fp, 0, SEEK_SET);

                uint32_t assetH = nextAssetHandle_++;
                GuestAsset ga{};
                ga.id = assetH;
                ga.fp = fp;
                ga.path = resolved;
                ga.length = len;
                ga.bufferGuestAddr = 0;
                ga.vfsStream = nullptr;
                ga.dataOffset = 0;
                guestAssetMap_[assetH] = std::move(ga);

                LOGI("AAssetManager_open('%s') -> File fisico Handle 0x%08x (%zu byte)", filename.c_str(), assetH, len);
                uc_reg_write(uc, UC_ARM_REG_R0, &assetH);
                return;
            }
        }

        // 2. Accesso tramite Virtual File System (streaming OBB e APK)
        auto vfsStream = ZipArchiveVfs::getInstance().openStream(filename);
        if (vfsStream) {
            size_t len = vfsStream->size();
            off_t dOffset = vfsStream->dataOffset();
            uint32_t assetH = nextAssetHandle_++;

            GuestAsset ga{};
            ga.id = assetH;
            ga.fp = nullptr;
            ga.path = filename;
            ga.length = len;
            ga.bufferGuestAddr = 0;
            ga.dataOffset = dOffset;
            ga.vfsStream = std::move(vfsStream);
            guestAssetMap_[assetH] = std::move(ga);

            LOGI("AAssetManager_open('%s') -> VFS Container Stream Handle 0x%08x (%zu byte, Offset: 0x%lx, Stored: %s)",
                 filename.c_str(), assetH, len, static_cast<long>(dOffset),
                 guestAssetMap_[assetH].vfsStream->isStored() ? "YES" : "NO");
            uc_reg_write(uc, UC_ARM_REG_R0, &assetH);
            return;
        }

        LOGW("AAssetManager_open: Asset '%s' non trovato ne' su disco ne' nei container VFS (.apk/.obb)!", filename.c_str());
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }

    if (name == "AAsset_read") {
        uint32_t assetH = 0, bufPtr = 0, count = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &assetH);
        uc_reg_read(uc, UC_ARM_REG_R1, &bufPtr);
        uc_reg_read(uc, UC_ARM_REG_R2, &count);

        auto it = guestAssetMap_.find(assetH);
        if (it == guestAssetMap_.end() || bufPtr == 0 || count == 0) {
            uint32_t zero = 0;
            uc_reg_write(uc, UC_ARM_REG_R0, &zero);
            return;
        }

        std::vector<uint8_t> tmp(count);
        size_t nRead = 0;
        if (it->second.vfsStream) {
            nRead = it->second.vfsStream->read(tmp.data(), count);
        } else if (it->second.fp) {
            nRead = fread(tmp.data(), 1, count, it->second.fp);
        }

        if (nRead > 0) {
            uc_mem_write(uc, bufPtr, tmp.data(), nRead);
        }
        uint32_t res = static_cast<uint32_t>(nRead);
        uc_reg_write(uc, UC_ARM_REG_R0, &res);
        return;
    }

    if (name == "AAsset_seek" || name == "AAsset_seek64") {
        uint32_t assetH = 0, offset = 0, whence = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &assetH);
        uc_reg_read(uc, UC_ARM_REG_R1, &offset);
        uc_reg_read(uc, UC_ARM_REG_R2, &whence);

        auto it = guestAssetMap_.find(assetH);
        if (it == guestAssetMap_.end()) {
            uint32_t err = 0xFFFFFFFFU;
            uc_reg_write(uc, UC_ARM_REG_R0, &err);
            return;
        }

        int w = (whence == 0) ? SEEK_SET : ((whence == 1) ? SEEK_CUR : SEEK_END);
        uint32_t res = 0xFFFFFFFFU;
        if (it->second.vfsStream) {
            if (it->second.vfsStream->seek(static_cast<off_t>(static_cast<int32_t>(offset)), w) == 0) {
                res = static_cast<uint32_t>(it->second.vfsStream->tell());
            }
        } else if (it->second.fp) {
            fseek(it->second.fp, static_cast<long>(static_cast<int32_t>(offset)), w);
            res = static_cast<uint32_t>(ftell(it->second.fp));
        }

        uc_reg_write(uc, UC_ARM_REG_R0, &res);
        return;
    }

    if (name == "AAsset_getLength" || name == "AAsset_getLength64") {
        uint32_t assetH = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &assetH);
        auto it = guestAssetMap_.find(assetH);
        uint32_t len = (it != guestAssetMap_.end()) ? static_cast<uint32_t>(it->second.length) : 0U;
        uc_reg_write(uc, UC_ARM_REG_R0, &len);
        return;
    }

    if (name == "AAsset_getRemainingLength" || name == "AAsset_getRemainingLength64") {
        uint32_t assetH = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &assetH);
        auto it = guestAssetMap_.find(assetH);
        uint32_t rem = 0;
        if (it != guestAssetMap_.end()) {
            if (it->second.vfsStream) {
                off_t cur = it->second.vfsStream->tell();
                if (cur >= 0 && static_cast<size_t>(cur) < it->second.length) {
                    rem = static_cast<uint32_t>(it->second.length - cur);
                }
            } else if (it->second.fp) {
                long cur = ftell(it->second.fp);
                if (cur >= 0 && static_cast<size_t>(cur) < it->second.length) {
                    rem = static_cast<uint32_t>(it->second.length - cur);
                }
            }
        }
        uc_reg_write(uc, UC_ARM_REG_R0, &rem);
        return;
    }

    if (name == "AAsset_getBuffer") {
        uint32_t assetH = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &assetH);
        auto it = guestAssetMap_.find(assetH);
        if (it == guestAssetMap_.end()) {
            uint32_t zero = 0;
            uc_reg_write(uc, UC_ARM_REG_R0, &zero);
            return;
        }

        if (it->second.bufferGuestAddr == 0 && it->second.length > 0) {
            uint32_t gBuf = allocGuestHeap(uc, static_cast<uint32_t>(it->second.length + 16U), 16);
            if (gBuf != 0) {
                if (it->second.vfsStream) {
                    const uint8_t* raw = it->second.vfsStream->getBuffer();
                    if (raw) {
                        uc_mem_write(uc, gBuf, raw, it->second.length);
                        it->second.bufferGuestAddr = gBuf;
                    }
                } else if (it->second.fp) {
                    fseek(it->second.fp, 0, SEEK_SET);
                    std::vector<uint8_t> data(it->second.length);
                    fread(data.data(), 1, it->second.length, it->second.fp);
                    uc_mem_write(uc, gBuf, data.data(), it->second.length);
                    it->second.bufferGuestAddr = gBuf;
                }
            }
        }

        uc_reg_write(uc, UC_ARM_REG_R0, &it->second.bufferGuestAddr);
        return;
    }

    if (name == "AAsset_openFileDescriptor" || name == "AAsset_openFileDescriptor64") {
        uint32_t assetH = 0, outStart = 0, outLength = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &assetH);
        uc_reg_read(uc, UC_ARM_REG_R1, &outStart);
        uc_reg_read(uc, UC_ARM_REG_R2, &outLength);

        auto it = guestAssetMap_.find(assetH);
        if (it == guestAssetMap_.end()) {
            int err = -1;
            uc_reg_write(uc, UC_ARM_REG_R0, reinterpret_cast<uint32_t*>(&err));
            return;
        }

        int fd = -1;
        uint32_t start = static_cast<uint32_t>(it->second.dataOffset);
        uint32_t len = static_cast<uint32_t>(it->second.length);

        if (it->second.vfsStream) {
            if (it->second.vfsStream->isStored()) {
                fd = it->second.vfsStream->getContainerFd();
                start = static_cast<uint32_t>(it->second.dataOffset);
            } else {
                const uint8_t* decomp = it->second.vfsStream->getBuffer();
                if (decomp && len > 0) {
                    FILE* tf = tmpfile();
                    if (tf) {
                        fwrite(decomp, 1, len, tf);
                        fflush(tf);
                        fseek(tf, 0, SEEK_SET);
                        fd = fileno(tf);
                        start = 0;
                    }
                }
            }
        } else if (it->second.fp) {
            fd = fileno(it->second.fp);
            start = 0;
        }

        if (fd < 0) {
            int err = -1;
            uc_reg_write(uc, UC_ARM_REG_R0, reinterpret_cast<uint32_t*>(&err));
            return;
        }

        if (name == "AAsset_openFileDescriptor64") {
            uint64_t start64 = static_cast<uint64_t>(start);
            uint64_t len64 = static_cast<uint64_t>(len);
            if (outStart != 0) uc_mem_write(uc, outStart, &start64, 8);
            if (outLength != 0) uc_mem_write(uc, outLength, &len64, 8);
        } else {
            uint32_t start32 = start;
            uint32_t len32 = len;
            if (outStart != 0) uc_mem_write(uc, outStart, &start32, 4);
            if (outLength != 0) uc_mem_write(uc, outLength, &len32, 4);
        }

        int dupFd = dup(fd);
        uint32_t retFd = static_cast<uint32_t>(dupFd >= 0 ? dupFd : fd);
        uc_reg_write(uc, UC_ARM_REG_R0, &retFd);
        return;
    }

    if (name == "AAsset_isAllocated") {
        uint32_t one = 1;
        uc_reg_write(uc, UC_ARM_REG_R0, &one);
        return;
    }

    if (name == "AAsset_close") {
        uint32_t assetH = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &assetH);
        auto it = guestAssetMap_.find(assetH);
        if (it != guestAssetMap_.end()) {
            if (it->second.fp) fclose(it->second.fp);
            it->second.vfsStream.reset();
            guestAssetMap_.erase(it);
        }
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }

    if (name == "AAssetManager_openDir") {
        uint32_t mgr = 0, dirNamePtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &mgr);
        uc_reg_read(uc, UC_ARM_REG_R1, &dirNamePtr);
        std::string dirName = readGuestCString(uc, dirNamePtr, 256);

        std::vector<std::string> dirCandidates = {
            filesDir_ + "/assets/" + packageName_ + "/" + dirName,
            filesDir_ + "/assets/" + dirName,
            nativeLibDir_ + "/assets/" + dirName,
            sCustomStoragePath + "/assets/" + dirName
        };
        uint32_t dirH = nextAssetDirHandle_++;
        GuestAssetDir gad;
        gad.id = dirH;
        gad.currentIndex = 0;

        for (const auto& resolvedDir : dirCandidates) {
            DIR* d = opendir(resolvedDir.c_str());
            if (d) {
                struct dirent* entry;
                while ((entry = readdir(d)) != nullptr) {
                    if (entry->d_name[0] != '.') {
                        gad.files.push_back(entry->d_name);
                    }
                }
                closedir(d);
                if (!gad.files.empty()) break;
            }
        }

        // Se la cartella fisica non esiste o e' vuota, interroga il Virtual File System OBB/APK
        if (gad.files.empty()) {
            gad.files = ZipArchiveVfs::getInstance().listDirectory(dirName);
        }

        guestAssetDirMap_[dirH] = std::move(gad);
        uc_reg_write(uc, UC_ARM_REG_R0, &dirH);
        return;
    }

    if (name == "AAssetDir_getNextFileName") {
        uint32_t dirH = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &dirH);
        auto it = guestAssetDirMap_.find(dirH);
        if (it != guestAssetDirMap_.end() && it->second.currentIndex < it->second.files.size()) {
            std::string f = it->second.files[it->second.currentIndex++];
            uint32_t strPtr = createGuestString(uc, f);
            uc_reg_write(uc, UC_ARM_REG_R0, &strPtr);
            return;
        }
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }

    if (name == "AAssetDir_rewind") {
        uint32_t dirH = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &dirH);
        auto it = guestAssetDirMap_.find(dirH);
        if (it != guestAssetDirMap_.end()) {
            it->second.currentIndex = 0;
        }
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }

    if (name == "AAssetDir_close") {
        uint32_t dirH = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &dirH);
        guestAssetDirMap_.erase(dirH);
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }

    // 15. Android AConfiguration APIs
    if (name == "AConfiguration_new") {
        uint32_t configH = 0x7D006000U;
        uc_reg_write(uc, UC_ARM_REG_R0, &configH);
        return;
    }

    if (name == "AConfiguration_fromAssetManager" || name == "AConfiguration_delete") {
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }

    if (name == "AConfiguration_getDensity") {
        uint32_t densityDpi = 320; // ACONFIGURATION_DENSITY_XHIGH
        uc_reg_write(uc, UC_ARM_REG_R0, &densityDpi);
        return;
    }

    if (name == "AConfiguration_getOrientation") {
        uint32_t orientation = 1; // ACONFIGURATION_ORIENTATION_PORT
        uc_reg_write(uc, UC_ARM_REG_R0, &orientation);
        return;
    }

    // Default fallback per stub non critici: traccia warning e ritorna 0
    LOGW("Trampoline: Simbolo '%s' non intercettato esplicitamente, restituzione fallback 0", name.c_str());
    uint32_t zero = 0;
    uc_reg_write(uc, UC_ARM_REG_R0, &zero);
}

} // namespace emux
