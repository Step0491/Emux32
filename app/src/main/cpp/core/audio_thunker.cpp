#include "audio_thunker.h"
#include "guest_memory.h"
#include "linker_hook.h"
#include "unicorn/arm.h"
#include <android/log.h>
#include <dlfcn.h>
#include <cstring>
#include <chrono>
#include <algorithm>

#define TAG "EmuxAudioThunker"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace emux {

// ============================================================================
// Tipizzazione e binding dinamico di AAudio per Android Host ARM64
// ============================================================================
typedef void* AAudioStream;
typedef void* AAudioStreamBuilder;
typedef int32_t aaudio_result_t;
typedef int32_t aaudio_format_t;
typedef int32_t aaudio_performance_mode_t;

constexpr aaudio_result_t AAUDIO_OK = 0;
constexpr aaudio_format_t AAUDIO_FORMAT_PCM_I16 = 1;
constexpr aaudio_performance_mode_t AAUDIO_PERFORMANCE_MODE_LOW_LATENCY = 12;

typedef aaudio_result_t (*fn_AAudio_createStreamBuilder)(AAudioStreamBuilder** builder);
typedef void (*fn_AAudioStreamBuilder_setSampleRate)(AAudioStreamBuilder* builder, int32_t sampleRate);
typedef void (*fn_AAudioStreamBuilder_setChannelCount)(AAudioStreamBuilder* builder, int32_t channelCount);
typedef void (*fn_AAudioStreamBuilder_setFormat)(AAudioStreamBuilder* builder, aaudio_format_t format);
typedef void (*fn_AAudioStreamBuilder_setPerformanceMode)(AAudioStreamBuilder* builder, aaudio_performance_mode_t mode);
typedef aaudio_result_t (*fn_AAudioStreamBuilder_openStream)(AAudioStreamBuilder* builder, AAudioStream** stream);
typedef aaudio_result_t (*fn_AAudioStreamBuilder_delete)(AAudioStreamBuilder* builder);
typedef aaudio_result_t (*fn_AAudioStream_requestStart)(AAudioStream* stream);
typedef aaudio_result_t (*fn_AAudioStream_requestStop)(AAudioStream* stream);
typedef aaudio_result_t (*fn_AAudioStream_close)(AAudioStream* stream);
typedef int32_t (*fn_AAudioStream_write)(AAudioStream* stream, const void* buffer, int32_t numFrames, int64_t timeoutNanoseconds);

static fn_AAudio_createStreamBuilder             sAAudio_createStreamBuilder = nullptr;
static fn_AAudioStreamBuilder_setSampleRate      sAAudioStreamBuilder_setSampleRate = nullptr;
static fn_AAudioStreamBuilder_setChannelCount    sAAudioStreamBuilder_setChannelCount = nullptr;
static fn_AAudioStreamBuilder_setFormat          sAAudioStreamBuilder_setFormat = nullptr;
static fn_AAudioStreamBuilder_setPerformanceMode sAAudioStreamBuilder_setPerformanceMode = nullptr;
static fn_AAudioStreamBuilder_openStream         sAAudioStreamBuilder_openStream = nullptr;
static fn_AAudioStreamBuilder_delete             sAAudioStreamBuilder_delete = nullptr;
static fn_AAudioStream_requestStart              sAAudioStream_requestStart = nullptr;
static fn_AAudioStream_requestStop               sAAudioStream_requestStop = nullptr;
static fn_AAudioStream_close                     sAAudioStream_close = nullptr;
static fn_AAudioStream_write                     sAAudioStream_write = nullptr;

// ============================================================================
// Tabella Interfacce e GUID OpenSL ES conformi alle specifiche Khronos
// ============================================================================
struct OpenSLInterfaceEntry {
    const char* name;
    uint32_t time_low;
    uint16_t time_mid;
    uint16_t time_hi_and_version;
    uint16_t clock_seq;
    uint8_t  node[6];
};

static const OpenSLInterfaceEntry sKnownInterfaces[] = {
    { "SL_IID_ENGINE",                    0x8d97c260, 0xddd4, 0x11db, 0x958f, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } },
    { "SL_IID_ANDROIDSIMPLEBUFFERQUEUE",  0x198e4940, 0xc5d7, 0x11df, 0xa2a6, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } },
    { "SL_IID_BUFFERQUEUE",               0x2bc99cc0, 0xddd4, 0x11db, 0x8d99, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } },
    { "SL_IID_PLAY",                      0xef0bd9c0, 0xddd7, 0x11db, 0xbf49, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } },
    { "SL_IID_VOLUME",                    0x09e8ede0, 0xddde, 0x11db, 0xb4f6, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } },
    { "SL_IID_OUTPUTMIX",                 0x97750f60, 0xddd7, 0x11db, 0x92b1, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } },
    { "SL_IID_SEEK",                      0xd43135a0, 0xdddc, 0x11db, 0xb458, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } },
    { "SL_IID_RECORD",                    0xc5657aa0, 0xdddb, 0x11db, 0x82f7, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } },
    { "SL_IID_MUTESOLO",                  0x5a28ebe0, 0xddd7, 0x11db, 0x8220, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } },
    { "SL_IID_DYNAMICSOURCE",             0xc55cc100, 0x038b, 0x11dc, 0xbb45, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } },
    { "SL_IID_DYNAMICINTERFACEMANAGEMENT",0x63936540, 0xf775, 0x11db, 0x9cc4, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } },
    { "SL_IID_OBJECT",                    0x79216360, 0xddd7, 0x11db, 0xac16, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } },
    { "SL_IID_NULL",                      0xec7178ec, 0xe5e1, 0x4432, 0xa3f4, { 0x46, 0x57, 0xe6, 0x79, 0x52, 0x10 } },
    { "SL_IID_PLAYBACKRATE",              0x2e3b2a40, 0xddda, 0x11db, 0xa349, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } },
    { "SL_IID_PREFETCHSTATUS",            0x2a41ee80, 0xddd8, 0x11db, 0xa41f, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } },
    { "SL_IID_EQUALIZER",                 0x0bed4300, 0xddd6, 0x11db, 0x8f34, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } },
    { "SL_IID_BASSBOOST",                 0x0634f220, 0xddd4, 0x11db, 0xa0fc, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } },
    { "SL_IID_VIRTUALIZER",               0x37cc2c00, 0xdddd, 0x11db, 0x8577, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } },
    { "SL_IID_ENVIRONMENTALREVERB",       0xc2e5d5f0, 0x94bd, 0x4763, 0x9cac, { 0x4e, 0x23, 0x4d, 0x06, 0x83, 0x9e } },
    { "SL_IID_PRESETREVERB",              0x47382d60, 0xddd8, 0x11db, 0xbf3a, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } },
    { "SL_IID_EFFECTSEND",                0x56e7d200, 0xddd4, 0x11db, 0xaefb, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } },
    { "SL_IID_ANDROIDCONFIGURATION",      0x89f6a7e0, 0xbeac, 0x11df, 0x8b5c, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } },
    { "SL_IID_ANDROIDEFFECT",             0xa8c520a0, 0xbeac, 0x11df, 0x9584, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } },
    { "SL_IID_3DCOMMIT",                  0x3564ad80, 0xdd0f, 0x11db, 0x9e19, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } },
    { "SL_IID_3DDOPPLER",                 0xb45c9a80, 0xddd2, 0x11db, 0xb028, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } },
    { "SL_IID_3DGROUPING",                0xebe844e0, 0xddd2, 0x11db, 0xb510, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } },
    { "SL_IID_3DLOCATION",                0x2b878020, 0xddd3, 0x11db, 0x8a01, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } },
    { "SL_IID_3DMACROSCOPIC",             0x5089aec0, 0xddd3, 0x11db, 0x9ad3, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } },
    { "SL_IID_3DSOURCE",                  0x70bc7b00, 0xddd3, 0x11db, 0xa873, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } },
    { "SL_IID_THREADSYNC",                0xf6ac6b40, 0xdddc, 0x11db, 0xa62e, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } },
    { "SL_IID_DEVICEVOLUME",              0xe1634760, 0xf3e2, 0x11db, 0x9ca9, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } }
};

