package com.emux.translator.model

import android.net.Uri
import androidx.documentfile.provider.DocumentFile

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
    val category: XmbCategory = XmbCategory.GAMES,
    val isArm32: Boolean = true
)

fun parseEmuxGames(rootGamesDir: DocumentFile): List<EmuxRom> {
    val gamesList = mutableListOf<EmuxRom>()
    val files = rootGamesDir.listFiles()

    // 1. Ricerca APK posizionati direttamente nella cartella root selezionata (es. /Emux32/BloodAndGlory.apk)
    for (file in files) {
        val fileName = file.name?.lowercase() ?: ""
        if (file.isFile && fileName.endsWith(".apk")) {
            val title = file.name?.substringBeforeLast(".") ?: "Sconosciuto"
            gamesList.add(
                EmuxRom(
                    title = title,
                    coverUri = null,
                    apkUri = file.uri,
                    obbDirUri = null,
                    category = XmbCategory.GAMES,
                    isArm32 = true
                )
            )
        }
    }

    // 2. Ricerca in sottocartelle dedicate (es. /Emux32/BloodAndGlory/game.apk)
    for (folder in files) {
        if (folder.isDirectory) {
            val title = folder.name ?: "Sconosciuto"
            var coverUri: Uri? = null
            var apkUri: Uri? = null
            var obbUri: Uri? = null

            for (subFile in folder.listFiles()) {
                val subName = subFile.name?.lowercase() ?: ""
                if (subName.endsWith(".apk")) {
                    apkUri = subFile.uri
                } else if (subName == "cover.jpg" || subName == "cover.png" || subName == "icon.png") {
                    coverUri = subFile.uri
                } else if (subName.endsWith(".obb") || (subFile.isDirectory && subName.contains("obb"))) {
                    obbUri = subFile.uri
                }
            }

            if (apkUri != null) {
                gamesList.add(
                    EmuxRom(
                        title = title,
                        coverUri = coverUri,
                        apkUri = apkUri,
                        obbDirUri = obbUri ?: folder.uri,
                        category = XmbCategory.GAMES,
                        isArm32 = true
                    )
                )
            }
        }
    }

    return gamesList
}
