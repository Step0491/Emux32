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

    // Scorre tutte le sottocartelle dentro la directory root (es. /games/NeedForSpeed/)
    val files = rootGamesDir.listFiles()
    for (gameFolder in files) {
        if (gameFolder.isDirectory) {
            val title = gameFolder.name ?: "Sconosciuto"
            var coverUri: Uri? = null
            var apkUri: Uri? = null

            for (file in gameFolder.listFiles()) {
                val fileName = file.name?.lowercase() ?: ""
                if (fileName.endsWith(".apk")) {
                    apkUri = file.uri
                } else if (fileName == "cover.jpg" || fileName == "cover.png" || fileName == "icon.png") {
                    coverUri = file.uri
                }
            }

            if (apkUri != null) {
                gamesList.add(
                    EmuxRom(
                        title = title,
                        coverUri = coverUri,
                        apkUri = apkUri,
                        obbDirUri = gameFolder.uri,
                        category = XmbCategory.GAMES,
                        isArm32 = true
                    )
                )
            }
        }
    }
    return gamesList
}
