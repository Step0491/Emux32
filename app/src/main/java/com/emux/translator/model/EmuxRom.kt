package com.emux.translator.model

import android.content.Context
import android.content.pm.ActivityInfo
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.drawable.BitmapDrawable
import android.graphics.drawable.Drawable
import android.net.Uri
import com.emux.translator.util.ImageUpscaler
import java.io.File
import java.io.FileOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.zip.ZipFile

data class EmuxRom(
    val title: String,
    val coverUri: Uri?,
    val apkFile: File,
    val obbFiles: List<File> = emptyList(),
    val dataDir: File? = null,
    val packageName: String? = null,
    val hasObb: Boolean = false,
    val hasData: Boolean = false,
    val preferredOrientation: Int = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
)

/**
 * Scansiona la cartella Emux32/apk ed Emux32/obb (oltre a /storage/emulated/0/Android/obb).
 * Riconosce automaticamente:
 * - Package Name nativo via pure-Kotlin AXML parser da AndroidManifest.xml (senza blocchi Android 14/15/16 SDK 36)
 * - File OBB con regex standard (main.<version>.<package>.obb / patch.<version>.<package>.obb)
 * - Cartelle estratte (es. com.turner.cardwars, com.glu.gladiator)
 * - Scansione ricorsiva completa in qualsiasi sotto-cartella OBB
 */
