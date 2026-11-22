#pragma once

#include <cstdint>

namespace emux {
namespace guest_layout {

// ============================================================================
// Single Source of Truth: Guest Runtime Region (0x7D000000 .. 0x7D00FFFF, 64KB)
// Ogni sottosistema possiede pagine dedicate e verificate con static_assert.
// ============================================================================

constexpr uint32_t kRuntimeRegionBase       = 0x7D000000U;
constexpr uint32_t kRuntimeRegionSize       = 0x00010000U; // 64 KB
constexpr uint32_t kRuntimeRegionEnd        = kRuntimeRegionBase + kRuntimeRegionSize;

// Pagina 0 (0x7D000000 .. 0x7D000FFF): JavaVM32 + JNIEnv32 + vtables
constexpr uint32_t kJavaVmAddr              = 0x7D000000U;
constexpr uint32_t kJavaVmVtableAddr        = 0x7D000010U;
constexpr uint32_t kJavaVmStubsBase         = 0x7D000100U; // 8 * 8B = 64B
constexpr uint32_t kJniEnvAddr              = 0x7D000200U;
constexpr uint32_t kJniEnvVtableAddr        = 0x7D000220U; // 256 * 4B = 1024B (..0x7D000620)

// Pagina 1 (0x7D001000 .. 0x7D001FFF): ESCLUSIVO per i 256 stub SVC di JNINativeInterface32
constexpr uint32_t kJniEnvStubsBase         = 0x7D001000U;
constexpr uint32_t kJniEnvStubsEnd          = 0x7D001800U; // 256 * 8B = 2048B

// Pagina 2 (0x7D002000 .. 0x7D002FFF): Bionic globals + mock ANativeWindow
constexpr uint32_t kErrnoAddr               = 0x7D002000U;
constexpr uint32_t kStackChkGuardAddr       = 0x7D002010U;
constexpr uint32_t kSFileAddr               = 0x7D002100U;
constexpr uint32_t kCtypeTableAddr          = 0x7D002400U;
constexpr uint32_t kNativeWindowAddr        = 0x7D002800U;
constexpr uint32_t kNativeWindowStubAddr    = 0x7D002900U;

// Pagina 3 (0x7D003000 .. 0x7D003FFF): ANativeActivity & Callbacks
constexpr uint32_t kNativeActivityAddr      = 0x7D003000U;
constexpr uint32_t kNativeActivityCbAddr    = 0x7D003200U;

// Pagine 5..6 (0x7D005000 .. 0x7D006FFF): NDK Mock Handles
constexpr uint32_t kAssetManagerHandle      = 0x7D005000U;
constexpr uint32_t kResourcesHandle         = 0x7D005100U;
constexpr uint32_t kDisplayMetricsHandle    = 0x7D005200U;
constexpr uint32_t kWindowManagerHandle     = 0x7D005300U;
constexpr uint32_t kDisplayHandle           = 0x7D005400U;
constexpr uint32_t kConfigurationHandle     = 0x7D006000U;

// Pagine 8..11 (0x7D008000 .. 0x7D00BFFF): ESCLUSIVO per OpenSL ES AudioThunker
constexpr uint32_t kAudioIidBaseAddr        = 0x7D008000U;
constexpr uint32_t kAudioIidVarBaseAddr     = 0x7D008400U;
constexpr uint32_t kAudioVtableObjEngine    = 0x7D009000U;
constexpr uint32_t kAudioVtableEngine       = 0x7D009200U;
constexpr uint32_t kAudioVtableObjOutputMix = 0x7D009400U;
constexpr uint32_t kAudioVtableObjPlayer    = 0x7D009600U;
constexpr uint32_t kAudioVtablePlay         = 0x7D009800U;
constexpr uint32_t kAudioVtableBufferQueue  = 0x7D009A00U;
constexpr uint32_t kAudioVtableVolume       = 0x7D009C00U;
constexpr uint32_t kAudioVtableSeek         = 0x7D009D00U;
constexpr uint32_t kAudioMethodTrampBase    = 0x7D009E00U;
constexpr uint32_t kAudioInstObjEngine      = 0x7D00A000U;
constexpr uint32_t kAudioInstEngine         = 0x7D00A010U;
constexpr uint32_t kAudioInstObjOutputMix   = 0x7D00A020U;
constexpr uint32_t kAudioInstObjPlayer      = 0x7D00A030U;
constexpr uint32_t kAudioInstPlay           = 0x7D00A040U;
constexpr uint32_t kAudioInstBufferQueue    = 0x7D00A050U;
constexpr uint32_t kAudioInstVolume         = 0x7D00A060U;
constexpr uint32_t kAudioInstSeek           = 0x7D00A070U;

// Verifiche compile-time anti-collisione
static_assert(kJniEnvVtableAddr + 256U * 4U <= kJniEnvStubsBase,
              "Collisione: JNIEnv vtable invade JNIEnv stubs");
static_assert(kJniEnvStubsEnd <= kErrnoAddr,
              "Collisione: JNIEnv stubs invade Bionic globals");
static_assert(kNativeWindowStubAddr + 0x100U <= kNativeActivityAddr,
              "Collisione: ANativeWindow stub invade ANativeActivity");
static_assert(kNativeActivityCbAddr + 0x200U <= kAssetManagerHandle,
              "Collisione: ANativeActivity invade NDK handles");
static_assert(kConfigurationHandle + 0x1000U <= kAudioIidBaseAddr,
              "Collisione: NDK handles invade AudioThunker");
static_assert(kAudioInstSeek + 0x100U <= kRuntimeRegionEnd,
              "Collisione: AudioThunker eccede regione runtime 64KB");

} // namespace guest_layout
} // namespace emux
