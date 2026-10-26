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
    val obbFile: File? = null,
    val packageName: String? = null,
    val hasObb: Boolean = false,
    val preferredOrientation: Int = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
)

/**
 * Scansiona la cartella Emux32/apk ed Emux32/obb.
 * Controlla se le cartelle esistono e crea SOLO ciò che manca senza cancellare o sovrascrivere nulla.
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

        // Cerca eventuale OBB associato in Emux32/obb/ o Emux32/obb/<packageName>/
        var matchedObbFile: File? = null
        if (obbDir.exists()) {
            if (pkgName != null) {
                val pkgFolder = File(obbDir, pkgName)
                if (pkgFolder.isDirectory) {
                    val obbInside = pkgFolder.listFiles { f -> f.isFile && f.name.endsWith(".obb") }
                    if (!obbInside.isNullOrEmpty()) {
                        matchedObbFile = obbInside.first()
                    }
                }
            }
            if (matchedObbFile == null) {
                val directObb = obbDir.listFiles { f ->
                    f.isFile && f.name.endsWith(".obb") && (f.name.contains(baseName, ignoreCase = true) || (pkgName != null && f.name.contains(pkgName, ignoreCase = true)))
                }
                if (!directObb.isNullOrEmpty()) {
                    matchedObbFile = directObb.first()
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

        // Esegue l'upscaling rapido 4x offline e salva il risultato permanente
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
                obbFile = matchedObbFile,
                packageName = pkgName,
                hasObb = (matchedObbFile != null),
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
