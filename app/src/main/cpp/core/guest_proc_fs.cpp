#include "core/guest_proc_fs.h"

#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <android/log.h>

#define LOG_TAG "EmuxGuestProcFs"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace emux {

namespace {
static void makeDirRecursive(const std::string& path) {
    size_t pos = 1;
    while ((pos = path.find('/', pos)) != std::string::npos) {
        std::string sub = path.substr(0, pos);
        mkdir(sub.c_str(), 0775);
        pos++;
    }
    mkdir(path.c_str(), 0775);
}
} // namespace

GuestProcFs& GuestProcFs::getInstance() {
    static GuestProcFs instance;
    return instance;
}

void GuestProcFs::initialize(const std::string& baseDir, const std::string& packageName) {
    std::lock_guard<std::mutex> lock(procMutex_);
    baseDir_ = baseDir.empty() ? "/sdcard/Emux32" : baseDir;
    if (!packageName.empty()) packageName_ = packageName;

    procDir_ = baseDir_ + "/guest_proc";
    makeDirRecursive(procDir_);

    ensureCpuinfoCreated();
    ensureAuxvCreated();
    ensureVersionCreated();
    ensureSysCpuCreated();
    ensureCmdlineCreated();
    refreshMapsFile();

    initialized_ = true;
    LOGI("GuestProcFs: Inizializzato con successo in '%s' per pacchetto '%s'",
         procDir_.c_str(), packageName_.c_str());
}

void GuestProcFs::ensureCpuinfoCreated() {
    std::string path = procDir_ + "/cpuinfo";
    FILE* fp = fopen(path.c_str(), "w");
    if (!fp) {
        LOGE("GuestProcFs: Impossibile creare cpuinfo sintetico: %s", path.c_str());
        return;
    }

    const char* cpuinfoContent =
        "Processor\t: ARMv7 Processor rev 4 (v7l)\n"
        "processor\t: 0\n"
        "BogoMIPS\t: 38.40\n"
        "Features\t: half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt\n"
        "CPU implementer\t: 0x51\n"
        "CPU architecture: 7\n"
        "CPU variant\t: 0x2\n"
        "CPU part\t: 0x06f\n"
        "CPU revision\t: 4\n\n"
        "processor\t: 1\n"
        "BogoMIPS\t: 38.40\n"
        "Features\t: half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt\n"
        "CPU implementer\t: 0x51\n"
        "CPU architecture: 7\n"
        "CPU variant\t: 0x2\n"
        "CPU part\t: 0x06f\n"
        "CPU revision\t: 4\n\n"
        "processor\t: 2\n"
        "BogoMIPS\t: 38.40\n"
        "Features\t: half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt\n"
        "CPU implementer\t: 0x51\n"
        "CPU architecture: 7\n"
        "CPU variant\t: 0x2\n"
        "CPU part\t: 0x06f\n"
        "CPU revision\t: 4\n\n"
        "processor\t: 3\n"
        "BogoMIPS\t: 38.40\n"
        "Features\t: half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt\n"
        "CPU implementer\t: 0x51\n"
        "CPU architecture: 7\n"
        "CPU variant\t: 0x2\n"
        "CPU part\t: 0x06f\n"
        "CPU revision\t: 4\n\n"
        "Hardware\t: Qualcomm Technologies, Inc MSM8996\n"
        "Revision\t: 0000\n"
        "Serial\t\t: 0000000000000000\n";

    fputs(cpuinfoContent, fp);
    fclose(fp);
}

void GuestProcFs::ensureAuxvCreated() {
    std::string path = procDir_ + "/auxv";
    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp) return;

    struct Elf32AuxvEntry {
        uint32_t a_type;
        uint32_t a_val;
    };

    std::vector<Elf32AuxvEntry> auxv = {
        { 6,  4096 },                         // AT_PAGESZ
        { 16, 0x000F7F96U },                  // AT_HWCAP (VFP, NEON, VFPv3, VFPv4, IDIVA, IDIVT, EDSP, THUMB, HALF, FAST_MULT, TLS)
        { 26, 0x0000001FU },                  // AT_HWCAP2 (AES, PMULL, SHA1, SHA2, CRC32)
        { 17, 100 },                          // AT_CLKTCK
        { 3,  0x40000034U },                  // AT_PHDR
        { 4,  32 },                           // AT_PHENT
        { 5,  8 },                            // AT_PHNUM
        { 7,  0x40000000U },                  // AT_BASE
        { 8,  0 },                            // AT_FLAGS
        { 9,  0x40001000U },                  // AT_ENTRY
        { 11, static_cast<uint32_t>(getuid()) },
        { 12, static_cast<uint32_t>(geteuid()) },
        { 13, static_cast<uint32_t>(getgid()) },
        { 14, static_cast<uint32_t>(getegid()) },
        { 23, 0 },                            // AT_SECURE
        { 25, 0x7D021000U },                  // AT_RANDOM
        { 0,  0 }                             // AT_NULL terminator
    };

    fwrite(auxv.data(), sizeof(Elf32AuxvEntry), auxv.size(), fp);
    fclose(fp);
}

