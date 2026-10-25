package com.emux.translator.ui.grid

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Folder
import androidx.compose.material.icons.filled.List
import androidx.compose.material.icons.filled.Menu
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.ViewCarousel
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import coil.compose.AsyncImage
import com.emux.translator.model.EmuxRom

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun EmuxGridScreen(
    games: List<EmuxRom>,
    onGameClick: (EmuxRom) -> Unit,
    onFolderSelectClick: () -> Unit,
    onToggleLayoutClick: () -> Unit
) {
    // Tema scuro forzato come da reference NetherSX2
    MaterialTheme(colorScheme = darkColorScheme(background = Color(0xFF121212))) {
        Scaffold(
            topBar = {
                TopAppBar(
                    title = { Text("Tutti i giochi (x32)") },
                    navigationIcon = {
                        IconButton(onClick = { /* Apri Drawer */ }) {
                            Icon(Icons.Default.Menu, contentDescription = "Menu")
                        }
                    },
                    actions = {
                        IconButton(onClick = { /* Logica di ricerca */ }) {
                            Icon(Icons.Default.Search, contentDescription = "Cerca")
                        }
                        IconButton(onClick = onToggleLayoutClick) {
                            Icon(Icons.Default.ViewCarousel, contentDescription = "Passa a XMB")
                        }
                    },
                    colors = TopAppBarDefaults.topAppBarColors(
                        containerColor = Color(0xFF1E1E1E),
                        titleContentColor = Color.White,
                        actionIconContentColor = Color.White,
                        navigationIconContentColor = Color.White
                    )
                )
            },
            floatingActionButton = {
                FloatingActionButton(
                    onClick = onFolderSelectClick,
                    containerColor = Color(0xFF333333),
                    contentColor = Color.White
                ) {
                    Icon(Icons.Default.Folder, contentDescription = "Seleziona Cartella Root")
                }
            },
            containerColor = MaterialTheme.colorScheme.background
        ) { paddingValues ->
            LazyVerticalGrid(
                columns = GridCells.Fixed(2), // Griglia a 2 colonne esatta
                modifier = Modifier
                    .fillMaxSize()
                    .padding(paddingValues)
                    .padding(horizontal = 8.dp),
                contentPadding = PaddingValues(top = 16.dp, bottom = 80.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalArrangement = Arrangement.spacedBy(16.dp)
            ) {
                items(games) { game ->
                    GameCoverCard(game = game, onClick = { onGameClick(game) })
                }
            }
        }
    }
}

@Composable
fun GameCoverCard(game: EmuxRom, onClick: () -> Unit) {
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .aspectRatio(0.7f) // Aspect ratio 3:4 tipico delle custodie DVD/PS2
            .clickable { onClick() },
        elevation = CardDefaults.cardElevation(defaultElevation = 4.dp),
        colors = CardDefaults.cardColors(containerColor = Color(0xFF252525))
    ) {
        if (game.coverUri != null) {
            AsyncImage(
                model = game.coverUri,
                contentDescription = "Copertina di ${game.title}",
                contentScale = ContentScale.Crop,
                modifier = Modifier.fillMaxSize()
            )
        } else {
            // Fallback se l'utente non ha inserito il file cover.jpg
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .background(Color.DarkGray),
                contentAlignment = androidx.compose.ui.Alignment.Center
            ) {
                Text(
                    text = game.title,
                    color = Color.White,
                    style = MaterialTheme.typography.titleMedium,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.padding(8.dp)
                )
            }
        }
    }
}
