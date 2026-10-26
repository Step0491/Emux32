package com.emux.translator.model

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.drawable.BitmapDrawable
import android.graphics.drawable.Drawable
import android.net.Uri
import java.io.File
import java.io.FileOutputStream

data class EmuxRom(
    val title: String,
    val coverUri: Uri?,
    val apkFile: File,
    val obbFile: File? = null,
    val packageName: String? = null,
    val hasObb: Boolean = false
)

/**
 * Scansiona la cartella Emux/apk ed Emux/obb nella memoria esterna principale.
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

    for (apkFile in apkFiles) {
        val baseName = apkFile.nameWithoutExtension
        val title = baseName.replace("_", " ")

        val (pkgName, extractedIconUri) = extractApkMetadata(context, apkFile)

        // Cerca eventuale OBB associato in Emux/obb/ o Emux/obb/<packageName>/
        var matchedObbFile: File? = null
        if (obbDir.exists()) {
            // Controlla se c'è un file obb con nome simile o dentro una sottocartella col nome pacchetto
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

        // Cerca copertina custom accanto all'apk (es: nome.jpg, nome.png o cover.jpg)
        var coverUri = extractedIconUri
        val possibleCovers = listOf(
            File(apkDir, "$baseName.jpg"),
            File(apkDir, "$baseName.png"),
            File(apkDir, "$baseName.jpeg")
        )
        for (cand in possibleCovers) {
            if (cand.exists() && cand.length() > 0) {
                coverUri = Uri.fromFile(cand)
                break
            }
        }

        gamesList.add(
            EmuxRom(
                title = title,
                coverUri = coverUri,
                apkFile = apkFile,
                obbFile = matchedObbFile,
                packageName = pkgName,
                hasObb = (matchedObbFile != null)
            )
        )
    }

    return gamesList
}

/**
 * Estrae l'icona e il package name dall'APK memorizzandoli nella cache interna
 */
private fun extractApkMetadata(context: Context, apkFile: File): Pair<String?, Uri?> {
    try {
        val iconsDir = File(context.cacheDir, "extracted_icons")
        if (!iconsDir.exists()) iconsDir.mkdirs()

        val safeName = apkFile.name.replace("[^a-zA-Z0-9]".toRegex(), "_")
        val iconFile = File(iconsDir, "$safeName.png")

        val pm = context.packageManager
        val pkgInfo = pm.getPackageArchiveInfo(apkFile.absolutePath, 0)
        val packageName = pkgInfo?.packageName

        if (iconFile.exists() && iconFile.length() > 0) {
            return Pair(packageName, Uri.fromFile(iconFile))
        }

        if (pkgInfo != null) {
            pkgInfo.applicationInfo.sourceDir = apkFile.absolutePath
            pkgInfo.applicationInfo.publicSourceDir = apkFile.absolutePath
            val drawable = pkgInfo.applicationInfo.loadIcon(pm)

            val bitmap = drawableToBitmap(drawable)
            FileOutputStream(iconFile).use { out ->
                bitmap.compress(Bitmap.CompressFormat.PNG, 95, out)
            }
            return Pair(packageName, Uri.fromFile(iconFile))
        }
    } catch (e: Exception) {
        e.printStackTrace()
    }
    return Pair(null, null)
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
