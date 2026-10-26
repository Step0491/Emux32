package com.emux.translator

import android.Manifest
import android.content.Intent
import android.content.pm.ActivityInfo
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.provider.Settings
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.*
import androidx.core.content.ContextCompat
import com.emux.translator.model.EmuxRom
import com.emux.translator.model.scanEmuxStorage
import com.emux.translator.nativebridge.NativeEngine
import com.emux.translator.ui.game.GamePlayerScreen
import com.emux.translator.ui.grid.EmuxGridScreen
import com.emux.translator.ui.theme.EmuxTranslatorTheme
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

class MainActivity : ComponentActivity() {

    private lateinit var apkFolder: File
    private lateinit var obbFolder: File

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) {
        checkAndInitializeFolders()
    }

    private val manageStorageLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) {
        checkAndInitializeFolders()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT

        val initialized = NativeEngine.nativeInitEngine(512)
        if (!initialized) {
            Toast.makeText(this, "Inizializzazione Engine ARM32 fallita", Toast.LENGTH_LONG).show()
        }

        // Definizione percorsi Emux/apk ed Emux/obb
        val externalStorage = Environment.getExternalStorageDirectory()
        val emuxRoot = File(externalStorage, "Emux")
        apkFolder = File(emuxRoot, "apk")
        obbFolder = File(emuxRoot, "obb")

        setContent {
            EmuxTranslatorTheme {
                var games by remember { mutableStateOf<List<EmuxRom>>(emptyList()) }
                var activeGame by remember { mutableStateOf<EmuxRom?>(null) }
                val scope = rememberCoroutineScope()

                val refreshGames = {
                    scope.launch(Dispatchers.IO) {
                        createEmuxDirectoriesIfNeeded()
                        val scanned = scanEmuxStorage(this@MainActivity, apkFolder, obbFolder)
                        withContext(Dispatchers.Main) {
                            games = scanned
                        }
                    }
                }

                LaunchedEffect(Unit) {
                    requestPermissionsAndSetup()
                    refreshGames()
                }

                LaunchedEffect(activeGame) {
                    if (activeGame != null) {
                        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR
                    } else {
                        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
                    }
                }

                if (activeGame != null) {
                    GamePlayerScreen(
                        game = activeGame!!,
                        onExitGame = {
                            activeGame = null
                            refreshGames()
                        }
                    )
                } else {
                    // Direttamente la griglia con tocco touch, 2 per riga, icone quadrate
                    EmuxGridScreen(
                        games = games,
                        onGameClick = { rom ->
                            activeGame = rom
                            scope.launch(Dispatchers.Default) {
                                launchGame(rom)
                            }
                        }
                    )
                }
            }
        }
    }

    override fun onResume() {
        super.onResume()
        checkAndInitializeFolders()
    }

    private fun requestPermissionsAndSetup() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            if (!Environment.isExternalStorageManager()) {
                try {
                    val intent = Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION).apply {
                        data = Uri.parse("package:$packageName")
                    }
                    manageStorageLauncher.launch(intent)
                } catch (e: Exception) {
                    val intent = Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION)
                    manageStorageLauncher.launch(intent)
                }
            } else {
                checkAndInitializeFolders()
            }
        } else {
            val permissionsNeeded = mutableListOf<String>()
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.READ_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED) {
                permissionsNeeded.add(Manifest.permission.READ_EXTERNAL_STORAGE)
            }
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.WRITE_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED) {
                permissionsNeeded.add(Manifest.permission.WRITE_EXTERNAL_STORAGE)
            }
            if (permissionsNeeded.isNotEmpty()) {
                permissionLauncher.launch(permissionsNeeded.toTypedArray())
            } else {
                checkAndInitializeFolders()
            }
        }
    }

    private fun checkAndInitializeFolders() {
        createEmuxDirectoriesIfNeeded()
    }

    private fun createEmuxDirectoriesIfNeeded() {
        try {
            if (!apkFolder.exists()) {
                apkFolder.mkdirs()
            }
            if (!obbFolder.exists()) {
                obbFolder.mkdirs()
            }
        } catch (e: Exception) {
            e.printStackTrace()
        }
    }

    private fun launchGame(rom: EmuxRom) {
        try {
            val apkFile = rom.apkFile
            if (!apkFile.exists()) return

            val zip = java.util.zip.ZipFile(apkFile)
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
        } catch (e: Exception) {
            e.printStackTrace()
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        NativeEngine.nativeReleaseEngine()
    }
}
