#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <mutex>
#include "unicorn/unicorn.h"
#include "unicorn/arm.h"

namespace emux {

constexpr int GUEST_NSIG = 64;

// Costanti segnali Linux ARM32 standard
constexpr int GUEST_SIGHUP    = 1;
constexpr int GUEST_SIGINT    = 2;
constexpr int GUEST_SIGQUIT   = 3;
constexpr int GUEST_SIGILL    = 4;
constexpr int GUEST_SIGTRAP   = 5;
constexpr int GUEST_SIGABRT   = 6;
constexpr int GUEST_SIGBUS    = 7;
constexpr int GUEST_SIGFPE    = 8;
constexpr int GUEST_SIGKILL   = 9;
constexpr int GUEST_SIGUSR1   = 10;
constexpr int GUEST_SIGSEGV   = 11;
constexpr int GUEST_SIGUSR2   = 12;
constexpr int GUEST_SIGPIPE   = 13;
constexpr int GUEST_SIGALRM   = 14;
constexpr int GUEST_SIGTERM   = 15;

// Flags sigaction per Linux ARM32
constexpr uint32_t GUEST_SA_NOCLDSTOP = 0x00000001U;
constexpr uint32_t GUEST_SA_NOCLDWAIT = 0x00000002U;
constexpr uint32_t GUEST_SA_SIGINFO   = 0x00000004U;
constexpr uint32_t GUEST_SA_THIRTYFOUR = 0x00000010U;
constexpr uint32_t GUEST_SA_RESTORER  = 0x04000000U;
constexpr uint32_t GUEST_SA_ONSTACK   = 0x08000000U;
constexpr uint32_t GUEST_SA_RESTART   = 0x10000000U;
constexpr uint32_t GUEST_SA_NODEFER   = 0x40000000U;
constexpr uint32_t GUEST_SA_RESETHAND = 0x80000000U;

// Handler speciali
constexpr uint32_t GUEST_SIG_DFL = 0U;
constexpr uint32_t GUEST_SIG_IGN = 1U;
constexpr uint32_t GUEST_SIG_ERR = 0xFFFFFFFFU;

// Flags sigaltstack
constexpr int32_t GUEST_SS_ONSTACK = 1;
constexpr int32_t GUEST_SS_DISABLE = 2;

// How per sigprocmask
constexpr int GUEST_SIG_BLOCK   = 0;
constexpr int GUEST_SIG_UNBLOCK = 1;
constexpr int GUEST_SIG_SETMASK = 2;

#pragma pack(push, 4)

struct GuestSigSet {
    uint32_t sig[2]{0, 0}; // 64-bit mask
};

struct GuestSigAction {
    uint32_t sa_handler{0};   // void (*)(int) oppure void (*)(int, siginfo_t*, void*)
    uint32_t sa_flags{0};     // SA_* flags
    uint32_t sa_restorer{0};  // restorer function
    GuestSigSet sa_mask{};    // Segnali bloccati
};

struct GuestStack {
    uint32_t ss_sp{0};
    int32_t  ss_flags{GUEST_SS_DISABLE};
    uint32_t ss_size{0};
};

// Contesto CPU salvato per Linux ARM32 (sigcontext / mcontext_t)
struct GuestSigContext {
    uint32_t trap_no{0};
    uint32_t error_code{0};
    uint32_t oldmask{0};
    uint32_t arm_r0{0};
    uint32_t arm_r1{0};
    uint32_t arm_r2{0};
    uint32_t arm_r3{0};
    uint32_t arm_r4{0};
    uint32_t arm_r5{0};
    uint32_t arm_r6{0};
    uint32_t arm_r7{0};
    uint32_t arm_r8{0};
    uint32_t arm_r9{0};
    uint32_t arm_r10{0};
    uint32_t arm_fp{0};  // r11
    uint32_t arm_ip{0};  // r12
    uint32_t arm_sp{0};  // r13
    uint32_t arm_lr{0};  // r14
    uint32_t arm_pc{0};  // r15
    uint32_t arm_cpsr{0};
    uint32_t fault_address{0};
};

// Layout completo di ucontext_t per Linux ARM32
struct GuestUContext {
    uint32_t uc_flags{0};
    uint32_t uc_link{0};
    GuestStack uc_stack{};
    GuestSigContext uc_mcontext{};
    GuestSigSet uc_sigmask{};
    uint32_t uc_regspace[128]{0}; // VFP/NEON aux area
};

// Rimuove eventuali collisioni con macro di sistema (es. asm-generic/siginfo.h)
#ifdef si_addr
#undef si_addr
#endif
#ifdef si_pid
#undef si_pid
#endif
#ifdef si_uid
#undef si_uid
#endif
#ifdef si_status
#undef si_status
#endif
#ifdef si_value
#undef si_value
#endif
#ifdef si_band
#undef si_band
#endif

// Layout completo di siginfo_t per Linux ARM32 (128 byte)
struct GuestSigInfo {
    int32_t  si_signo{0};
    int32_t  si_errno{0};
    int32_t  si_code{0};
    uint32_t si_addr{0};
    uint8_t  _pad[128 - 16]{0};
};

#pragma pack(pop)

/**
 * GuestSignalManager: Gestore virtualizzato per segnali POSIX / Linux su architettura ARM32.
 * Gestisce la registrazione degli handler (sigaction/rt_sigaction), maschere di segnali,
 * alternate stack (sigaltstack) e l'assemblaggio di frame ucontext_t su fault hardware
 * (NullReferenceException in C#/Mono, trap di sistema) con ripristino sicuro tramite rt_sigreturn.
 */
class GuestSignalManager {
public:
    static GuestSignalManager& getInstance();

