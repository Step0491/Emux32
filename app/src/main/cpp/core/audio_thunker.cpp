#include "audio_thunker.h"
#include "unicorn/arm.h"
#include <android/log.h>
#include <cstring>

#define TAG "EmuxAudioThunker"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace emux {

uint32_t AudioThunker::sAudioEngineVtable = 0x7D003000;
uint32_t AudioThunker::sAudioOutputMixVtable = 0x7D003200;
uint32_t AudioThunker::sAudioPlayerVtable = 0x7D003400;
uint32_t AudioThunker::sAudioPlayVtable = 0x7D003600;
uint32_t AudioThunker::sAudioBufferQueueVtable = 0x7D003800;

void AudioThunker::initialize(uc_engine* uc) {
    if (!uc) return;

    // Stub assembly per metodi OpenSL ES:
    // MOV R0, #0 (SL_RESULT_SUCCESS)
    // BX LR
    static const uint32_t stubCode[2] = {
        0xE3A00000, // MOV R0, #0
        0xE12FFF1E  // BX LR
    };
    uint32_t stubAddr = 0x7D003E00;
    uc_mem_write(uc, stubAddr, stubCode, sizeof(stubCode));

    // Popola le VTable per gli oggetti OpenSL ES (ciascuno con puntatori allo stubAddress)
    uint32_t vtableEntries[32];
    for (int i = 0; i < 32; ++i) {
        vtableEntries[i] = stubAddr;
    }

    uc_mem_write(uc, sAudioEngineVtable, vtableEntries, sizeof(vtableEntries));
    uc_mem_write(uc, sAudioOutputMixVtable, vtableEntries, sizeof(vtableEntries));
    uc_mem_write(uc, sAudioPlayerVtable, vtableEntries, sizeof(vtableEntries));
    uc_mem_write(uc, sAudioPlayVtable, vtableEntries, sizeof(vtableEntries));
    uc_mem_write(uc, sAudioBufferQueueVtable, vtableEntries, sizeof(vtableEntries));

    LOGI("AudioThunker: Mock OpenSL ES inizializzato con VTable a 0x7D003000.");
}

bool AudioThunker::handleAudioCall(const std::string& name, uc_engine* uc) {
    if (!uc) return false;

    // slCreateEngine(&engineObject, ...)
    if (name == "slCreateEngine") {
        uint32_t pEngineObj = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &pEngineObj);

        if (pEngineObj != 0) {
            // Struttura SLObjectItf: puntatore a puntatore alla vtable
            uint32_t objInst = 0x7D004000;
            uint32_t vtablePtr = sAudioEngineVtable;
            uc_mem_write(uc, objInst, &vtablePtr, 4);
            uc_mem_write(uc, pEngineObj, &objInst, 4);
        }

        uint32_t res = 0; // SL_RESULT_SUCCESS
        uc_reg_write(uc, UC_ARM_REG_R0, &res);
        LOGI("AudioThunker: slCreateEngine simulato con successo.");
        return true;
    }

    return false;
}

} // namespace emux
