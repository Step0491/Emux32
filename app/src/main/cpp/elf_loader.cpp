#include "elf_loader.h"
#include "linker_hook.h"
#include "unicorn/arm.h"

#include <android/log.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>
#include <algorithm>
#include <atomic>

#define TAG "EmuxElfLoader"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace emux {

static std::atomic<uint32_t> sNextLoadBias{0x40000000U};

bool LoadedLibrary::mapToUnicorn(uc_engine* uc) const {
    if (!uc || total_memory_size == 0) {
        LOGE("mapToUnicorn: Parametri non validi");
        return false;
    }

    uint64_t fullLibraryBase = load_bias + aligned_min_vaddr;
    uint64_t fullLibrarySize = PAGE_ALIGN_UP(total_memory_size);

    LOGI("Mappatura contigua Unicorn ELF [%s]: Base=0x%08llx, Size=0x%llx (%zu byte)",
         path.c_str(), (unsigned long long)fullLibraryBase, (unsigned long long)fullLibrarySize, (size_t)fullLibrarySize);

    uc_err mapErr = uc_mem_map(uc, fullLibraryBase, static_cast<size_t>(fullLibrarySize), UC_PROT_ALL);
    if (mapErr != UC_ERR_OK && mapErr != UC_ERR_MAP) {
        LOGE("uc_mem_map fallita per libreria a 0x%llx: %s", (unsigned long long)fullLibraryBase, uc_strerror(mapErr));
        return false;
    }

    for (const auto& seg : segments) {
        if (seg.type != PT_LOAD || seg.memsz == 0) continue;

        uint64_t targetAddr = load_bias + seg.vaddr;
        if (seg.filesz > 0 && !raw_binary.empty()) {
            if (seg.offset + seg.filesz <= raw_binary.size()) {
                uc_err writeErr = uc_mem_write(uc, targetAddr, raw_binary.data() + seg.offset, seg.filesz);
                if (writeErr != UC_ERR_OK) {
                    LOGE("uc_mem_write fallita per segmento a 0x%llx: %s", (unsigned long long)targetAddr, uc_strerror(writeErr));
                    return false;
                }
            } else {
                LOGE("Offset segmento fuori dai limiti del binario");
                return false;
            }
        }

        if (seg.memsz > seg.filesz) {
            size_t bssSize = seg.memsz - seg.filesz;
            uint64_t bssVaddr = targetAddr + seg.filesz;
            std::vector<uint8_t> zeroes(bssSize, 0);
            uc_mem_write(uc, bssVaddr, zeroes.data(), bssSize);
        }
    }

    LOGI("Segmenti ELF mappati con successo in Unicorn Engine per [%s].", path.c_str());
    return true;
}