static constexpr size_t kNumKnownInterfaces = sizeof(sKnownInterfaces) / sizeof(sKnownInterfaces[0]);

// Interrupt callback per il thread dedicato ai callback OpenSL ES
static void audioInterruptCallback(uc_engine* uc, uint32_t intno, void* /* userData */) {
    if (!uc) return;
    if (intno & 0x80000000U) {
        uint32_t svcComment = intno & 0x00FFFFFFU;
        GuestRuntimeEnvironment::getInstance().handleTrampolineCall(svcComment, uc);
        return;
    }

    uint32_t r7 = 0, r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0;
    uc_reg_read(uc, UC_ARM_REG_R7, &r7);
    uc_reg_read(uc, UC_ARM_REG_R0, &r0);
    uc_reg_read(uc, UC_ARM_REG_R1, &r1);
    uc_reg_read(uc, UC_ARM_REG_R2, &r2);
    uc_reg_read(uc, UC_ARM_REG_R3, &r3);
    uc_reg_read(uc, UC_ARM_REG_R4, &r4);
    uc_reg_read(uc, UC_ARM_REG_R5, &r5);

    int32_t ret = SyscallDispatcher::handleSyscall(r7, r0, r1, r2, r3, r4, r5, uc);
    uint32_t retVal = static_cast<uint32_t>(ret);
    uc_reg_write(uc, UC_ARM_REG_R0, &retVal);
}

// ============================================================================
// Singleton & Inizializzazione Ambiente OpenSL ES
// ============================================================================
AudioThunker& AudioThunker::getInstance() {
    static AudioThunker sInstance;
    return sInstance;
}