fun scanEmuxStorage(context: Context, apkDir: File, obbDir: File): List<EmuxRom> {
    val gamesList = mutableListOf<EmuxRom>()

    if (!apkDir.exists()) apkDir.mkdirs()
    if (!obbDir.exists()) obbDir.mkdirs()

    val candidateApkDirs = mutableListOf<File>()
    candidateApkDirs.add(apkDir)
    apkDir.parentFile?.let { parent ->
        if (parent.exists()) {
            candidateApkDirs.add(parent)
            val romsDir = File(parent, "roms")
            if (romsDir.exists()) candidateApkDirs.add(romsDir)
            val gamesDir = File(parent, "games")
            if (gamesDir.exists()) candidateApkDirs.add(gamesDir)
        }
    }
    val downloadDir = File("/storage/emulated/0/Download")
    if (downloadDir.exists() && downloadDir.canRead()) {
        candidateApkDirs.add(downloadDir)
    }

    val discoveredApkFiles = mutableSetOf<File>()
    for (dir in candidateApkDirs) {
        if (!dir.exists()) continue
        dir.listFiles { file ->
            file.isFile && file.name.lowercase().endsWith(".apk")
        }?.forEach { discoveredApkFiles.add(it) }
        if (dir != downloadDir) {
            try {
                dir.walkTopDown().maxDepth(2).filter { it.isFile && it.name.lowercase().endsWith(".apk") }.forEach {
                    discoveredApkFiles.add(it)
                }
            } catch (_: Throwable) {}
        }
    }
    val apkFiles = discoveredApkFiles.toTypedArray()

    val upscaledCoversDir = File(apkDir, ".upscaled_covers")
    if (!upscaledCoversDir.exists()) upscaledCoversDir.mkdirs()

    // Raccoglie tutti i file .obb, archivi .zip e cartelle dati cercando in:
    // 1. obbDir configurata dall'emulatore (es. Emux32/obb)
    // 2. dataDir configurata dall'emulatore (es. Emux32/data)
    // 3. apkDir (spesso gli utenti posizionano lo ZIP della cache nella stessa cartella)
    // 4. /storage/emulated/0/Download (cartella download utente)
    // 5. /storage/emulated/0/Android/obb e /storage/emulated/0/Android/data (standard Android)
    val allDiscoveredObbs = mutableListOf<File>()
    val allDiscoveredDirs = mutableListOf<File>()

    val isArchiveFile = { file: File ->
        val l = file.name.lowercase()
        l.endsWith(".obb") || l.endsWith(".zip") || l.endsWith(".cache")
    }

    try {
        if (obbDir.exists()) {
            obbDir.walkTopDown().filter { it.isFile && isArchiveFile(it) }.forEach {
                allDiscoveredObbs.add(it)
            }
            obbDir.listFiles { file -> file.isDirectory }?.forEach {
                allDiscoveredDirs.add(it)
            }
        }
        val dataDir = File(obbDir.parentFile, "data")
        if (dataDir.exists()) {
            dataDir.walkTopDown().filter { it.isFile && isArchiveFile(it) }.forEach {
                allDiscoveredObbs.add(it)
            }
            dataDir.listFiles { file -> file.isDirectory }?.forEach {
                allDiscoveredDirs.add(it)
            }
        }
        if (apkDir.exists()) {
            apkDir.listFiles { file -> file.isFile && isArchiveFile(file) }?.forEach {
                allDiscoveredObbs.add(it)
            }
        }
        val downloadDir = File("/storage/emulated/0/Download")
        if (downloadDir.exists() && downloadDir.canRead()) {
            downloadDir.listFiles { file -> file.isFile && isArchiveFile(file) }?.forEach {
                allDiscoveredObbs.add(it)
            }
        }
        val systemObbDir = File("/storage/emulated/0/Android/obb")
        if (systemObbDir.exists() && systemObbDir.canRead()) {
            systemObbDir.walkTopDown().maxDepth(3).filter { it.isFile && isArchiveFile(it) }.forEach {
                allDiscoveredObbs.add(it)
            }
            systemObbDir.listFiles { file -> file.isDirectory }?.forEach {
                allDiscoveredDirs.add(it)
            }
        }
        val systemDataDir = File("/storage/emulated/0/Android/data")
        if (systemDataDir.exists() && systemDataDir.canRead()) {
            systemDataDir.listFiles { file -> file.isDirectory }?.forEach {
                allDiscoveredDirs.add(it)
            }
        }
    } catch (e: Throwable) {
        e.printStackTrace()
    }

    val obbPattern = Regex("""^(?:main|patch)\.\d+\.([a-zA-Z0-9_.]+)\.obb$""", RegexOption.IGNORE_CASE)

    for (apkFile in apkFiles) {
        val baseName = apkFile.nameWithoutExtension
        val title = baseName.replace("_", " ")

        val (pkgName, rawIconFile, orientation) = extractApkMetadata(context, apkFile)
        val cleanPkg = pkgName?.lowercase()?.trim()
        val cleanBase = baseName.lowercase().replace("[^a-z0-9]".toRegex(), "")

        // Ricerca file OBB / Cache corrispondenti
        val matchedObbList = mutableListOf<File>()

        val isBloodGlory = (cleanBase.contains("blood") && cleanBase.contains("glory")) ||
                           (cleanBase.contains("gladiator")) ||
                           (cleanPkg != null && cleanPkg.contains("gladiator"))

        val isCardWars = cleanBase.contains("cardwars") ||
                         (cleanBase.contains("card") && cleanBase.contains("wars")) ||
                         (cleanBase.contains("adventure") && cleanBase.contains("card")) ||
                         (cleanPkg != null && cleanPkg.contains("cardwars"))

        for (candidate in allDiscoveredObbs) {
            val fileName = candidate.name.lowercase()
            val parentName = candidate.parentFile?.name?.lowercase() ?: ""

            var matched = false

            // 1. Corrispondenza esatta tramite nome standard OBB (main.<version>.<package>.obb)
            val matchRes = obbPattern.matchEntire(candidate.name)
            if (matchRes != null && cleanPkg != null) {
                val obbPkg = matchRes.groupValues[1].lowercase()
                if (obbPkg == cleanPkg) {
                    matched = true
                }
            }

            // 2. Corrispondenza per cartella genitore (es. obb/com.glu.gladiator/...)
            if (!matched && cleanPkg != null) {
                if (parentName == cleanPkg || parentName.contains(cleanPkg) || cleanPkg.contains(parentName)) {
                    matched = true
                }
            }

            // 3. Corrispondenza per nome file contenente il package
            if (!matched && cleanPkg != null && fileName.contains(cleanPkg)) {
                matched = true
            }

            // 4. Corrispondenza per nome gioco / baseName
            if (!matched) {
                val cleanParent = parentName.replace("[^a-z0-9]".toRegex(), "")
                val cleanFile = fileName.replace("[^a-z0-9]".toRegex(), "")
                if (cleanParent.isNotEmpty() && (cleanParent.contains(cleanBase) || cleanBase.contains(cleanParent))) {
                    matched = true
                } else if (cleanFile.contains(cleanBase) || cleanBase.contains(cleanFile)) {
                    matched = true
                }
            }

            // 5. Corrispondenza specifica per titoli noti ed archivi cache terze parti (es: 01319, 10752, cahce1, powervr)
            if (!matched) {
                if (isBloodGlory) {
                    if (fileName.contains("blood") && fileName.contains("glory")) matched = true
                    if (fileName.contains("gladiator") || parentName.contains("gladiator")) matched = true
                    if (fileName.contains("01319") || fileName.contains("powervr")) matched = true
                    if (fileName.contains("glu")) matched = true
                }
                if (isCardWars) {
                    if (fileName.contains("cardwars") || parentName.contains("cardwars")) matched = true
                    if (fileName.contains("adventure") && (fileName.contains("card") || fileName.contains("cahce") || fileName.contains("cache"))) matched = true
                    if (fileName.contains("10752") || fileName.contains("turner")) matched = true
                }
            }

            // 6. Ispezione euristica interna allo ZIP se presente
            if (!matched && fileName.endsWith(".zip")) {
                try {
                    java.util.zip.ZipFile(candidate).use { zf ->
                        val entries = zf.entries()
                        var count = 0
                        while (entries.hasMoreElements() && count++ < 50) {
                            val entryName = entries.nextElement().name.lowercase()
                            if (cleanPkg != null && entryName.contains(cleanPkg)) {
                                matched = true
                                break
                            }
                            if (isBloodGlory && (entryName.contains("gladiator") || entryName.contains("glu"))) {
                                matched = true
                                break
                            }
                            if (isCardWars && (entryName.contains("cardwars") || entryName.contains("turner"))) {
                                matched = true
                                break
                            }
                        }
                    }
                } catch (_: Throwable) {}
            }

            if (matched && !matchedObbList.contains(candidate)) {
                matchedObbList.add(candidate)
            }
        }

        // 6. Rilevamento cartelle Dati / Cache di gioco (es. cartella con sottocartelle per Blood & Glory)
        var matchedDataDir: File? = null
        for (candDir in allDiscoveredDirs) {
            val dName = candDir.name.lowercase()
            val cleanDir = dName.replace("[^a-z0-9]".toRegex(), "")
            var dirMatched = false

            if (cleanPkg != null && (dName == cleanPkg || dName.contains(cleanPkg) || cleanPkg.contains(dName))) {
                dirMatched = true
            }
            if (!dirMatched && cleanDir.isNotEmpty() && (cleanDir.contains(cleanBase) || cleanBase.contains(cleanDir))) {
                dirMatched = true
            }
            if (!dirMatched) {
                if (cleanBase.contains("blood") || cleanBase.contains("glory")) {
                    if (dName.contains("gladiator") || dName.contains("glu")) {
                        dirMatched = true
                    }
                }
                if (cleanBase.contains("cardwars") || cleanBase.contains("card_wars")) {
                    if (dName.contains("cardwars") || dName.contains("turner")) {
                        dirMatched = true
                    }
                }
            }

            if (dirMatched) {
                val hasItems = try {
                    candDir.listFiles()?.isNotEmpty() == true
                } catch (_: Throwable) { false }

                if (hasItems) {
                    matchedDataDir = candDir
                    try {
                        candDir.walkTopDown().filter { it.isFile && it.name.lowercase().endsWith(".obb") }.forEach {
                            if (!matchedObbList.contains(it)) matchedObbList.add(it)
                        }
                    } catch (_: Throwable) {}
                    break
                }
            }
        }

        val hasGameData = (matchedDataDir != null)
        val hasObb = matchedObbList.isNotEmpty() || hasGameData

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
                dataDir = matchedDataDir,
                packageName = pkgName,
                hasObb = hasObb,
                hasData = hasGameData,
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
    var detectedPackageName: String? = ApkManifestParser.extractPackageName(apkFile)
    var gameOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
    var iconFile: File? = null

    try {
        val iconsDir = File(context.cacheDir, "extracted_icons")
        if (!iconsDir.exists()) iconsDir.mkdirs()

        val safeName = apkFile.name.replace("[^a-zA-Z0-9]".toRegex(), "_")
        iconFile = File(iconsDir, "$safeName.png")

        val pm = context.packageManager
        val pkgInfo = try {
            pm.getPackageArchiveInfo(apkFile.absolutePath, PackageManager.GET_ACTIVITIES)
        } catch (e: Throwable) {
            null
        }

        if (detectedPackageName == null && pkgInfo?.packageName != null) {
            detectedPackageName = pkgInfo.packageName
        }

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
            return Triple(detectedPackageName, iconFile, gameOrientation)
        }

        // Tentativo di estrazione icona tramite PackageManager
        if (pkgInfo != null) {
            try {
                pkgInfo.applicationInfo.sourceDir = apkFile.absolutePath
                pkgInfo.applicationInfo.publicSourceDir = apkFile.absolutePath
                val drawable = pkgInfo.applicationInfo.loadIcon(pm)
                val bitmap = drawableToBitmap(drawable)
                FileOutputStream(iconFile).use { out ->
                    bitmap.compress(Bitmap.CompressFormat.PNG, 95, out)
                }
                return Triple(detectedPackageName, iconFile, gameOrientation)
            } catch (e: Throwable) {
                // Fallback a lettura diretta dal file ZIP
            }
        }

        // Fallback: estrazione diretta dell'icona dal file ZIP dell'APK
        if (extractIconFromZip(apkFile, iconFile)) {
            return Triple(detectedPackageName, iconFile, gameOrientation)
        }
    } catch (e: Throwable) {
        e.printStackTrace()
    }

    return Triple(detectedPackageName, if (iconFile?.exists() == true) iconFile else null, gameOrientation)
}

