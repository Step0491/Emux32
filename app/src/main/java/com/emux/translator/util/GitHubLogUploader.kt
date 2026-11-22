package com.emux.translator.util

import android.content.Context
import android.util.Base64
import com.emux.translator.BuildConfig
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL

object GitHubLogUploader {
    private const val PREFS_NAME = "emux_prefs"
    private const val KEY_GITHUB_PAT = "github_log_pat"
    private const val TARGET_BRANCH = "logs"
    private const val TARGET_PATH = "device_logs/latest.log"
    private const val CONTENTS_API_URL =
        "https://api.github.com/repos/Step0491/Emux32/contents/$TARGET_PATH"

    fun getSavedToken(context: Context): String {
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        return prefs.getString(KEY_GITHUB_PAT, "")?.trim() ?: ""
    }

    fun saveToken(context: Context, token: String) {
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        prefs.edit().putString(KEY_GITHUB_PAT, token.trim()).apply()
    }

    suspend fun uploadLatestLogs(context: Context, token: String): Result<String> = withContext(Dispatchers.IO) {
        val cleanToken = token.trim()
        if (cleanToken.isBlank()) {
            return@withContext Result.failure(IllegalArgumentException("Token GitHub mancante."))
        }

        try {
            val bundleText = EmuxLogger.getCombinedUploadBundle()
            val existingSha = fetchExistingFileSha(cleanToken)

            val payload = JSONObject().apply {
                put("message", "Upload device log (${BuildConfig.BUILD_TAG})")
                put(
                    "content",
                    Base64.encodeToString(bundleText.toByteArray(Charsets.UTF_8), Base64.NO_WRAP)
                )
                put("branch", TARGET_BRANCH)
                if (!existingSha.isNullOrBlank()) {
                    put("sha", existingSha)
                }
            }

            val conn = (URL(CONTENTS_API_URL).openConnection() as HttpURLConnection).apply {
                requestMethod = "PUT"
                connectTimeout = 15000
                readTimeout = 30000
                doOutput = true
                setRequestProperty("Accept", "application/vnd.github+json")
                setRequestProperty("Authorization", "Bearer $cleanToken")
                setRequestProperty("Content-Type", "application/json; charset=UTF-8")
                setRequestProperty("User-Agent", "Emux32-Log-Uploader")
            }

            conn.outputStream.use { os ->
                os.write(payload.toString().toByteArray(Charsets.UTF_8))
                os.flush()
            }

            val code = conn.responseCode
            val responseText = try {
                val stream = if (code in 200..299) conn.inputStream else conn.errorStream
                stream?.bufferedReader()?.use { it.readText() } ?: ""
            } catch (_: Exception) {
                ""
            } finally {
                conn.disconnect()
            }

            if (code !in 200..299) {
                val msg = try {
                    JSONObject(responseText).optString("message", "HTTP $code")
                } catch (_: Exception) {
                    "HTTP $code"
                }
                return@withContext Result.failure(IllegalStateException("Upload fallito: $msg"))
            }

            EmuxLogger.i("Emux32", "Log inviati con successo su GitHub (branch: $TARGET_BRANCH, path: $TARGET_PATH)")
            Result.success("https://github.com/Step0491/Emux32/blob/$TARGET_BRANCH/$TARGET_PATH")
        } catch (t: Throwable) {
            EmuxLogger.e("Emux32", "Errore invio log su GitHub: ${t.message}")
            Result.failure(t)
        }
    }

    private fun fetchExistingFileSha(token: String): String? {
        val url = "$CONTENTS_API_URL?ref=$TARGET_BRANCH"
        val conn = (URL(url).openConnection() as HttpURLConnection).apply {
            requestMethod = "GET"
            connectTimeout = 15000
            readTimeout = 15000
            setRequestProperty("Accept", "application/vnd.github+json")
            setRequestProperty("Authorization", "Bearer $token")
            setRequestProperty("User-Agent", "Emux32-Log-Uploader")
        }
        return try {
            if (conn.responseCode in 200..299) {
                val body = conn.inputStream.bufferedReader().use { it.readText() }
                JSONObject(body).optString("sha", "").ifBlank { null }
            } else {
                null
            }
        } finally {
            conn.disconnect()
        }
    }
}