bool LoadedLibrary::applyRelocations(uc_engine* uc) const {
    if (!uc || raw_binary.size() < sizeof(Elf32_Ehdr)) {
        return false;
    }

    const auto* ehdr = reinterpret_cast<const Elf32_Ehdr*>(raw_binary.data());
    size_t totalRelocsApplied = 0;
    auto& env = GuestRuntimeEnvironment::getInstance();

    auto vaddrToPtr = [this](uint32_t vaddr) -> const uint8_t* {
        for (const auto& seg : segments) {
            if (vaddr >= seg.vaddr && vaddr < (seg.vaddr + seg.filesz)) {
                size_t off = seg.offset + (vaddr - seg.vaddr);
                if (off < raw_binary.size()) return raw_binary.data() + off;
            }
        }
        return nullptr;
    };

    auto resolveAndWriteSymbol = [&](uint32_t type, uint32_t symIdx, uint64_t targetAddr,
                                     const Elf32_Sym* symtab, size_t maxSyms,
                                     const char* strtab, size_t strtabSz) {
        if (type == 23) { // R_ARM_RELATIVE
            uint32_t addend = 0;
            uc_mem_read(uc, targetAddr, &addend, sizeof(addend));
            uint32_t relocated = load_bias + addend;
            uc_mem_write(uc, targetAddr, &relocated, sizeof(relocated));
            totalRelocsApplied++;
            return;
        }

        if (type != 21 && type != 22 && type != 2 && type != 3 && type != 20) {
            return;
        }

        uint32_t resolvedAddr = 0;
        if (symtab && (maxSyms == 0 || symIdx < maxSyms)) {
            const auto& sym = symtab[symIdx];
            if (sym.st_shndx != 0 && sym.st_value != 0) {
                resolvedAddr = load_bias + sym.st_value;
            } else if (strtab && (strtabSz == 0 || sym.st_name < strtabSz)) {
                const char* symName = strtab + sym.st_name;
                if (symName && symName[0] != '\0') {
                    resolvedAddr = env.resolveOrRegisterSymbol(symName, uc);
                }
            }
        }

        // Garanzia Zero-NULL: nessuno slot GOT/PLT rilocato puo' mai restare a 0x00000000
        if (resolvedAddr == 0) {
            std::string fallbackName = "__unresolved_sym_" + std::to_string(symIdx);
            resolvedAddr = env.registerSymbolTrampoline(fallbackName, uc);
        }

        if (type == 2) { // R_ARM_ABS32
            uint32_t addend = 0;
            uc_mem_read(uc, targetAddr, &addend, sizeof(addend));
            uint32_t relocated = resolvedAddr + addend;
            uc_mem_write(uc, targetAddr, &relocated, sizeof(relocated));
        } else if (type == 3) { // R_ARM_REL32
            uint32_t addend = 0;
            uc_mem_read(uc, targetAddr, &addend, sizeof(addend));
            uint32_t relocated = resolvedAddr + addend - static_cast<uint32_t>(targetAddr);
            uc_mem_write(uc, targetAddr, &relocated, sizeof(relocated));
        } else if (type != 20) { // R_ARM_GLOB_DAT (21), R_ARM_JUMP_SLOT (22)
            uc_mem_write(uc, targetAddr, &resolvedAddr, sizeof(resolvedAddr));
        }
        totalRelocsApplied++;
    };

    // 1. Rilocazione via PT_DYNAMIC (standard per binari Android commerciali stripped)
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
                resolveAndWriteSymbol(
                    ELF32_R_TYPE(rel.r_info),
                    ELF32_R_SYM(rel.r_info),
                    load_bias + rel.r_offset,
                    symtab, 0, dynstr, strtabSz
                );
            }
        };

        processRelTable(relVaddr, relSz);
        processRelTable(jmpRelVaddr, jmpRelSz);

        if (totalRelocsApplied > 0) {
            const_cast<std::vector<uint8_t>&>(raw_binary).clear();
            const_cast<std::vector<uint8_t>&>(raw_binary).shrink_to_fit();
            LOGI("applyRelocations (via PT_DYNAMIC) per [%s]: %zu rilocazioni applicate.", path.c_str(), totalRelocsApplied);
            return true;
        }
    }

    // 2. Fallback via Section Headers (.rel.dyn / .rel.plt)
    if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0) {
        return true;
    }

    size_t shTableEnd = ehdr->e_shoff + (ehdr->e_shnum * sizeof(Elf32_Shdr));
    if (shTableEnd > raw_binary.size() || ehdr->e_shstrndx >= ehdr->e_shnum) {
        return false;
    }

    const auto* shdrs = reinterpret_cast<const Elf32_Shdr*>(raw_binary.data() + ehdr->e_shoff);
    const Elf32_Shdr* dynstrShdr = nullptr;
    for (uint16_t s = 0; s < ehdr->e_shnum; ++s) {
        if (shdrs[s].sh_type == SHT_STRTAB && s != ehdr->e_shstrndx) {
            dynstrShdr = &shdrs[s];
            break;
        }
    }

    const char* dynstr = dynstrShdr ? reinterpret_cast<const char*>(raw_binary.data() + dynstrShdr->sh_offset) : nullptr;
    size_t dynstrSize = dynstrShdr ? dynstrShdr->sh_size : 0;

    for (uint16_t s = 0; s < ehdr->e_shnum; ++s) {
        const auto& sh = shdrs[s];
        if (sh.sh_type == SHT_REL) {
            if (sh.sh_offset + sh.sh_size > raw_binary.size() || sh.sh_entsize < sizeof(Elf32_Rel)) continue;
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
                resolveAndWriteSymbol(
                    ELF32_R_TYPE(rel.r_info),
                    ELF32_R_SYM(rel.r_info),
                    load_bias + rel.r_offset,
                    symtab, numSyms, dynstr, dynstrSize
                );
            }
        }
    }

    // Libera il buffer binario raw dopo aver completato tutte le rilocazioni per risparmiare RAM host
    const_cast<std::vector<uint8_t>&>(raw_binary).clear();
    const_cast<std::vector<uint8_t>&>(raw_binary).shrink_to_fit();

    LOGI("Rilocazioni dinamiche applicate per [%s]: %zu simboli rilocati.", path.c_str(), totalRelocsApplied);
    return true;
}

