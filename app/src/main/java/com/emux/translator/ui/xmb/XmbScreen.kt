package com.emux.translator.ui.xmb

import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.snapping.rememberSnapFlingBehavior
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.scale
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import coil.compose.AsyncImage
import com.emux.translator.R
import com.emux.translator.model.EmuxRom
import com.emux.translator.model.XmbCategory
import kotlinx.coroutines.launch

@OptIn(ExperimentalFoundationApi::class)
@Composable
fun XmbScreen(
    games: List<EmuxRom>,
    onGameClick: (EmuxRom) -> Unit,
    onFolderSelectClick: () -> Unit,
    onToggleLayoutClick: () -> Unit
) {
    val configuration = LocalConfiguration.current
    val screenWidth = configuration.screenWidthDp.dp
    val screenHeight = configuration.screenHeightDp.dp
    val scope = rememberCoroutineScope()

    val categories = remember {
        listOf(
            XmbCategory.SETTINGS,
            XmbCategory.GAMES,
            XmbCategory.FAVORITES,
            XmbCategory.SYSTEM
        )
    }

    val horizontalState = rememberLazyListState(initialFirstVisibleItemIndex = 1)
    val horizontalSnap = rememberSnapFlingBehavior(lazyListState = horizontalState)

    val centeredCategoryIndex by remember {
        derivedStateOf {
            val visible = horizontalState.layoutInfo.visibleItemsInfo
            if (visible.isEmpty()) 0
            else {
                val centerOffset = horizontalState.layoutInfo.viewportEndOffset / 2
                visible.minByOrNull { kotlin.math.abs((it.offset + it.size / 2) - centerOffset) }?.index ?: 0
            }
        }
    }

    val selectedCategory = categories.getOrElse(centeredCategoryIndex) { XmbCategory.GAMES }
    val itemsForCategory = remember(selectedCategory, games) {
        when (selectedCategory) {
            XmbCategory.GAMES -> games
            XmbCategory.FAVORITES -> games.take(3)
            XmbCategory.SETTINGS -> emptyList()
            XmbCategory.SYSTEM -> emptyList()
        }
    }

    val verticalState = rememberLazyListState()
    val verticalSnap = rememberSnapFlingBehavior(lazyListState = verticalState)

    val centeredVerticalIndex by remember {
        derivedStateOf {
            val visible = verticalState.layoutInfo.visibleItemsInfo
            if (visible.isEmpty()) 0
            else {
                val centerOffset = verticalState.layoutInfo.viewportEndOffset / 2
                visible.minByOrNull { kotlin.math.abs((it.offset + it.size / 2) - centerOffset) }?.index ?: 0
            }
        }
    }

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(
                Brush.verticalGradient(
                    colors = listOf(Color(0xFF090D16), Color(0xFF0F172A), Color(0xFF060911))
                )
            )
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .statusBarsPadding()
                .padding(horizontal = 20.dp, vertical = 12.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(10.dp)
            ) {
                Image(
                    painter = painterResource(id = R.drawable.app_logo),
                    contentDescription = "Emux32 Logo",
                    modifier = Modifier
                        .size(38.dp)
                        .clip(RoundedCornerShape(8.dp))
                )
                Column {
                    Text(
                        text = "EMUX 32",
                        color = Color.White,
                        fontSize = 17.sp,
                        fontWeight = FontWeight.Bold,
                        letterSpacing = 1.sp
                    )
                    Text(
                        text = "ARM64 DBT Frontend",
                        color = Color(0xFF38BDF8),
                        fontSize = 11.sp,
                        fontWeight = FontWeight.Medium
                    )
                }
            }

            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                IconButton(
                    onClick = onFolderSelectClick,
                    modifier = Modifier.background(Color.White.copy(alpha = 0.08f), CircleShape)
                ) {
                    Icon(Icons.Default.Folder, contentDescription = "Seleziona Cartella", tint = Color.White)
                }
                IconButton(
                    onClick = onToggleLayoutClick,
                    modifier = Modifier.background(Color.White.copy(alpha = 0.08f), CircleShape)
                ) {
                    Icon(Icons.Default.Apps, contentDescription = "Passa a Griglia", tint = Color.White)
                }
            }
        }

        Box(
            modifier = Modifier
                .fillMaxWidth()
                .padding(top = 95.dp),
            contentAlignment = Alignment.Center
        ) {
            LazyRow(
                state = horizontalState,
                flingBehavior = horizontalSnap,
                contentPadding = PaddingValues(horizontal = screenWidth / 2 - 32.dp),
                horizontalArrangement = Arrangement.spacedBy(32.dp),
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.fillMaxWidth()
            ) {
                itemsIndexed(categories) { index, category ->
                    val isSelected = (index == centeredCategoryIndex)
                    val scale by animateFloatAsState(
                        targetValue = if (isSelected) 1.22f else 0.85f,
                        animationSpec = spring(dampingRatio = Spring.DampingRatioMediumBouncy),
                        label = "CategoryScale"
                    )
                    val alpha by animateFloatAsState(
                        targetValue = if (isSelected) 1.0f else 0.45f,
                        label = "CategoryAlpha"
                    )

                    val icon = when (category) {
                        XmbCategory.SETTINGS -> Icons.Default.Settings
                        XmbCategory.GAMES -> Icons.Default.PlayArrow
                        XmbCategory.FAVORITES -> Icons.Default.Star
                        XmbCategory.SYSTEM -> Icons.Default.Info
                    }

                    Column(
                        horizontalAlignment = Alignment.CenterHorizontally,
                        modifier = Modifier
                            .scale(scale)
                            .alpha(alpha)
                            .clickable {
                                scope.launch {
                                    horizontalState.animateScrollToItem(index)
                                }
                            }
                    ) {
                        Icon(
                            imageVector = icon,
                            contentDescription = category.title,
                            tint = if (isSelected) Color(0xFF38BDF8) else Color.White,
                            modifier = Modifier.size(36.dp)
                        )
                        Spacer(modifier = Modifier.height(4.dp))
                        Text(
                            text = category.title,
                            color = if (isSelected) Color.White else Color.Gray,
                            fontSize = 11.sp,
                            fontWeight = if (isSelected) FontWeight.SemiBold else FontWeight.Normal
                        )
                    }
                }
            }
        }

        Box(
            modifier = Modifier
                .fillMaxSize()
                .padding(top = 185.dp, bottom = 16.dp),
            contentAlignment = Alignment.Center
        ) {
            if (itemsForCategory.isEmpty()) {
                Column(
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.Center,
                    modifier = Modifier.fillMaxSize().padding(horizontal = 24.dp)
                ) {
                    Icon(
                        imageVector = Icons.Default.Folder,
                        contentDescription = null,
                        tint = Color.DarkGray,
                        modifier = Modifier.size(64.dp)
                    )
                    Spacer(modifier = Modifier.height(12.dp))
                    Text(
                        text = "Nessun gioco trovato",
                        color = Color.White,
                        fontSize = 16.sp,
                        fontWeight = FontWeight.Bold
                    )
                    Spacer(modifier = Modifier.height(4.dp))
                    Text(
                        text = "Tocca l'icona della cartella in alto per selezionare la cartella con i tuoi file APK a 32-bit",
                        color = Color.Gray,
                        fontSize = 13.sp,
                        lineHeight = 18.sp
                    )
                }
            } else {
                LazyColumn(
                    state = verticalState,
                    flingBehavior = verticalSnap,
                    contentPadding = PaddingValues(vertical = screenHeight / 3),
                    verticalArrangement = Arrangement.spacedBy(16.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    modifier = Modifier.fillMaxSize()
                ) {
                    itemsIndexed(itemsForCategory) { index, game ->
                        val isSelected = (index == centeredVerticalIndex)
                        val scale by animateFloatAsState(
                            targetValue = if (isSelected) 1.04f else 0.92f,
                            animationSpec = spring(dampingRatio = Spring.DampingRatioMediumBouncy),
                            label = "ItemScale"
                        )
                        val alpha by animateFloatAsState(
                            targetValue = if (isSelected) 1.0f else 0.45f,
                            label = "ItemAlpha"
                        )

                        XmbVerticalGameCard(
                            game = game,
                            isSelected = isSelected,
                            scale = scale,
                            alpha = alpha,
                            onClick = {
                                if (isSelected) {
                                    onGameClick(game)
                                } else {
                                    scope.launch {
                                        verticalState.animateScrollToItem(index)
                                    }
                                }
                            }
                        )
                    }
                }
            }
        }
    }
}