void AudioThunker::initialize(uc_engine* uc) {
    if (!uc) return;
    std::lock_guard<std::mutex> lock(audioMutex_);

    auto& gm = GuestMemory::getSharedInstance();

    // 1. Mappatura Stack dedicato e TLS per i callback del motore audio guest
    if (!gm.isMappedRange(kAudioStackBase, kAudioStackSize)) {
        gm.map(kAudioStackBase, kAudioStackSize, GP_READ | GP_WRITE);
    }
    if (!gm.isMappedRange(kAudioTlsAddr, 4096)) {
        gm.map(kAudioTlsAddr, 4096, GP_READ | GP_WRITE);
    }

    // Configurazione TLS self-pointer
    uint32_t selfPtr = kAudioTlsAddr;
    uc_mem_write(uc, kAudioTlsAddr, &selfPtr, 4);

    // 2. Registrazione GUID struct e pointer variables per tutti i simboli SL_IID_*
    for (size_t i = 0; i < kNumKnownInterfaces; ++i) {
        uint32_t guidAddr = kIidBaseAddr + static_cast<uint32_t>(i * 16U);
        uint32_t varAddr  = kIidVarBaseAddr + static_cast<uint32_t>(i * 4U);

        GuestOpenSLGuid guidData;
        guidData.time_low            = sKnownInterfaces[i].time_low;
        guidData.time_mid            = sKnownInterfaces[i].time_mid;
        guidData.time_hi_and_version = sKnownInterfaces[i].time_hi_and_version;
        guidData.clock_seq           = sKnownInterfaces[i].clock_seq;
        std::memcpy(guidData.node, sKnownInterfaces[i].node, 6);

        uc_mem_write(uc, guidAddr, &guidData, sizeof(guidData));
        uc_mem_write(uc, varAddr, &guidAddr, 4);
    }

    // 3. Generazione Trampoline SVC per ciascun Method ID OpenSL ES (0 .. 79)
    for (uint32_t m = 0; m < 80; ++m) {
        uint32_t trampAddr = kMethodTrampolineBase + (m * 8U);
        uint32_t stubInsn[2] = {
            0xEF000000U | (0x5000U + m), // SVC #(0x5000 + m)
            0xE12FFF1EU                  // BX LR
        };
        uc_mem_write(uc, trampAddr, stubInsn, sizeof(stubInsn));
    }

    auto getTramp = [](uint32_t m) -> uint32_t {
        return kMethodTrampolineBase + (m * 8U);
    };

    // 4. Costruzione e popolamento VTable SLObjectItf (Engine, OutputMix, AudioPlayer)
    uint32_t objVtable[16];
    objVtable[0] = getTramp(METHOD_OBJECT_REALIZE);
    objVtable[1] = getTramp(METHOD_OBJECT_RESUME);
    objVtable[2] = getTramp(METHOD_OBJECT_GET_STATE);
    objVtable[3] = getTramp(METHOD_OBJECT_GET_INTERFACE);
    objVtable[4] = getTramp(METHOD_OBJECT_REGISTER_CALLBACK);
    objVtable[5] = getTramp(METHOD_OBJECT_ABORT_ASYNC);
    objVtable[6] = getTramp(METHOD_OBJECT_DESTROY);
    objVtable[7] = getTramp(METHOD_OBJECT_SET_PRIORITY);
    objVtable[8] = getTramp(METHOD_OBJECT_GET_PRIORITY);
    objVtable[9] = getTramp(METHOD_OBJECT_SET_LOSS_OF_CONTROL);
    for (int k = 10; k < 16; ++k) objVtable[k] = getTramp(METHOD_OBJECT_REALIZE);

    uc_mem_write(uc, kVtableObjEngine, objVtable, sizeof(objVtable));
    uc_mem_write(uc, kVtableObjOutputMix, objVtable, sizeof(objVtable));
    uc_mem_write(uc, kVtableObjPlayer, objVtable, sizeof(objVtable));

    // 5. Costruzione VTable SLEngineItf
    uint32_t engineVtable[20];
    engineVtable[0]  = getTramp(METHOD_ENGINE_CREATE_LED);
    engineVtable[1]  = getTramp(METHOD_ENGINE_CREATE_VIBRA);
    engineVtable[2]  = getTramp(METHOD_ENGINE_CREATE_AUDIO_PLAYER);
    engineVtable[3]  = getTramp(METHOD_ENGINE_CREATE_AUDIO_RECORDER);
    engineVtable[4]  = getTramp(METHOD_ENGINE_CREATE_MIDI_PLAYER);
    engineVtable[5]  = getTramp(METHOD_ENGINE_CREATE_LISTENER);
    engineVtable[6]  = getTramp(METHOD_ENGINE_CREATE_3D_GROUP);
    engineVtable[7]  = getTramp(METHOD_ENGINE_CREATE_OUTPUT_MIX);
    engineVtable[8]  = getTramp(METHOD_ENGINE_CREATE_METADATA_EXTRACTOR);
    engineVtable[9]  = getTramp(METHOD_ENGINE_CREATE_EXTENSION_OBJECT);
    engineVtable[10] = getTramp(METHOD_ENGINE_QUERY_NUM_INTERFACES);
    engineVtable[11] = getTramp(METHOD_ENGINE_QUERY_SUPPORTED_INTERFACES);
    engineVtable[12] = getTramp(METHOD_ENGINE_QUERY_NUM_EXTENSIONS);
    engineVtable[13] = getTramp(METHOD_ENGINE_QUERY_SUPPORTED_EXTENSION);
    engineVtable[14] = getTramp(METHOD_ENGINE_IS_EXTENSION_SUPPORTED);
    for (int k = 15; k < 20; ++k) engineVtable[k] = getTramp(METHOD_OBJECT_REALIZE);

    uc_mem_write(uc, kVtableEngine, engineVtable, sizeof(engineVtable));

    // 6. Costruzione VTable SLPlayItf
    uint32_t playVtable[16];
    playVtable[0]  = getTramp(METHOD_PLAY_SET_PLAY_STATE);
    playVtable[1]  = getTramp(METHOD_PLAY_GET_PLAY_STATE);
    playVtable[2]  = getTramp(METHOD_PLAY_GET_DURATION);
    playVtable[3]  = getTramp(METHOD_PLAY_GET_POSITION);
    playVtable[4]  = getTramp(METHOD_PLAY_REGISTER_CALLBACK);
    playVtable[5]  = getTramp(METHOD_PLAY_SET_CALLBACK_EVENTS_MASK);
    playVtable[6]  = getTramp(METHOD_PLAY_GET_CALLBACK_EVENTS_MASK);
    playVtable[7]  = getTramp(METHOD_PLAY_SET_MARKER_POSITION);
    playVtable[8]  = getTramp(METHOD_PLAY_CLEAR_MARKER_POSITION);
    playVtable[9]  = getTramp(METHOD_PLAY_GET_MARKER_POSITION);
    playVtable[10] = getTramp(METHOD_PLAY_SET_POS_UPDATE_PERIOD);
    playVtable[11] = getTramp(METHOD_PLAY_GET_POS_UPDATE_PERIOD);
    for (int k = 12; k < 16; ++k) playVtable[k] = getTramp(METHOD_OBJECT_REALIZE);

    uc_mem_write(uc, kVtablePlay, playVtable, sizeof(playVtable));

    // 7. Costruzione VTable SLAndroidSimpleBufferQueueItf / SLBufferQueueItf
    uint32_t bqVtable[8];
    bqVtable[0] = getTramp(METHOD_BQ_ENQUEUE);
    bqVtable[1] = getTramp(METHOD_BQ_CLEAR);
    bqVtable[2] = getTramp(METHOD_BQ_GET_STATE);
    bqVtable[3] = getTramp(METHOD_BQ_REGISTER_CALLBACK);
    for (int k = 4; k < 8; ++k) bqVtable[k] = getTramp(METHOD_OBJECT_REALIZE);

    uc_mem_write(uc, kVtableBufferQueue, bqVtable, sizeof(bqVtable));

    // 8. Costruzione VTable SLVolumeItf
    uint32_t volVtable[12];
    volVtable[0] = getTramp(METHOD_VOLUME_SET_VOLUME_LEVEL);
    volVtable[1] = getTramp(METHOD_VOLUME_GET_VOLUME_LEVEL);
    volVtable[2] = getTramp(METHOD_VOLUME_GET_MAX_VOLUME_LEVEL);
    volVtable[3] = getTramp(METHOD_VOLUME_SET_MUTE);
    volVtable[4] = getTramp(METHOD_VOLUME_GET_MUTE);
    volVtable[5] = getTramp(METHOD_VOLUME_ENABLE_STEREO_POSITION);
    volVtable[6] = getTramp(METHOD_VOLUME_IS_ENABLED_STEREO_POSITION);
    volVtable[7] = getTramp(METHOD_VOLUME_SET_STEREO_POSITION);
    volVtable[8] = getTramp(METHOD_VOLUME_GET_STEREO_POSITION);
    for (int k = 9; k < 12; ++k) volVtable[k] = getTramp(METHOD_OBJECT_REALIZE);

    uc_mem_write(uc, kVtableVolume, volVtable, sizeof(volVtable));

    // 9. Costruzione VTable SLSeekItf
    uint32_t seekVtable[8];
    seekVtable[0] = getTramp(METHOD_SEEK_SET_POSITION);
    seekVtable[1] = getTramp(METHOD_SEEK_SET_LOOP);
    seekVtable[2] = getTramp(METHOD_SEEK_GET_LOOP);
    for (int k = 3; k < 8; ++k) seekVtable[k] = getTramp(METHOD_OBJECT_REALIZE);

    uc_mem_write(uc, kVtableSeek, seekVtable, sizeof(seekVtable));

    // 10. Collegamento puntatori alle VTable nelle istanze statiche degli oggetti OpenSL ES
    uint32_t ptrObjEngine    = kVtableObjEngine;
    uint32_t ptrEngine       = kVtableEngine;
    uint32_t ptrObjOutputMix = kVtableObjOutputMix;
    uint32_t ptrObjPlayer    = kVtableObjPlayer;
    uint32_t ptrPlay         = kVtablePlay;
    uint32_t ptrBq           = kVtableBufferQueue;
    uint32_t ptrVol          = kVtableVolume;
    uint32_t ptrSeek         = kVtableSeek;

    uc_mem_write(uc, kInstObjEngine,    &ptrObjEngine, 4);
    uc_mem_write(uc, kInstEngine,       &ptrEngine, 4);
    uc_mem_write(uc, kInstObjOutputMix, &ptrObjOutputMix, 4);
    uc_mem_write(uc, kInstObjPlayer,    &ptrObjPlayer, 4);
    uc_mem_write(uc, kInstPlay,         &ptrPlay, 4);
    uc_mem_write(uc, kInstBufferQueue,  &ptrBq, 4);
    uc_mem_write(uc, kInstVolume,       &ptrVol, 4);
    uc_mem_write(uc, kInstSeek,         &ptrSeek, 4);

    // 11. Avvio worker thread di playback audio e streaming host
    if (!isRunning_.load(std::memory_order_relaxed)) {
        isRunning_.store(true);
        playbackThread_ = std::make_unique<std::thread>(&AudioThunker::audioPlaybackWorkerLoop, this);
    }

    LOGI("AudioThunker: Sottosistema Audio OpenSL ES virtualizzato con successo (Engine=0x%08x, Player=0x%08x, BQ=0x%08x)",
         kInstObjEngine, kInstObjPlayer, kInstBufferQueue);
}

