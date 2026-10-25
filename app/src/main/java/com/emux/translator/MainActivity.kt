package com.emux.translator

import android.content.pm.ActivityInfo
import android.net.Uri
import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.*
import androidx.documentfile.provider.DocumentFile
import com.emux.translator.model.EmuxRom
import com.emux.translator.model.parseEmuxGames
import com.emux.translator.nativebridge.NativeEngine
import com.emux.translator.ui.game.GamePlayerScreen
import com.emux.translator.ui.grid.EmuxGridScreen
import com.emux.translator.ui.theme.EmuxTranslatorTheme
import com.emux.translator.ui.xmb.XmbScreen
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

enum class DisplayMode {
    XMB,
    GRID
}

class MainActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // All'avvio dell'app blocca l'orientamento in Verticale (Portrait) per il selettore
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT

        // Inizializza l'Engine Dynamic Binary Translator ARM32
        val initialized = NativeEngine.nativeInitEngine(512)
        if (!initialized) {
            Toast.makeText(this, "Inizializzazione Engine ARM32 fallita", Toast.LENGTH_LONG).show()
        }

        setContent {
            EmuxTranslatorTheme {
                var games by remember { mutableStateOf<List<EmuxRom>>(emptyList()) }
                var displayMode by remember { mutableStateOf(DisplayMode.XMB) }
                var activeGame by remember { mutableStateOf<EmuxRom?>(null) }
                val scope = rememberCoroutineScope()

                // Gestione dinamica dell'orientamento dello schermo
                LaunchedEffect(activeGame) {
                    if (activeGame != null) {
                        // In-Game: si adatta automaticamente all'orientamento del dispositivo tramite sensore
                        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR
                    } else {
                        // Nel selettore/menu: forzato rigidamente in Verticale (Portrait)
                        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
                    }
                }

                val folderPickerLauncher = rememberLauncherForActivityResult(
                    contract = ActivityResultContracts.OpenDocumentTree()
                ) { uri: Uri? ->
                    if (uri != null) {
                        contentResolver.takePersistableUriPermission(
                            uri,
                            android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION
                        )
                        val rootDoc = DocumentFile.fromTreeUri(this, uri)
                        if (rootDoc != null && rootDoc.isDirectory) {
                            scope.launch(Dispatchers.IO) {
                                val parsed = parseEmuxGames(rootDoc)
                                withContext(Dispatchers.Main) {
                                    games = parsed
                                    Toast.makeText(
                                        this@MainActivity,
                                        "Trovati ${parsed.size} giochi x32",
                                        Toast.LENGTH_SHORT
                                    ).show()
                                }
                            }
                        }
                    }
                }

                val onGameSelected: (EmuxRom) -> Unit = { rom ->
                    activeGame = rom
                    scope.launch(Dispatchers.Default) {
                        launchGame(rom)
                    }
                }

                if (activeGame != null) {
                    // Modalità In-Game: schermo adattivo con SurfaceView nativa
                    GamePlayerScreen(
                        game = activeGame!!,
                        onExitGame = { activeGame = null }
                    )
                } else {
                    // Modalità Selettore: rigorosamente in Verticale (Portrait)
                    when (displayMode) {
                        DisplayMode.XMB -> {
                            XmbScreen(
                                games = games,
                                onGameClick = onGameSelected,
                                onFolderSelectClick = { folderPickerLauncher.launch(null) },
                                onToggleLayoutClick = { displayMode = DisplayMode.GRID }
                            )
                        }
                        DisplayMode.GRID -> {
                            EmuxGridScreen(
                                games = games,
                                onGameClick = onGameSelected,
                                onFolderSelectClick = { folderPickerLauncher.launch(null) },
                                onToggleLayoutClick = { displayMode = DisplayMode.XMB }
                            )
                        }
                    }
                }
            }
        }
    }

    private fun launchGame(rom: EmuxRom) {
        if (rom.apkUri == null) return

        try {
            contentResolver.openInputStream(rom.apkUri)?.use { input ->
                val tempApk = java.io.File(cacheDir, "temp_guest.apk")
                tempApk.outputStream().use { output -> input.copyTo(output) }

                val zip = java.util.zip.ZipFile(tempApk)
                val entries = zip.entries()
                var extractedSo: java.io.File? = null

                while (entries.hasMoreElements()) {
                    val entry = entries.nextElement()
                    if (entry.name.startsWith("lib/armeabi-v7a/") && entry.name.endsWith(".so")) {
                        val outFile = java.io.File(cacheDir, java.io.File(entry.name).name)
                        zip.getInputStream(entry).use { inStream ->
                            outFile.outputStream().use { outStream -> inStream.copyTo(outStream) }
                        }
                        extractedSo = outFile
                        break
                    }
                }
                zip.close()

                if (extractedSo != null && extractedSo.exists()) {
                    val loaded = NativeEngine.nativeLoadElfLibrary(extractedSo.absolutePath)
                    if (loaded) {
                        NativeEngine.nativeExecuteEntryPoint()
                    }
                }
            }
        } catch (e: Exception) {
            e.printStackTrace()
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        NativeEngine.nativeReleaseEngine()
    }
}
