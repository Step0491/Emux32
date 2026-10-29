package com.emux.translator.ui.game

import android.app.Activity
import android.os.Build
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowBack
import androidx.compose.material.icons.filled.Terminal
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import com.emux.translator.model.EmuxRom
import com.emux.translator.nativebridge.NativeEngine

@Composable
fun GamePlayerScreen(
    game: EmuxRom,
    onExitGame: () -> Unit,
    onShowLogs: () -> Unit = {}
) {
    var showHud by remember { mutableStateOf(true) }
    val context = LocalContext.current

    // Mostra l'HUD informativa per 4 secondi all'avvio, poi sfuma per lasciare il fullscreen
    LaunchedEffect(Unit) {
        kotlinx.coroutines.delay(4000)
        showHud = false
    }

    // Modalità Immersiva Fullscreen completa (nasconde Navigation Bar e Status Bar)
    DisposableEffect(Unit) {
        val window = (context as? Activity)?.window
        if (window != null) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                window.setDecorFitsSystemWindows(false)
                val controller = window.insetsController
                controller?.hide(WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars())
                controller?.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            } else {
                @Suppress("DEPRECATION")
                window.decorView.systemUiVisibility = (
                    View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    or View.SYSTEM_UI_FLAG_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                )
            }
        }

        NativeEngine.nativeConfigurePerformance(
            targetFps = 60,
            enableFhd = true,
            throttleGuard = true
        )

        onDispose {
            NativeEngine.nativeStopEmulation()
            val w = (context as? Activity)?.window
            if (w != null) {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    w.setDecorFitsSystemWindows(true)
                    w.insetsController?.show(WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars())
                } else {
                    @Suppress("DEPRECATION")
                    w.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_VISIBLE
                }
            }
        }
    }

    // Intercetta il tasto Indietro hardware per ritornare al selettore
    BackHandler {
        NativeEngine.nativeStopEmulation()
        onExitGame()
    }

    Box(
        modifier = Modifier.fillMaxSize()
    ) {
        // SurfaceView nativa con inoltro diretto touch screen 1:1 e buffer fisso Full HD 1080p
        AndroidView(
            factory = { ctx ->
                SurfaceView(ctx).apply {
                    try {
                        setZOrderMediaOverlay(true)
                    } catch (_: Throwable) {}
                    holder.setFixedSize(1920, 1080)
                    holder.addCallback(object : SurfaceHolder.Callback {
                        override fun surfaceCreated(holder: SurfaceHolder) {
                            NativeEngine.nativeSetSurface(holder.surface)
                        }

                        override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                            NativeEngine.nativeSetSurface(holder.surface)
                        }

                        override fun surfaceDestroyed(holder: SurfaceHolder) {
                            NativeEngine.nativeSetSurface(null)
                        }
                    })

                    // Inoltro diretto reattivo dei comandi touch hardware verso l'engine nativo
                    setOnTouchListener { _, event ->
                        val action = event.actionMasked
                        val pointerIndex = event.actionIndex
                        val pointerId = event.getPointerId(pointerIndex)
                        val normX = (event.getX(pointerIndex) / width.coerceAtLeast(1)) * 1920f
                        val normY = (event.getY(pointerIndex) / height.coerceAtLeast(1)) * 1080f

                        NativeEngine.nativeSendTouchEvent(action, normX, normY, pointerId)

                        // Un triplo tocco rapido con 3 dita o tocco prolungato in alto apre l'HUD
                        if (event.pointerCount >= 3 || (event.action == MotionEvent.ACTION_DOWN && event.y < 80f)) {
                            showHud = !showHud
                        }
                        true
                    }
                }
            },
            modifier = Modifier.fillMaxSize()
        )

        // Overlay HUD In-Game a scomparsa rapida
        AnimatedVisibility(
            visible = showHud,
            enter = fadeIn(),
            exit = fadeOut(),
            modifier = Modifier.fillMaxSize()
        ) {
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(16.dp)
            ) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .align(Alignment.TopCenter),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    IconButton(
                        onClick = {
                            NativeEngine.nativeStopEmulation()
                            onExitGame()
                        },
                        modifier = Modifier.background(Color.Black.copy(alpha = 0.6f), CircleShape)
                    ) {
                        Icon(
                            imageVector = Icons.Default.ArrowBack,
                            contentDescription = "Torna al Selettore",
                            tint = Color.White
                        )
                    }

                    Surface(
                        color = Color.Black.copy(alpha = 0.75f),
                        shape = RoundedCornerShape(20.dp),
                        modifier = Modifier.clickable { showHud = false }
                    ) {
                        Row(
                            modifier = Modifier.padding(horizontal = 14.dp, vertical = 6.dp),
                            horizontalArrangement = Arrangement.spacedBy(10.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Text(
                                text = game.title,
                                color = Color.White,
                                fontSize = 14.sp,
                                fontWeight = FontWeight.Bold
                            )
                            Surface(
                                color = Color(0xFF0284C7),
                                shape = RoundedCornerShape(6.dp)
                            ) {
                                Text(
                                    text = "FHD 1080p",
                                    color = Color.White,
                                    fontSize = 10.sp,
                                    fontWeight = FontWeight.Bold,
                                    modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp)
                                )
                            }
                            Text(
                                text = "60 FPS Touch",
                                color = Color(0xFF4ADE80),
                                fontSize = 12.sp,
                                fontWeight = FontWeight.Bold
                            )
                        }
                    }

                    IconButton(
                        onClick = onShowLogs,
                        modifier = Modifier.background(Color.Black.copy(alpha = 0.6f), CircleShape)
                    ) {
                        Icon(
                            imageVector = Icons.Default.Terminal,
                            contentDescription = "Log di Sistema",
                            tint = Color(0xFF38BDF8)
                        )
                    }
                }
            }
        }
    }
}