    // Inizializza il sottosistema e registra l'indirizzo del restorer trampoline
    void initialize(uc_engine* uc, uint32_t restorerAddr);

    // Syscall 174 (rt_sigaction) o libc sigaction
    int32_t handleSigAction(uint32_t sig, uint32_t actPtr, uint32_t oldActPtr, size_t sigsetsize, uc_engine* uc);

    // Syscall 175 (rt_sigprocmask) o libc sigprocmask / pthread_sigmask
    int32_t handleSigProcMask(int how, uint32_t setPtr, uint32_t oldSetPtr, size_t sigsetsize, uc_engine* uc);

    // Syscall 186 (sigaltstack) o libc sigaltstack
    int32_t handleSigAltStack(uint32_t ssPtr, uint32_t ossPtr, uc_engine* uc);

    // Syscall 173 (rt_sigreturn): ripristina il contesto dal frame ucontext_t
    int32_t handleSigReturn(uc_engine* uc);

    // libc signal() o bsd_signal()
    uint32_t handleSignal(uint32_t sig, uint32_t handler, uc_engine* uc);

    // Consegna virtualizzata di un segnale guest (SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE)
    bool deliverSignal(uc_engine* uc, int signum, uint32_t faultAddr = 0, int code = 1);

    // Verifica se un segnale dispone di un handler utente personalizzato attivo
    bool hasCustomHandler(int signum) const;

    // Indirizzo del restorer trampoline
    uint32_t getRestorerAddress() const { return restorerAddr_; }

    void reset();

private:
    GuestSignalManager();
    ~GuestSignalManager() = default;

    GuestSignalManager(const GuestSignalManager&) = delete;
    GuestSignalManager& operator=(const GuestSignalManager&) = delete;

    mutable std::mutex mutex_;
    GuestSigAction actions_[GUEST_NSIG + 1]{};
    GuestSigSet procMask_{0, 0};
    GuestStack altStack_{0, GUEST_SS_DISABLE, 0};
    uint32_t restorerAddr_{0};

    struct ActiveFrame {
        uint32_t ucontextAddr{0};
        uint32_t siginfoAddr{0};
        uint32_t savedSp{0};
        GuestSigSet savedMask{0, 0};
        int signum{0};
    };
    std::vector<ActiveFrame> activeFrames_;
};

} // namespace emux
