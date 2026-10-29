#include "guest_thread.h"
#include "guest_memory.h"
#include "linker_hook.h"
#include "unicorn/arm.h"
#include <android/log.h>
#include <cerrno>
#include <vector>

#define TAG "EmuxThreadScheduler"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace emux {

static thread_local uint32_t t_currentGuestTid = 1; // 1 = Main Thread

GuestThreadScheduler& GuestThreadScheduler::getInstance() {
    static GuestThreadScheduler instance;
    return instance;
}

GuestThreadScheduler::~GuestThreadScheduler() {
    shutdown();
}

void GuestThreadScheduler::initialize(uc_engine* mainUc) {
    std::lock_guard<std::mutex> lock(schedulerMutex_);
    mainUc_ = mainUc;
    isShutdown_.store(false);
    t_currentGuestTid = 1;
    auto mainThread = std::make_shared<GuestThread>();
    mainThread->tid = 1;
    mainThread->uc = mainUc;
    mainThread->isRunning.store(true);
    threads_[1] = mainThread;
    LOGI("GuestThreadScheduler inizializzato (MainThread TID=1)");
}

void GuestThreadScheduler::shutdown() {
    isShutdown_.store(true);
    std::vector<std::shared_ptr<GuestThread>> activeThreads;
    {
        std::lock_guard<std::mutex> lock(schedulerMutex_);
        for (auto& pair : threads_) {
            if (pair.second) {
                activeThreads.push_back(pair.second);
            }
        }
    }

    for (auto& th : activeThreads) {
        if (!th || th->tid == 1) continue;
        if (th->uc && th->isRunning.load()) {
            uc_emu_stop(th->uc);
        }
        {
            std::lock_guard<std::mutex> lock(th->mutex);
            th->cv.notify_all();
        }
        if (th->hostThread && th->hostThread->joinable()) {
            th->hostThread->join();
        }
        if (th->uc) {
            uc_close(th->uc);
            th->uc = nullptr;
        }
    }

    std::lock_guard<std::mutex> lock(schedulerMutex_);
    threads_.clear();
}

static void workerInterruptCallback(uc_engine* uc, uint32_t intno, void* /* userData */) {
    if (!uc) return;
    if (intno & 0x80000000U) {
        uint32_t svcComment = intno & 0x00FFFFFFU;
        GuestRuntimeEnvironment::getInstance().handleTrampolineCall(svcComment, uc);
        return;
    }

    uint32_t r7 = 0, r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0;
    uc_reg_read(uc, UC_ARM_REG_R7, &r7);
    uc_reg_read(uc, UC_ARM_REG_R0, &r0);
    uc_reg_read(uc, UC_ARM_REG_R1, &r1);
    uc_reg_read(uc, UC_ARM_REG_R2, &r2);
    uc_reg_read(uc, UC_ARM_REG_R3, &r3);
    uc_reg_read(uc, UC_ARM_REG_R4, &r4);
    uc_reg_read(uc, UC_ARM_REG_R5, &r5);

    int32_t ret = SyscallDispatcher::handleSyscall(r7, r0, r1, r2, r3, r4, r5, uc);
    uint32_t retVal = static_cast<uint32_t>(ret);
    uc_reg_write(uc, UC_ARM_REG_R0, &retVal);
}

