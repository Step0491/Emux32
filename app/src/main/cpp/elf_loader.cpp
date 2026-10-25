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
        LOGI("Rilasciata memoria ELF: base 0x%lx, size %zu byte", (unsigned long)base_address, total_memory_size);
        base_address = 0;
        total_memory_size = 0;
    }
}

Elf32Loader::Elf32Loader() {
    pageSize_ = getSystemPageSize();
    LOGI("Elf32Loader inizializzato. Page size di sistema: %zu byte", pageSize_);
}

Elf32Loader::~Elf32Loader() = default;

size_t Elf32Loader::getSystemPageSize() const {
    long sz = sysconf(_SC_PAGESIZE);
    return (sz > 0) ? static_cast<size_t>(sz) : 4096;
}

uintptr_t Elf32Loader::alignDown(uintptr_t addr, size_t pageSize) const {
    return addr & ~(static_cast<uintptr_t>(pageSize) - 1);
}

uintptr_t Elf32Loader::alignUp(uintptr_t addr, size_t pageSize) const {
    return (addr + pageSize - 1) & ~(static_cast<uintptr_t>(pageSize) - 1);
}

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
            loadSegments.push_back(seg);
        }
    }

    if (loadSegments.empty()) {
        LOGE("Nessun segmento PT_LOAD trovato nel binario ELF");
        return nullptr;
    }

    uintptr_t alignedMinVaddr = alignDown(minVaddr, pageSize_);
    uintptr_t alignedMaxVaddr = alignUp(maxVaddr, pageSize_);
    size_t totalMemorySize = alignedMaxVaddr - alignedMinVaddr;

    LOGI("ELF Analysis [%s]: minVaddr=0x%x, maxVaddr=0x%x, totalSize=%zu (0x%zx)",
         name.c_str(), minVaddr, maxVaddr, totalMemorySize, totalMemorySize);

    void* reservedAddress = mmap(
        nullptr,
        totalMemorySize,
        PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0
    );

    if (reservedAddress == MAP_FAILED) {
        LOGE("Fallita mmap della regione contigua: %s", strerror(errno));
        return nullptr;
    }

    auto loadedLib = std::make_shared<LoadedLibrary>();
    loadedLib->path = name;
    loadedLib->base_address = reinterpret_cast<uintptr_t>(reservedAddress);
    loadedLib->total_memory_size = totalMemorySize;
    loadedLib->is_pie = (ehdr->e_type == ET_DYN);
    loadedLib->load_bias = loadedLib->is_pie
        ? static_cast<uint32_t>(loadedLib->base_address - alignedMinVaddr)
        : 0;
    loadedLib->entry_point = ehdr->e_entry + (loadedLib->is_pie ? loadedLib->load_bias : 0);
    loadedLib->segments = loadSegments;

    LOGI("Base Address allocato: 0x%lx (Load Bias: 0x%x, Entry: 0x%x)",
         (unsigned long)loadedLib->base_address, loadedLib->load_bias, loadedLib->entry_point);

    for (const auto& seg : loadSegments) {
        uintptr_t segTargetAddr = loadedLib->base_address + (seg.vaddr - alignedMinVaddr);
        uintptr_t pageStart = alignDown(segTargetAddr, pageSize_);
        uintptr_t pageEnd = alignUp(segTargetAddr + seg.memsz, pageSize_);
        size_t pageLen = pageEnd - pageStart;

        if (mprotect(reinterpret_cast<void*>(pageStart), pageLen, PROT_READ | PROT_WRITE) != 0) {
            LOGE("mprotect RW fallita per segmento a 0x%lx: %s", (unsigned long)pageStart, strerror(errno));
            return nullptr;
        }

        if (seg.filesz > 0) {
            if (seg.offset + seg.filesz > bufferSize) {
                LOGE("Offset segmento fuori dai limiti del buffer file!");
                return nullptr;
            }
            std::memcpy(
                reinterpret_cast<void*>(segTargetAddr),
                buffer + seg.offset,
                seg.filesz
            );
        }

        if (seg.memsz > seg.filesz) {
            size_t bssSize = seg.memsz - seg.filesz;
            std::memset(
                reinterpret_cast<void*>(segTargetAddr + seg.filesz),
                0,
                bssSize
            );
        }

        int finalProt = seg.prot;
        if (finalProt & PROT_EXEC) {
            finalProt &= ~PROT_WRITE;
        }

        if (mprotect(reinterpret_cast<void*>(pageStart), pageLen, finalProt) != 0) {
            LOGE("mprotect finale fallita per segmento [prot=0x%x] a 0x%lx: %s",
                 finalProt, (unsigned long)pageStart, strerror(errno));
            return nullptr;
        }

        LOGI("Segmento PT_LOAD caricato: vaddr=0x%x, memsz=%u, prot=0x%x (W^X enforced)",
             seg.vaddr, seg.memsz, finalProt);
    }

    LOGI("Caricamento ELF 32-bit completato con successo per: %s", name.c_str());
    return loadedLib;
}

} // namespace emux
