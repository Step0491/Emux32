#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <mutex>
#include <vector>
#include <queue>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <memory>
#include "unicorn/unicorn.h"

namespace emux {

struct GuestOpenSLGuid {
    uint32_t time_low;
    uint16_t time_mid;
    uint16_t time_hi_and_version;
    uint16_t clock_seq;
    uint8_t  node[6];
};

struct AudioPcmBuffer {
    std::vector<uint8_t> data;
    uint32_t guestBufferPtr{0};
    uint32_t size{0};
};

enum OpenSLMethodId : uint32_t {
    // SLObjectItf methods (0..9)
    METHOD_OBJECT_REALIZE                   = 0,
    METHOD_OBJECT_RESUME                    = 1,
    METHOD_OBJECT_GET_STATE                 = 2,
    METHOD_OBJECT_GET_INTERFACE             = 3,
    METHOD_OBJECT_REGISTER_CALLBACK         = 4,
    METHOD_OBJECT_ABORT_ASYNC               = 5,
    METHOD_OBJECT_DESTROY                   = 6,
    METHOD_OBJECT_SET_PRIORITY              = 7,
    METHOD_OBJECT_GET_PRIORITY              = 8,
    METHOD_OBJECT_SET_LOSS_OF_CONTROL       = 9,

    // SLEngineItf methods (10..24)
    METHOD_ENGINE_CREATE_LED                = 10,
    METHOD_ENGINE_CREATE_VIBRA              = 11,
    METHOD_ENGINE_CREATE_AUDIO_PLAYER       = 12,
    METHOD_ENGINE_CREATE_AUDIO_RECORDER     = 13,
    METHOD_ENGINE_CREATE_MIDI_PLAYER        = 14,
    METHOD_ENGINE_CREATE_LISTENER           = 15,
    METHOD_ENGINE_CREATE_3D_GROUP           = 16,
    METHOD_ENGINE_CREATE_OUTPUT_MIX         = 17,
    METHOD_ENGINE_CREATE_METADATA_EXTRACTOR = 18,
    METHOD_ENGINE_CREATE_EXTENSION_OBJECT   = 19,
    METHOD_ENGINE_QUERY_NUM_INTERFACES      = 20,
    METHOD_ENGINE_QUERY_SUPPORTED_INTERFACES= 21,
    METHOD_ENGINE_QUERY_NUM_EXTENSIONS      = 22,
    METHOD_ENGINE_QUERY_SUPPORTED_EXTENSION = 23,
    METHOD_ENGINE_IS_EXTENSION_SUPPORTED    = 24,

    // SLPlayItf methods (30..41)
    METHOD_PLAY_SET_PLAY_STATE              = 30,
    METHOD_PLAY_GET_PLAY_STATE              = 31,
    METHOD_PLAY_GET_DURATION                = 32,
    METHOD_PLAY_GET_POSITION                = 33,
    METHOD_PLAY_REGISTER_CALLBACK           = 34,
    METHOD_PLAY_SET_CALLBACK_EVENTS_MASK    = 35,
    METHOD_PLAY_GET_CALLBACK_EVENTS_MASK    = 36,
    METHOD_PLAY_SET_MARKER_POSITION         = 37,
    METHOD_PLAY_CLEAR_MARKER_POSITION       = 38,
    METHOD_PLAY_GET_MARKER_POSITION         = 39,
    METHOD_PLAY_SET_POS_UPDATE_PERIOD       = 40,
    METHOD_PLAY_GET_POS_UPDATE_PERIOD       = 41,

    // SLAndroidSimpleBufferQueueItf / SLBufferQueueItf (50..53)
    METHOD_BQ_ENQUEUE                       = 50,
    METHOD_BQ_CLEAR                         = 51,
    METHOD_BQ_GET_STATE                     = 52,
    METHOD_BQ_REGISTER_CALLBACK             = 53,

    // SLVolumeItf (60..68)
    METHOD_VOLUME_SET_VOLUME_LEVEL          = 60,
    METHOD_VOLUME_GET_VOLUME_LEVEL          = 61,
    METHOD_VOLUME_GET_MAX_VOLUME_LEVEL      = 62,
    METHOD_VOLUME_SET_MUTE                  = 63,
    METHOD_VOLUME_GET_MUTE                  = 64,
    METHOD_VOLUME_ENABLE_STEREO_POSITION    = 65,
    METHOD_VOLUME_IS_ENABLED_STEREO_POSITION= 66,
    METHOD_VOLUME_SET_STEREO_POSITION       = 67,
    METHOD_VOLUME_GET_STEREO_POSITION       = 68,

    // SLSeekItf (70..72)
    METHOD_SEEK_SET_POSITION                = 70,
    METHOD_SEEK_SET_LOOP                    = 71,
    METHOD_SEEK_GET_LOOP                    = 72,
};

class AudioThunker {
public:
    static AudioThunker& getInstance();

    void initialize(uc_engine* uc);
    void shutdown();

    bool handleAudioCall(const std::string& name, uc_engine* uc);
    void handleMethodCall(uint32_t methodId, uc_engine* uc);
    uint32_t resolveInterfaceId(const std::string& name, uc_engine* uc);

