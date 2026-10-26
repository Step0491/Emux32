package com.emux.translator.util

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import java.io.File
import java.io.FileOutputStream
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

object EmuxLogger {
    private const val DEFAULT_TAG = "Emux32"
    private val timeFormat = SimpleDateFormat("HH:mm:ss.SSS", Locale.US)

    private val _logs = MutableStateFlow<List<String>>(emptyList())
    val logs: StateFlow<List<String>> = _logs.asStateFlow()

    private val logBuffer = mutableListOf<String>()
    private const val MAX_LOG_LINES = 2000

    private var primaryLogFile: File? = null
    private var externalLogFile: File? = null
    private var isCapturingLogcat = false

    fun initLogFiles(internalCacheDir: File, externalEmuxDir: File?) {
        try {
            // 1. Log interno sempre disponibile (privato sandbox dell'app)
            primaryLogFile = File(internalCacheDir, "emux_execution.log")
            if (primaryLogFile?.exists() == true) {
                primaryLogFile?.delete()
            }
            primaryLogFile?.createNewFile()

            // 2. Log pubblico richiesto in Emux32/log/emux_execution.log
            if (externalEmuxDir != null) {
                val logDir = File(externalEmuxDir, "log")
                if (!logDir.exists()) {
                    logDir.mkdirs()
                }
                externalLogFile = File(logDir, "emux_execution.log")
                if (externalLogFile?.exists() == true) {
                    externalLogFile?.delete()
                }
                externalLogFile?.createNewFile()
                Log.i(DEFAULT_TAG, "File di log pubblico configurato: ${externalLogFile?.absolutePath}")
            }
        } catch (e: Exception) {
            Log.e(DEFAULT_TAG, "Impossibile inizializzare file di log: ${e.message}")
        }
    }

    @Synchronized
    private fun appendLog(level: String, tag: String, message: String) {
        val timestamp = timeFormat.format(Date())
        val formattedLine = "[$timestamp][$level][$tag] $message"

        if (logBuffer.size >= MAX_LOG_LINES) {
            logBuffer.removeAt(0)
        }
        logBuffer.add(formattedLine)
        _logs.value = logBuffer.toList()

        writeToFileSafe(formattedLine)
    }

    @Synchronized
    fun appendRawLog(rawLine: String) {
        if (logBuffer.size >= MAX_LOG_LINES) {
            logBuffer.removeAt(0)
        }
        logBuffer.add(rawLine)
        _logs.value = logBuffer.toList()

        writeToFileSafe(rawLine)
    }

    private fun writeToFileSafe(line: String) {
        // Scrittura file primario sandbox
        try {
            primaryLogFile?.let { file ->
                FileOutputStream(file, true).bufferedWriter().use { writer ->
                    writer.appendLine(line)
                }
            }
        } catch (_: Exception) {}

        // Scrittura file pubblico in Emux32/log
        try {
            externalLogFile?.let { file ->
                FileOutputStream(file, true).bufferedWriter().use { writer ->
                    writer.appendLine(line)
                }
            }
        } catch (_: Exception) {}
    }

    fun i(tag: String = DEFAULT_TAG, message: String) {
        Log.i(tag, message)
        appendLog("INFO", tag, message)
    }

    fun w(tag: String = DEFAULT_TAG, message: String) {
        Log.w(tag, message)
        appendLog("WARN", tag, message)
    }

    fun e(tag: String = DEFAULT_TAG, message: String, throwable: Throwable? = null) {
        val fullMsg = if (throwable != null) "$message | ${throwable.message}\n${Log.getStackTraceString(throwable)}" else message
        Log.e(tag, fullMsg)
        appendLog("ERROR", tag, fullMsg)
    }

    fun clear() {
        synchronized(this) {
            logBuffer.clear()
            _logs.value = emptyList()
            try {
                primaryLogFile?.writeText("")
                externalLogFile?.writeText("")
            } catch (_: Exception) {}
        }
    }

    fun getExportText(): String {
        return synchronized(this) {
            logBuffer.joinToString("\n")
        }
    }

    fun copyToClipboard(context: Context) {
        val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        val clip = ClipData.newPlainText("Emux32 Logs", getExportText())
        clipboard.setPrimaryClip(clip)
    }

    fun startLogcatCapture(scope: CoroutineScope) {
        if (isCapturingLogcat) return
        isCapturingLogcat = true

        scope.launch(Dispatchers.IO) {
            try {
                Runtime.getRuntime().exec(arrayOf("logcat", "-c")).waitFor()
                val process = Runtime.getRuntime().exec(
                    arrayOf(
                        "logcat",
                        "-v", "time",
                        "Emux32:V",
                        "EmuxDbtEngine:V",
                        "EmuxElfLoader:V",
                        "UnicornCore:V",
                        "EmuxLinkerHook:V",
                        "NativeEngine:V",
                        "*:S"
                    )
                )
                val reader = process.inputStream.bufferedReader()
                var line: String?
                while (reader.readLine().also { line = it } != null) {
                    val l = line
                    if (!l.isNullOrBlank() && !l.contains("EmuxLogger")) {
                        appendRawLog(l)
                    }
                }
            } catch (e: Exception) {
                Log.w(DEFAULT_TAG, "Logcat capture non disponibile: ${e.message}")
            }
        }
    }
}
