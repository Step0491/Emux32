package com.emux.translator.model

import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.drawable.BitmapDrawable
import android.graphics.drawable.Drawable
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import java.io.File
import java.io.FileOutputStream

enum class XmbCategory(val title: String, val iconResName: String) {
    SETTINGS("Impostazioni", "ic_settings"),
    GAMES("Giochi", "ic_games"),
    FAVORITES("Preferiti", "ic_star"),
    SYSTEM("Sistema", "ic_info")
}

data class EmuxRom(
    val title: String,
    val coverUri: Uri?,
    val apkUri: Uri?,
    val obbDirUri: Uri?,
    val packageName: String? = null,
    val category: XmbCategory = XmbCategory.GAMES,
    val isArm32: Boolean = true,
    val hasObb: Boolean = false
)

fun parseEmuxGames(context: Context, rootGamesDir: DocumentFile): List<EmuxRom> {
    val gamesList = mutableListOf<EmuxRom>()
    val files = rootGamesDir.listFiles()

    // 1. MODELLO FLAT: File APK posizionati direttamente nella cartella root
    for (file in files) {
        val fileName = file.name?.lowercase() ?: ""
        if (file.isFile && fileName.endsWith(".apk")) {
            val defaultTitle = file.name?.substringBeforeLast(".")?.replace("_", " ") ?: "Sconosciuto"
            val (pkgName, extractedIconUri) = extractApkMetadata(context, file)

            gamesList.add(
                EmuxRom(
                    title = defaultTitle,
                    coverUri = extractedIconUri,
                    apkUri = file.uri,
                    obbDirUri = null,
                    packageName = pkgName,
                    category = XmbCategory.GAMES,
                    isArm32 = true,
                    hasObb = false
                )
            )
        }
    }

    // 2. MODELLO SOTTOCARTELLE: Cartelle dedicate con APK + OBB + Cover personalizzata
    for (folder in files) {
        if (folder.isDirectory) {
            val folderTitle = folder.name?.replace("_", " ") ?: "Sconosciuto"
            var coverUri: Uri? = null
            var apkDocFile: DocumentFile? = null
            var obbUri: Uri? = null

            for (subFile in folder.listFiles()) {
                val subName = subFile.name?.lowercase() ?: ""
                if (subName.endsWith(".apk")) {
                    apkDocFile = subFile
                } else if (subName == "cover.jpg" || subName == "cover.png" || subName == "folder.jpg") {
                    // Priorità massima alla locandina 3:4 personalizzata
                    coverUri = subFile.uri
                } else if (subName.endsWith(".obb") || (subFile.isDirectory && subName.contains("obb"))) {
                    obbUri = subFile.uri
                }
            }

            if (apkDocFile != null) {
                var pkgName: String? = null

                // Se non c'è cover.jpg esterna, estrae automaticamente l'icona interna dell'APK
                if (coverUri == null) {
                    val (extractedPkg, extractedIcon) = extractApkMetadata(context, apkDocFile)
                    pkgName = extractedPkg
                    coverUri = extractedIcon
                }

                gamesList.add(
                    EmuxRom(
                        title = folderTitle,
                        coverUri = coverUri,
                        apkUri = apkDocFile.uri,
                        obbDirUri = obbUri ?: folder.uri,
                        packageName = pkgName,
                        category = XmbCategory.GAMES,
                        isArm32 = true,
                        hasObb = (obbUri != null)
                    )
                )
            }
        }
    }

    return gamesList
}

/**
 * Estrae l'icona e i metadati dell'APK memorizzandoli nella cache interna per Coil
 */
private fun extractApkMetadata(context: Context, apkDocFile: DocumentFile): Pair<String?, Uri?> {
    try {
        val iconsDir = File(context.cacheDir, "extracted_icons")
        if (!iconsDir.exists()) iconsDir.mkdirs()

        val safeName = (apkDocFile.name ?: "temp").replace("[^a-zA-Z0-9]".toRegex(), "_")
        val iconFile = File(iconsDir, "$safeName.png")

        if (iconFile.exists() && iconFile.length() > 0) {
            return Pair(null, Uri.fromFile(iconFile))
        }

        // Copia temporanea leggera dell'APK per interrogare PackageManager
        val tempApk = File(context.cacheDir, "meta_${System.currentTimeMillis()}.apk")
        context.contentResolver.openInputStream(apkDocFile.uri)?.use { inStream ->
            tempApk.outputStream().use { outStream -> inStream.copyTo(outStream) }
        }

        if (tempApk.exists()) {
            val pm = context.packageManager
            val pkgInfo = pm.getPackageArchiveInfo(tempApk.absolutePath, 0)
            if (pkgInfo != null) {
                pkgInfo.applicationInfo.sourceDir = tempApk.absolutePath
                pkgInfo.applicationInfo.publicSourceDir = tempApk.absolutePath
                val drawable = pkgInfo.applicationInfo.loadIcon(pm)

                val bitmap = drawableToBitmap(drawable)
                FileOutputStream(iconFile).use { out ->
                    bitmap.compress(Bitmap.CompressFormat.PNG, 95, out)
                }

                tempApk.delete()
                return Pair(pkgInfo.packageName, Uri.fromFile(iconFile))
            }
            tempApk.delete()
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
