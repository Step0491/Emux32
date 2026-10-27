#include "audio_thunker.h"
#include "unicorn/arm.h"
#include <android/log.h>
#include <cstring>

#define TAG "EmuxAudioThunker"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace emux {

uint32_t AudioThunker::sAudioEngineVtable      = 0x7D003000;
uint32_t AudioThunker::sAudioOutputMixVtable   = 0x7D003200;
uint32_t AudioThunker::sAudioPlayerVtable      = 0x7D003400;
uint32_t AudioThunker::sAudioPlayVtable        = 0x7D003600;
uint32_t AudioThunker::sAudioBufferQueueVtable = 0x7D003800;

void AudioThunker::initialize(uc_engine* uc) {
    if (!uc) return;

    // 1. Stub generico per metodi OpenSL ES senza output pointer:
    //    MOV R0, #0 (SL_RESULT_SUCCESS)
    //    BX  LR
    static const uint32_t stubSuccess[2] = {
        0xE3A00000U, // MOV R0, #0
        0xE12FFF1EU  // BX  LR
    };
    const uint32_t stubSuccessAddr = 0x7D003E00U;
    uc_mem_write(uc, stubSuccessAddr, stubSuccess, sizeof(stubSuccess));

    // 2. Stub per SLObjectItf::GetInterface(self=r0, iid=r1, pInterface=r2):
    //    Scrive un puntatore all'interfaccia valida (0x7D004010) in *r2 se r2 != 0, poi ritorna 0:
    //    CMP R2, #0
    //    LDRNE R3, [PC, #4]
    //    STRNE R3, [R2]
    //    MOV R0, #0
    //    BX  LR
    //    .word 0x7D004010
    static const uint32_t stubGetInterface[6] = {
        0xE3520000U, // CMP R2, #0
        0x159F3004U, // LDRNE R3, [PC, #4]
        0x15823000U, // STRNE R3, [R2]
        0xE3A00000U, // MOV R0, #0
        0xE12FFF1EU, // BX LR
        0x7D004010U  // Puntatore istanza interfaccia generica (punta a sAudioEngineVtable)
    };
    const uint32_t stubGetIfaceAddr = 0x7D003E20U;
    uc_mem_write(uc, stubGetIfaceAddr, stubGetInterface, sizeof(stubGetInterface));

    // 3. Stub per SLEngineItf::CreateOutputMix(self=r0, pMix=r1, ...) e CreateAudioPlayer(self=r0, pPlayer=r1, ...):
    //    Scrive un puntatore a SLObjectItf valido (0x7D004000) in *r1 se r1 != 0, poi ritorna 0:
    static const uint32_t stubCreateObj[6] = {
        0xE3510000U, // CMP R1, #0
        0x159F3004U, // LDRNE R3, [PC, #4]
        0x15813000U, // STRNE R3, [R1]
        0xE3A00000U, // MOV R0, #0
        0xE12FFF1EU, // BX LR
        0x7D004000U  // Puntatore istanza SLObjectItf
    };
    const uint32_t stubCreateObjAddr = 0x7D003E40U;
    uc_mem_write(uc, stubCreateObjAddr, stubCreateObj, sizeof(stubCreateObj));

    // Popola le VTable con puntatori agli stub appropriati
    uint32_t vtableEntries[32];
    for (int i = 0; i < 32; ++i) {
        vtableEntries[i] = stubSuccessAddr;
    }
    // Slot 2 di SLObjectItf è GetInterface(self, iid, *pInterface)
    vtableEntries[2] = stubGetIfaceAddr;
    // Slot 0 e 2 di SLEngineItf sono CreateLEDDevice / CreateAudioPlayer / CreateOutputMix (pObj in r1)
    uint32_t engineVtable[32];
    for (int i = 0; i < 32; ++i) {
        engineVtable[i] = stubCreateObjAddr;
    }
    engineVtable[0] = stubSuccessAddr;  // Realize
    engineVtable[1] = stubSuccessAddr;  // Resume
    engineVtable[2] = stubGetIfaceAddr; // GetInterface

    uc_mem_write(uc, sAudioEngineVtable, engineVtable, sizeof(engineVtable));
    uc_mem_write(uc, sAudioOutputMixVtable, vtableEntries, sizeof(vtableEntries));
    uc_mem_write(uc, sAudioPlayerVtable, vtableEntries, sizeof(vtableEntries));
    uc_mem_write(uc, sAudioPlayVtable, vtableEntries, sizeof(vtableEntries));
    uc_mem_write(uc, sAudioBufferQueueVtable, vtableEntries, sizeof(vtableEntries));

    // Pre-popola le istanze oggetto a 0x7D004000 (SLObjectItf) e 0x7D004010 (SLEngineItf/SLPlayItf/SLBufferQueueItf)
    uint32_t objVtablePtr = sAudioEngineVtable;
    uc_mem_write(uc, 0x7D004000U, &objVtablePtr, 4);
    uc_mem_write(uc, 0x7D004010U, &objVtablePtr, 4);

    LOGI("AudioThunker: Mock OpenSL ES inizializzato (VTable=0x7D003000, Obj=0x7D004000, Iface=0x7D004010).");
}

bool AudioThunker::handleAudioCall(const std::string& name, uc_engine* uc) {
    if (!uc) return false;

    if (name == "slCreateEngine") {
        uint32_t pEngineObj = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &pEngineObj);

        if (pEngineObj != 0) {
            uint32_t objInst = 0x7D004000U;
            uc_mem_write(uc, pEngineObj, &objInst, 4);
        }

        uint32_t res = 0; // SL_RESULT_SUCCESS
        uc_reg_write(uc, UC_ARM_REG_R0, &res);
        LOGI("AudioThunker: slCreateEngine(pEngine=0x%08x) -> SLObjectItf=0x7D004000", pEngineObj);
        return true;
    }

    if (name == "slQueryNumSupportedEngineInterfaces") {
        uint32_t pNumSupported = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &pNumSupported);
        if (pNumSupported != 0) {
            uint32_t count = 1;
            uc_mem_write(uc, pNumSupported, &count, 4);
        }
        uint32_t res = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &res);
        return true;
    }

    return false;
}

} // namespace emux