void GuestProcFs::ensureVersionCreated() {
    std::string path = procDir_ + "/version";
    FILE* fp = fopen(path.c_str(), "w");
    if (!fp) return;
    fputs("Linux version 4.19.0-emux32-armv7 (android-build@google) (Android clang version 12.0.8) #1 SMP PREEMPT 2026\n", fp);
    fclose(fp);
}

void GuestProcFs::ensureSysCpuCreated() {
    FILE* fp1 = fopen((procDir_ + "/sys_cpu_online").c_str(), "w");
    if (fp1) { fputs("0-3\n", fp1); fclose(fp1); }

    FILE* fp2 = fopen((procDir_ + "/sys_cpu_present").c_str(), "w");
    if (fp2) { fputs("0-3\n", fp2); fclose(fp2); }

    FILE* fp3 = fopen((procDir_ + "/cpuinfo_max_freq").c_str(), "w");
    if (fp3) { fputs("2800000\n", fp3); fclose(fp3); }
}

void GuestProcFs::ensureCmdlineCreated() {
    std::string path = procDir_ + "/cmdline";
    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp) return;
    fwrite(packageName_.c_str(), 1, packageName_.size() + 1, fp);
    fclose(fp);
}

void GuestProcFs::refreshMapsFile() {
    std::string path = procDir_ + "/maps";
    FILE* fp = fopen(path.c_str(), "w");
    if (!fp) return;

    // Genera mapping sintetico a 32-bit conformante (8 cifre hex) per prevenire crash nei parser guest
    fprintf(fp, "40000000-58000000 r-xp 00000000 00:00 0                          /data/app/%s/libmain.so\n", packageName_.c_str());
    fprintf(fp, "58000000-70000000 rwxp 00000000 00:00 0                          [heap]\n");
    fprintf(fp, "70000000-70100000 r-xp 00000000 00:00 0                          [trampolines]\n");
    fprintf(fp, "7a000000-7c000000 rw-p 00000000 00:00 0                          [worker_stacks]\n");
    fprintf(fp, "7d000000-7d030000 rwxp 00000000 00:00 0                          [bionic_tls_jni]\n");
    fprintf(fp, "7e000000-7e200000 rw-p 00000000 00:00 0                          [stack]\n");
    fprintf(fp, "80000000-c0000000 rwxp 00000000 00:00 0                          [anon:mmap]\n");
    fprintf(fp, "ffff0000-ffff1000 r-xp 00000000 00:00 0                          [vectors]\n");
    fclose(fp);
}

std::string GuestProcFs::resolveProcPath(const std::string& guestPath) {
    if (guestPath.empty()) return "";

    if (!initialized_) {
        initialize("/data/data/com.antigravity.emux32/files", "com.antigravity.emux32");
    }

    if (guestPath == "/proc/cpuinfo") {
        return procDir_ + "/cpuinfo";
    }
    if (guestPath == "/proc/version") {
        return procDir_ + "/version";
    }
    if (guestPath == "/proc/self/maps" || guestPath == "/proc/maps") {
        refreshMapsFile();
        return procDir_ + "/maps";
    }
    if (guestPath == "/proc/self/auxv" || guestPath == "/proc/auxv") {
        return procDir_ + "/auxv";
    }
    if (guestPath == "/proc/self/cmdline" || guestPath == "/proc/cmdline") {
        return procDir_ + "/cmdline";
    }
    if (guestPath == "/sys/devices/system/cpu/online") {
        return procDir_ + "/sys_cpu_online";
    }
    if (guestPath == "/sys/devices/system/cpu/present") {
        return procDir_ + "/sys_cpu_present";
    }
    if (guestPath.rfind("/sys/devices/system/cpu/", 0) == 0 && guestPath.find("cpuinfo_max_freq") != std::string::npos) {
        return procDir_ + "/cpuinfo_max_freq";
    }

    // Intercettazione generica /proc/<tid>/ o /proc/<pid>/
    if (guestPath.rfind("/proc/", 0) == 0) {
        size_t nextSlash = guestPath.find('/', 6);
        if (nextSlash != std::string::npos) {
            std::string sub = guestPath.substr(nextSlash + 1);
            if (sub == "maps") {
                refreshMapsFile();
                return procDir_ + "/maps";
            }
            if (sub == "auxv") {
                return procDir_ + "/auxv";
            }
            if (sub == "cmdline") {
                return procDir_ + "/cmdline";
            }
        }
    }

    return "";
}

} // namespace emux
