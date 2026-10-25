#include "elf_loader.h"
#include <iostream>
#include <cassert>
#include <cstring>

using namespace emux;

void testElfHeaderValidation() {
    std::cout << "[TEST] Avvio validazione Header ELF ARM32..." << std::endl;

    Elf32Loader loader;

    // 1. Buffer fittizio non ELF
    uint8_t garbage[64] = {0};
    auto libNull = loader.loadFromMemory(garbage, sizeof(garbage), "garbage.bin");
    assert(libNull == nullptr);
    std::cout << "  ✓ Rifiutato buffer non ELF" << std::endl;

    // 2. Buffer con Magic ELF valido ma classe a 64-bit (ELFCLASS64 = 2)
    Elf32_Ehdr mock64{};
    mock64.e_ident[EI_MAG0] = ELFMAG0;
    mock64.e_ident[EI_MAG1] = ELFMAG1;
    mock64.e_ident[EI_MAG2] = ELFMAG2;
    mock64.e_ident[EI_MAG3] = ELFMAG3;
    mock64.e_ident[EI_CLASS] = ELFCLASS64;
    mock64.e_ident[EI_DATA] = ELFDATA2LSB;
    mock64.e_ident[EI_VERSION] = EV_CURRENT;
    mock64.e_machine = EM_ARM;
    mock64.e_type = ET_DYN;

    auto lib64 = loader.loadFromMemory(reinterpret_cast<uint8_t*>(&mock64), sizeof(mock64), "x64.so");
    assert(lib64 == nullptr);
    std::cout << "  ✓ Rifiutato binario con ELFCLASS errata" << std::endl;

    // 3. Buffer con Magic ELF ARM32 valido (ELFCLASS32 = 1, EM_ARM = 40)
    struct MockElfArm32 {
        Elf32_Ehdr header;
        Elf32_Phdr phdrLoadCode;
        Elf32_Phdr phdrLoadData;
        uint8_t dummyCode[128];
        uint8_t dummyData[64];
    } mockArm{};

    mockArm.header.e_ident[EI_MAG0] = ELFMAG0;
    mockArm.header.e_ident[EI_MAG1] = ELFMAG1;
    mockArm.header.e_ident[EI_MAG2] = ELFMAG2;
    mockArm.header.e_ident[EI_MAG3] = ELFMAG3;
    mockArm.header.e_ident[EI_CLASS] = ELFCLASS32;
    mockArm.header.e_ident[EI_DATA] = ELFDATA2LSB;
    mockArm.header.e_ident[EI_VERSION] = EV_CURRENT;
    mockArm.header.e_machine = EM_ARM;
    mockArm.header.e_type = ET_DYN;
    mockArm.header.e_phoff = offsetof(MockElfArm32, phdrLoadCode);
    mockArm.header.e_phnum = 2;
    mockArm.header.e_phentsize = sizeof(Elf32_Phdr);
    mockArm.header.e_entry = 0x1000;

    // Segmento 1: Codice (R-X)
    mockArm.phdrLoadCode.p_type = PT_LOAD;
    mockArm.phdrLoadCode.p_flags = PF_R | PF_X;
    mockArm.phdrLoadCode.p_offset = offsetof(MockElfArm32, dummyCode);
    mockArm.phdrLoadCode.p_vaddr = 0x1000;
    mockArm.phdrLoadCode.p_filesz = sizeof(mockArm.dummyCode);
    mockArm.phdrLoadCode.p_memsz = 0x1000;
    mockArm.phdrLoadCode.p_align = 0x1000;

    // Segmento 2: Dati (RW-)
    mockArm.phdrLoadData.p_type = PT_LOAD;
    mockArm.phdrLoadData.p_flags = PF_R | PF_W;
    mockArm.phdrLoadData.p_offset = offsetof(MockElfArm32, dummyData);
    mockArm.phdrLoadData.p_vaddr = 0x2000;
    mockArm.phdrLoadData.p_filesz = sizeof(mockArm.dummyData);
    mockArm.phdrLoadData.p_memsz = 0x1000;
    mockArm.phdrLoadData.p_align = 0x1000;

    auto libArm = loader.loadFromMemory(
        reinterpret_cast<uint8_t*>(&mockArm),
        sizeof(mockArm),
        "libgame_arm32.so"
    );

    assert(libArm != nullptr);
    assert(libArm->base_address != 0);
    assert(libArm->total_memory_size > 0);
    assert(libArm->segments.size() == 2);
    std::cout << "  ✓ Caricamento e allineamento memoria ELF ARM32 verificato con successo!" << std::endl;
}

int main() {
    std::cout << "=== EMUX 32-BIT TEST HARNESS ===" << std::endl;
    testElfHeaderValidation();
    std::cout << "=== TUTTI I TEST SONO PASSATI CON SUCCESSO ===" << std::endl;
    return 0;
}