void AudioThunker::shutdown() {
    if (isRunning_.load(std::memory_order_relaxed)) {
        isRunning_.store(false);
        queueCv_.notify_all();
        if (playbackThread_ && playbackThread_->joinable()) {
            playbackThread_->join();
        }
        playbackThread_.reset();
    }

    closeHostAudioStream();

    if (callbackUc_) {
        uc_close(callbackUc_);
        callbackUc_ = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        while (!bufferQueue_.empty()) bufferQueue_.pop();
    }
}

// ============================================================================
// Risoluzione dei Simboli del Linker (slCreateEngine & SL_IID_*)
// ============================================================================
uint32_t AudioThunker::resolveInterfaceId(const std::string& name, uc_engine* uc) {
    for (size_t i = 0; i < kNumKnownInterfaces; ++i) {
        if (name == sKnownInterfaces[i].name) {
            uint32_t varAddr = kIidVarBaseAddr + static_cast<uint32_t>(i * 4U);
            LOGI("AudioThunker: Risolto simbolo esportato '%s' -> 0x%08x (Var Pointer)", name.c_str(), varAddr);
            return varAddr;
        }
    }
    return 0;
}

bool AudioThunker::handleAudioCall(const std::string& name, uc_engine* uc) {
    if (!uc) return false;

    if (name == "slCreateEngine") {
        uint32_t pEngineObj = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &pEngineObj);

        if (pEngineObj != 0) {
            uint32_t inst = kInstObjEngine;
            uc_mem_write(uc, pEngineObj, &inst, 4);
        }

        uint32_t res = 0; // SL_RESULT_SUCCESS
        uc_reg_write(uc, UC_ARM_REG_R0, &res);
        LOGI("AudioThunker: slCreateEngine(pEngine=0x%08x) -> SLObjectItf=0x%08x", pEngineObj, kInstObjEngine);
        return true;
    }

    if (name == "slQueryNumSupportedEngineInterfaces") {
        uint32_t pNumSupported = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &pNumSupported);
        if (pNumSupported != 0) {
            uint32_t count = static_cast<uint32_t>(kNumKnownInterfaces);
            uc_mem_write(uc, pNumSupported, &count, 4);
        }
        uint32_t res = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &res);
        return true;
    }

    if (name == "slQuerySupportedEngineInterfaces") {
        uint32_t index = 0, pInterfaceId = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &index);
        uc_reg_read(uc, UC_ARM_REG_R1, &pInterfaceId);

        if (pInterfaceId != 0 && index < kNumKnownInterfaces) {
            uint32_t guidAddr = kIidBaseAddr + (index * 16U);
            uc_mem_write(uc, pInterfaceId, &guidAddr, 4);
        }

        uint32_t res = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &res);
        return true;
    }

    return false;
}