bool LoadedLibrary::executeInitFunctions(uc_engine* uc, uint32_t sp) const {
    if (!uc || init_executed) return true;
    init_executed = true;

    size_t executedCount = 0;
    auto runSingleConstructor = [&](uint32_t fnAddr) {
        if (fnAddr == 0 || fnAddr == 0xFFFFFFFFU) return;
        // Se il puntatore in .init_array non era già rilocato con load_bias, aggiungilo
        if (fnAddr < load_bias) {
            fnAddr += load_bias;
        }
        uint32_t minLibAddr = load_bias + aligned_min_vaddr;
        uint32_t maxLibAddr = load_bias + aligned_max_vaddr;
        uint32_t cleanPc = fnAddr & ~1U;
        if (cleanPc < minLibAddr || cleanPc >= maxLibAddr) {
            return;
        }

        uint32_t argc = 0, argv = 0, envp = 0;
        uint32_t lr = 0xDEADBEEF;
        uint32_t cpsr = (fnAddr & 1U) ? 0x00000030U : 0x00000010U;

        uc_reg_write(uc, UC_ARM_REG_R0, &argc);
        uc_reg_write(uc, UC_ARM_REG_R1, &argv);
        uc_reg_write(uc, UC_ARM_REG_R2, &envp);
        uc_reg_write(uc, UC_ARM_REG_SP, &sp);
        uc_reg_write(uc, UC_ARM_REG_LR, &lr);
        uc_reg_write(uc, UC_ARM_REG_CPSR, &cpsr);

        uc_emu_start(uc, fnAddr, lr, 0, 250000);
        executedCount++;
    };

    auto runArray = [&](uint32_t arrayVaddr, uint32_t arraySz) {
        if (arrayVaddr == 0 || arraySz < 4) return;
        uint32_t count = arraySz / 4U;
        uint64_t baseAddr = load_bias + arrayVaddr;
        for (uint32_t i = 0; i < count && i < 2048; ++i) {
            uint32_t fnPtr = 0;
            if (uc_mem_read(uc, baseAddr + (i * 4U), &fnPtr, 4) == UC_ERR_OK) {
                runSingleConstructor(fnPtr);
            }
        }
    };

    runArray(dt_preinit_array_vaddr, dt_preinit_array_size);
    if (dt_init_vaddr != 0) {
        runSingleConstructor(load_bias + dt_init_vaddr);
    }
    runArray(dt_init_array_vaddr, dt_init_array_size);

    LOGI("executeInitFunctions per [%s]: eseguiti %zu costruttori statici C++ (DT_INIT / DT_INIT_ARRAY).",
         path.c_str(), executedCount);
    return true;
}

Elf32Loader::Elf32Loader() = default;
Elf32Loader::~Elf32Loader() = default;

