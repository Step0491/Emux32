#include <jni.h>
#include <android/log.h>
#include <unicorn/unicorn.h>

#include <elf.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstdint>
#include <cstring>
#include <vector>

#define TAG "EmuxNativeLib"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ============================================================================
// DIRETTIVA 2: Macro di allineamento della memoria (Pagina di sistema 4KB)
// ============================================================================
#define PAGE_SIZE 0x1000
#define ALIGN_DOWN(addr, size) ((addr) & ~((size) - 1))
#define ALIGN_UP(addr, size) (((addr) + (size) - 1) & ~((size) - 1))

// ============================================================================
// DIRETTIVA 3: Gestione Interrupt/Syscall Bionic ARM32 (SVC #0)
// ============================================================================
static void hook_intr(uc_engine *uc, uint32_t intno, void *user_data) {
    if (intno == 2) { // 2 = Software Interrupt / Supervisor Call (SVC) su ARM32
        uint32_t r7 = 0; // Syscall Number
        uint32_t r0 = 0; // Primo argomento / Valore di ritorno
        uint32_t pc = 0;

        uc_reg_read(uc, UC_ARM_REG_R7, &r7);
        uc_reg_read(uc, UC_ARM_REG_R0, &r0);
        uc_reg_read(uc, UC_ARM_REG_PC, &pc);

        LOGI("SVC intercettata a PC=0x%08X: Syscall #%u, R0=0x%08X", pc, r7, r0);

        // Gestione base per evitare SIGSEGV o SIGABRT su chiamate standard Bionic
        uint32_t ret_val = 0;
        switch (r7) {
            case 1:  // __NR_exit
                LOGI("Syscall __NR_exit: terminazione controllata emulatore");
                uc_emu_stop(uc);
                return;
            case 20: // __NR_getpid
                ret_val = 10001;
                break;
            case 125: // __NR_mprotect
            case 192: // __NR_mmap2
                ret_val = 0; // Successo
                break;
            default:
                LOGW("Syscall #%u non implementata - fallback stub", r7);
                ret_val = 0;
                break;
        }

        uc_reg_write(uc, UC_ARM_REG_R0, &ret_val);
    } else {
        LOGW("Interrupt non gestito: intno=%u", intno);
    }
}

// Converte flag ELF (PF_R, PF_W, PF_X) in flag di protezione Unicorn
static uint32_t elf_flags_to_uc_prot(uint32_t p_flags) {
    uint32_t prot = UC_PROT_NONE;
    if (p_flags & PF_R) prot |= UC_PROT_READ;
    if (p_flags & PF_W) prot |= UC_PROT_WRITE;
    if (p_flags & PF_X) prot |= UC_PROT_EXEC;
    return (prot == UC_PROT_NONE) ? (UC_PROT_READ | UC_PROT_WRITE) : prot;
}