// ============================================================================
// Dispatcher Centrale per i Metodi OpenSL ES Invocati dal Guest
// ============================================================================
void AudioThunker::handleMethodCall(uint32_t methodId, uc_engine* uc) {
    if (!uc) return;

    uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0, sp = 0;
    uc_reg_read(uc, UC_ARM_REG_R0, &r0);
    uc_reg_read(uc, UC_ARM_REG_R1, &r1);
    uc_reg_read(uc, UC_ARM_REG_R2, &r2);
    uc_reg_read(uc, UC_ARM_REG_R3, &r3);
    uc_reg_read(uc, UC_ARM_REG_SP, &sp);

    switch (methodId) {
        // SLObjectItf
        case METHOD_OBJECT_REALIZE:
            handleObjectRealize(uc, r0, r1);
            break;
        case METHOD_OBJECT_RESUME:
        case METHOD_OBJECT_ABORT_ASYNC:
        case METHOD_OBJECT_SET_PRIORITY:
        case METHOD_OBJECT_GET_PRIORITY:
        case METHOD_OBJECT_SET_LOSS_OF_CONTROL: {
            uint32_t res = 0;
            uc_reg_write(uc, UC_ARM_REG_R0, &res);
            break;
        }
        case METHOD_OBJECT_GET_STATE:
            handleObjectGetState(uc, r0, r1);
            break;
        case METHOD_OBJECT_GET_INTERFACE:
            handleObjectGetInterface(uc, r0, r1, r2);
            break;
        case METHOD_OBJECT_DESTROY:
            handleObjectDestroy(uc, r0);
            break;

        // SLEngineItf
        case METHOD_ENGINE_CREATE_AUDIO_PLAYER:
            handleEngineCreateAudioPlayer(uc, r0, r1, r2, r3);
            break;
        case METHOD_ENGINE_CREATE_OUTPUT_MIX:
            handleEngineCreateOutputMix(uc, r0, r1);
            break;
        case METHOD_ENGINE_CREATE_LED:
        case METHOD_ENGINE_CREATE_VIBRA:
        case METHOD_ENGINE_CREATE_AUDIO_RECORDER:
        case METHOD_ENGINE_CREATE_MIDI_PLAYER:
        case METHOD_ENGINE_CREATE_LISTENER:
        case METHOD_ENGINE_CREATE_3D_GROUP:
        case METHOD_ENGINE_CREATE_METADATA_EXTRACTOR:
        case METHOD_ENGINE_CREATE_EXTENSION_OBJECT: {
            if (r1 != 0) {
                uint32_t dummyObj = kInstObjOutputMix;
                uc_mem_write(uc, r1, &dummyObj, 4);
            }
            uint32_t res = 0;
            uc_reg_write(uc, UC_ARM_REG_R0, &res);
            break;
        }
        case METHOD_ENGINE_QUERY_NUM_INTERFACES:
            if (r2 != 0) {
                uint32_t count = 4;
                uc_mem_write(uc, r2, &count, 4);
            }
            { uint32_t res = 0; uc_reg_write(uc, UC_ARM_REG_R0, &res); }
            break;
        case METHOD_ENGINE_QUERY_SUPPORTED_INTERFACES:
            if (r3 != 0) {
                uint32_t guidAddr = kIidBaseAddr;
                uc_mem_write(uc, r3, &guidAddr, 4);
            }
            { uint32_t res = 0; uc_reg_write(uc, UC_ARM_REG_R0, &res); }
            break;
        case METHOD_ENGINE_QUERY_NUM_EXTENSIONS:
            if (r1 != 0) {
                uint32_t zero = 0;
                uc_mem_write(uc, r1, &zero, 4);
            }
            { uint32_t res = 0; uc_reg_write(uc, UC_ARM_REG_R0, &res); }
            break;

        // SLPlayItf
        case METHOD_PLAY_SET_PLAY_STATE:
            handlePlaySetPlayState(uc, r0, r1);
            break;
        case METHOD_PLAY_GET_PLAY_STATE:
            handlePlayGetPlayState(uc, r0, r1);
            break;
        case METHOD_PLAY_GET_DURATION:
            handlePlayGetDuration(uc, r0, r1);
            break;
        case METHOD_PLAY_GET_POSITION:
            handlePlayGetPosition(uc, r0, r1);
            break;
        case METHOD_PLAY_REGISTER_CALLBACK:
        case METHOD_PLAY_SET_CALLBACK_EVENTS_MASK:
        case METHOD_PLAY_GET_CALLBACK_EVENTS_MASK:
        case METHOD_PLAY_SET_MARKER_POSITION:
        case METHOD_PLAY_CLEAR_MARKER_POSITION:
        case METHOD_PLAY_GET_MARKER_POSITION:
        case METHOD_PLAY_SET_POS_UPDATE_PERIOD:
        case METHOD_PLAY_GET_POS_UPDATE_PERIOD: {
            uint32_t res = 0;
            uc_reg_write(uc, UC_ARM_REG_R0, &res);
            break;
        }

        // SLAndroidSimpleBufferQueueItf
        case METHOD_BQ_ENQUEUE:
            handleBqEnqueue(uc, r0, r1, r2);
            break;
        case METHOD_BQ_CLEAR:
            handleBqClear(uc, r0);
            break;
        case METHOD_BQ_GET_STATE:
            handleBqGetState(uc, r0, r1);
            break;
        case METHOD_BQ_REGISTER_CALLBACK:
            handleBqRegisterCallback(uc, r0, r1, r2);
            break;

        // SLVolumeItf
        case METHOD_VOLUME_SET_VOLUME_LEVEL:
            handleVolumeSetLevel(uc, r0, static_cast<int32_t>(r1));
            break;
        case METHOD_VOLUME_GET_VOLUME_LEVEL:
            handleVolumeGetLevel(uc, r0, r1);
            break;
        case METHOD_VOLUME_GET_MAX_VOLUME_LEVEL:
            handleVolumeGetMaxLevel(uc, r0, r1);
            break;
        case METHOD_VOLUME_SET_MUTE:
            handleVolumeSetMute(uc, r0, r1);
            break;
        case METHOD_VOLUME_GET_MUTE:
            handleVolumeGetMute(uc, r0, r1);
            break;
        case METHOD_VOLUME_ENABLE_STEREO_POSITION:
        case METHOD_VOLUME_IS_ENABLED_STEREO_POSITION:
        case METHOD_VOLUME_SET_STEREO_POSITION:
        case METHOD_VOLUME_GET_STEREO_POSITION: {
            uint32_t res = 0;
            uc_reg_write(uc, UC_ARM_REG_R0, &res);
            break;
        }

        // SLSeekItf
        case METHOD_SEEK_SET_POSITION: {
            uint32_t res = 0;
            uc_reg_write(uc, UC_ARM_REG_R0, &res);
            break;
        }
        case METHOD_SEEK_SET_LOOP: {
            loopEnabled_ = (r1 != 0);
            uint32_t res = 0;
            uc_reg_write(uc, UC_ARM_REG_R0, &res);
            break;
        }
        case METHOD_SEEK_GET_LOOP: {
            if (r1 != 0) {
                uint32_t l = loopEnabled_ ? 1 : 0;
                uc_mem_write(uc, r1, &l, 4);
            }
            uint32_t res = 0;
            uc_reg_write(uc, UC_ARM_REG_R0, &res);
            break;
        }

        default: {
            uint32_t res = 0;
            uc_reg_write(uc, UC_ARM_REG_R0, &res);
            break;
        }
    }
}

// ============================================================================
// Implementazioni Specifiche Metodi OpenSL ES
// ============================================================================
void AudioThunker::handleObjectRealize(uc_engine* uc, uint32_t /* self */, uint32_t /* async */) {
    uint32_t res = 0; // SL_RESULT_SUCCESS
    uc_reg_write(uc, UC_ARM_REG_R0, &res);
}

void AudioThunker::handleObjectGetState(uc_engine* uc, uint32_t /* self */, uint32_t pState) {
    if (pState != 0) {
        uint32_t state = 2; // SL_OBJECT_STATE_REALIZED
        uc_mem_write(uc, pState, &state, 4);
    }
    uint32_t res = 0;
    uc_reg_write(uc, UC_ARM_REG_R0, &res);
}

