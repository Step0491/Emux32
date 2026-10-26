#include "elf_loader.h"

#include <android/log.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>
#include <algorithm>
#include <stdexcept>

#define TAG "EmuxElfLoader"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace emux {

LoadedLibrary::~LoadedLibrary() {
    if (base_address != 0 && total_memory_size > 0) {
        munmap(reinterpret_cast<void*>(base_address), total_memory_size);
        LOGI("Rilasciata memoria ELF host: base 0x%lx, size %zu byte", (unsigned long)base_address, total_memory_size);
        base_address = 0;
        total_memory_size = 0;
    }
}

bool LoadedLibrary::mapToUnicorn(uc_engine* uc) const {
    if (!uc) {
        LOGE("mapToUnicorn: Istanza uc_engine nulla!");
        return false;
    }

    LOGI("Avvio mapping dei segmenti ELF in Unicorn Engine (Totale segmenti: %zu)...", segments.size());

    for (const auto& seg : segments) {
        if (seg.type != PT_LOAD || seg.memsz == 0) {
            continue;
        }

        // Applicazione rigorosa delle macro di allineamento alla pagina di 0x1000 byte
        uint64_t alignedBase = PAGE_ALIGN_DOWN(seg.vaddr);
        uint64_t alignedEnd = PAGE_ALIGN_UP(seg.vaddr + seg.memsz);
        size_t alignedSize = static_cast<size_t>(alignedEnd - alignedBase);

        LOGI("Mapping PT_LOAD -> Aligned Guest Addr: 0x%08llx, Size: 0x%zx (%zu byte), Perms: 0x%x",
             (unsigned long long)alignedBase, alignedSize, alignedSize, seg.uc_prot);

        uc_err mapErr = uc_mem_map(uc, alignedBase, alignedSize, seg.uc_prot);
        if (mapErr != UC_ERR_OK && mapErr != UC_ERR_MAP) {
            LOGE("uc_mem_map fallita per segmento a 0x%llx: %s (codice: %d)",
                 (unsigned long long)alignedBase, uc_strerror(mapErr), mapErr);
            return false;
        }

        // Scrittura dei byte del segmento dall'immagine binaria
        if (seg.filesz > 0 && !raw_binary.empty()) {
            if (seg.offset + seg.filesz <= raw_binary.size()) {
                uc_err writeErr = uc_mem_write(uc, seg.vaddr, raw_binary.data() + seg.offset, seg.filesz);
                if (writeErr != UC_ERR_OK) {
                    LOGE("uc_mem_write fallita per segmento a 0x%x: %s", seg.vaddr, uc_strerror(writeErr));
                    return false;
                }
            } else {
                LOGE("Offset segmento (0x%x + 0x%x) supera la dimensione del binario (%zu)",
                     seg.offset, seg.filesz, raw_binary.size());
                return false;
            }
        }

        // Inizializzazione a zero della sezione BSS (.bss: memsz > filesz)
        if (seg.memsz > seg.filesz) {
            size_t bssSize = seg.memsz - seg.filesz;
            uint32_t bssVaddr = seg.vaddr + seg.filesz;
            std::vector<uint8_t> zeroes(bssSize, 0);
            uc_mem_write(uc, bssVaddr, zeroes.data(), bssSize);
        }
    }

    LOGI("Tutti i segmenti ELF sono stati mappati con successo in Unicorn Engine con allineamento 0x1000.");
    return true;
}

Elf32Loader::Elf32Loader() {
    LOGI("Elf32Loader inizializzato con PAGE_SIZE=0x%x (%d byte)", PAGE_SIZE, PAGE_SIZE);
}

Elf32Loader::~Elf32Loader() = default;

bool Elf32Loader::validateHeader(const Elf32_Ehdr& header) const {
    if (header.e_ident[EI_MAG0] != ELFMAG0 ||
        header.e_ident[EI_MAG1] != ELFMAG1 ||
        header.e_ident[EI_MAG2] != ELFMAG2 ||
        header.e_ident[EI_MAG3] != ELFMAG3) {
        LOGE("Magic number ELF non valido!");
        return false;
    }

    if (header.e_ident[EI_CLASS] != ELFCLASS32) {
        LOGE("Target ELF non a 32-bit (trovato EI_CLASS=%d)", header.e_ident[EI_CLASS]);
        return false;
    }

    if (header.e_ident[EI_DATA] != ELFDATA2LSB) {
        LOGE("Endianness non supportata (trovato EI_DATA=%d)", header.e_ident[EI_DATA]);
        return false;
    }

    if (header.e_ident[EI_VERSION] != EV_CURRENT) {
        LOGE("Versione ELF non valida (trovato EI_VERSION=%d)", header.e_ident[EI_VERSION]);
        return false;
    }

    if (header.e_machine != EM_ARM) {
        LOGE("Architettura target errata: atteso EM_ARM (40), trovato %d", header.e_machine);
        return false;
    }

    if (header.e_type != ET_DYN && header.e_type != ET_EXEC) {
        LOGE("Tipo file ELF non supportato: e_type=%d", header.e_type);
        return false;
    }

    return true;
}

int Elf32Loader::convertElfFlagsToMmapProt(uint32_t p_flags) const {
    int prot = PROT_NONE;
    if (p_flags & PF_R) prot |= PROT_READ;
    if (p_flags & PF_W) prot |= PROT_WRITE;
    if (p_flags & PF_X) prot |= PROT_EXEC;
    return prot;
}

