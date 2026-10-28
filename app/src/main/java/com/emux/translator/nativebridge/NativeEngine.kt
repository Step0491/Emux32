package com.emux.translator.nativebridge

import android.util.Log

object NativeEngine {
    private const val TAG = "NativeEngine"

    init {
        try {
            System.loadLibrary("emux_engine")
            Log.i(TAG, "Libreria nativa emux_engine caricata con successo.")
        } catch (e: UnsatisfiedLinkError) {
            Log.e(TAG, "Impossibile caricare la libreria nativa emux_engine", e)
        }
    }

    external fun nativeInitEngine(guestRamMb: Long): Boolean
    external fun nativeConfigurePerformance(targetFps: Int, enableFhd: Boolean, throttleGuard: Boolean): Boolean
    external fun nativeSetStoragePath(path: String): Boolean
    external fun nativeSetGameContext(
        packageName: String,
        apkPath: String,
        obbDir: String,
        nativeLibDir: String,
        filesDir: String
    )
    external fun nativeLoadElfLibrary(libraryPath: String): Boolean
    external fun startEmulationNative(): Boolean
    external fun nativeExecuteEntryPoint(): Boolean
    external fun nativeStopEmulation()
    external fun nativeSetSurface(surface: Any?)
    external fun nativeSendTouchEvent(action: Int, x: Float, y: Float, pointerId: Int): Boolean
    external fun nativeReleaseEngine()
    external fun nativeRunSelfTest(): String
}