bool Elf32Loader::validateHeader(const Elf32_Ehdr& header) const {
    if (header.e_ident[EI_MAG0] != ELFMAG0 ||
        header.e_ident[EI_MAG1] != ELFMAG1 ||
        header.e_ident[EI_MAG2] != ELFMAG2 ||
        header.e_ident[EI_MAG3] != ELFMAG3) {
        return false;
    }
    if (header.e_ident[EI_CLASS] != ELFCLASS32 || header.e_ident[EI_DATA] != ELFDATA2LSB) {
        return false;
    }
    if (header.e_machine != EM_ARM) {
        return false;
    }
    return (header.e_type == ET_DYN || header.e_type == ET_EXEC);
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
    if (!buffer || bufferSize < sizeof(Elf32_Ehdr)) return 0;

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
                size_t maxScan = (strtabVaddr > symtabVaddr) ? ((strtabVaddr - symtabVaddr) / sizeof(Elf32_Sym)) : 16384;
                if (maxScan > 32768) maxScan = 32768;
                for (size_t i = 0; i < maxScan; ++i) {
                    if (strtabSz > 0 && syms[i].st_name >= strtabSz) break;
                    const char* symName = dynstr + syms[i].st_name;
                    if (!symName || symName[0] == '\0') continue;

                    if (std::strcmp(symName, "JNI_OnLoad") == 0 && syms[i].st_value != 0) {
                        LOGI("PT_DYNAMIC: Trovato 'JNI_OnLoad' a 0x%08x", syms[i].st_value);
                        return syms[i].st_value;
                    }
                    if (std::strcmp(symName, "ANativeActivity_onCreate") == 0 && syms[i].st_value != 0) {
                        LOGI("PT_DYNAMIC: Trovato 'ANativeActivity_onCreate' a 0x%08x", syms[i].st_value);
                        return syms[i].st_value;
                    }
                }
            }
        }
    }

    if (ehdr.e_shoff == 0 || ehdr.e_shnum == 0) return 0;
    size_t shTableEnd = ehdr.e_shoff + (ehdr.e_shnum * sizeof(Elf32_Shdr));
    if (shTableEnd > bufferSize || ehdr.e_shstrndx >= ehdr.e_shnum) return 0;

    const auto* shdrs = reinterpret_cast<const Elf32_Shdr*>(buffer + ehdr.e_shoff);
    const auto& strSection = shdrs[ehdr.e_shstrndx];
    if (strSection.sh_offset + strSection.sh_size > bufferSize) return 0;
    const char* shstrtab = reinterpret_cast<const char*>(buffer + strSection.sh_offset);

    const Elf32_Shdr* dynsymShdr = nullptr;
    const Elf32_Shdr* dynstrShdr = nullptr;

    for (uint16_t s = 0; s < ehdr.e_shnum; ++s) {
        const auto& sh = shdrs[s];
        if (sh.sh_name < strSection.sh_size) {
            const char* name = shstrtab + sh.sh_name;
            if (sh.sh_type == SHT_DYNSYM || std::strcmp(name, ".dynsym") == 0) dynsymShdr = &sh;
            else if (sh.sh_type == SHT_STRTAB && std::strcmp(name, ".dynstr") == 0) dynstrShdr = &sh;
        }
    }

    if (dynsymShdr && dynstrShdr &&
        dynsymShdr->sh_offset + dynsymShdr->sh_size <= bufferSize &&
        dynstrShdr->sh_offset + dynstrShdr->sh_size <= bufferSize &&
        dynsymShdr->sh_entsize >= sizeof(Elf32_Sym)) {
        const auto* syms = reinterpret_cast<const Elf32_Sym*>(buffer + dynsymShdr->sh_offset);
        size_t numSyms = dynsymShdr->sh_size / sizeof(Elf32_Sym);
        const char* strings = reinterpret_cast<const char*>(buffer + dynstrShdr->sh_offset);

        for (size_t i = 0; i < numSyms; ++i) {
            if (syms[i].st_name < dynstrShdr->sh_size && syms[i].st_value != 0) {
                const char* symName = strings + syms[i].st_name;
                if (std::strcmp(symName, "JNI_OnLoad") == 0) return syms[i].st_value;
            }
        }
    }

    return 0;
}

std::shared_ptr<LoadedLibrary> Elf32Loader::loadFromFile(const std::string& filePath) {
    int fd = open(filePath.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        LOGE("Impossibile aprire il file binario: %s (errno=%d)", filePath.c_str(), errno);
        return nullptr;
    }

    off_t fileSize = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (fileSize <= static_cast<off_t>(sizeof(Elf32_Ehdr))) {
        close(fd);
        return nullptr;
    }

    std::vector<uint8_t> buffer(static_cast<size_t>(fileSize));
    ssize_t bytesRead = read(fd, buffer.data(), buffer.size());
    close(fd);

    if (bytesRead != static_cast<ssize_t>(buffer.size())) {
        return nullptr;
    }

    return loadFromMemory(buffer.data(), buffer.size(), filePath);
}