int32_t GuestThreadScheduler::createThread(
    uint32_t threadOutPtr,
    uint32_t /* attrPtr */,
    uint32_t startRoutine,
    uint32_t arg,
    uc_engine* callerUc
) {
    if (startRoutine == 0) {
        LOGE("GuestThreadScheduler::createThread fallito: startRoutine nullo");
        return -EINVAL;
    }

    uint32_t tid = nextTid_++;
    auto thread = std::make_shared<GuestThread>();
    thread->tid = tid;
    thread->startRoutine = startRoutine;
    thread->arg = arg;

    // Dimensionamento stack dedicato: 1 MB per thread nello spazio guest alto
    thread->stackSize = 1024 * 1024; // 1 MB
    thread->stackBase = 0x7B000000U - (tid * 0x100000U);
    thread->stackTop  = thread->stackBase + thread->stackSize - 16U;

    // Dimensionamento TLS dedicato a granularità 4 KB (0x7D020000 + tid*0x1000)
    thread->tlsAddr   = 0x7D020000U + (tid * 0x1000U);

    auto& gm = GuestMemory::getSharedInstance();
    // Mappa lo stack nello spazio guest condiviso se non ancora mappato
    if (!gm.isMappedRange(thread->stackBase, thread->stackSize)) {
        gm.map(thread->stackBase, thread->stackSize, GP_READ | GP_WRITE);
    }
    // Mappa il blocco TLS
    if (!gm.isMappedRange(thread->tlsAddr, 4096)) {
        gm.map(thread->tlsAddr, 4096, GP_READ | GP_WRITE);
    }

    // Inizializzazione completa del blocco TLS conforme all'ABI Android Bionic ARM32
    uint32_t tlsBlock[64] = {0};
    tlsBlock[0] = thread->tlsAddr;         // TLS_SLOT_SELF
    tlsBlock[1] = thread->tid;             // TLS_SLOT_THREAD_ID (pthread_t)
    tlsBlock[2] = thread->tlsAddr + 0x100; // TLS_SLOT_ERRNO (allocato a offset +256 nel blocco TLS 4KB)
    tlsBlock[3] = 0;                       // TLS_SLOT_OPENGL
    tlsBlock[4] = 0;                       // TLS_SLOT_OPENGL_API
    tlsBlock[5] = 0xDEADBEE0U;             // TLS_SLOT_STACK_GUARD
    tlsBlock[6] = 0xDEADBEE0U;             // Stack guard backup
    tlsBlock[7] = 0;                       // TLS_SLOT_ART_THREAD_SELF

    uint32_t zeroErrno = 0;
    std::memcpy(gm.g2h(thread->tlsAddr), tlsBlock, sizeof(tlsBlock));
    std::memcpy(gm.g2h(thread->tlsAddr + 0x100), &zeroErrno, sizeof(zeroErrno));

    if (callerUc) {
        uc_mem_write(callerUc, thread->tlsAddr, tlsBlock, sizeof(tlsBlock));
        uc_mem_write(callerUc, thread->tlsAddr + 0x100, &zeroErrno, sizeof(zeroErrno));
    }

    // Istanzia il motore di emulazione CPU dedicato per questo thread
    uc_engine* threadUc = nullptr;
    uc_err err = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &threadUc);
    if (err != UC_ERR_OK || !threadUc) {
        LOGE("GuestThreadScheduler::createThread: uc_open fallito: %s", uc_strerror(err));
        return -EAGAIN;
    }
    thread->uc = threadUc;

    // Registra l'hook degli interrupt SVC e syscall sul thread dedicato
    uc_hook h;
    uc_hook_add(threadUc, &h, UC_HOOK_INTR, reinterpret_cast<void*>(workerInterruptCallback), nullptr, 1, 0);

    // Registri CPU iniziali conformi all'AAPCS ARM32
    uint32_t sp = thread->stackTop;
    uint32_t lr = 0xDEADBEEFU;
    uint32_t r0 = arg;
    uint32_t cpsr = (startRoutine & 1U) ? 0x00000030U : 0x00000010U; // Thumb vs ARM
    uint32_t tls = thread->tlsAddr;

    uc_reg_write(threadUc, UC_ARM_REG_R0, &r0);
    uc_reg_write(threadUc, UC_ARM_REG_SP, &sp);
    uc_reg_write(threadUc, UC_ARM_REG_LR, &lr);
    uc_reg_write(threadUc, UC_ARM_REG_CPSR, &cpsr);
    uc_reg_write(threadUc, UC_ARM_REG_C13_C0_2, &tls);
    uc_reg_write(threadUc, UC_ARM_REG_C13_C0_3, &tls);

    // Salva il TID/handle del thread nella destinazione richiesta dal chiamante
    if (threadOutPtr != 0 && callerUc) {
        uc_mem_write(callerUc, threadOutPtr, &tid, 4);
    }

    {
        std::lock_guard<std::mutex> lock(schedulerMutex_);
        threads_[tid] = thread;
    }

    // Avvio del thread host asincrono
    thread->hostThread = std::make_unique<std::thread>(&GuestThreadScheduler::threadWorkerLoop, this, thread);

    LOGI("GuestThreadScheduler: Thread creato con successo: TID=%u, Entry=0x%08x, Arg=0x%08x, Stack=0x%08x, TLS=0x%08x",
         tid, startRoutine, arg, sp, tls);
    return 0; // SUCCESS
}

