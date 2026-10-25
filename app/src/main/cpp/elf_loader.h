#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>
#include <elf.h>

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
};

struct LoadedLibrary {
    std::string path;
    uintptr_t base_address;       // Indirizzo base virtuale nell'host
    size_t total_memory_size;     // Dimensione totale contigua allocata
    uint32_t entry_point;         // Entry point ARM32
    uint32_t load_bias;           // Offset rilocazione base (base_address - min_vaddr)
    std::vector<ElfSegment> segments;
    bool is_pie;

    ~LoadedLibrary();
};

class Elf32Loader {
public:
    Elf32Loader();
    ~Elf32Loader();

    // Carica ed esegue il parsing completo di una libreria ELF 32-bit da disco
    std::shared_ptr<LoadedLibrary> loadFromFile(const std::string& filePath);

    // Carica direttamente da buffer di memoria (es. estratto in RAM da un file APK)
    std::shared_ptr<LoadedLibrary> loadFromMemory(const uint8_t* buffer, size_t bufferSize, const std::string& name);

private:
    bool validateHeader(const Elf32_Ehdr& header) const;
    int convertElfFlagsToMmapProt(uint32_t p_flags) const;
    size_t getSystemPageSize() const;
    uintptr_t alignDown(uintptr_t addr, size_t pageSize) const;
    uintptr_t alignUp(uintptr_t addr, size_t pageSize) const;

    size_t pageSize_;
};

} // namespace emux
