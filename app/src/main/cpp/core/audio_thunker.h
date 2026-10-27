#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <mutex>
#include <vector>
#include "unicorn/unicorn.h"

namespace emux {

/**
 * Strutture e mock di OpenSL ES (32-bit pointers).
 * Simula SLObjectItf, SLEngineItf, SLPlayItf, SLBufferQueueItf per titoli Android NDK a 32-bit.
 */
class AudioThunker {
public:
    static void initialize(uc_engine* uc);
    static bool handleAudioCall(const std::string& name, uc_engine* uc);

private:
    static uint32_t sAudioEngineVtable;
    static uint32_t sAudioOutputMixVtable;
    static uint32_t sAudioPlayerVtable;
    static uint32_t sAudioBufferQueueVtable;
    static uint32_t sAudioPlayVtable;
};

} // namespace emux