/**
 * Estrae un'icona valida cercando nei percorsi standard di risorse dell'APK zip
 */
private fun extractIconFromZip(apkFile: File, outputFile: File): Boolean {
    return try {
        ZipFile(apkFile).use { zip ->
            val entries = zip.entries().toList()
            val iconEntry = entries.firstOrNull {
                val name = it.name.lowercase()
                (name.startsWith("res/mipmap") || name.startsWith("res/drawable")) &&
                (name.endsWith("ic_launcher.png") || name.endsWith("icon.png") || name.endsWith("app_icon.png"))
            } ?: entries.firstOrNull {
                val name = it.name.lowercase()
                name.endsWith(".png") && (name.contains("icon") || name.contains("launcher"))
            }

            if (iconEntry != null) {
                zip.getInputStream(iconEntry).use { input ->
                    FileOutputStream(outputFile).use { output ->
                        input.copyTo(output)
                    }
                }
                outputFile.exists() && outputFile.length() > 0
            } else {
                false
            }
        }
    } catch (e: Throwable) {
        false
    }
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

/**
 * Parser binario AXML ad alte prestazioni per AndroidManifest.xml.
 * Funziona su tutte le versioni di Android senza restrizioni di permessi o targetSdkVersion.
 */
object ApkManifestParser {
    fun extractPackageName(apkFile: File): String? {
        return try {
            ZipFile(apkFile).use { zip ->
                val entry = zip.getEntry("AndroidManifest.xml") ?: return null
                val bytes = zip.getInputStream(entry).use { it.readBytes() }
                parseAxmlPackage(bytes)
            }
        } catch (e: Throwable) {
            null
        }
    }

    private fun parseAxmlPackage(data: ByteArray): String? {
        if (data.size < 8) return null
        val buf = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
        val magic = buf.int
        if (magic != 0x00080003) return null
        val fileSize = buf.int

        var pos = 8
        val stringTable = mutableListOf<String>()

        while (pos + 8 <= data.size) {
            val chunkType = buf.getInt(pos)
            val chunkSize = buf.getInt(pos + 4)
            if (chunkSize <= 0 || pos + chunkSize > data.size) break

            when (chunkType) {
                0x001C0001 -> { // RES_STRING_POOL_TYPE
                    val stringCount = buf.getInt(pos + 8)
                    val flags = buf.getInt(pos + 16)
                    val stringsStart = pos + buf.getInt(pos + 20)
                    val isUtf8 = (flags and (1 shl 8)) != 0

                    val offsets = IntArray(stringCount)
                    for (i in 0 until stringCount) {
                        offsets[i] = buf.getInt(pos + 28 + (i * 4))
                    }

                    for (off in offsets) {
                        val strPos = stringsStart + off
                        if (strPos >= data.size) {
                            stringTable.add("")
                            continue
                        }
                        if (isUtf8) {
                            var p = strPos
                            var u8Len = data[p].toInt() and 0xFF
                            p++
                            if ((u8Len and 0x80) != 0) {
                                u8Len = ((u8Len and 0x7F) shl 8) or (data[p].toInt() and 0xFF)
                                p++
                            }
                            var bLen = data[p].toInt() and 0xFF
                            p++
                            if ((bLen and 0x80) != 0) {
                                bLen = ((bLen and 0x7F) shl 8) or (data[p].toInt() and 0xFF)
                                p++
                            }
                            if (p + bLen <= data.size) {
                                stringTable.add(String(data, p, bLen, Charsets.UTF_8))
                            } else {
                                stringTable.add("")
                            }
                        } else {
                            var p = strPos
                            var u16Len = buf.getShort(p).toInt() and 0xFFFF
                            p += 2
                            if ((u16Len and 0x8000) != 0) {
                                u16Len = ((u16Len and 0x7FFF) shl 16) or (buf.getShort(p).toInt() and 0xFFFF)
                                p += 2
                            }
                            val bLen = u16Len * 2
                            if (p + bLen <= data.size) {
                                stringTable.add(String(data, p, bLen, Charsets.UTF_16LE))
                            } else {
                                stringTable.add("")
                            }
                        }
                    }
                }

                0x00100102 -> { // RES_XML_START_ELEMENT_TYPE
                    val nameIdx = buf.getInt(pos + 20)
                    val tagName = stringTable.getOrNull(nameIdx) ?: ""
                    if (tagName == "manifest") {
                        val attrCount = buf.getShort(pos + 28).toInt() and 0xFFFF
                        var attrPos = pos + 36
                        for (a in 0 until attrCount) {
                            if (attrPos + 20 > data.size) break
                            val attrNameIdx = buf.getInt(attrPos + 4)
                            val attrValIdx = buf.getInt(attrPos + 8)
                            val attrName = stringTable.getOrNull(attrNameIdx) ?: ""
                            if (attrName == "package") {
                                val pkgVal = stringTable.getOrNull(attrValIdx)
                                if (!pkgVal.isNullOrEmpty()) return pkgVal
                            }
                            attrPos += 20
                        }
                    }
                }
            }
            pos += chunkSize
        }

        // Fallback Euristico: cerca pattern di package valido nel pool delle stringhe
        val pkgRegex = Regex("""^[a-zA-Z][a-zA-Z0-9_]*(\.[a-zA-Z0-9_]+)+$""")
        for (str in stringTable) {
            if (pkgRegex.matches(str) &&
                !str.startsWith("android.") &&
                !str.startsWith("java.") &&
                !str.startsWith("schemas.") &&
                !str.contains("http") &&
                str.contains(".")
            ) {
                return str
            }
        }

        return null
    }
}
