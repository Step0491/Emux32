#include "guest_signal.h"
#include <android/log.h>
#include <cerrno>
#include <cstring>

#define TAG "GuestSignalManager"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace emux {

GuestSignalManager& GuestSignalManager::getInstance() {
    static GuestSignalManager instance;
    return instance;
}

GuestSignalManager::GuestSignalManager() {
    reset();
}

void GuestSignalManager::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i <= GUEST_NSIG; ++i) {
        actions_[i] = GuestSigAction{};
        actions_[i].sa_handler = GUEST_SIG_DFL;
    }
    procMask_ = GuestSigSet{0, 0};
    altStack_ = GuestStack{0, GUEST_SS_DISABLE, 0};
    activeFrames_.clear();
    restorerAddr_ = 0;
}

void GuestSignalManager::initialize(uc_engine* uc, uint32_t restorerAddr) {
    std::lock_guard<std::mutex> lock(mutex_);
    restorerAddr_ = restorerAddr;

    if (uc && restorerAddr != 0) {
        // Sequenza ARM32 per rt_sigreturn: MOV R7, #173 (0xE3A070AD); SVC #0 (0xEF000000)
        uint32_t stubInsn[2] = { 0xE3A070ADU, 0xEF000000U };
        uc_err err = uc_mem_write(uc, restorerAddr, stubInsn, sizeof(stubInsn));
        if (err == UC_ERR_OK) {
            LOGI("GuestSignalManager: Restorer trampoline rt_sigreturn installata a 0x%08x", restorerAddr);
        } else {
            LOGE("GuestSignalManager: Impossibile scrivere restorer trampoline a 0x%08x: %s",
                 restorerAddr, uc_strerror(err));
        }
    }
}

