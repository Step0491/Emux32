package com.emux.translator.util

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.provider.Settings
import androidx.core.content.FileProvider
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedInputStream
import java.io.File
import java.io.FileOutputStream
import java.net.HttpURLConnection
import java.net.URL

data class ReleaseInfo(
    val tagName: String,
    val title: String,
    val publishedAt: String,
    val apkName: String,
    val downloadUrl: String,
    val sizeBytes: Long,
    val signature: String
)

object AppUpdater {
    private const val RELEASES_LATEST_URL = "https://api.github.com/repos/Step0491/Emux32/releases/latest"
    private const val RELEASES_LIST_URL = "https://api.github.com/repos/Step0491/Emux32/releases?per_page=1"
    private const val PREF_LAST_RELEASE_SIG = "last_installed_release_sig"

    suspend fun fetchLatestRelease(): Result<ReleaseInfo> = withContext(Dispatchers.IO) {
        try {
            val releaseObj = fetchJsonObject(RELEASES_LATEST_URL)
                ?: fetchFirstReleaseFromList(RELEASES_LIST_URL)
                ?: return@withContext Result.failure(IllegalStateException("Nessuna release trovata su GitHub."))

            val tagName = releaseObj.optString("tag_name", "latest")
            val title = releaseObj.optString("name", tagName).ifBlank { tagName }
            val publishedAt = releaseObj.optString("published_at", "")
            val assets = releaseObj.optJSONArray("assets")
                ?: return@withContext Result.failure(IllegalStateException("Nessun asset APK presente nella release."))

            var selectedAsset: JSONObject? = null
            for (i in 0 until assets.length()) {
                val asset = assets.optJSONObject(i) ?: continue
                val name = asset.optString("name", "")
                if (name.endsWith(".apk", ignoreCase = true)) {
                    selectedAsset = asset
                    if (name.startsWith("Emux32_d", ignoreCase = true)) {
                        break
                    }
                }
            }

            if (selectedAsset == null) {
                return@withContext Result.failure(IllegalStateException("File APK non trovato nella release '$title'."))
            }

            val apkName = selectedAsset.optString("name", "Emux32-latest.apk")
            val downloadUrl = selectedAsset.optString("browser_download_url", "")
            val sizeBytes = selectedAsset.optLong("size", -1L)
            val assetUpdatedAt = selectedAsset.optString("updated_at", publishedAt)

            if (downloadUrl.isBlank()) {
                return@withContext Result.failure(IllegalStateException("URL di download APK non valido."))
            }

            val signature = "$title|$apkName|$assetUpdatedAt|$sizeBytes"
            Result.success(
                ReleaseInfo(
                    tagName = tagName,
                    title = title,
                    publishedAt = publishedAt,
                    apkName = apkName,
                    downloadUrl = downloadUrl,
                    sizeBytes = sizeBytes,
                    signature = signature
                )
            )
        } catch (t: Throwable) {
            EmuxLogger.e("Emux32", "Errore verifica aggiornamenti GitHub: ${t.message}")
            Result.failure(t)
        }
    }

    fun isSameAsLastInstalled(context: Context, release: ReleaseInfo): Boolean {
        val prefs = context.getSharedPreferences("emux_prefs", Context.MODE_PRIVATE)
        val lastSig = prefs.getString(PREF_LAST_RELEASE_SIG, null)
        return lastSig != null && lastSig == release.signature
    }

    fun markReleaseDownloaded(context: Context, release: ReleaseInfo) {
        val prefs = context.getSharedPreferences("emux_prefs", Context.MODE_PRIVATE)
        prefs.edit().putString(PREF_LAST_RELEASE_SIG, release.signature).apply()
    }

