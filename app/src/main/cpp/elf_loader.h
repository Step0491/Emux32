#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>
#include <elf.h>
#include "unicorn/unicorn.h"

#ifndef PAGE_SIZE
#define PAGE_SIZE 0x1000
#endif

// Macro obbligatorie per l'allineamento dei blocchi alla pagina di sistema (4096 / 0x1000 byte)
#define PAGE_ALIGN_DOWN(addr) ((addr) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_UP(addr) (((addr) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

namespace emux {

struct ElfSegment {
    uint32_t type;
    uint32_t flags;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t align;
    int prot;
    uint32_t uc_prot;
};

struct ExportedSymbol {
    std::string name;
    uint32_t vaddr{0};
};

struct LoadedLibrary {
    std::string path;
    uintptr_t base_address{0};
    size_t total_memory_size{0};
    uint32_t aligned_min_vaddr{0};
    uint32_t aligned_max_vaddr{0};
    uint32_t entry_point{0};
    uint32_t load_bias{0};
    uint32_t dt_init_vaddr{0};
    uint32_t dt_preinit_array_vaddr{0};
    uint32_t dt_preinit_array_size{0};
    uint32_t dt_init_array_vaddr{0};
    uint32_t dt_init_array_size{0};
    mutable bool init_executed{false};
    std::vector<ElfSegment> segments;
    std::vector<ExportedSymbol> symbols;
    std::vector<uint8_t> raw_binary;
    bool is_pie{false};

    ~LoadedLibrary();

    // Mappa tutti i segmenti PT_LOAD all'interno dell'istanza Unicorn applicando strict page alignment
    bool mapToUnicorn(uc_engine* uc) const;

    // Applica le rilocazioni ELF (R_ARM_RELATIVE, R_ARM_GLOB_DAT, R_ARM_JUMP_SLOT, R_ARM_ABS32)
    bool applyRelocations(uc_engine* uc) const;

    // Esegue DT_PREINIT_ARRAY, DT_INIT e DT_INIT_ARRAY (costruttori statici C++) prima di JNI_OnLoad
    bool executeInitFunctions(uc_engine* uc, uint32_t sp) const;
};

class Elf32Loader {
public:
    Elf32Loader();
    ~Elf32Loader();

    std::shared_ptr<LoadedLibrary> loadFromFile(const std::string& filePath);
    std::shared_ptr<LoadedLibrary> loadFromMemory(const uint8_t* buffer, size_t bufferSize, const std::string& name);

private:
    bool validateHeader(const Elf32_Ehdr& header) const;
    int convertElfFlagsToMmapProt(uint32_t p_flags) const;
    uint32_t convertElfFlagsToUcProt(uint32_t p_flags) const;
    uint32_t findJniOnLoadSymbol(const uint8_t* buffer, size_t bufferSize, const Elf32_Ehdr& ehdr) const;
};

} // namespace emux
