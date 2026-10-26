package com.emux.translator

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
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
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CreateNewFolder
import androidx.compose.material.icons.filled.FolderOpen
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
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

    private lateinit var prefs: SharedPreferences
    private var customRootPath: String? = null

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) {
    }

    private val manageStorageLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) {
    }

    private val chooseFolderLauncher = registerForActivityResult(
        ActivityResultContracts.OpenDocumentTree()
    ) { uri: Uri? ->
        if (uri != null) {
            contentResolver.takePersistableUriPermission(
                uri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
            )
            handleCustomTreeUri(uri)
        }
    }

    private var onFolderSelectedCallback: ((File) -> Unit)? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        prefs = getSharedPreferences("emux_prefs", Context.MODE_PRIVATE)
        customRootPath = prefs.getString("custom_emux_path", null)

        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT

        val initialized = NativeEngine.nativeInitEngine(512)
        if (!initialized) {
            Toast.makeText(this, "Inizializzazione Engine ARM32 fallita", Toast.LENGTH_LONG).show()
        }

        setContent {
            EmuxTranslatorTheme {
                var games by remember { mutableStateOf<List<EmuxRom>>(emptyList()) }
                var activeGame by remember { mutableStateOf<EmuxRom?>(null) }
                var showSetupDialog by remember { mutableStateOf(false) }
                var currentEmuxDir by remember { mutableStateOf(resolveEmuxDirectory()) }

                val scope = rememberCoroutineScope()

                val refreshGames: () -> Unit = {
                    scope.launch(Dispatchers.IO) {
                        val base = resolveEmuxDirectory()
                        val apkDir = File(base, "apk")
                        val obbDir = File(base, "obb")

                        ensureDirectoryTreeSafe(base)

                        val scanned = scanEmuxStorage(this@MainActivity, apkDir, obbDir)
                        withContext(Dispatchers.Main) {
                            games = scanned
                            currentEmuxDir = base
                        }
                    }
                }

                LaunchedEffect(Unit) {
                    requestPermissionsIfNeeded()
                    val setupDone = prefs.getBoolean("emux_setup_completed", false)
                    if (!setupDone) {
                        showSetupDialog = true
                    } else {
                        refreshGames()
                    }
                }

                // Orientamento adattivo dinamico in base al titolo giocato
                LaunchedEffect(activeGame) {
                    if (activeGame != null) {
                        requestedOrientation = activeGame!!.preferredOrientation
                    } else {
                        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
                    }
                }

                if (showSetupDialog) {
                    SetupStorageDialog(
                        defaultPath = getDefaultEmuxDirectory().absolutePath,
                        onConfirmDefault = {
                            prefs.edit()
                                .putBoolean("emux_setup_completed", true)
                                .putString("custom_emux_path", getDefaultEmuxDirectory().absolutePath)
                                .apply()
                            showSetupDialog = false
                            refreshGames()
                        },
                        onChooseCustom = {
                            onFolderSelectedCallback = { chosenDir ->
                                prefs.edit()
                                    .putBoolean("emux_setup_completed", true)
                                    .putString("custom_emux_path", chosenDir.absolutePath)
                                    .apply()
                                showSetupDialog = false
                                refreshGames()
                            }
                            chooseFolderLauncher.launch(null)
                        }
                    )
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
                    EmuxGridScreen(
                        games = games,
                        currentPathLabel = currentEmuxDir.absolutePath,
                        onGameClick = { rom ->
                            activeGame = rom
                            scope.launch(Dispatchers.Default) {
                                launchGame(rom)
                            }
                        },
                        onChangePathClick = {
                            onFolderSelectedCallback = { chosenDir ->
                                prefs.edit()
                                    .putString("custom_emux_path", chosenDir.absolutePath)
                                    .apply()
                                refreshGames()
                            }
                            chooseFolderLauncher.launch(null)
                        }
                    )
                }
            }
        }
    }

    private fun handleCustomTreeUri(treeUri: Uri) {
        val path = treeUri.path ?: ""
        val resolvedDir: File = if (path.contains("primary:")) {
            val relative = path.substringAfter("primary:")
            val target = File(Environment.getExternalStorageDirectory(), relative)
            if (target.name.equals("Emux", ignoreCase = true)) target else File(target, "Emux")
        } else {
            getDefaultEmuxDirectory()
        }

        ensureDirectoryTreeSafe(resolvedDir)
        onFolderSelectedCallback?.invoke(resolvedDir)
    }

    private fun getDefaultEmuxDirectory(): File {
        return File(Environment.getExternalStorageDirectory(), "Emux")
    }

    private fun resolveEmuxDirectory(): File {
        val saved = prefs.getString("custom_emux_path", null)
        return if (!saved.isNullOrBlank()) {
            File(saved)
        } else {
            getDefaultEmuxDirectory()
        }
    }

    private fun ensureDirectoryTreeSafe(emuxDir: File) {
        try {
            if (!emuxDir.exists()) {
                emuxDir.mkdirs()
            }
            val apkDir = File(emuxDir, "apk")
            if (!apkDir.exists()) {
                apkDir.mkdirs()
            }
            val obbDir = File(emuxDir, "obb")
            if (!obbDir.exists()) {
                obbDir.mkdirs()
            }
        } catch (e: Exception) {
            e.printStackTrace()
        }
    }

    private fun requestPermissionsIfNeeded() {
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
            }
        }
    }

    private fun launchGame(rom: EmuxRom) {
        try {
            val apkFile = rom.apkFile
            if (!apkFile.exists()) return

            val zip = java.util.zip.ZipFile(apkFile)
            val entries = zip.entries()
            var extractedSo: File? = null

            while (entries.hasMoreElements()) {
                val entry = entries.nextElement()
                if (entry.name.startsWith("lib/armeabi-v7a/") && entry.name.endsWith(".so")) {
                    val outFile = File(cacheDir, File(entry.name).name)
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

@Composable
fun SetupStorageDialog(
    defaultPath: String,
    onConfirmDefault: () -> Unit,
    onChooseCustom: () -> Unit
) {
    AlertDialog(
        onDismissRequest = { },
        shape = RoundedCornerShape(18.dp),
        containerColor = Color(0xFF1E1E1E),
        titleContentColor = Color.White,
        textContentColor = Color.LightGray,
        title = {
            Text("Configurazione Cartella Emux", fontWeight = FontWeight.Bold)
        },
        text = {
            Column {
                Text(
                    text = "Vuoi configurare la cartella Emux e le sottocartelle (apk e obb) nel percorso predefinito:\n",
                    fontSize = 14.sp
                )
                Surface(
                    color = Color(0xFF121212),
                    shape = RoundedCornerShape(8.dp),
                    modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp)
                ) {
                    Text(
                        text = defaultPath,
                        color = Color(0xFF38BDF8),
                        fontSize = 12.sp,
                        fontWeight = FontWeight.SemiBold,
                        modifier = Modifier.padding(10.dp)
                    )
                }
                Spacer(modifier = Modifier.height(6.dp))
                Text(
                    text = "Nota: se la cartella esiste già, non verrà eliminato né sovrascritto alcun file.",
                    fontSize = 12.sp,
                    color = Color.Gray
                )
            }
        },
        confirmButton = {
            Button(
                onClick = onConfirmDefault,
                colors = ButtonDefaults.buttonColors(containerColor = Color(0xFF38BDF8), contentColor = Color.Black)
            ) {
                Text("Crea / Usa Qui", fontWeight = FontWeight.Bold)
            }
        },
        dismissButton = {
            OutlinedButton(
                onClick = onChooseCustom,
                colors = ButtonDefaults.outlinedButtonColors(contentColor = Color.White)
            ) {
                Text("Altro Percorso")
            }
        }
    )
}
