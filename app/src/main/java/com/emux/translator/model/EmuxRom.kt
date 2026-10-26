package com.emux.translator.model

import android.content.Context
import android.content.pm.ActivityInfo
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.drawable.BitmapDrawable
import android.graphics.drawable.Drawable
import android.net.Uri
import com.emux.translator.util.ImageUpscaler
import java.io.File
import java.io.FileOutputStream

data class EmuxRom(
    val title: String,
    val coverUri: Uri?,
    val apkFile: File,
    val obbFiles: List<File> = emptyList(), // Supporta sia singolo file che più OBB (main + patch) o cartella
    val packageName: String? = null,
    val hasObb: Boolean = false,
    val preferredOrientation: Int = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
)

/**
 * Scansiona la cartella Emux32/apk ed Emux32/obb.
 * Riconosce automaticamente sia file .obb sciolti che cartelle package estratte (es. com.turner.cardwars).
 */
fun scanEmuxStorage(context: Context, apkDir: File, obbDir: File): List<EmuxRom> {
    val gamesList = mutableListOf<EmuxRom>()

    if (!apkDir.exists()) {
        apkDir.mkdirs()
    }
    if (!obbDir.exists()) {
        obbDir.mkdirs()
    }

    val apkFiles = apkDir.listFiles { file ->
        file.isFile && file.name.lowercase().endsWith(".apk")
    } ?: emptyArray()

    val upscaledCoversDir = File(apkDir, ".upscaled_covers")
    if (!upscaledCoversDir.exists()) {
        upscaledCoversDir.mkdirs()
    }

    for (apkFile in apkFiles) {
        val baseName = apkFile.nameWithoutExtension
        val title = baseName.replace("_", " ")

        val (pkgName, rawIconFile, orientation) = extractApkMetadata(context, apkFile)

        // Cerca i file OBB associati:
        // Caso A: Cartella estratta con il package name (es. Emux32/obb/com.turner.cardwars/main.xxx.obb)
        // Caso B: Cartella estratta con nome simile (es. Emux32/obb/BloodAndGlory/)
        // Caso C: File .obb diretto (es. Emux32/obb/main.xxx.com.turner.cardwars.obb)
        val matchedObbList = mutableListOf<File>()

        if (obbDir.exists()) {
            val allEntries = obbDir.listFiles() ?: emptyArray()

            // 1. Cerca dentro cartelle
            for (entry in allEntries) {
                if (entry.isDirectory) {
                    val folderName = entry.name.lowercase()
                    val matchPackage = (pkgName != null && folderName.contains(pkgName.lowercase()))
                    val matchBase = folderName.contains(baseName.lowercase()) || baseName.lowercase().contains(folderName)

                    if (matchPackage || matchBase) {
                        val obbs = entry.listFiles { f -> f.isFile && f.name.endsWith(".obb") }
                        if (!obbs.isNullOrEmpty()) {
                            matchedObbList.addAll(obbs)
                        }
                    }
                }
            }

            // 2. Se non ancora trovato, cerca file .obb sciolti direttamente nella root di obb/
            if (matchedObbList.isEmpty()) {
                val directObbs = obbDir.listFiles { f ->
                    f.isFile && f.name.endsWith(".obb") && (
                        (pkgName != null && f.name.lowercase().contains(pkgName.lowercase())) ||
                        f.name.lowercase().contains(baseName.lowercase())
                    )
                }
                if (!directObbs.isNullOrEmpty()) {
                    matchedObbList.addAll(directObbs)
                }
            }
        }

        // Cerca copertina custom omonima (es: nome.jpg, nome.png, cover.jpg)
        var sourceCoverFile: File? = null
        val possibleCovers = listOf(
            File(apkDir, "$baseName.jpg"),
            File(apkDir, "$baseName.png"),
            File(apkDir, "$baseName.jpeg"),
            File(apkDir, "cover.jpg"),
            File(apkDir, "cover.png")
        )
        for (cand in possibleCovers) {
            if (cand.exists() && cand.length() > 0) {
                sourceCoverFile = cand
                break
            }
        }

        if (sourceCoverFile == null) {
            sourceCoverFile = rawIconFile
        }

        // Upscale rapido 4x offline e cache
        val finalCoverUri: Uri? = if (sourceCoverFile != null && sourceCoverFile.exists()) {
            val upscaledTarget = File(upscaledCoversDir, "${baseName}_upscaled.png")
            val readyCover = ImageUpscaler.getOrUpscaleImage(sourceCoverFile, upscaledTarget, 512)
            Uri.fromFile(readyCover)
        } else {
            null
        }

        gamesList.add(
            EmuxRom(
                title = title,
                coverUri = finalCoverUri,
                apkFile = apkFile,
                obbFiles = matchedObbList,
                packageName = pkgName,
                hasObb = matchedObbList.isNotEmpty(),
                preferredOrientation = orientation
            )
        )
    }

    return gamesList
}

/**
 * Estrae l'icona, il package name e l'orientamento nativo del gioco dall'APK
 */
private fun extractApkMetadata(context: Context, apkFile: File): Triple<String?, File?, Int> {
    try {
        val iconsDir = File(context.cacheDir, "extracted_icons")
        if (!iconsDir.exists()) iconsDir.mkdirs()

        val safeName = apkFile.name.replace("[^a-zA-Z0-9]".toRegex(), "_")
        val iconFile = File(iconsDir, "$safeName.png")

        val pm = context.packageManager
        val pkgInfo = pm.getPackageArchiveInfo(apkFile.absolutePath, PackageManager.GET_ACTIVITIES)
        val packageName = pkgInfo?.packageName

        var gameOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
        if (pkgInfo?.activities != null && pkgInfo.activities.isNotEmpty()) {
            val mainAct = pkgInfo.activities.firstOrNull { it.screenOrientation != ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED }
                ?: pkgInfo.activities.first()
            gameOrientation = when (mainAct.screenOrientation) {
                ActivityInfo.SCREEN_ORIENTATION_PORTRAIT,
                ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT,
                ActivityInfo.SCREEN_ORIENTATION_USER_PORTRAIT -> ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
                ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE,
                ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE,
                ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE -> ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
                else -> ActivityInfo.SCREEN_ORIENTATION_SENSOR
            }
        }

        if (iconFile.exists() && iconFile.length() > 0) {
            return Triple(packageName, iconFile, gameOrientation)
        }

        if (pkgInfo != null) {
            pkgInfo.applicationInfo.sourceDir = apkFile.absolutePath
            pkgInfo.applicationInfo.publicSourceDir = apkFile.absolutePath
            val drawable = pkgInfo.applicationInfo.loadIcon(pm)

            val bitmap = drawableToBitmap(drawable)
            FileOutputStream(iconFile).use { out ->
                bitmap.compress(Bitmap.CompressFormat.PNG, 95, out)
            }
            return Triple(packageName, iconFile, gameOrientation)
        }
    } catch (e: Exception) {
        e.printStackTrace()
    }
    return Triple(null, null, ActivityInfo.SCREEN_ORIENTATION_SENSOR)
}

private fun drawableToBitmap(drawable: Drawable): Bitmap {
    if (drawable is BitmapDrawable && drawable.bitmap != null) {
        return drawable.bitmap
    }
    val width = if (drawable.intrinsicWidth > 0) drawable.intrinsicWidth else 128
    val height = if (drawable.intrinsicHeight > 0) drawable.intrinsicHeight else 128
    val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
    val canvas = Canvas(bitmap)
    drawable.setBounds(0, 0, canvas.width, canvas.height)
    drawable.draw(canvas)
    return bitmap
}