uint32_t Elf32Loader::convertElfFlagsToUcProt(uint32_t p_flags) const {
    uint32_t prot = UC_PROT_NONE;
    if (p_flags & PF_R) prot |= UC_PROT_READ;
    if (p_flags & PF_W) prot |= UC_PROT_WRITE;
    if (p_flags & PF_X) prot |= UC_PROT_EXEC;
    if (prot == UC_PROT_NONE) prot = UC_PROT_READ | UC_PROT_WRITE;
    return prot;
}

std::shared_ptr<LoadedLibrary> Elf32Loader::loadFromFile(const std::string& filePath) {
    int fd = open(filePath.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        LOGE("Impossibile aprire il file binario: %s", filePath.c_str());
        return nullptr;
    }

    off_t fileSize = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    if (fileSize <= static_cast<off_t>(sizeof(Elf32_Ehdr))) {
        LOGE("File troppo piccolo per contenere un header ELF: %ld byte", (long)fileSize);
        close(fd);
        return nullptr;
    }

    std::vector<uint8_t> buffer(static_cast<size_t>(fileSize));
    ssize_t bytesRead = read(fd, buffer.data(), buffer.size());
    close(fd);

    if (bytesRead != static_cast<ssize_t>(buffer.size())) {
        LOGE("Errore nella lettura completa del file binario");
        return nullptr;
    }

    return loadFromMemory(buffer.data(), buffer.size(), filePath);
}

std::shared_ptr<LoadedLibrary> Elf32Loader::loadFromMemory(
    const uint8_t* buffer,
    size_t bufferSize,
    const std::string& name) {

    if (!buffer || bufferSize < sizeof(Elf32_Ehdr)) {
        LOGE("Buffer non valido o dimensione insufficiente");
        return nullptr;
    }

    const auto* ehdr = reinterpret_cast<const Elf32_Ehdr*>(buffer);
    if (!validateHeader(*ehdr)) {
        return nullptr;
    }

    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        LOGE("Nessun Program Header presente nel binario ELF");
        return nullptr;
    }

    size_t phTableEnd = ehdr->e_phoff + (ehdr->e_phnum * sizeof(Elf32_Phdr));
    if (phTableEnd > bufferSize) {
        LOGE("Program Header Table fuori dai limiti del file!");
        return nullptr;
    }

    const auto* phdrTable = reinterpret_cast<const Elf32_Phdr*>(buffer + ehdr->e_phoff);

    uint32_t minVaddr = 0xFFFFFFFF;
    uint32_t maxVaddr = 0x00000000;
    std::vector<ElfSegment> loadSegments;

    for (uint16_t i = 0; i < ehdr->e_phnum; ++i) {
        const Elf32_Phdr& ph = phdrTable[i];
        if (ph.p_type == PT_LOAD) {
            if (ph.p_memsz == 0) {
                continue;
            }

            if (ph.p_vaddr < minVaddr) {
                minVaddr = ph.p_vaddr;
            }
            if (ph.p_vaddr + ph.p_memsz > maxVaddr) {
                maxVaddr = ph.p_vaddr + ph.p_memsz;
            }

            ElfSegment seg{};
            seg.type = ph.p_type;
            seg.flags = ph.p_flags;
            seg.offset = ph.p_offset;
            seg.vaddr = ph.p_vaddr;
            seg.filesz = ph.p_filesz;
            seg.memsz = ph.p_memsz;
            seg.align = ph.p_align;
            seg.prot = convertElfFlagsToMmapProt(ph.p_flags);
            seg.uc_prot = convertElfFlagsToUcProt(ph.p_flags);
            loadSegments.push_back(seg);
        }
    }

    if (loadSegments.empty()) {
        LOGE("Nessun segmento PT_LOAD trovato nel binario ELF");
        return nullptr;
    }

    // Calcolo allineamento memoria conforme a PAGE_ALIGN_DOWN / PAGE_ALIGN_UP (0x1000)
    uintptr_t alignedMinVaddr = PAGE_ALIGN_DOWN(minVaddr);
    uintptr_t alignedMaxVaddr = PAGE_ALIGN_UP(maxVaddr);
    size_t totalMemorySize = alignedMaxVaddr - alignedMinVaddr;

    LOGI("ELF Analysis [%s]: minVaddr=0x%x, maxVaddr=0x%x, totalSize=%zu (0x%zx)",
         name.c_str(), minVaddr, maxVaddr, totalMemorySize, totalMemorySize);

    auto loadedLib = std::make_shared<LoadedLibrary>();
    loadedLib->path = name;
    loadedLib->total_memory_size = totalMemorySize;
    loadedLib->is_pie = (ehdr->e_type == ET_DYN);
    loadedLib->load_bias = 0;
    loadedLib->entry_point = ehdr->e_entry;
    loadedLib->segments = loadSegments;
    loadedLib->raw_binary.assign(buffer, buffer + bufferSize);

    LOGI("Caricamento ELF completato: %s (Entry Point: 0x%08x, Segmenti: %zu)",
         name.c_str(), loadedLib->entry_point, loadSegments.size());
    return loadedLib;
}

} // namespace emux