void AudioThunker::handleObjectGetInterface(uc_engine* uc, uint32_t self, uint32_t iid, uint32_t pInterface) {
    if (pInterface == 0 || iid == 0) {
        uint32_t err = 2; // SL_RESULT_PARAMETER_INVALID
        uc_reg_write(uc, UC_ARM_REG_R0, &err);
        return;
    }

    uint32_t guidVal = 0;
    uc_mem_read(uc, iid, &guidVal, 4);

    // Se iid punta a una variabile di puntatore esportata, dereferenzia un livello aggiuntivo
    if (guidVal >= kIidBaseAddr && guidVal < kIidBaseAddr + 0x800U) {
        uint32_t realGuidVal = 0;
        if (uc_mem_read(uc, guidVal, &realGuidVal, 4) == UC_ERR_OK) {
            guidVal = realGuidVal;
        }
    }

    uint32_t outInterface = 0;

    if (self == kInstObjEngine) {
        // Interfaccia SLEngineItf (GUID time_low 0x8d97c260)
        outInterface = kInstEngine;
    } else if (self == kInstObjOutputMix) {
        // OutputMix non espone interfacce di playback diretto, ritorna se stesso
        outInterface = kInstObjOutputMix;
    } else if (self == kInstObjPlayer) {
        // Verifica del tipo di interfaccia richiesta sull'AudioPlayer
        if (guidVal == 0xef0bd9c0U) {
            // SL_IID_PLAY
            outInterface = kInstPlay;
        } else if (guidVal == 0x198e4940U || guidVal == 0x2bc99cc0U) {
            // SL_IID_ANDROIDSIMPLEBUFFERQUEUE o SL_IID_BUFFERQUEUE
            outInterface = kInstBufferQueue;
        } else if (guidVal == 0x09e8ede0U) {
            // SL_IID_VOLUME
            outInterface = kInstVolume;
        } else if (guidVal == 0xd43135a0U) {
            // SL_IID_SEEK
            outInterface = kInstSeek;
        } else {
            // Fallback safe su SLPlayItf per evitare nullptr crash
            outInterface = kInstPlay;
        }
    } else {
        outInterface = self;
    }

    uc_mem_write(uc, pInterface, &outInterface, 4);
    uint32_t res = 0; // SL_RESULT_SUCCESS
    uc_reg_write(uc, UC_ARM_REG_R0, &res);

    LOGI("AudioThunker: GetInterface(self=0x%08x, iid=0x%08x [guidVal=0x%08x]) -> 0x%08x",
         self, iid, guidVal, outInterface);
}

void AudioThunker::handleObjectDestroy(uc_engine* uc, uint32_t self) {
    if (self == kInstObjPlayer) {
        playState_ = 1; // STOPPED
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            while (!bufferQueue_.empty()) bufferQueue_.pop();
        }
        closeHostAudioStream();
        LOGI("AudioThunker: AudioPlayer distrutto, playback hardware arrestato.");
    }
}

void AudioThunker::handleEngineCreateOutputMix(uc_engine* uc, uint32_t /* self */, uint32_t pMix) {
    if (pMix != 0) {
        uint32_t inst = kInstObjOutputMix;
        uc_mem_write(uc, pMix, &inst, 4);
    }
    uint32_t res = 0; // SL_RESULT_SUCCESS
    uc_reg_write(uc, UC_ARM_REG_R0, &res);
    LOGI("AudioThunker: CreateOutputMix -> SLObjectItf=0x%08x", kInstObjOutputMix);
}

void AudioThunker::handleEngineCreateAudioPlayer(
    uc_engine* uc,
    uint32_t /* self */,
    uint32_t pPlayer,
    uint32_t pAudioSrc,
    uint32_t /* pAudioSnk */
) {
    // Parsing dei parametri PCM da SLDataSource se forniti
    if (pAudioSrc != 0) {
        uint32_t pFormat = 0;
        if (uc_mem_read(uc, pAudioSrc + 4, &pFormat, 4) == UC_ERR_OK && pFormat != 0) {
            uint32_t formatType = 0, numChannels = 0, samplesPerSec = 0, bitsPerSample = 0;
            uc_mem_read(uc, pFormat, &formatType, 4);
            uc_mem_read(uc, pFormat + 4, &numChannels, 4);
            uc_mem_read(uc, pFormat + 8, &samplesPerSec, 4);
            uc_mem_read(uc, pFormat + 12, &bitsPerSample, 4);

            if (numChannels >= 1 && numChannels <= 8) channelCount_ = numChannels;
            if (samplesPerSec >= 8000000U && samplesPerSec <= 192000000U) {
                sampleRate_ = samplesPerSec / 1000U;
            }
            if (bitsPerSample == 8 || bitsPerSample == 16 || bitsPerSample == 24 || bitsPerSample == 32) {
                bitsPerSample_ = bitsPerSample;
            }

            LOGI("AudioThunker: Configurazione PCM AudioPlayer: SampleRate=%u Hz, Canali=%u, Bits=%u",
                 sampleRate_, channelCount_, bitsPerSample_);
        }
    }

    if (pPlayer != 0) {
        uint32_t inst = kInstObjPlayer;
        uc_mem_write(uc, pPlayer, &inst, 4);
    }

    // Inizializza lo stream audio host nativo tramite AAudio
    initHostAudioStream(sampleRate_, channelCount_);

    uint32_t res = 0; // SL_RESULT_SUCCESS
    uc_reg_write(uc, UC_ARM_REG_R0, &res);
    LOGI("AudioThunker: CreateAudioPlayer -> SLObjectItf=0x%08x", kInstObjPlayer);
}

void AudioThunker::handlePlaySetPlayState(uc_engine* uc, uint32_t /* self */, uint32_t state) {
    playState_ = state;
    if (state == 3) { // SL_PLAYSTATE_PLAYING
        queueCv_.notify_all();
        LOGI("AudioThunker: SetPlayState -> SL_PLAYSTATE_PLAYING (3)");
    } else if (state == 2) {
        LOGI("AudioThunker: SetPlayState -> SL_PLAYSTATE_PAUSED (2)");
    } else {
        LOGI("AudioThunker: SetPlayState -> SL_PLAYSTATE_STOPPED (1)");
    }
    uint32_t res = 0;
    uc_reg_write(uc, UC_ARM_REG_R0, &res);
}

void AudioThunker::handlePlayGetPlayState(uc_engine* uc, uint32_t /* self */, uint32_t pState) {
    if (pState != 0) {
        uc_mem_write(uc, pState, &playState_, 4);
    }
    uint32_t res = 0;
    uc_reg_write(uc, UC_ARM_REG_R0, &res);
}

void AudioThunker::handlePlayGetDuration(uc_engine* uc, uint32_t /* self */, uint32_t pMsec) {
    if (pMsec != 0) {
        uint32_t unknown = 0xFFFFFFFFU; // SL_TIME_UNKNOWN
        uc_mem_write(uc, pMsec, &unknown, 4);
    }
    uint32_t res = 0;
    uc_reg_write(uc, UC_ARM_REG_R0, &res);
}