int32_t GuestSignalManager::handleSigAction(uint32_t sig, uint32_t actPtr, uint32_t oldActPtr,
                                          size_t /* sigsetsize */, uc_engine* uc) {
    if (!uc || sig == 0 || sig > GUEST_NSIG || sig == GUEST_SIGKILL) {
        return -EINVAL;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (oldActPtr != 0) {
        if (uc_mem_write(uc, oldActPtr, &actions_[sig], sizeof(GuestSigAction)) != UC_ERR_OK) {
            return -EFAULT;
        }
    }

    if (actPtr != 0) {
        GuestSigAction newAct{};
        if (uc_mem_read(uc, actPtr, &newAct, sizeof(GuestSigAction)) != UC_ERR_OK) {
            return -EFAULT;
        }
        actions_[sig] = newAct;
        LOGI("GuestSignalManager: Configurato sigaction(sig=%u, handler=0x%08x, flags=0x%08x, restorer=0x%08x)",
             sig, newAct.sa_handler, newAct.sa_flags, newAct.sa_restorer);
    }

    return 0;
}

int32_t GuestSignalManager::handleSigProcMask(int how, uint32_t setPtr, uint32_t oldSetPtr,
                                            size_t /* sigsetsize */, uc_engine* uc) {
    if (!uc) return -EFAULT;

    std::lock_guard<std::mutex> lock(mutex_);

    if (oldSetPtr != 0) {
        if (uc_mem_write(uc, oldSetPtr, &procMask_, sizeof(GuestSigSet)) != UC_ERR_OK) {
            return -EFAULT;
        }
    }

    if (setPtr != 0) {
        GuestSigSet newSet{};
        if (uc_mem_read(uc, setPtr, &newSet, sizeof(GuestSigSet)) != UC_ERR_OK) {
            return -EFAULT;
        }

        switch (how) {
            case GUEST_SIG_BLOCK:
                procMask_.sig[0] |= newSet.sig[0];
                procMask_.sig[1] |= newSet.sig[1];
                break;
            case GUEST_SIG_UNBLOCK:
                procMask_.sig[0] &= ~newSet.sig[0];
                procMask_.sig[1] &= ~newSet.sig[1];
                break;
            case GUEST_SIG_SETMASK:
                procMask_ = newSet;
                break;
            default:
                return -EINVAL;
        }

        // SIGKILL e SIGSTOP non possono essere mai mascherati
        procMask_.sig[0] &= ~(1U << (GUEST_SIGKILL - 1));
    }

    return 0;
}

int32_t GuestSignalManager::handleSigAltStack(uint32_t ssPtr, uint32_t ossPtr, uc_engine* uc) {
    if (!uc) return -EFAULT;

    std::lock_guard<std::mutex> lock(mutex_);

    if (ossPtr != 0) {
        if (uc_mem_write(uc, ossPtr, &altStack_, sizeof(GuestStack)) != UC_ERR_OK) {
            return -EFAULT;
        }
    }

    if (ssPtr != 0) {
        GuestStack newStack{};
        if (uc_mem_read(uc, ssPtr, &newStack, sizeof(GuestStack)) != UC_ERR_OK) {
            return -EFAULT;
        }

        if (!(newStack.ss_flags & GUEST_SS_DISABLE)) {
            if (newStack.ss_size < 2048) {
                return -ENOMEM;
            }
        }
        altStack_ = newStack;
        LOGI("GuestSignalManager: sigaltstack aggiornato: sp=0x%08x, size=%u, flags=0x%x",
             altStack_.ss_sp, altStack_.ss_size, altStack_.ss_flags);
    }

    return 0;
}

uint32_t GuestSignalManager::handleSignal(uint32_t sig, uint32_t handler, uc_engine* uc) {
    if (!uc || sig == 0 || sig > GUEST_NSIG || sig == GUEST_SIGKILL) {
        return GUEST_SIG_ERR;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t oldHandler = actions_[sig].sa_handler;
    actions_[sig].sa_handler = handler;
    actions_[sig].sa_flags = 0;
    actions_[sig].sa_restorer = restorerAddr_;
    actions_[sig].sa_mask = GuestSigSet{0, 0};

    LOGI("GuestSignalManager: signal(sig=%u, handler=0x%08x) -> oldHandler=0x%08x", sig, handler, oldHandler);
    return oldHandler;
}

bool GuestSignalManager::hasCustomHandler(int signum) const {
    if (signum <= 0 || signum > GUEST_NSIG) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t h = actions_[signum].sa_handler;
    return (h != 0 && h != GUEST_SIG_DFL && h != GUEST_SIG_IGN && h != GUEST_SIG_ERR);
}

bool GuestSignalManager::deliverSignal(uc_engine* uc, int signum, uint32_t faultAddr, int code) {
    if (!uc || signum <= 0 || signum > GUEST_NSIG) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    const auto& act = actions_[signum];

    if (act.sa_handler == GUEST_SIG_IGN) {
        LOGI("GuestSignalManager: Segnale %d ignorato (SIG_IGN)", signum);
        return true;
    }
    if (act.sa_handler == GUEST_SIG_DFL || act.sa_handler == 0 || act.sa_handler == GUEST_SIG_ERR) {
        LOGW("GuestSignalManager: Nessun custom handler attivo per segnale %d (default)", signum);
        return false;
    }

    // 1. Snapshot completo registri CPU
    GuestSigContext mctx{};
    uc_reg_read(uc, UC_ARM_REG_R0,  &mctx.arm_r0);
    uc_reg_read(uc, UC_ARM_REG_R1,  &mctx.arm_r1);
    uc_reg_read(uc, UC_ARM_REG_R2,  &mctx.arm_r2);
    uc_reg_read(uc, UC_ARM_REG_R3,  &mctx.arm_r3);
    uc_reg_read(uc, UC_ARM_REG_R4,  &mctx.arm_r4);
    uc_reg_read(uc, UC_ARM_REG_R5,  &mctx.arm_r5);
    uc_reg_read(uc, UC_ARM_REG_R6,  &mctx.arm_r6);
    uc_reg_read(uc, UC_ARM_REG_R7,  &mctx.arm_r7);
    uc_reg_read(uc, UC_ARM_REG_R8,  &mctx.arm_r8);
    uc_reg_read(uc, UC_ARM_REG_R9,  &mctx.arm_r9);
    uc_reg_read(uc, UC_ARM_REG_R10, &mctx.arm_r10);
    uc_reg_read(uc, UC_ARM_REG_R11, &mctx.arm_fp);
    uc_reg_read(uc, UC_ARM_REG_R12, &mctx.arm_ip);
    uc_reg_read(uc, UC_ARM_REG_SP,  &mctx.arm_sp);
    uc_reg_read(uc, UC_ARM_REG_LR,  &mctx.arm_lr);
    uc_reg_read(uc, UC_ARM_REG_PC,  &mctx.arm_pc);
    uc_reg_read(uc, UC_ARM_REG_CPSR,&mctx.arm_cpsr);
    mctx.fault_address = faultAddr;
    mctx.trap_no = static_cast<uint32_t>(signum);
    mctx.error_code = static_cast<uint32_t>(code);

    // 2. Selezione stack: sigaltstack se SA_ONSTACK attivo
    uint32_t targetSp = mctx.arm_sp;
    bool usingAltStack = false;
    if ((act.sa_flags & GUEST_SA_ONSTACK) &&
        altStack_.ss_sp != 0 &&
        !(altStack_.ss_flags & GUEST_SS_DISABLE)) {
        targetSp = (altStack_.ss_sp + altStack_.ss_size) & ~7U;
        usingAltStack = true;
    } else {
        targetSp = targetSp & ~7U;
    }

    // 3. Allocazione frame nello stack guest
    targetSp -= sizeof(GuestUContext);
    uint32_t uctxAddr = targetSp;

    targetSp -= sizeof(GuestSigInfo);
    uint32_t siginfoAddr = targetSp;
    targetSp = targetSp & ~7U;

    // 4. Riempimento GuestSigInfo
    GuestSigInfo siginfo{};
    siginfo.si_signo = signum;
    siginfo.si_errno = 0;
    siginfo.si_code  = code;
    siginfo.si_addr  = faultAddr;

    // 5. Riempimento GuestUContext
    GuestUContext uctx{};
    uctx.uc_flags = 0;
    uctx.uc_link  = 0;
    uctx.uc_stack = altStack_;
    if (usingAltStack) uctx.uc_stack.ss_flags |= GUEST_SS_ONSTACK;
    uctx.uc_sigmask = procMask_;
    uctx.uc_mcontext = mctx;

    // Scrittura del frame di segnale in memoria guest
    if (uc_mem_write(uc, siginfoAddr, &siginfo, sizeof(siginfo)) != UC_ERR_OK ||
        uc_mem_write(uc, uctxAddr, &uctx, sizeof(uctx)) != UC_ERR_OK) {
        LOGE("GuestSignalManager: Fallita scrittura frame segnale su guest stack 0x%08x", targetSp);
        return false;
    }

    // Registra frame attivo per il recupero in rt_sigreturn
    ActiveFrame frame{};
    frame.ucontextAddr = uctxAddr;
    frame.siginfoAddr  = siginfoAddr;
    frame.savedSp      = mctx.arm_sp;
    frame.savedMask    = procMask_;
    frame.signum       = signum;
    activeFrames_.push_back(frame);

    // 6. Preparazione registri secondo ABI ARM32: R0=signum, R1=siginfo, R2=ucontext, LR=restorer
    uint32_t r0 = static_cast<uint32_t>(signum);
    uint32_t r1 = (act.sa_flags & GUEST_SA_SIGINFO) ? siginfoAddr : 0;
    uint32_t r2 = (act.sa_flags & GUEST_SA_SIGINFO) ? uctxAddr : 0;
    uint32_t lr = (act.sa_restorer != 0) ? act.sa_restorer : restorerAddr_;

    uc_reg_write(uc, UC_ARM_REG_R0, &r0);
    uc_reg_write(uc, UC_ARM_REG_R1, &r1);
    uc_reg_write(uc, UC_ARM_REG_R2, &r2);
    uc_reg_write(uc, UC_ARM_REG_SP, &targetSp);
    uc_reg_write(uc, UC_ARM_REG_LR, &lr);

    // Imposta PC di destinazione e stato Thumb/ARM
    uint32_t targetPc = act.sa_handler;
    uint32_t newCpsr = mctx.arm_cpsr;
    if (targetPc & 1U) {
        newCpsr |= (1U << 5); // Thumb state bit
        targetPc &= ~1U;
    } else {
        newCpsr &= ~(1U << 5);
        targetPc &= ~3U;
    }
    uc_reg_write(uc, UC_ARM_REG_CPSR, &newCpsr);
    uc_reg_write(uc, UC_ARM_REG_PC,   &targetPc);

    // 7. Mascheramento segnali durante l'esecuzione dell'handler
    procMask_.sig[0] |= act.sa_mask.sig[0];
    procMask_.sig[1] |= act.sa_mask.sig[1];
    if (!(act.sa_flags & GUEST_SA_NODEFER)) {
        if (signum <= 32) procMask_.sig[0] |= (1U << (signum - 1));
        else              procMask_.sig[1] |= (1U << (signum - 33));
    }

    LOGI("GuestSignalManager: Invocazione handler per segnale %d -> PC=0x%08x, SP=0x%08x, LR=0x%08x, fault=0x%08x",
         signum, targetPc, targetSp, lr, faultAddr);
    return true;
}

int32_t GuestSignalManager::handleSigReturn(uc_engine* uc) {
    if (!uc) return -EFAULT;

    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t uctxAddr = 0;

    if (!activeFrames_.empty()) {
        uctxAddr = activeFrames_.back().ucontextAddr;
        activeFrames_.pop_back();
    } else {
        uint32_t sp = 0;
        uc_reg_read(uc, UC_ARM_REG_SP, &sp);
        uctxAddr = sp;
    }

    GuestUContext uctx{};
    if (uc_mem_read(uc, uctxAddr, &uctx, sizeof(uctx)) != UC_ERR_OK) {
        LOGE("GuestSignalManager: rt_sigreturn fallito: ucontext non leggibile a 0x%08x", uctxAddr);
        return -EFAULT;
    }

    // Ripristina l'intero stato dei registri CPU guest salvati nel contesto
    uc_reg_write(uc, UC_ARM_REG_R0,  &uctx.uc_mcontext.arm_r0);
    uc_reg_write(uc, UC_ARM_REG_R1,  &uctx.uc_mcontext.arm_r1);
    uc_reg_write(uc, UC_ARM_REG_R2,  &uctx.uc_mcontext.arm_r2);
    uc_reg_write(uc, UC_ARM_REG_R3,  &uctx.uc_mcontext.arm_r3);
    uc_reg_write(uc, UC_ARM_REG_R4,  &uctx.uc_mcontext.arm_r4);
    uc_reg_write(uc, UC_ARM_REG_R5,  &uctx.uc_mcontext.arm_r5);
    uc_reg_write(uc, UC_ARM_REG_R6,  &uctx.uc_mcontext.arm_r6);
    uc_reg_write(uc, UC_ARM_REG_R7,  &uctx.uc_mcontext.arm_r7);
    uc_reg_write(uc, UC_ARM_REG_R8,  &uctx.uc_mcontext.arm_r8);
    uc_reg_write(uc, UC_ARM_REG_R9,  &uctx.uc_mcontext.arm_r9);
    uc_reg_write(uc, UC_ARM_REG_R10, &uctx.uc_mcontext.arm_r10);
    uc_reg_write(uc, UC_ARM_REG_R11, &uctx.uc_mcontext.arm_fp);
    uc_reg_write(uc, UC_ARM_REG_R12, &uctx.uc_mcontext.arm_ip);
    uc_reg_write(uc, UC_ARM_REG_SP,  &uctx.uc_mcontext.arm_sp);
    uc_reg_write(uc, UC_ARM_REG_LR,  &uctx.uc_mcontext.arm_lr);
    uc_reg_write(uc, UC_ARM_REG_CPSR,&uctx.uc_mcontext.arm_cpsr);
    uc_reg_write(uc, UC_ARM_REG_PC,  &uctx.uc_mcontext.arm_pc);

    // Ripristina maschera dei segnali originaria
    procMask_ = uctx.uc_sigmask;

    LOGI("GuestSignalManager: rt_sigreturn eseguito: ripristinato PC=0x%08x, SP=0x%08x, R0=0x%08x",
         uctx.uc_mcontext.arm_pc, uctx.uc_mcontext.arm_sp, uctx.uc_mcontext.arm_r0);

    return static_cast<int32_t>(uctx.uc_mcontext.arm_r0);
}

} // namespace emux
