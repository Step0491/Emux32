package com.emux.translator

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

        // Inizializza l'Engine Dynamic Binary Translator ARM32 con 512MB di memoria virtuale guest
        val initialized = NativeEngine.nativeInitEngine(512)
        if (!initialized) {
            Toast.makeText(this, "Inizializzazione Engine ARM32 fallita", Toast.LENGTH_LONG).show()
        }

        setContent {
            EmuxTranslatorTheme {
                var games by remember { mutableStateOf<List<EmuxRom>>(emptyList()) }
                var displayMode by remember { mutableStateOf(DisplayMode.XMB) }
                val scope = rememberCoroutineScope()

                // Launcher Storage Access Framework (SAF) per selezione cartella giochi
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
                    Toast.makeText(this, "Avvio DBT x32: ${rom.title}", Toast.LENGTH_SHORT).show()
                    // Dispatch asincrono del caricamento ed esecuzione ELF 32-bit
                    scope.launch(Dispatchers.Default) {
                        launchGame(rom)
                    }
                }

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

    private fun launchGame(rom: EmuxRom) {
        // Logica di estrazione .so da APK e avvio nel Dynamic Binary Translation Engine
        if (rom.apkUri == null) return

        try {
            contentResolver.openInputStream(rom.apkUri)?.use { input ->
                val tempApk = java.io.File(cacheDir, "temp_guest.apk")
                tempApk.outputStream().use { output -> input.copyTo(output) }

                // Esempio: estrazione libgame.so da lib/armeabi-v7a/ dentro il file zip/apk
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
