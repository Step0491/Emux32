#include "elf_loader.h"
#include "linker_hook.h"

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

    if (total_memory_size == 0) {
        LOGE("mapToUnicorn: Dimensione memoria totale ELF pari a 0!");
        return false;
    }

    // Calcolo della regione contigua complessiva con allineamento rigoroso alla pagina di 0x1000 byte
    uint64_t fullLibraryBase = load_bias + aligned_min_vaddr;
    uint64_t fullLibrarySize = PAGE_ALIGN_UP(total_memory_size);

    LOGI("Mappatura contigua Unicorn ELF [%s]: Base=0x%08llx, Size=0x%llx (%zu byte)",
         path.c_str(), (unsigned long long)fullLibraryBase, (unsigned long long)fullLibrarySize, (size_t)fullLibrarySize);

    // Mappatura primaria dell'intero spazio virtuale della libreria in Unicorn
    uc_err mapErr = uc_mem_map(uc, fullLibraryBase, static_cast<size_t>(fullLibrarySize), UC_PROT_ALL);
    if (mapErr != UC_ERR_OK && mapErr != UC_ERR_MAP) {
        LOGE("uc_mem_map fallita per l'intera libreria a 0x%llx: %s (codice: %d)",
             (unsigned long long)fullLibraryBase, uc_strerror(mapErr), mapErr);
        return false;
    }

    // Scrittura dei singoli segmenti PT_LOAD all'interno dello spazio virtuale garantito
    for (const auto& seg : segments) {
        if (seg.type != PT_LOAD || seg.memsz == 0) {
            continue;
        }

        uint64_t targetAddr = load_bias + seg.vaddr;

        LOGI("Scrittura segmento PT_LOAD: Target=0x%08llx, FileSz=%u, MemSz=%u",
             (unsigned long long)targetAddr, seg.filesz, seg.memsz);

        if (seg.filesz > 0 && !raw_binary.empty()) {
            if (seg.offset + seg.filesz <= raw_binary.size()) {
                uc_err writeErr = uc_mem_write(uc, targetAddr, raw_binary.data() + seg.offset, seg.filesz);
                if (writeErr != UC_ERR_OK) {
                    LOGE("uc_mem_write fallita per segmento a 0x%llx: %s",
                         (unsigned long long)targetAddr, uc_strerror(writeErr));
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
            uint64_t bssVaddr = targetAddr + seg.filesz;
            std::vector<uint8_t> zeroes(bssSize, 0);
            uc_mem_write(uc, bssVaddr, zeroes.data(), bssSize);
        }
    }

    LOGI("Tutti i segmenti ELF sono stati mappati e scritti con successo in Unicorn Engine.");
    return true;
}

bool LoadedLibrary::applyRelocations(uc_engine* uc) const {
    if (!uc || raw_binary.size() < sizeof(Elf32_Ehdr)) {
        return false;
    }

    const auto* ehdr = reinterpret_cast<const Elf32_Ehdr*>(raw_binary.data());
    size_t totalRelocsApplied = 0;

    auto vaddrToPtr = [this](uint32_t vaddr) -> const uint8_t* {
        for (const auto& seg : segments) {
            if (vaddr >= seg.vaddr && vaddr < (seg.vaddr + seg.filesz)) {
                size_t off = seg.offset + (vaddr - seg.vaddr);
                if (off < raw_binary.size()) return raw_binary.data() + off;
            }
        }
        return nullptr;
    };

    // 1. TENTATIVO VIA PT_DYNAMIC (Essenziale per binari Android di produzione stripped)
    const Elf32_Phdr* phdrTable = (ehdr->e_phoff + (ehdr->e_phnum * sizeof(Elf32_Phdr)) <= raw_binary.size())
        ? reinterpret_cast<const Elf32_Phdr*>(raw_binary.data() + ehdr->e_phoff)
        : nullptr;

    const Elf32_Phdr* dynPhdr = nullptr;
    if (phdrTable) {
        for (uint16_t i = 0; i < ehdr->e_phnum; ++i) {
            if (phdrTable[i].p_type == PT_DYNAMIC) {
                dynPhdr = &phdrTable[i];
                break;
            }
        }
    }

    if (dynPhdr && (dynPhdr->p_offset + dynPhdr->p_filesz <= raw_binary.size())) {
        const auto* dyn = reinterpret_cast<const Elf32_Dyn*>(raw_binary.data() + dynPhdr->p_offset);
        size_t numDyn = dynPhdr->p_filesz / sizeof(Elf32_Dyn);
        uint32_t relVaddr = 0, relSz = 0, jmpRelVaddr = 0, jmpRelSz = 0;
        uint32_t symtabVaddr = 0, strtabVaddr = 0, strtabSz = 0;

        for (size_t d = 0; d < numDyn && dyn[d].d_tag != DT_NULL; ++d) {
            switch (dyn[d].d_tag) {
                case DT_REL: relVaddr = dyn[d].d_un.d_ptr; break;
                case DT_RELSZ: relSz = dyn[d].d_un.d_val; break;
                case DT_JMPREL: jmpRelVaddr = dyn[d].d_un.d_ptr; break;
                case DT_PLTRELSZ: jmpRelSz = dyn[d].d_un.d_val; break;
                case DT_SYMTAB: symtabVaddr = dyn[d].d_un.d_ptr; break;
                case DT_STRTAB: strtabVaddr = dyn[d].d_un.d_ptr; break;
                case DT_STRSZ: strtabSz = dyn[d].d_un.d_val; break;
            }
        }

        const char* dynstr = strtabVaddr ? reinterpret_cast<const char*>(vaddrToPtr(strtabVaddr)) : nullptr;
        const auto* symtab = symtabVaddr ? reinterpret_cast<const Elf32_Sym*>(vaddrToPtr(symtabVaddr)) : nullptr;

        auto processRelTable = [&](uint32_t tblVaddr, uint32_t tblSz) {
            if (!tblVaddr || !tblSz) return;
            const auto* rels = reinterpret_cast<const Elf32_Rel*>(vaddrToPtr(tblVaddr));
            if (!rels) return;
            size_t count = tblSz / sizeof(Elf32_Rel);

            for (size_t r = 0; r < count; ++r) {
                const auto& rel = rels[r];
                uint32_t type = ELF32_R_TYPE(rel.r_info);
                uint32_t symIdx = ELF32_R_SYM(rel.r_info);
                uint64_t targetAddr = load_bias + rel.r_offset;

                if (type == 23) { // R_ARM_RELATIVE
                    uint32_t addend = 0;
                    uc_mem_read(uc, targetAddr, &addend, sizeof(addend));
                    uint32_t relocated = load_bias + addend;
                    uc_mem_write(uc, targetAddr, &relocated, sizeof(relocated));
                    totalRelocsApplied++;
                } else if (type == 21 || type == 22 || type == 2) { // R_ARM_GLOB_DAT, R_ARM_JUMP_SLOT, R_ARM_ABS32
                    if (symtab && dynstr) {
                        const auto& sym = symtab[symIdx];
                        if (strtabSz == 0 || sym.st_name < strtabSz) {
                            const char* symName = dynstr + sym.st_name;
                            if (symName && symName[0] != '\0') {
                                uint32_t tramp = GuestRuntimeEnvironment::getInstance().registerSymbolTrampoline(symName, uc);
                                uc_mem_write(uc, targetAddr, &tramp, sizeof(tramp));
                                totalRelocsApplied++;
                            }
                        }
                    }
                }
            }
        };

        processRelTable(relVaddr, relSz);
        processRelTable(jmpRelVaddr, jmpRelSz);

        if (totalRelocsApplied > 0) {
            LOGI("applyRelocations (via PT_DYNAMIC) per [%s]: %zu simboli rilocati con successo.",
                 path.c_str(), totalRelocsApplied);
            return true;
        }
    }

    if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0) {
        LOGI("applyRelocations: Nessuna tabella sezioni presente, rilocazioni statiche");
        return true;
    }

    size_t shTableEnd = ehdr->e_shoff + (ehdr->e_shnum * sizeof(Elf32_Shdr));
    if (shTableEnd > raw_binary.size() || ehdr->e_shstrndx >= ehdr->e_shnum) {
        return false;
    }

    const auto* shdrs = reinterpret_cast<const Elf32_Shdr*>(raw_binary.data() + ehdr->e_shoff);

    // Tabella delle stringhe dinamiche (.dynstr)
    const Elf32_Shdr* dynstrShdr = nullptr;
    for (uint16_t s = 0; s < ehdr->e_shnum; ++s) {
        if (shdrs[s].sh_type == SHT_STRTAB && s != ehdr->e_shstrndx) {
            dynstrShdr = &shdrs[s];
            break;
        }
    }

    const char* dynstr = dynstrShdr ? reinterpret_cast<const char*>(raw_binary.data() + dynstrShdr->sh_offset) : nullptr;
    size_t dynstrSize = dynstrShdr ? dynstrShdr->sh_size : 0;

    size_t totalRelocsApplied = 0;

    for (uint16_t s = 0; s < ehdr->e_shnum; ++s) {
        const auto& sh = shdrs[s];
        if (sh.sh_type == SHT_REL) {
            if (sh.sh_offset + sh.sh_size > raw_binary.size() || sh.sh_entsize < sizeof(Elf32_Rel)) {
                continue;
            }

            const auto* rels = reinterpret_cast<const Elf32_Rel*>(raw_binary.data() + sh.sh_offset);
            size_t numRels = sh.sh_size / sizeof(Elf32_Rel);

            const Elf32_Sym* symtab = nullptr;
            size_t numSyms = 0;
            if (sh.sh_link < ehdr->e_shnum) {
                const auto& symSh = shdrs[sh.sh_link];
                if (symSh.sh_offset + symSh.sh_size <= raw_binary.size() && symSh.sh_entsize >= sizeof(Elf32_Sym)) {
                    symtab = reinterpret_cast<const Elf32_Sym*>(raw_binary.data() + symSh.sh_offset);
                    numSyms = symSh.sh_size / sizeof(Elf32_Sym);
                }
            }

            for (size_t r = 0; r < numRels; ++r) {
                const auto& rel = rels[r];
                uint32_t type = ELF32_R_TYPE(rel.r_info);
                uint32_t symIdx = ELF32_R_SYM(rel.r_info);
                uint64_t targetAddr = load_bias + rel.r_offset;

                if (type == 23) { // R_ARM_RELATIVE
                    uint32_t addend = 0;
                    uc_mem_read(uc, targetAddr, &addend, sizeof(addend));
                    uint32_t relocated = load_bias + addend;
                    uc_mem_write(uc, targetAddr, &relocated, sizeof(relocated));
                    totalRelocsApplied++;
                } else if (type == 21 || type == 22 || type == 2) { // R_ARM_GLOB_DAT, R_ARM_JUMP_SLOT, R_ARM_ABS32
                    if (symtab && symIdx < numSyms && dynstr) {
                        const auto& sym = symtab[symIdx];
                        if (sym.st_name < dynstrSize) {
                            const char* symName = dynstr + sym.st_name;
                            if (symName && symName[0] != '\0') {
                                uint32_t tramp = GuestRuntimeEnvironment::getInstance().registerSymbolTrampoline(symName, uc);
                                uc_mem_write(uc, targetAddr, &tramp, sizeof(tramp));
                                totalRelocsApplied++;
                            }
                        }
                    }
                }
            }
        }
    }

    LOGI("Rilocazioni dinamiche applicate con successo per [%s]: %zu simboli rilocati.",
         path.c_str(), totalRelocsApplied);
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

uint32_t Elf32Loader::findJniOnLoadSymbol(const uint8_t* buffer, size_t bufferSize, const Elf32_Ehdr& ehdr) const {
    if (!buffer || bufferSize < sizeof(Elf32_Ehdr)) {
        return 0;
    }

    // 1. TENTATIVO VIA PROGRAM HEADER PT_DYNAMIC (Garantito per binari Android commerciali stripped)
    if (ehdr.e_phoff != 0 && ehdr.e_phnum != 0 && (ehdr.e_phoff + (ehdr.e_phnum * sizeof(Elf32_Phdr)) <= bufferSize)) {
        const auto* phdrTable = reinterpret_cast<const Elf32_Phdr*>(buffer + ehdr.e_phoff);
        const Elf32_Phdr* dynPhdr = nullptr;
        for (uint16_t i = 0; i < ehdr.e_phnum; ++i) {
            if (phdrTable[i].p_type == PT_DYNAMIC) {
                dynPhdr = &phdrTable[i];
                break;
            }
        }

        if (dynPhdr && (dynPhdr->p_offset + dynPhdr->p_filesz <= bufferSize)) {
            const auto* dyn = reinterpret_cast<const Elf32_Dyn*>(buffer + dynPhdr->p_offset);
            size_t numDyn = dynPhdr->p_filesz / sizeof(Elf32_Dyn);
            uint32_t symtabVaddr = 0, strtabVaddr = 0, strtabSz = 0;

            for (size_t d = 0; d < numDyn && dyn[d].d_tag != DT_NULL; ++d) {
                if (dyn[d].d_tag == DT_SYMTAB) symtabVaddr = dyn[d].d_un.d_ptr;
                else if (dyn[d].d_tag == DT_STRTAB) strtabVaddr = dyn[d].d_un.d_ptr;
                else if (dyn[d].d_tag == DT_STRSZ) strtabSz = dyn[d].d_un.d_val;
            }

            auto vaddrToBuf = [&](uint32_t vaddr) -> const uint8_t* {
                for (uint16_t i = 0; i < ehdr.e_phnum; ++i) {
                    const auto& ph = phdrTable[i];
                    if (ph.p_type == PT_LOAD && vaddr >= ph.p_vaddr && vaddr < (ph.p_vaddr + ph.p_filesz)) {
                        size_t off = ph.p_offset + (vaddr - ph.p_vaddr);
                        if (off < bufferSize) return buffer + off;
                    }
                }
                return nullptr;
            };

            const char* dynstr = strtabVaddr ? reinterpret_cast<const char*>(vaddrToBuf(strtabVaddr)) : nullptr;
            const auto* syms = symtabVaddr ? reinterpret_cast<const Elf32_Sym*>(vaddrToBuf(symtabVaddr)) : nullptr;

            if (dynstr && syms) {
                for (size_t i = 0; i < 4096; ++i) {
                    if (strtabSz > 0 && syms[i].st_name >= strtabSz) break;
                    const char* symName = dynstr + syms[i].st_name;
                    if (!symName || symName[0] == '\0') continue;

                    if (std::strcmp(symName, "JNI_OnLoad") == 0) {
                        LOGI("PT_DYNAMIC: Trovato 'JNI_OnLoad' a 0x%08x", syms[i].st_value);
                        return syms[i].st_value;
                    }
                    if (std::strcmp(symName, "ANativeActivity_onCreate") == 0) {
                        LOGI("PT_DYNAMIC: Trovato 'ANativeActivity_onCreate' a 0x%08x", syms[i].st_value);
                        return syms[i].st_value;
                    }
                }
            }
        }
    }

    if (ehdr.e_shoff == 0 || ehdr.e_shnum == 0) {
        return 0;
    }

    size_t shTableEnd = ehdr.e_shoff + (ehdr.e_shnum * sizeof(Elf32_Shdr));
    if (shTableEnd > bufferSize || ehdr.e_shstrndx >= ehdr.e_shnum) {
        return 0;
    }

    const auto* shdrs = reinterpret_cast<const Elf32_Shdr*>(buffer + ehdr.e_shoff);
    const auto& strSection = shdrs[ehdr.e_shstrndx];
    if (strSection.sh_offset + strSection.sh_size > bufferSize) {
        return 0;
    }

    const char* shstrtab = reinterpret_cast<const char*>(buffer + strSection.sh_offset);

    const Elf32_Shdr* dynsymShdr = nullptr;
    const Elf32_Shdr* dynstrShdr = nullptr;

    for (uint16_t s = 0; s < ehdr.e_shnum; ++s) {
        const auto& sh = shdrs[s];
        if (sh.sh_name < strSection.sh_size) {
            const char* name = shstrtab + sh.sh_name;
            if (sh.sh_type == SHT_DYNSYM || std::strcmp(name, ".dynsym") == 0) {
                dynsymShdr = &sh;
            } else if (sh.sh_type == SHT_STRTAB && std::strcmp(name, ".dynstr") == 0) {
                dynstrShdr = &sh;
            }
        }
    }

    if (dynsymShdr && dynstrShdr) {
        if (dynsymShdr->sh_offset + dynsymShdr->sh_size <= bufferSize &&
            dynstrShdr->sh_offset + dynstrShdr->sh_size <= bufferSize &&
            dynsymShdr->sh_entsize >= sizeof(Elf32_Sym)) {

            const auto* syms = reinterpret_cast<const Elf32_Sym*>(buffer + dynsymShdr->sh_offset);
            size_t numSyms = dynsymShdr->sh_size / sizeof(Elf32_Sym);
            const char* strings = reinterpret_cast<const char*>(buffer + dynstrShdr->sh_offset);

            for (size_t i = 0; i < numSyms; ++i) {
                if (syms[i].st_name < dynstrShdr->sh_size) {
                    const char* symName = strings + syms[i].st_name;
                    if (std::strcmp(symName, "JNI_OnLoad") == 0) {
                        LOGI("Simbolo 'JNI_OnLoad' individuato nella tabella simboli a offset: 0x%08x", syms[i].st_value);
                        return syms[i].st_value;
                    }
                }
            }
        }
    }

    return 0;
}

std::shared_ptr<LoadedLibrary> Elf32Loader::loadFromFile(const std::string& filePath) {
    int fd = open(filePath.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        LOGE("Impossibile aprire il file binario: %s (errno=%d: %s)", filePath.c_str(), errno, strerror(errno));
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

    LOGI("File binario letto da disco: %s (%zu byte)", filePath.c_str(), buffer.size());
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
    loadedLib->aligned_min_vaddr = static_cast<uint32_t>(alignedMinVaddr);
    loadedLib->aligned_max_vaddr = static_cast<uint32_t>(alignedMaxVaddr);
    loadedLib->total_memory_size = totalMemorySize;
    loadedLib->is_pie = (ehdr->e_type == ET_DYN);

    // Per librerie condivise (.so) viene impostato un load bias sicuro (0x40000000 = 1GB)
    loadedLib->load_bias = loadedLib->is_pie ? 0x40000000U : 0U;

    // Determinazione del punto di ingresso: e_entry, oppure ricerca del simbolo JNI_OnLoad
    if (ehdr->e_entry != 0) {
        loadedLib->entry_point = loadedLib->load_bias + ehdr->e_entry;
    } else {
        uint32_t jniOnLoad = findJniOnLoadSymbol(buffer, bufferSize, *ehdr);
        if (jniOnLoad != 0) {
            loadedLib->entry_point = loadedLib->load_bias + jniOnLoad;
        } else {
            uint32_t execVaddr = 0;
            for (const auto& seg : loadSegments) {
                if (seg.flags & PF_X) {
                    execVaddr = seg.vaddr;
                    break;
                }
            }
            loadedLib->entry_point = loadedLib->load_bias + (execVaddr != 0 ? execVaddr : loadSegments[0].vaddr);
            LOGW("JNI_OnLoad non trovato in %s: punto di ingresso impostato a segmento eseguibile 0x%08x",
                 name.c_str(), loadedLib->entry_point);
        }
    }

    loadedLib->segments = loadSegments;
    loadedLib->raw_binary.assign(buffer, buffer + bufferSize);

    LOGI("Caricamento ELF completato: %s (Entry Point: 0x%08x, Load Bias: 0x%08x, Segmenti: %zu)",
         name.c_str(), loadedLib->entry_point, loadedLib->load_bias, loadSegments.size());
    return loadedLib;
}

} // namespace emux
