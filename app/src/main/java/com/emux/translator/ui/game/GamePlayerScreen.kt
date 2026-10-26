package com.emux.translator.ui.game

import android.view.SurfaceHolder
import android.view.SurfaceView
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
    var showHud by remember { mutableStateOf(false) }

    LaunchedEffect(Unit) {
        // FHD (1920x1080), 60 FPS Lock, Throttle Guard attivo
        NativeEngine.nativeConfigurePerformance(
            targetFps = 60,
            enableFhd = true,
            throttleGuard = true
        )
    }

    // Intercetta il tasto Indietro hardware per ritornare al selettore
    BackHandler {
        onExitGame()
    }

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black)
            .clickable { showHud = !showHud }
    ) {
        // SurfaceView nativa con buffer fisso Full HD 1080p (1920x1080) per garantire rendering 1:1 nitido
        AndroidView(
            factory = { context ->
                SurfaceView(context).apply {
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
                }
            },
            modifier = Modifier.fillMaxSize()
        )

        // Overlay HUD In-Game
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
                        onClick = onExitGame,
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
                        shape = RoundedCornerShape(20.dp)
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
                                text = "60 FPS",
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
