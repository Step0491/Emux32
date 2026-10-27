#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_map>
#include <mutex>

namespace emux {

/**
 * Rappresenta un blocco base tradotto/indicizzato (Basic Block).
 * Contiene metadati e informazioni per il dispatch rapido.
 */
struct BasicBlock {
    uint32_t guestPc       = 0;     // Indirizzo di partenza nel guest (con bit 0 se Thumb)
    uint32_t guestEndPc    = 0;     // Indirizzo finale del blocco
    uint32_t insnCount     = 0;     // Numero di istruzioni nel blocco
    uint32_t sizeBytes     = 0;     // Dimensione in byte nel guest
    bool     isThumb       = false; // Modalità Thumb attiva
    bool     hasBranch     = false; // Termina con istruzione di salto/ritorno
};

/**
 * Cache dei blocchi tradotti (Translation Block Cache).
 * Mappa i puntatori di ingresso guest ai blocchi analizzati per evitare overhead di decoding continuo.
 */
class TranslationBlockCache {
public:
    static constexpr size_t kFastTableSize = 65536; // 64K entry direct-map fast index

    TranslationBlockCache();
    ~TranslationBlockCache();

    void clear();

    inline BasicBlock* lookupFast(uint32_t pc) const noexcept {
        uint32_t idx = (pc >> 1) & (kFastTableSize - 1);
        BasicBlock* bb = fastTable_[idx];
        if (bb != nullptr && bb->guestPc == pc) {
            return bb;
        }
        return nullptr;
    }

    BasicBlock* findOrInsert(uint32_t pc, bool isThumb);
    void invalidateRange(uint32_t startAddr, uint32_t size);

private:
    BasicBlock** fastTable_ = nullptr;
    std::unordered_map<uint32_t, BasicBlock> blockMap_;
    mutable std::mutex cacheMutex_;
};

} // namespace emux