void AudioThunker::handlePlayGetPosition(uc_engine* uc, uint32_t /* self */, uint32_t pMsec) {
    if (pMsec != 0) {
        uint32_t posMs = static_cast<uint32_t>((totalFramesPlayed_ * 1000ULL) / (sampleRate_ ? sampleRate_ : 44100));
        uc_mem_write(uc, pMsec, &posMs, 4);
    }
    uint32_t res = 0;
    uc_reg_write(uc, UC_ARM_REG_R0, &res);
}

void AudioThunker::handleBqRegisterCallback(uc_engine* uc, uint32_t /* self */, uint32_t callback, uint32_t pContext) {
    bqCallback_ = callback;
    bqContext_  = pContext;
    LOGI("AudioThunker: Registrato callback BufferQueue guest: Fn=0x%08x, Context=0x%08x", callback, pContext);
    uint32_t res = 0;
    uc_reg_write(uc, UC_ARM_REG_R0, &res);
}

void AudioThunker::handleBqEnqueue(uc_engine* uc, uint32_t /* self */, uint32_t pBuffer, uint32_t size) {
    if (pBuffer == 0 || size == 0) {
        uint32_t err = 2; // SL_RESULT_PARAMETER_INVALID
        uc_reg_write(uc, UC_ARM_REG_R0, &err);
        return;
    }

    AudioPcmBuffer buf;
    buf.guestBufferPtr = pBuffer;
    buf.size = size;
    buf.data.resize(size);

    if (uc_mem_read(uc, pBuffer, buf.data.data(), size) == UC_ERR_OK) {
        std::lock_guard<std::mutex> lock(queueMutex_);
        bufferQueue_.push(std::move(buf));
        queueCv_.notify_one();
    } else {
        LOGE("AudioThunker: Enqueue lettura memoria guest fallita a 0x%08x (size=%u)", pBuffer, size);
    }

    uint32_t res = 0; // SL_RESULT_SUCCESS
    uc_reg_write(uc, UC_ARM_REG_R0, &res);
}

void AudioThunker::handleBqClear(uc_engine* uc, uint32_t /* self */) {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        while (!bufferQueue_.empty()) bufferQueue_.pop();
    }
    uint32_t res = 0;
    uc_reg_write(uc, UC_ARM_REG_R0, &res);
}

void AudioThunker::handleBqGetState(uc_engine* uc, uint32_t /* self */, uint32_t pState) {
    if (pState != 0) {
        std::lock_guard<std::mutex> lock(queueMutex_);
        uint32_t count = static_cast<uint32_t>(bufferQueue_.size());
        uint32_t index = playIndex_;
        uint32_t stateData[2] = { count, index };
        uc_mem_write(uc, pState, stateData, sizeof(stateData));
    }
    uint32_t res = 0;
    uc_reg_write(uc, UC_ARM_REG_R0, &res);
}

void AudioThunker::handleVolumeSetLevel(uc_engine* uc, uint32_t /* self */, int32_t level) {
    volumeLevel_ = level;
    uint32_t res = 0;
    uc_reg_write(uc, UC_ARM_REG_R0, &res);
}

void AudioThunker::handleVolumeGetLevel(uc_engine* uc, uint32_t /* self */, uint32_t pLevel) {
    if (pLevel != 0) {
        uc_mem_write(uc, pLevel, &volumeLevel_, 4);
    }
    uint32_t res = 0;
    uc_reg_write(uc, UC_ARM_REG_R0, &res);
}

void AudioThunker::handleVolumeGetMaxLevel(uc_engine* uc, uint32_t /* self */, uint32_t pMaxLevel) {
    if (pMaxLevel != 0) {
        uint32_t maxLvl = 0; // 0 mB (massimo livello OpenSL ES)
        uc_mem_write(uc, pMaxLevel, &maxLvl, 4);
    }
    uint32_t res = 0;
    uc_reg_write(uc, UC_ARM_REG_R0, &res);
}

void AudioThunker::handleVolumeSetMute(uc_engine* uc, uint32_t /* self */, uint32_t mute) {
    isMuted_ = (mute != 0);
    uint32_t res = 0;
    uc_reg_write(uc, UC_ARM_REG_R0, &res);
}

void AudioThunker::handleVolumeGetMute(uc_engine* uc, uint32_t /* self */, uint32_t pMute) {
    if (pMute != 0) {
        uint32_t m = isMuted_ ? 1 : 0;
        uc_mem_write(uc, pMute, &m, 4);
    }
    uint32_t res = 0;
    uc_reg_write(uc, UC_ARM_REG_R0, &res);
}

// ============================================================================
// Driver AAudio Host & Streaming Hardware Passthrough
// ============================================================================
bool AudioThunker::initHostAudioStream(uint32_t sampleRate, uint32_t channels) {
    closeHostAudioStream();

    if (!aaudioLib_) {
        aaudioLib_ = dlopen("libaaudio.so", RTLD_NOW);
        if (aaudioLib_) {
            sAAudio_createStreamBuilder             = reinterpret_cast<fn_AAudio_createStreamBuilder>(dlsym(aaudioLib_, "AAudio_createStreamBuilder"));
            sAAudioStreamBuilder_setSampleRate      = reinterpret_cast<fn_AAudioStreamBuilder_setSampleRate>(dlsym(aaudioLib_, "AAudioStreamBuilder_setSampleRate"));
            sAAudioStreamBuilder_setChannelCount    = reinterpret_cast<fn_AAudioStreamBuilder_setChannelCount>(dlsym(aaudioLib_, "AAudioStreamBuilder_setChannelCount"));
            sAAudioStreamBuilder_setFormat          = reinterpret_cast<fn_AAudioStreamBuilder_setFormat>(dlsym(aaudioLib_, "AAudioStreamBuilder_setFormat"));
            sAAudioStreamBuilder_setPerformanceMode = reinterpret_cast<fn_AAudioStreamBuilder_setPerformanceMode>(dlsym(aaudioLib_, "AAudioStreamBuilder_setPerformanceMode"));
            sAAudioStreamBuilder_openStream         = reinterpret_cast<fn_AAudioStreamBuilder_openStream>(dlsym(aaudioLib_, "AAudioStreamBuilder_openStream"));
            sAAudioStreamBuilder_delete             = reinterpret_cast<fn_AAudioStreamBuilder_delete>(dlsym(aaudioLib_, "AAudioStreamBuilder_delete"));
            sAAudioStream_requestStart              = reinterpret_cast<fn_AAudioStream_requestStart>(dlsym(aaudioLib_, "AAudioStream_requestStart"));
            sAAudioStream_requestStop               = reinterpret_cast<fn_AAudioStream_requestStop>(dlsym(aaudioLib_, "AAudioStream_requestStop"));
            sAAudioStream_close                     = reinterpret_cast<fn_AAudioStream_close>(dlsym(aaudioLib_, "AAudioStream_close"));
            sAAudioStream_write                     = reinterpret_cast<fn_AAudioStream_write>(dlsym(aaudioLib_, "AAudioStream_write"));
        }
    }

    if (!sAAudio_createStreamBuilder || !sAAudioStreamBuilder_openStream) {
        LOGW("AudioThunker: Driver hardware AAudio non disponibile sul sistema host, fallback su timing virtuale.");
        return false;
    }

    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t res = sAAudio_createStreamBuilder(&builder);
    if (res != AAUDIO_OK || !builder) {
        LOGW("AudioThunker: AAudio_createStreamBuilder fallito (code=%d)", res);
        return false;
    }

    sAAudioStreamBuilder_setSampleRate(builder, static_cast<int32_t>(sampleRate));
    sAAudioStreamBuilder_setChannelCount(builder, static_cast<int32_t>(channels));
    sAAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    sAAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);

    AAudioStream* stream = nullptr;
    res = sAAudioStreamBuilder_openStream(builder, &stream);
    sAAudioStreamBuilder_delete(builder);

    if (res == AAUDIO_OK && stream) {
        sAAudioStream_requestStart(stream);
        aaudioStream_ = stream;
        LOGI("AudioThunker: Stream hardware AAudio aperto con successo (%u Hz, %u canali, Low-Latency)",
             sampleRate, channels);
        return true;
    }

    LOGW("AudioThunker: AAudioStreamBuilder_openStream fallito (code=%d)", res);
    return false;
}