std::shared_ptr<LoadedLibrary> Elf32Loader::loadFromMemory(
    const uint8_t* buffer,
    size_t bufferSize,
    const std::string& name) {

    if (!buffer || bufferSize < sizeof(Elf32_Ehdr)) return nullptr;
    const auto* ehdr = reinterpret_cast<const Elf32_Ehdr*>(buffer);
    if (!validateHeader(*ehdr)) return nullptr;
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) return nullptr;
    if (ehdr->e_phoff + (ehdr->e_phnum * sizeof(Elf32_Phdr)) > bufferSize) return nullptr;

    const auto* phdrTable = reinterpret_cast<const Elf32_Phdr*>(buffer + ehdr->e_phoff);
    uint32_t minVaddr = 0xFFFFFFFF;
    uint32_t maxVaddr = 0x00000000;
    std::vector<ElfSegment> loadSegments;
    const Elf32_Phdr* dynPhdr = nullptr;

    for (uint16_t i = 0; i < ehdr->e_phnum; ++i) {
        const Elf32_Phdr& ph = phdrTable[i];
        if (ph.p_type == PT_DYNAMIC) {
            dynPhdr = &ph;
        } else if (ph.p_type == PT_LOAD && ph.p_memsz > 0) {
            if (ph.p_vaddr < minVaddr) minVaddr = ph.p_vaddr;
            if (ph.p_vaddr + ph.p_memsz > maxVaddr) maxVaddr = ph.p_vaddr + ph.p_memsz;

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

    if (loadSegments.empty()) return nullptr;

    uintptr_t alignedMinVaddr = PAGE_ALIGN_DOWN(minVaddr);
    uintptr_t alignedMaxVaddr = PAGE_ALIGN_UP(maxVaddr);
    size_t totalMemorySize = alignedMaxVaddr - alignedMinVaddr;

    auto loadedLib = std::make_shared<LoadedLibrary>();
    loadedLib->path = name;
    loadedLib->aligned_min_vaddr = static_cast<uint32_t>(alignedMinVaddr);
    loadedLib->aligned_max_vaddr = static_cast<uint32_t>(alignedMaxVaddr);
    loadedLib->total_memory_size = totalMemorySize;
    loadedLib->is_pie = (ehdr->e_type == ET_DYN);

    if (loadedLib->is_pie) {
        loadedLib->load_bias = sNextLoadBias.load();
        sNextLoadBias.fetch_add(static_cast<uint32_t>(PAGE_ALIGN_UP(totalMemorySize) + 0x200000U));
    } else {
        loadedLib->load_bias = 0U;
    }

    uint32_t jniOnLoad = findJniOnLoadSymbol(buffer, bufferSize, *ehdr);
    if (jniOnLoad != 0) {
        loadedLib->entry_point = loadedLib->load_bias + jniOnLoad;
    } else if (ehdr->e_entry != 0) {
        loadedLib->entry_point = loadedLib->load_bias + ehdr->e_entry;
    } else {
        uint32_t execVaddr = 0;
        for (const auto& seg : loadSegments) {
            if (seg.flags & PF_X) { execVaddr = seg.vaddr; break; }
        }
        loadedLib->entry_point = loadedLib->load_bias + (execVaddr != 0 ? execVaddr : loadSegments[0].vaddr);
    }

    loadedLib->segments = loadSegments;
    loadedLib->raw_binary.assign(buffer, buffer + bufferSize);

    // Estrazione DT_INIT / DT_INIT_ARRAY e simboli esportati da PT_DYNAMIC (fondamentale per ELF stripped)
    if (dynPhdr && (dynPhdr->p_offset + dynPhdr->p_filesz <= bufferSize)) {
        const auto* dyn = reinterpret_cast<const Elf32_Dyn*>(buffer + dynPhdr->p_offset);
        size_t numDyn = dynPhdr->p_filesz / sizeof(Elf32_Dyn);
        uint32_t symtabVaddr = 0, strtabVaddr = 0, strtabSz = 0;

        for (size_t d = 0; d < numDyn && dyn[d].d_tag != DT_NULL; ++d) {
            switch (dyn[d].d_tag) {
                case DT_INIT: loadedLib->dt_init_vaddr = dyn[d].d_un.d_ptr; break;
                case DT_PREINIT_ARRAY: loadedLib->dt_preinit_array_vaddr = dyn[d].d_un.d_ptr; break;
                case DT_PREINIT_ARRAYSZ: loadedLib->dt_preinit_array_size = dyn[d].d_un.d_val; break;
                case DT_INIT_ARRAY: loadedLib->dt_init_array_vaddr = dyn[d].d_un.d_ptr; break;
                case DT_INIT_ARRAYSZ: loadedLib->dt_init_array_size = dyn[d].d_un.d_val; break;
                case DT_SYMTAB: symtabVaddr = dyn[d].d_un.d_ptr; break;
                case DT_STRTAB: strtabVaddr = dyn[d].d_un.d_ptr; break;
                case DT_STRSZ: strtabSz = dyn[d].d_un.d_val; break;
            }
        }

        auto vaddrToBuf = [&](uint32_t vaddr) -> const uint8_t* {
            for (const auto& seg : loadSegments) {
                if (vaddr >= seg.vaddr && vaddr < (seg.vaddr + seg.filesz)) {
                    size_t off = seg.offset + (vaddr - seg.vaddr);
                    if (off < bufferSize) return buffer + off;
                }
            }
            return nullptr;
        };

        const char* dynstr = strtabVaddr ? reinterpret_cast<const char*>(vaddrToBuf(strtabVaddr)) : nullptr;
        const auto* syms = symtabVaddr ? reinterpret_cast<const Elf32_Sym*>(vaddrToBuf(symtabVaddr)) : nullptr;
        if (dynstr && syms) {
            size_t maxSyms = (strtabVaddr > symtabVaddr) ? ((strtabVaddr - symtabVaddr) / sizeof(Elf32_Sym)) : 8192;
            if (maxSyms > 32768) maxSyms = 32768;
            for (size_t i = 0; i < maxSyms; ++i) {
                if (strtabSz > 0 && syms[i].st_name >= strtabSz) break;
                if (syms[i].st_shndx != 0 && syms[i].st_value != 0) {
                    const char* sName = dynstr + syms[i].st_name;
                    if (sName && sName[0] != '\0') {
                        loadedLib->symbols.push_back({sName, syms[i].st_value});
                    }
                }
            }
        }
    }

    // Fallback estrazione simboli da Section Headers se non già estratti da PT_DYNAMIC
    if (loadedLib->symbols.empty() && ehdr->e_shoff != 0 && ehdr->e_shnum != 0 &&
        (ehdr->e_shoff + (ehdr->e_shnum * sizeof(Elf32_Shdr)) <= bufferSize)) {
        const auto* shdrs = reinterpret_cast<const Elf32_Shdr*>(buffer + ehdr->e_shoff);
        const Elf32_Shdr* symShdr = nullptr;
        const Elf32_Shdr* strShdr = nullptr;
        for (uint16_t s = 0; s < ehdr->e_shnum; ++s) {
            if (shdrs[s].sh_type == SHT_DYNSYM) symShdr = &shdrs[s];
            if (shdrs[s].sh_type == SHT_STRTAB && s != ehdr->e_shstrndx) strShdr = &shdrs[s];
        }
        if (symShdr && strShdr && strShdr->sh_offset < bufferSize && symShdr->sh_offset + symShdr->sh_size <= bufferSize) {
            const auto* syms = reinterpret_cast<const Elf32_Sym*>(buffer + symShdr->sh_offset);
            size_t nSyms = symShdr->sh_size / sizeof(Elf32_Sym);
            const char* strtab = reinterpret_cast<const char*>(buffer + strShdr->sh_offset);
            for (size_t i = 0; i < nSyms; ++i) {
                if (syms[i].st_name < strShdr->sh_size && syms[i].st_value != 0 && syms[i].st_shndx != 0) {
                    const char* sName = strtab + syms[i].st_name;
                    if (sName && sName[0] != '\0') {
                        loadedLib->symbols.push_back({sName, syms[i].st_value});
                    }
                }
            }
        }
    }

    LOGI("Caricamento ELF completato: %s (Entry: 0x%08x, Bias: 0x%08x, DT_INIT: 0x%x, DT_INIT_ARRAY: 0x%x/%uB, Simboli: %zu)",
         name.c_str(), loadedLib->entry_point, loadedLib->load_bias,
         loadedLib->dt_init_vaddr, loadedLib->dt_init_array_vaddr, loadedLib->dt_init_array_size,
         loadedLib->symbols.size());
    return loadedLib;
}

} // namespace emux