    suspend fun downloadApk(
        context: Context,
        release: ReleaseInfo,
        onProgress: (downloadedBytes: Long, totalBytes: Long) -> Unit
    ): Result<File> = withContext(Dispatchers.IO) {
        try {
            val updatesDir = File(context.cacheDir, "updates")
            if (!updatesDir.exists()) {
                updatesDir.mkdirs()
            }
            val targetApk = File(updatesDir, "Emux32-latest.apk")
            if (targetApk.exists()) {
                targetApk.delete()
            }

            val conn = openRedirectableConnection(release.downloadUrl)
            val responseCode = conn.responseCode
            if (responseCode !in 200..299) {
                conn.disconnect()
                return@withContext Result.failure(IllegalStateException("Download fallito (HTTP $responseCode)."))
            }

            val totalBytes = if (conn.contentLengthLong > 0) conn.contentLengthLong else release.sizeBytes
            var downloaded = 0L
            var lastReportedBytes = 0L

            BufferedInputStream(conn.inputStream).use { input ->
                FileOutputStream(targetApk).use { output ->
                    val buffer = ByteArray(32 * 1024)
                    while (true) {
                        val read = input.read(buffer)
                        if (read <= 0) break
                        output.write(buffer, 0, read)
                        downloaded += read
                        if (downloaded - lastReportedBytes >= 128 * 1024 || downloaded == totalBytes) {
                            lastReportedBytes = downloaded
                            val dlSnapshot = downloaded
                            withContext(Dispatchers.Main) {
                                onProgress(dlSnapshot, totalBytes)
                            }
                        }
                    }
                    output.flush()
                }
            }
            withContext(Dispatchers.Main) {
                onProgress(downloaded, totalBytes)
            }
            conn.disconnect()

            if (!targetApk.exists() || targetApk.length() <= 0L) {
                return@withContext Result.failure(IllegalStateException("File APK scaricato vuoto o corrotto."))
            }

            markReleaseDownloaded(context, release)
            EmuxLogger.i("Emux32", "Aggiornamento scaricato: ${release.apkName} (${targetApk.length()} byte)")
            Result.success(targetApk)
        } catch (t: Throwable) {
            EmuxLogger.e("Emux32", "Errore download aggiornamento: ${t.message}")
            Result.failure(t)
        }
    }

    fun canRequestPackageInstalls(context: Context): Boolean {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            context.packageManager.canRequestPackageInstalls()
        } else {
            true
        }
    }

    fun buildInstallPermissionIntent(context: Context): Intent {
        return Intent(
            Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
            Uri.parse("package:${context.packageName}")
        ).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
    }

    fun launchApkInstaller(context: Context, apkFile: File) {
        val apkUri = FileProvider.getUriForFile(
            context,
            "${context.packageName}.fileprovider",
            apkFile
        )
        val intent = Intent(Intent.ACTION_VIEW).apply {
            setDataAndType(apkUri, "application/vnd.android.package-archive")
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }
        context.startActivity(intent)
    }

    private fun fetchJsonObject(urlString: String): JSONObject? {
        val conn = openRedirectableConnection(urlString)
        return try {
            if (conn.responseCode !in 200..299) {
                null
            } else {
                val body = conn.inputStream.bufferedReader().use { it.readText() }
                JSONObject(body)
            }
        } finally {
            conn.disconnect()
        }
    }

    private fun fetchFirstReleaseFromList(urlString: String): JSONObject? {
        val conn = openRedirectableConnection(urlString)
        return try {
            if (conn.responseCode !in 200..299) {
                null
            } else {
                val body = conn.inputStream.bufferedReader().use { it.readText() }
                val arr = JSONArray(body)
                if (arr.length() > 0) arr.optJSONObject(0) else null
            }
        } finally {
            conn.disconnect()
        }
    }

    private fun openRedirectableConnection(urlString: String): HttpURLConnection {
        var currentUrl = urlString
        var redirects = 0
        while (redirects < 5) {
            val conn = (URL(currentUrl).openConnection() as HttpURLConnection).apply {
                requestMethod = "GET"
                connectTimeout = 15000
                readTimeout = 30000
                instanceFollowRedirects = true
                setRequestProperty("Accept", "application/vnd.github+json")
                setRequestProperty("User-Agent", "Emux32-Android-Updater")
            }
            val code = conn.responseCode
            if (code == HttpURLConnection.HTTP_MOVED_TEMP ||
                code == HttpURLConnection.HTTP_MOVED_PERM ||
                code == HttpURLConnection.HTTP_SEE_OTHER ||
                code == 307 || code == 308
            ) {
                val nextLocation = conn.getHeaderField("Location")
                conn.disconnect()
                if (nextLocation.isNullOrBlank()) {
                    throw IllegalStateException("Redirect HTTP senza header Location.")
                }
                currentUrl = nextLocation
                redirects++
            } else {
                return conn
            }
        }
        throw IllegalStateException("Troppi redirect HTTP durante la connessione.")
    }
}
