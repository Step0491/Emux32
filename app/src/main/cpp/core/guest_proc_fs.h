#pragma once

#include <string>
#include <mutex>
#include <vector>

namespace emux {

/**
 * GuestProcFs: Sottosistema per la virtualizzazione accurata del filesystem
 * di sistema (/proc e /sys) per l'ambiente ARMv7-A 32-bit su kernel e host ARM64.
 *
 * Previene crash fatali causati da:
 * 1. Stringhe di CPU feature a 64-bit in /proc/cpuinfo (mancanza di neon/vfp).
 * 2. Range esadecimali a 64-bit in /proc/self/maps che causano overflow di sscanf.
 * 3. Strutture Elf64_auxv_t (16 byte) in /proc/self/auxv lette come 32-bit (8 byte).
 */
class GuestProcFs {
public:
    static GuestProcFs& getInstance();

    // Inizializza la directory del procfs virtuale nella sandbox dell'app
    void initialize(const std::string& baseDir, const std::string& packageName);

    // Risolve il percorso richiesto dal guest. Restituisce il percorso virtualizzato sintetico se intercettato, altrimenti vuoto ""
    std::string resolveProcPath(const std::string& guestPath);

    // Aggiorna dinamicamente la mappa di memoria guest in /proc/self/maps
    void refreshMapsFile();

private:
    GuestProcFs() = default;
    ~GuestProcFs() = default;

    GuestProcFs(const GuestProcFs&) = delete;
    GuestProcFs& operator=(const GuestProcFs&) = delete;

    void ensureCpuinfoCreated();
    void ensureAuxvCreated();
    void ensureVersionCreated();
    void ensureSysCpuCreated();
    void ensureCmdlineCreated();

    std::string baseDir_;
    std::string procDir_;
    std::string packageName_{"com.emux.guest"};
    std::mutex procMutex_;
    bool initialized_{false};
};

} // namespace emux