void AudioThunker::closeHostAudioStream() {
    if (aaudioStream_) {
        if (sAAudioStream_requestStop) sAAudioStream_requestStop(static_cast<AAudioStream*>(aaudioStream_));
        if (sAAudioStream_close) sAAudioStream_close(static_cast<AAudioStream*>(aaudioStream_));
        aaudioStream_ = nullptr;
    }
}

// ============================================================================
// Worker Loop Playback Asincrono & Invocazione Callback Guest
// ============================================================================
void AudioThunker::audioPlaybackWorkerLoop() {
    LOGI("AudioThunker: Audio Playback Worker Thread avviato.");

    while (isRunning_.load(std::memory_order_relaxed)) {
        AudioPcmBuffer buf;

        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.wait(lock, [this]() {
                return !isRunning_.load(std::memory_order_relaxed) ||
                       (playState_ == 3 && !bufferQueue_.empty());
            });

            if (!isRunning_.load(std::memory_order_relaxed)) break;

            if (playState_ != 3 || bufferQueue_.empty()) {
                continue;
            }

            buf = std::move(bufferQueue_.front());
            bufferQueue_.pop();
        }

        uint32_t bytesPerFrame = channelCount_ * (bitsPerSample_ / 8U);
        if (bytesPerFrame == 0) bytesPerFrame = 4;
        uint32_t numFrames = buf.size / bytesPerFrame;
        uint64_t durationUs = (static_cast<uint64_t>(numFrames) * 1000000ULL) / (sampleRate_ ? sampleRate_ : 44100);

        // Streaming verso driver audio hardware host o calcolo clock deterministico
        if (aaudioStream_ && sAAudioStream_write && !isMuted_) {
            int64_t timeoutNanos = static_cast<int64_t>(durationUs * 1500ULL);
            int32_t written = sAAudioStream_write(
                static_cast<AAudioStream*>(aaudioStream_),
                buf.data.data(),
                static_cast<int32_t>(numFrames),
                timeoutNanos
            );
            if (written < 0) {
                // In caso di underrun temporaneo del driver audio, attendi la durata naturale
                std::this_thread::sleep_for(std::chrono::microseconds(durationUs));
            }
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(durationUs));
        }

        playIndex_++;
        totalFramesPlayed_ += numFrames;

        // Invocazione del callback di fine buffer registrato dal motore di gioco
        uint32_t cb = bqCallback_;
        uint32_t ctx = bqContext_;
        if (cb != 0) {
            executeGuestCallback(cb, kInstBufferQueue, ctx);
        }
    }

    LOGI("AudioThunker: Audio Playback Worker Thread arrestato regolarmente.");
}

void AudioThunker::executeGuestCallback(uint32_t callbackFn, uint32_t bqItf, uint32_t context) {
    if (callbackFn == 0) return;

    std::lock_guard<std::mutex> lock(audioMutex_);

    if (!callbackUc_) {
        uc_err err = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &callbackUc_);
        if (err != UC_ERR_OK || !callbackUc_) {
            LOGE("AudioThunker: Creazione uc_engine per callback fallita: %s", uc_strerror(err));
            return;
        }

        // Registrazione hook SVC e syscall per il contesto CPU del worker audio
        uc_hook h;
        uc_hook_add(callbackUc_, &h, UC_HOOK_INTR, reinterpret_cast<void*>(audioInterruptCallback), nullptr, 1, 0);
    }

    uint32_t sp   = kAudioStackBase + kAudioStackSize - 64;
    uint32_t lr   = 0xDEADBEEFU;
    uint32_t r0   = bqItf;
    uint32_t r1   = context;
    uint32_t cpsr = (callbackFn & 1U) ? 0x00000030U : 0x00000010U; // Thumb vs ARM
    uint32_t tls  = kAudioTlsAddr;

    uc_reg_write(callbackUc_, UC_ARM_REG_R0, &r0);
    uc_reg_write(callbackUc_, UC_ARM_REG_R1, &r1);
    uc_reg_write(callbackUc_, UC_ARM_REG_SP, &sp);
    uc_reg_write(callbackUc_, UC_ARM_REG_LR, &lr);
    uc_reg_write(callbackUc_, UC_ARM_REG_CPSR, &cpsr);
    uc_reg_write(callbackUc_, UC_ARM_REG_C13_C0_2, &tls);
    uc_reg_write(callbackUc_, UC_ARM_REG_C13_C0_3, &tls);

    uc_err err = uc_emu_start(callbackUc_, callbackFn, 0xDEADBEEFU, 0, 0);
    if (err != UC_ERR_OK && err != UC_ERR_FETCH_UNMAPPED) {
        LOGW("AudioThunker: Callback guest (0x%08x) terminato con status=%d (%s)",
             callbackFn, err, uc_strerror(err));
    }
}

} // namespace emux