// Esecuzione protetta con verifica di ogni uc_err senza crash dell'host
static bool execute_elf_unicorn(const char* binary_path) {
    if (!binary_path) {
        LOGE("Percorso binario nullo");
        return false;
    }

    int fd = open(binary_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        LOGE("Impossibile aprire binario: %s", binary_path);
        return false;
    }

    struct stat st{};
    if (fstat(fd, &st) < 0 || st.st_size < static_cast<off_t>(sizeof(Elf32_Ehdr))) {
        LOGE("Dimensione ELF non valida: %s", binary_path);
        close(fd);
        return false;
    }

    std::vector<uint8_t> buffer(st.st_size);
    ssize_t bytesRead = read(fd, buffer.data(), buffer.size());
    close(fd);

    if (bytesRead != static_cast<ssize_t>(buffer.size())) {
        LOGE("Lettura incompleta binario ELF");
        return false;
    }

    const auto* ehdr = reinterpret_cast<const Elf32_Ehdr*>(buffer.data());
    if (std::memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        LOGE("Magic number ELF non valido");
        return false;
    }

    if (ehdr->e_ident[EI_CLASS] != ELFCLASS32 || ehdr->e_machine != EM_ARM) {
        LOGE("Target ELF non a 32-bit ARM!");
        return false;
    }

    // Creazione motore Unicorn
    uc_engine *uc = nullptr;
    uc_err err = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc);
    if (err != UC_ERR_OK) {
        LOGE("uc_open fallita: %u (%s)", err, uc_strerror(err));
        return false;
    }

    // DIRETTIVA 2: Mappatura segmenti PT_LOAD con allineamento 0x1000
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        LOGE("Nessun Program Header");
        uc_close(uc);
        return false;
    }

    const auto* phdr_table = reinterpret_cast<const Elf32_Phdr*>(buffer.data() + ehdr->e_phoff);

    for (uint16_t i = 0; i < ehdr->e_phnum; ++i) {
        const Elf32_Phdr& ph = phdr_table[i];
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;

        uint32_t aligned_base = ALIGN_DOWN(ph.p_vaddr, PAGE_SIZE);
        uint32_t aligned_end  = ALIGN_UP(ph.p_vaddr + ph.p_memsz, PAGE_SIZE);
        uint32_t aligned_size = aligned_end - aligned_base;
        uint32_t prot         = elf_flags_to_uc_prot(ph.p_flags);

        LOGI("PT_LOAD [%u]: Mappatura 0x%08X (size 0x%08X, prot 0x%X)",
             i, aligned_base, aligned_size, prot);

        err = uc_mem_map(uc, aligned_base, aligned_size, prot);
        if (err != UC_ERR_OK) {
            LOGE("uc_mem_map fallita su 0x%08X: %u (%s)", aligned_base, err, uc_strerror(err));
            uc_close(uc);
            return false;
        }

        if (ph.p_filesz > 0 && (ph.p_offset + ph.p_filesz <= buffer.size())) {
            err = uc_mem_write(uc, ph.p_vaddr, buffer.data() + ph.p_offset, ph.p_filesz);
            if (err != UC_ERR_OK) {
                LOGE("uc_mem_write fallita su 0x%08X: %u (%s)", ph.p_vaddr, err, uc_strerror(err));
                uc_close(uc);
                return false;
            }
        }
    }

    // DIRETTIVA 4: Setup Stack Dedicato (2MB) e Contesto CPU
    const uint32_t STACK_BASE = 0x80000000;
    const uint32_t STACK_SIZE = 2 * 1024 * 1024; // 2MB
    const uint32_t STACK_TOP  = STACK_BASE + STACK_SIZE;

    err = uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_READ | UC_PROT_WRITE);
    if (err != UC_ERR_OK) {
        LOGE("uc_mem_map Stack fallita: %u (%s)", err, uc_strerror(err));
        uc_close(uc);
        return false;
    }

    uint32_t sp = (STACK_TOP - 8) & ~7U;
    err = uc_reg_write(uc, UC_ARM_REG_SP, &sp);
    if (err != UC_ERR_OK) {
        LOGE("uc_reg_write SP fallita: %u (%s)", err, uc_strerror(err));
        uc_close(uc);
        return false;
    }

    uint32_t entry_point = ehdr->e_entry;
    err = uc_reg_write(uc, UC_ARM_REG_PC, &entry_point);
    if (err != UC_ERR_OK) {
        LOGE("uc_reg_write PC fallita: %u (%s)", err, uc_strerror(err));
        uc_close(uc);
        return false;
    }

    // DIRETTIVA 3: Registrazione UC_HOOK_INTR
    uc_hook hh;
    err = uc_hook_add(uc, &hh, UC_HOOK_INTR, reinterpret_cast<void*>(hook_intr), nullptr, 1, 0);
    if (err != UC_ERR_OK) {
        LOGW("uc_hook_add fallita: %u (%s)", err, uc_strerror(err));
    }

    // Avvio emulazione con gestione errori sicura
    LOGI("Avvio uc_emu_start da PC=0x%08X...", entry_point);
    err = uc_emu_start(uc, entry_point, 0, 0, 0);
    if (err != UC_ERR_OK) {
        LOGE("uc_emu_start interrotta: %u (%s)", err, uc_strerror(err));
        uc_close(uc);
        return false;
    }

    LOGI("Emulazione guest completata regolarmente.");
    uc_close(uc);
    return true;
}

// ============================================================================
// Interfaccia JNI Esportata (Compatibile sia con NativeEngine che MainActivity)
// ============================================================================
extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_emux_translator_nativebridge_NativeEngine_startEmulationNative(
    JNIEnv* env,
    jobject /* thiz */,
    jstring binaryPath
) {
    if (!binaryPath) return JNI_FALSE;
    const char* path = env->GetStringUTFChars(binaryPath, nullptr);
    if (!path) return JNI_FALSE;

    bool res = execute_elf_unicorn(path);
    env->ReleaseStringUTFChars(binaryPath, path);
    return res ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_emux_translator_MainActivity_startEmulationNative(
    JNIEnv* env,
    jobject thiz,
    jstring binaryPath
) {
    return Java_com_emux_translator_nativebridge_NativeEngine_startEmulationNative(env, thiz, binaryPath);
}

} // extern "C"
