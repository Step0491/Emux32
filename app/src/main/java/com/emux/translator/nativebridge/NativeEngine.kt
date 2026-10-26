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
    external fun nativeLoadElfLibrary(libraryPath: String): Boolean
    external fun nativeExecuteEntryPoint(): Boolean
    external fun nativeReleaseEngine()
}