@Composable
fun XmbVerticalGameCard(
    game: EmuxRom,
    isSelected: Boolean,
    scale: Float,
    alpha: Float,
    onClick: () -> Unit
) {
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 20.dp)
            .height(105.dp)
            .scale(scale)
            .alpha(alpha)
            .clickable { onClick() }
            .then(
                if (isSelected) {
                    Modifier.border(
                        BorderStroke(2.dp, Color(0xFF38BDF8)),
                        RoundedCornerShape(14.dp)
                    )
                } else Modifier
            ),
        shape = RoundedCornerShape(14.dp),
        colors = CardDefaults.cardColors(
            containerColor = if (isSelected) Color(0xFF1E293B) else Color(0xFF111827)
        ),
        elevation = CardDefaults.cardElevation(defaultElevation = if (isSelected) 10.dp else 2.dp)
    ) {
        Row(
            modifier = Modifier
                .fillMaxSize()
                .padding(12.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Box(
                modifier = Modifier
                    .width(62.dp)
                    .height(81.dp)
                    .clip(RoundedCornerShape(8.dp))
                    .background(Color(0xFF0F172A)),
                contentAlignment = Alignment.Center
            ) {
                if (game.coverUri != null) {
                    AsyncImage(
                        model = game.coverUri,
                        contentDescription = game.title,
                        contentScale = ContentScale.Crop,
                        modifier = Modifier.fillMaxSize()
                    )
                } else {
                    Icon(
                        imageVector = Icons.Default.PlayArrow,
                        contentDescription = "Cover fallback",
                        tint = Color.Gray,
                        modifier = Modifier.size(32.dp)
                    )
                }
            }

            Spacer(modifier = Modifier.width(14.dp))

            Column(
                modifier = Modifier.weight(1f),
                verticalArrangement = Arrangement.Center
            ) {
                Text(
                    text = game.title,
                    color = Color.White,
                    fontSize = 15.sp,
                    fontWeight = FontWeight.Bold,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
                Spacer(modifier = Modifier.height(4.dp))
                Row(
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Surface(
                        color = Color(0xFF0284C7).copy(alpha = 0.3f),
                        shape = RoundedCornerShape(4.dp)
                    ) {
                        Text(
                            text = "ARMv7 (32-bit)",
                            color = Color(0xFF38BDF8),
                            fontSize = 10.sp,
                            fontWeight = FontWeight.Bold,
                            modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp)
                        )
                    }

                    if (game.hasObb) {
                        Surface(
                            color = Color(0xFF10B981).copy(alpha = 0.25f),
                            shape = RoundedCornerShape(4.dp)
                        ) {
                            Text(
                                text = "OBB Ready",
                                color = Color(0xFF34D399),
                                fontSize = 10.sp,
                                fontWeight = FontWeight.Bold,
                                modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp)
                            )
                        }
                    } else {
                        Text(
                            text = "DBT Engine",
                            color = Color.Gray,
                            fontSize = 11.sp
                        )
                    }
                }
            }

            if (isSelected) {
                Icon(
                    imageVector = Icons.Default.PlayArrow,
                    contentDescription = "Play",
                    tint = Color(0xFF38BDF8),
                    modifier = Modifier.size(32.dp).padding(end = 4.dp)
                )
            }
        }
    }
}
