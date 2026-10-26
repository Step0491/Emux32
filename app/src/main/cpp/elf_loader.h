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

struct LoadedLibrary {
    std::string path;
    uintptr_t base_address{0};    // Indirizzo base virtuale nell'host
    size_t total_memory_size{0};  // Dimensione totale contigua allocata
    uint32_t entry_point{0};      // Entry point ARM32
    uint32_t load_bias{0};        // Offset rilocazione base
    std::vector<ElfSegment> segments;
    std::vector<uint8_t> raw_binary; // Buffer binario per mapping JIT / Unicorn
    bool is_pie{false};

    ~LoadedLibrary();

    // Mappa tutti i segmenti PT_LOAD all'interno dell'istanza Unicorn applicando strict page alignment
    bool mapToUnicorn(uc_engine* uc) const;
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
};

} // namespace emux
