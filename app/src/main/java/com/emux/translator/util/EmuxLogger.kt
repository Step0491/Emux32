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
    private var prevPrimaryLogFile: File? = null
    private var externalLogFile: File? = null
    private var prevExternalLogFile: File? = null
    private var isCapturingLogcat = false

    private fun rotateIfMeaningful(curFile: File, prevFile: File) {
        if (!curFile.exists()) return
        val len = curFile.length()
        // Ruota in _prev solo se la sessione precedente ha prodotto log significativi (> 512 byte),
        // evitando che un riavvio rapido post-crash sovrascriva il log del crash precedente.
        if (len > 512L || !prevFile.exists() || prevFile.length() == 0L) {
            if (len > 0L) {
                try {
                    curFile.copyTo(prevFile, overwrite = true)
                } catch (_: Exception) {}
            }
        }
        curFile.delete()
    }

    fun initLogFiles(internalCacheDir: File, externalEmuxDir: File?) {
        try {
            // 1. Log interno sempre disponibile (privato sandbox dell'app)
            val prevInternal = File(internalCacheDir, "emux_execution_prev.log")
            val curInternal = File(internalCacheDir, "emux_execution.log")
            rotateIfMeaningful(curInternal, prevInternal)
            curInternal.createNewFile()
            primaryLogFile = curInternal
            prevPrimaryLogFile = prevInternal

            // 2. Log pubblico richiesto in Emux32/log/emux_execution.log
            if (externalEmuxDir != null) {
                val logDir = File(externalEmuxDir, "log")
                if (!logDir.exists()) {
                    logDir.mkdirs()
                }
                val prevExt = File(logDir, "emux_execution_prev.log")
                val curExt = File(logDir, "emux_execution.log")
                rotateIfMeaningful(curExt, prevExt)
                curExt.createNewFile()
                externalLogFile = curExt
                prevExternalLogFile = prevExt
                Log.i(DEFAULT_TAG, "File di log pubblico configurato: ${externalLogFile?.absolutePath}")
            }

            // 3. Ripristina nel viewer UI i log della sessione precedente (utile dopo un crash)
            val prevText = readFileSafe(prevExternalLogFile) ?: readFileSafe(prevPrimaryLogFile)
            if (!prevText.isNullOrBlank()) {
                synchronized(this) {
                    val prevLines = prevText.lines().filter { it.isNotBlank() }.takeLast(1000)
                    if (prevLines.isNotEmpty()) {
                        logBuffer.clear()
                        logBuffer.add("---------- [SESSIONE PRECEDENTE (pre-crash)] ----------")
                        logBuffer.addAll(prevLines)
                        logBuffer.add("---------- [SESSIONE CORRENTE] ----------")
                        while (logBuffer.size > MAX_LOG_LINES) {
                            logBuffer.removeAt(0)
                        }
                        _logs.value = logBuffer.toList()
                    }
                }
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

    fun getCombinedUploadBundle(): String {
        val sb = StringBuilder()
        sb.appendLine("====================================================")
        sb.appendLine("EMUX32 DEVICE LOG BUNDLE")
        sb.appendLine("Build Tag : ${com.emux.translator.BuildConfig.BUILD_TAG}")
        sb.appendLine("Build Time: ${com.emux.translator.BuildConfig.BUILD_TIME}")
        sb.appendLine("Device    : ${android.os.Build.MANUFACTURER} ${android.os.Build.MODEL} (SDK ${android.os.Build.VERSION.SDK_INT})")
        sb.appendLine("Uploaded  : ${SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date())}")
        sb.appendLine("====================================================")
        sb.appendLine()

        val prevText = readFileSafe(prevExternalLogFile) ?: readFileSafe(prevPrimaryLogFile)
        if (!prevText.isNullOrBlank()) {
            sb.appendLine("---------- [PREVIOUS SESSION LOG (emux_execution_prev.log)] ----------")
            sb.appendLine(prevText.trimEnd())
            sb.appendLine()
        }

        val curText = readFileSafe(externalLogFile) ?: readFileSafe(primaryLogFile) ?: getExportText()
        sb.appendLine("---------- [CURRENT SESSION LOG (emux_execution.log)] ----------")
        sb.appendLine(curText.ifBlank { "(nessun log nella sessione corrente)" })
        return sb.toString()
    }

    private fun readFileSafe(file: File?): String? {
        return try {
            if (file != null && file.exists() && file.length() > 0L) {
                file.readText()
            } else {
                null
            }
        } catch (_: Exception) {
            null
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
                // Non eseguire `logcat -c`: preserva eventuali crash nativi/ART della sessione precedente.
                val process = Runtime.getRuntime().exec(
                    arrayOf(
                        "logcat",
                        "-v", "time",
                        "-T", "200",
                        "Emux32:V",
                        "EmuxDbtEngine:V",
                        "EmuxElfLoader:V",
                        "UnicornCore:V",
                        "EmuxLinkerHook:V",
                        "EmuxGuestMemory:V",
                        "GuestSignalManager:V",
                        "NativeEngine:V",
                        "AndroidRuntime:E",
                        "art:E",
                        "libc:F",
                        "DEBUG:F",
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
