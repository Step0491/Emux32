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
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import com.emux.translator.model.EmuxRom
import com.emux.translator.model.scanEmuxStorage
import com.emux.translator.nativebridge.NativeEngine
import com.emux.translator.ui.game.GamePlayerScreen
import com.emux.translator.ui.grid.EmuxGridScreen
import com.emux.translator.ui.log.SystemLogViewerDialog
import com.emux.translator.ui.theme.EmuxTranslatorTheme
import com.emux.translator.util.EmuxLogger
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream

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

        val emuxDir = resolveEmuxDirectory()
        ensureDirectoryTreeSafe(emuxDir)

        // Inizializza file di log persistente: sia interno che in Emux32/log/emux_execution.log
        EmuxLogger.initLogFiles(cacheDir, emuxDir)
        EmuxLogger.startLogcatCapture(lifecycleScope)
        EmuxLogger.i("Emux32", "Emux32 avviato su Android SDK ${Build.VERSION.SDK_INT}. Cartella storage: ${emuxDir.absolutePath}")

        // GESTIONE DELLA CONCORRENZA (DIRETTIVA 1): Inizializzazione asincrona isolata su Dispatchers.IO.
        // Mai bloccare o eseguire chiamate JNI pesanti all'interno di onCreate() sul Main Thread.
        lifecycleScope.launch(Dispatchers.IO) {
            try {
                NativeEngine.nativeSetStoragePath(emuxDir.absolutePath)
                val initialized = NativeEngine.nativeInitEngine(512)
                if (initialized) {
                    EmuxLogger.i("Emux32", "Unicorn ARM32 DBT Engine pre-inizializzato con successo (512 MB guest RAM).")
                } else {
                    EmuxLogger.w("Emux32", "Pre-inizializzazione Unicorn differita all'avvio del titolo.")
                }
            } catch (t: Throwable) {
                EmuxLogger.e("Emux32", "Eccezione durante pre-inizializzazione JNI asincrona: ${t.message}")
            }
        }

        setContent {
            EmuxTranslatorTheme {
                var games by remember { mutableStateOf<List<EmuxRom>>(emptyList()) }
                var activeGame by remember { mutableStateOf<EmuxRom?>(null) }
                var showSetupDialog by remember { mutableStateOf(false) }
                var showLogDialog by remember { mutableStateOf(false) }
                var currentEmuxDir by remember { mutableStateOf(resolveEmuxDirectory()) }

                val scope = rememberCoroutineScope()

                val refreshGames: () -> Unit = {
                    scope.launch(Dispatchers.IO) {
                        val base = resolveEmuxDirectory()
                        val apkDir = File(base, "apk")
                        val obbDir = File(base, "obb")

                        ensureDirectoryTreeSafe(base)
                        NativeEngine.nativeSetStoragePath(base.absolutePath)

                        EmuxLogger.i("Emux32", "Scansione libreria giochi in: ${base.absolutePath}")
                        val scanned = scanEmuxStorage(this@MainActivity, apkDir, obbDir)
                        EmuxLogger.i("Emux32", "Scansione completata: trovati ${scanned.size} titoli supportati.")

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
                            val dir = getDefaultEmuxDirectory()
                            prefs.edit()
                                .putBoolean("emux_setup_completed", true)
                                .putString("custom_emux_path", dir.absolutePath)
                                .apply()
                            NativeEngine.nativeSetStoragePath(dir.absolutePath)
                            showSetupDialog = false
                            refreshGames()
                        },
                        onChooseCustom = {
                            onFolderSelectedCallback = { chosenDir ->
                                prefs.edit()
                                    .putBoolean("emux_setup_completed", true)
                                    .putString("custom_emux_path", chosenDir.absolutePath)
                                    .apply()
                                NativeEngine.nativeSetStoragePath(chosenDir.absolutePath)
                                showSetupDialog = false
                                refreshGames()
                            }
                            chooseFolderLauncher.launch(null)
                        }
                    )
                }

                if (showLogDialog) {
                    SystemLogViewerDialog(
                        onDismiss = { showLogDialog = false }
                    )
                }

                if (activeGame != null) {
                    GamePlayerScreen(
                        game = activeGame!!,
                        onExitGame = {
                            activeGame = null
                            refreshGames()
                        },
                        onShowLogs = {
                            showLogDialog = true
                        }
                    )
                } else {
                    EmuxGridScreen(
                        games = games,
                        currentPathLabel = currentEmuxDir.absolutePath,
                        onGameClick = { rom ->
                            activeGame = rom
                            EmuxLogger.i("Emux32", "Click su titolo: '${rom.title}' (${rom.packageName ?: "senza package"})")
                            // GESTIONE DELLA CONCORRENZA: Invocazione asincrona isolata su Dispatchers.IO
                            scope.launch(Dispatchers.IO) {
                                launchGameAsync(rom)
                            }
                        },
                        onChangePathClick = {
                            onFolderSelectedCallback = { chosenDir ->
                                prefs.edit()
                                    .putString("custom_emux_path", chosenDir.absolutePath)
                                    .apply()
                                NativeEngine.nativeSetStoragePath(chosenDir.absolutePath)
                                refreshGames()
                            }
                            chooseFolderLauncher.launch(null)
                        },
                        onShowLogsClick = {
                            showLogDialog = true
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
            if (target.name.equals("Emux32", ignoreCase = true)) target else File(target, "Emux32")
        } else {
            getDefaultEmuxDirectory()
        }

        ensureDirectoryTreeSafe(resolvedDir)
        onFolderSelectedCallback?.invoke(resolvedDir)
    }

    private fun getDefaultEmuxDirectory(): File {
        return File(Environment.getExternalStorageDirectory(), "Emux32")
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
            val logDir = File(emuxDir, "log")
            if (!logDir.exists()) {
                logDir.mkdirs()
            }
        } catch (e: Exception) {
            EmuxLogger.e("Emux32", "Errore creazione directory Emux: ${e.message}")
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

    /**
     * Esecuzione completa e rigorosa in 5 fasi con log di sistema per ogni step diagnostico.
     */
    private suspend fun launchGameAsync(rom: EmuxRom) = withContext(Dispatchers.IO) {
        try {
            // ==========================================
            // FASE 1: Rilevamento e Validazione APK
            // ==========================================
            EmuxLogger.i("Emux32", "========== AVVIO GIOCO: ${rom.title} ==========")
            EmuxLogger.i("Emux32", "[FASE 1/5] Validazione file APK...")
            val apkFile = rom.apkFile
            if (!apkFile.exists()) {
                EmuxLogger.e("Emux32", "[FASE 1/5] ERRORE FATALE: File APK inesistente: ${apkFile.absolutePath}")
                withContext(Dispatchers.Main) {
                    Toast.makeText(this@MainActivity, "File APK non trovato: ${apkFile.name}", Toast.LENGTH_SHORT).show()
                }
                return@withContext
            }

            EmuxLogger.i("Emux32", "[FASE 1/5] APK valido: ${apkFile.name} (${apkFile.length()} byte, path: ${apkFile.absolutePath})")
            val pkg = rom.packageName ?: "com.game.legacy"
            EmuxLogger.i("Emux32", "[FASE 1/5] Package Name target: $pkg")

            // ==========================================
            // FASE 2: Verifica e Linking File OBB
            // ==========================================
            EmuxLogger.i("Emux32", "[FASE 2/5] Verifica file OBB associati...")
            val obbList = rom.obbFiles
            if (obbList.isEmpty()) {
                EmuxLogger.i("Emux32", "[FASE 2/5] Nessun file OBB richiesto o presente per questo titolo.")
            } else {
                EmuxLogger.i("Emux32", "[FASE 2/5] Trovati ${obbList.size} file OBB da collegare:")
                for (obb in obbList) {
                    EmuxLogger.i("Emux32", "   -> File OBB sorgente: ${obb.name} (${obb.length()} byte)")
                }

                // Percorsi target multipli per massima compatibilità con Scoped Storage
                val targetDirs = listOf(
                    File(Environment.getExternalStorageDirectory(), "Android/obb/$pkg"),
                    File(getExternalFilesDir(null)?.parentFile?.parentFile, "obb/$pkg"),
                    File(filesDir, "obb/$pkg")
                )

                for (targetDir in targetDirs) {
                    try {
                        if (!targetDir.exists()) {
                            targetDir.mkdirs()
                        }
                    } catch (_: Exception) {}

                    if (targetDir.exists() && targetDir.canWrite()) {
                        for (obbFile in obbList) {
                            val targetObb = File(targetDir, obbFile.name)
                            if (!targetObb.exists() || targetObb.length() != obbFile.length()) {
                                try {
                                    // Tentativo symlink a costo zero
                                    android.system.Os.symlink(obbFile.absolutePath, targetObb.absolutePath)
                                    EmuxLogger.i("Emux32", "[FASE 2/5] Symlink OBB creato con successo: ${targetObb.absolutePath}")
                                } catch (e: Exception) {
                                    // Fallback copia fisica se symlink non supportato dal filesystem FAT/exFAT
                                    try {
                                        FileInputStream(obbFile).use { input ->
                                            FileOutputStream(targetObb).use { output ->
                                                input.copyTo(output)
                                            }
                                        }
                                        EmuxLogger.i("Emux32", "[FASE 2/5] OBB collegato tramite copia: ${targetObb.absolutePath} (${targetObb.length()} byte)")
                                    } catch (copyErr: Exception) {
                                        EmuxLogger.w("Emux32", "[FASE 2/5] Impossibile scrivere OBB in ${targetDir.absolutePath}: ${copyErr.message}")
                                    }
                                }
                            } else {
                                EmuxLogger.i("Emux32", "[FASE 2/5] OBB già presente e verificato in: ${targetObb.absolutePath}")
                            }
                        }
                    }
                }
            }

            // ==========================================
            // FASE 3: Estrazione Librerie Native ARM32 (.so)
            // ==========================================
            EmuxLogger.i("Emux32", "[FASE 3/5] Analisi APK ed estrazione librerie native ARM32 (armeabi-v7a / armeabi)...")
            val zip = java.util.zip.ZipFile(apkFile)
            val entries = zip.entries()
            val extractedSos = mutableListOf<File>()

            while (entries.hasMoreElements()) {
                val entry = entries.nextElement()
                val isArm32 = (entry.name.startsWith("lib/armeabi-v7a/") || entry.name.startsWith("lib/armeabi/")) && entry.name.endsWith(".so")
                if (isArm32) {
                    val outFile = File(cacheDir, File(entry.name).name)
                    if (outFile.exists()) {
                        outFile.delete()
                    }
                    zip.getInputStream(entry).use { inStream ->
                        outFile.outputStream().use { outStream -> inStream.copyTo(outStream) }
                    }
                    if (outFile.length() > 0) {
                        extractedSos.add(outFile)
                        EmuxLogger.i("Emux32", "   -> Estratta libreria ARM32: ${outFile.name} (${outFile.length()} byte)")
                    }
                }
            }
            zip.close()

            if (extractedSos.isEmpty()) {
                EmuxLogger.e("Emux32", "[FASE 3/5] ERRORE: Nessun binario nativo ARM32 (armeabi-v7a/armeabi) trovato nell'APK.")
                withContext(Dispatchers.Main) {
                    Toast.makeText(this@MainActivity, "Nessuna libreria ARM32 trovata nell'APK", Toast.LENGTH_LONG).show()
                }
                return@withContext
            }

            // Selezione della libreria principale del gioco:
            // 1) Blood & Glory e titoli Glu Mobile usano 'libg.so'
            // 2) Titoli Unity/Unreal Engine: 'libunity.so' contiene il motore primario di gioco e rendering.
            //    libmain.so è solo il bridge JNI (spesso di pochi KB); preferiamo sempre il core pesante.
            var mainSo: File? = extractedSos.firstOrNull { file ->
                val name = file.name.lowercase()
                name == "libg.so" || name == "libunity.so" || name == "libgame.so" ||
                name.contains("blood") || name.contains("glory")
            }

            if (mainSo == null) {
                mainSo = extractedSos.firstOrNull { file ->
                    val name = file.name.lowercase()
                    name == "libmain.so"
                }
            }

            if (mainSo == null) {
                mainSo = extractedSos.firstOrNull { file ->
                    val name = file.name.lowercase()
                    !name.contains("stl") && !name.contains("shared") && !name.contains("c++")
                }
            }

            if (mainSo == null) {
                mainSo = extractedSos.first()
            }

            EmuxLogger.i("Emux32", "[FASE 3/5] Libreria principale selezionata per l'esecuzione: ${mainSo.name} (${mainSo.length()} byte)")

            // ==========================================
            // FASE 4: Setup Motore Nativo DBT Unicorn
            // ==========================================
            EmuxLogger.i("Emux32", "[FASE 4/5] Configurazione e avvio Unicorn ARM32 DBT Engine...")
            NativeEngine.nativeSetStoragePath(resolveEmuxDirectory().absolutePath)
            val engineReady = NativeEngine.nativeInitEngine(512)
            if (!engineReady) {
                EmuxLogger.e("Emux32", "[FASE 4/5] Inizializzazione motore nativo fallita!")
                return@withContext
            }
            NativeEngine.nativeConfigurePerformance(60, enableFhd = true, throttleGuard = true)
            EmuxLogger.i("Emux32", "[FASE 4/5] Motore DBT configurato: 60 FPS Lock, Buffer FHD 1080p, Throttle Guard attivo.")

            // ==========================================
            // FASE 5: Caricamento ELF ed Esecuzione Guest
            // ==========================================
            // Pre-caricamento librerie di supporto condivise (STL, libc++, audio) se presenti nell'APK
            for (depFile in extractedSos) {
                if (depFile != mainSo) {
                    val depName = depFile.name.lowercase()
                    if (depName.contains("stl") || depName.contains("c++") || depName.contains("shared") || depName.contains("fmod")) {
                        EmuxLogger.i("Emux32", "[FASE 5/5] Pre-caricamento libreria di supporto: ${depFile.name}")
                        NativeEngine.nativeLoadElfLibrary(depFile.absolutePath)
                    }
                }
            }

            EmuxLogger.i("Emux32", "[FASE 5/5] Caricamento ELF binario guest principale (${mainSo.name})...")
            val loaded = NativeEngine.nativeLoadElfLibrary(mainSo.absolutePath)
            if (loaded) {
                EmuxLogger.i("Emux32", "[FASE 5/5] ELF caricato e rilocato. Avvio uc_emu_start su thread isolato Dispatchers.IO...")
                val success = NativeEngine.startEmulationNative()
                if (success) {
                    EmuxLogger.i("Emux32", "[FASE 5/5] Esecuzione guest completata regolarmente.")
                } else {
                    EmuxLogger.w("Emux32", "[FASE 5/5] startEmulationNative ha terminato il ciclo.")
                }
            } else {
                EmuxLogger.e("Emux32", "[FASE 5/5] ERRORE: Caricamento ELF fallito per ${mainSo.name}")
                withContext(Dispatchers.Main) {
                    Toast.makeText(this@MainActivity, "Caricamento ELF fallito per ${mainSo.name}", Toast.LENGTH_SHORT).show()
                }
            }
        } catch (e: Throwable) {
            EmuxLogger.e("Emux32", "Eccezione critica durante il lancio del gioco: ${e.message}", e)
            withContext(Dispatchers.Main) {
                Toast.makeText(this@MainActivity, "Errore avvio gioco: ${e.localizedMessage}", Toast.LENGTH_LONG).show()
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        EmuxLogger.i("Emux32", "MainActivity distrutta. Rilascio risorse motore nativo.")
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
            Text("Configurazione Cartella Emux32", fontWeight = FontWeight.Bold)
        },
        text = {
            Column {
                Text(
                    text = "Vuoi configurare la cartella Emux32 e le sottocartelle (apk e obb) nel percorso predefinito:\n",
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