    // Gestori specifici per le chiamate degli oggetti OpenSL ES
    void handleObjectRealize(uc_engine* uc, uint32_t self, uint32_t async);
    void handleObjectGetState(uc_engine* uc, uint32_t self, uint32_t pState);
    void handleObjectGetInterface(uc_engine* uc, uint32_t self, uint32_t iid, uint32_t pInterface);
    void handleObjectDestroy(uc_engine* uc, uint32_t self);

    void handleEngineCreateAudioPlayer(uc_engine* uc, uint32_t self, uint32_t pPlayer, uint32_t pAudioSrc, uint32_t pAudioSnk);
    void handleEngineCreateOutputMix(uc_engine* uc, uint32_t self, uint32_t pMix);

    void handlePlaySetPlayState(uc_engine* uc, uint32_t self, uint32_t state);
    void handlePlayGetPlayState(uc_engine* uc, uint32_t self, uint32_t pState);
    void handlePlayGetDuration(uc_engine* uc, uint32_t self, uint32_t pMsec);
    void handlePlayGetPosition(uc_engine* uc, uint32_t self, uint32_t pMsec);

    void handleBqEnqueue(uc_engine* uc, uint32_t self, uint32_t pBuffer, uint32_t size);
    void handleBqClear(uc_engine* uc, uint32_t self);
    void handleBqGetState(uc_engine* uc, uint32_t self, uint32_t pState);
    void handleBqRegisterCallback(uc_engine* uc, uint32_t self, uint32_t callback, uint32_t pContext);

    void handleVolumeSetLevel(uc_engine* uc, uint32_t self, int32_t level);
    void handleVolumeGetLevel(uc_engine* uc, uint32_t self, uint32_t pLevel);
    void handleVolumeGetMaxLevel(uc_engine* uc, uint32_t self, uint32_t pMaxLevel);
    void handleVolumeSetMute(uc_engine* uc, uint32_t self, uint32_t mute);
    void handleVolumeGetMute(uc_engine* uc, uint32_t self, uint32_t pMute);

private:
    AudioThunker() = default;
    ~AudioThunker() { shutdown(); }

    void audioPlaybackWorkerLoop();
    void executeGuestCallback(uint32_t callbackFn, uint32_t bqItf, uint32_t context);
    bool initHostAudioStream(uint32_t sampleRate, uint32_t channels);
    void closeHostAudioStream();

    // Mapping e costanti OpenSL ES Guest Memory
    static constexpr uint32_t kIidBaseAddr          = 0x7D001000U;
    static constexpr uint32_t kIidVarBaseAddr       = 0x7D001400U;
    static constexpr uint32_t kVtableObjEngine      = 0x7D003000U;
    static constexpr uint32_t kVtableEngine         = 0x7D003200U;
    static constexpr uint32_t kVtableObjOutputMix   = 0x7D003400U;
    static constexpr uint32_t kVtableObjPlayer      = 0x7D003600U;
    static constexpr uint32_t kVtablePlay           = 0x7D003800U;
    static constexpr uint32_t kVtableBufferQueue    = 0x7D003A00U;
    static constexpr uint32_t kVtableVolume         = 0x7D003C00U;
    static constexpr uint32_t kVtableSeek           = 0x7D003D00U;
    static constexpr uint32_t kMethodTrampolineBase = 0x7D003E00U;

    static constexpr uint32_t kInstObjEngine        = 0x7D004000U;
    static constexpr uint32_t kInstEngine           = 0x7D004010U;
    static constexpr uint32_t kInstObjOutputMix     = 0x7D004020U;
    static constexpr uint32_t kInstObjPlayer        = 0x7D004030U;
    static constexpr uint32_t kInstPlay             = 0x7D004040U;
    static constexpr uint32_t kInstBufferQueue      = 0x7D004050U;
    static constexpr uint32_t kInstVolume           = 0x7D004060U;
    static constexpr uint32_t kInstSeek             = 0x7D004070U;

    static constexpr uint32_t kAudioStackBase       = 0x79E00000U;
    static constexpr uint32_t kAudioStackSize       = 0x00100000U; // 1MB
    static constexpr uint32_t kAudioTlsAddr         = 0x79D00000U;

    std::mutex audioMutex_;
    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::queue<AudioPcmBuffer> bufferQueue_;
    std::unique_ptr<std::thread> playbackThread_;
    std::atomic<bool> isRunning_{false};

    uc_engine* callbackUc_{nullptr};

    uint32_t sampleRate_{44100};
    uint32_t channelCount_{2};
    uint32_t bitsPerSample_{16};

    uint32_t playState_{1}; // 1 = SL_PLAYSTATE_STOPPED, 2 = PAUSED, 3 = PLAYING
    uint32_t bqCallback_{0};
    uint32_t bqContext_{0};
    uint32_t playIndex_{0};
    uint64_t totalFramesPlayed_{0};

    int32_t volumeLevel_{0};
    bool isMuted_{false};
    bool loopEnabled_{false};

    void* aaudioStream_{nullptr};
    void* aaudioLib_{nullptr};
};

} // namespace emux