void GuestThreadScheduler::threadWorkerLoop(std::shared_ptr<GuestThread> thread) {
    if (!thread || !thread->uc) return;
    t_currentGuestTid = thread->tid;
    thread->isRunning.store(true);

    LOGI("GuestThreadScheduler: [TID=%u] Inizio esecuzione guest uc_emu_start...", thread->tid);

    uc_err err = uc_emu_start(thread->uc, thread->startRoutine, 0xDEADBEEFU, 0, 0);
    if (err != UC_ERR_OK && err != UC_ERR_INSN_INVALID) {
        LOGW("GuestThreadScheduler: [TID=%u] uc_emu_start terminato: %s", thread->tid, uc_strerror(err));
    }

    uint32_t ret = 0;
    uc_reg_read(thread->uc, UC_ARM_REG_R0, &ret);
    thread->returnValue = ret;

    LOGI("GuestThreadScheduler: [TID=%u] Esecuzione guest conclusa (ret=0x%08x)", thread->tid, ret);

    {
        std::lock_guard<std::mutex> lock(thread->mutex);
        thread->isRunning.store(false);
        thread->isFinished.store(true);
    }
    thread->cv.notify_all();

    if (thread->isDetached.load()) {
        std::lock_guard<std::mutex> lock(schedulerMutex_);
        if (thread->uc) {
            uc_close(thread->uc);
            thread->uc = nullptr;
        }
        threads_.erase(thread->tid);
    }
}

int32_t GuestThreadScheduler::joinThread(uint32_t tid, uint32_t retvalPtr, uc_engine* callerUc) {
    std::shared_ptr<GuestThread> target;
    {
        std::lock_guard<std::mutex> lock(schedulerMutex_);
        auto it = threads_.find(tid);
        if (it == threads_.end()) {
            return -ESRCH;
        }
        target = it->second;
    }

    if (target->isDetached.load()) {
        return -EINVAL;
    }

    {
        std::unique_lock<std::mutex> lock(target->mutex);
        target->cv.wait(lock, [&]() {
            return target->isFinished.load() || isShutdown_.load();
        });
    }

    if (target->hostThread && target->hostThread->joinable()) {
        target->hostThread->join();
    }

    if (retvalPtr != 0 && callerUc) {
        uint32_t ret = target->returnValue;
        uc_mem_write(callerUc, retvalPtr, &ret, 4);
    }

    if (target->uc) {
        uc_close(target->uc);
        target->uc = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(schedulerMutex_);
        threads_.erase(tid);
    }

    LOGI("GuestThreadScheduler: [TID=%u] Thread joined con successo.", tid);
    return 0;
}

int32_t GuestThreadScheduler::detachThread(uint32_t tid) {
    std::lock_guard<std::mutex> lock(schedulerMutex_);
    auto it = threads_.find(tid);
    if (it == threads_.end()) return -ESRCH;
    it->second->isDetached.store(true);
    if (it->second->hostThread && it->second->hostThread->joinable()) {
        it->second->hostThread->detach();
    }
    LOGI("GuestThreadScheduler: [TID=%u] Thread detached.", tid);
    return 0;
}

uint32_t GuestThreadScheduler::getCurrentTid() {
    return t_currentGuestTid;
}

void GuestThreadScheduler::exitThread(uint32_t retval, uc_engine* callerUc) {
    uint32_t tid = getCurrentTid();
    std::shared_ptr<GuestThread> target;
    {
        std::lock_guard<std::mutex> lock(schedulerMutex_);
        auto it = threads_.find(tid);
        if (it != threads_.end()) {
            target = it->second;
        }
    }

    if (target) {
        target->returnValue = retval;
    }

    if (callerUc) {
        uc_emu_stop(callerUc);
    }
}

uint32_t GuestThreadScheduler::createTlsKey(uint32_t destructorFn) {
    std::lock_guard<std::mutex> lock(schedulerMutex_);
    uint32_t k = nextTlsKey_++;
    tlsKeyDestructors_[k] = destructorFn;
    return k;
}

int32_t GuestThreadScheduler::deleteTlsKey(uint32_t key) {
    std::lock_guard<std::mutex> lock(schedulerMutex_);
    tlsKeyDestructors_.erase(key);
    return 0;
}

int32_t GuestThreadScheduler::setTlsSpecific(uint32_t key, uint32_t val) {
    uint32_t tid = getCurrentTid();
    std::lock_guard<std::mutex> lock(schedulerMutex_);
    auto it = threads_.find(tid);
    if (it != threads_.end()) {
        it->second->tlsKeyValues[key] = val;
    }
    return 0;
}

uint32_t GuestThreadScheduler::getTlsSpecific(uint32_t key) {
    uint32_t tid = getCurrentTid();
    std::lock_guard<std::mutex> lock(schedulerMutex_);
    auto it = threads_.find(tid);
    if (it != threads_.end()) {
        auto kit = it->second->tlsKeyValues.find(key);
        if (kit != it->second->tlsKeyValues.end()) {
            return kit->second;
        }
    }
    return 0;
}

size_t GuestThreadScheduler::getActiveThreadCount() const {
    std::lock_guard<std::mutex> lock(schedulerMutex_);
    return threads_.size();
}

} // namespace emux
