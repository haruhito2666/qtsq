package com.haruhito.qtsq.screens

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.haruhito.qtsq.ui.theme.*
import java.io.File
import java.text.SimpleDateFormat
import java.util.*

private data class LibCategory(val label: String, val sub: String, val icon: ImageVector, val tile: Color, val filter: String)

@Composable
fun LibraryScreen(onFileClick: (String) -> Unit = {}) {
    val context = LocalContext.current
    var query by remember { mutableStateOf("") }

    val allFiles = remember {
        val files = context.filesDir.listFiles { file -> file.extension == "qtsq" } ?: emptyArray()
        files.sortedByDescending { it.lastModified() }
    }
    val videoFiles = allFiles.filter { it.name.startsWith("video_") }
    val imageFiles = allFiles.filter {
        (it.name.startsWith("QTSQ_") || it.name.startsWith("converted_")) && !it.name.startsWith("video_")
    }
    val audioFiles = allFiles.filter { it.name.startsWith("audio_") }
    val convertedCount = allFiles.count { it.name.startsWith("converted_") }
    val otherFiles = allFiles.filter {
        !it.name.startsWith("QTSQ_") && !it.name.startsWith("audio_") &&
            !it.name.startsWith("converted_") && !it.name.startsWith("video_")
    }

    val categories = listOf(
        LibCategory("Images", "${imageFiles.size} items", Icons.Rounded.Image, TileBlue, "image"),
        LibCategory("Video", "${videoFiles.size} items", Icons.Rounded.Movie, TileOrange, "video"),
        LibCategory("Audio", "${audioFiles.size} items", Icons.Rounded.GraphicEq, TilePurple, "audio"),
        LibCategory("Converted", "$convertedCount items", Icons.Rounded.SwapHoriz, TileGreen, "converted"),
        LibCategory("Other", "${otherFiles.size} items", Icons.Rounded.DataObject, TileDark, "other"),
        LibCategory("Encrypted", "${allFiles.count { it.name.contains("encrypted", true) }} items", Icons.Rounded.Lock, TilePink, "encrypted")
    )

    var activeFilter by remember { mutableStateOf<String?>(null) }

    val baseFiltered = when (activeFilter) {
        "image" -> imageFiles
        "audio" -> audioFiles
        "video" -> videoFiles
        "converted" -> allFiles.filter { it.name.startsWith("converted_") }
        "other" -> otherFiles
        "encrypted" -> allFiles.filter { it.name.contains("encrypted", true) || it.name.contains("decrypted", true) }
        else -> allFiles
    }
    val filteredFiles = if (query.isBlank()) baseFiltered
    else baseFiltered.filter { it.name.contains(query, ignoreCase = true) }

    LazyColumn(
        modifier = Modifier.fillMaxSize().background(Color.White),
        contentPadding = PaddingValues(bottom = 110.dp)
    ) {
        item { QtsqTopBar(title = "Library") }

        // Search (reference: "Search for service" gray field)
        item {
            Spacer(Modifier.height(8.dp))
            QtsqSearchBar(value = query, onValueChange = { query = it }, placeholder = "Search files")
        }

        // Categories grid — 2-col cards like "Active Plans"
        item {
            Spacer(Modifier.height(18.dp))
            QtsqSectionTitle("Categories")
            Spacer(Modifier.height(12.dp))
        }
        items(categories.chunked(2)) { row ->
            Row(
                modifier = Modifier.fillMaxWidth().padding(horizontal = ScreenHPadding),
                horizontalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                row.forEach { cat ->
                    val isActive = activeFilter == cat.filter
                    Column(
                        modifier = Modifier
                            .weight(1f)
                            .clip(RoundedCornerShape(CardRadius))
                            .background(if (isActive) InkBlack else CardGray)
                            .clickable { activeFilter = if (isActive) null else cat.filter }
                            .padding(16.dp)
                    ) {
                        Box(
                            modifier = Modifier.size(38.dp)
                                .clip(RoundedCornerShape(TileRadius))
                                .background(if (isActive) Color.White.copy(alpha = 0.15f) else cat.tile),
                            contentAlignment = Alignment.Center
                        ) {
                            Icon(cat.icon, null, tint = Color.White, modifier = Modifier.size(19.dp))
                        }
                        Spacer(Modifier.height(22.dp))
                        Text(
                            cat.label,
                            style = Typography.titleSmall.copy(fontSize = 14.sp, fontWeight = FontWeight.SemiBold),
                            color = if (isActive) Color.White else TextPrimary
                        )
                        Spacer(Modifier.height(2.dp))
                        Text(
                            cat.sub,
                            style = Typography.bodySmall.copy(fontSize = 12.sp),
                            color = if (isActive) Color.White.copy(alpha = 0.7f) else TextTertiary
                        )
                    }
                }
                if (row.size == 1) Spacer(Modifier.weight(1f))
            }
            Spacer(Modifier.height(12.dp))
        }

        // Tags row — horizontal pills
        item {
            Spacer(Modifier.height(6.dp))
            QtsqSectionTitle("Tags")
            Spacer(Modifier.height(10.dp))
            LazyRow(
                contentPadding = PaddingValues(horizontal = ScreenHPadding),
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                val tags = listOf("All", "Images", "Video", "Audio", "Encrypted")
                items(tags) { tag ->
                    val key = tag.lowercase()
                    val sel = (activeFilter == key) || (activeFilter == null && tag == "All")
                    QtsqFilterPill(
                        label = tag, selected = sel,
                        onClick = { activeFilter = if (tag == "All") null else key }
                    )
                }
            }
        }

        // File list header
        item {
            Spacer(Modifier.height(20.dp))
            Row(
                modifier = Modifier.fillMaxWidth().padding(horizontal = ScreenHPadding),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    if (activeFilter != null) activeFilter!!.replaceFirstChar { it.uppercase() } else "All files",
                    style = Typography.titleSmall.copy(fontSize = 14.sp, fontWeight = FontWeight.SemiBold),
                    color = TextPrimary
                )
                Text(
                    "${filteredFiles.size} items",
                    style = Typography.bodySmall.copy(fontSize = 12.sp),
                    color = TextTertiary
                )
            }
            Spacer(Modifier.height(6.dp))
        }

        if (filteredFiles.isEmpty()) {
            item {
                Spacer(Modifier.height(6.dp))
                QtsqEmptyCard(icon = Icons.Rounded.FolderOpen, title = "No files found", subtitle = "Try a different search or filter")
            }
        } else {
            items(filteredFiles) { file ->
                LibraryRow(file = file, onClick = { onFileClick(file.absolutePath) })
            }
        }
    }
}

@Composable
private fun LibraryRow(file: File, onClick: () -> Unit) {
    val sizeKb = file.length() / 1024
    val sizeStr = if (sizeKb >= 1024) "${"%.1f".format(sizeKb / 1024f)} MB" else "$sizeKb KB"
    val dateStr = remember(file.absolutePath) {
        SimpleDateFormat("MMM dd, HH:mm", Locale.getDefault()).format(Date(file.lastModified()))
    }
    val (icon, tile) = when {
        file.name.startsWith("video_") -> Icons.Rounded.Movie to TileOrange
        file.name.startsWith("audio_") -> Icons.Rounded.GraphicEq to TilePurple
        file.name.startsWith("converted_") -> Icons.Rounded.SwapHoriz to TileGreen
        else -> Icons.Rounded.Image to TileBlue
    }
    Row(
        modifier = Modifier.fillMaxWidth().clickable { onClick() }
            .padding(horizontal = ScreenHPadding, vertical = 9.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Box(
            modifier = Modifier.size(40.dp).clip(CircleShape).background(tile.copy(alpha = 0.12f)),
            contentAlignment = Alignment.Center
        ) { Icon(icon, null, tint = tile, modifier = Modifier.size(18.dp)) }
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            Text(
                file.name, style = Typography.bodyMedium.copy(fontSize = 14.sp, fontWeight = FontWeight.Medium),
                color = TextPrimary, maxLines = 1, overflow = TextOverflow.Ellipsis
            )
            Text("$sizeStr • $dateStr", style = Typography.bodySmall.copy(fontSize = 12.sp), color = TextTertiary)
        }
        Text(sizeStr, style = Typography.bodyMedium.copy(fontWeight = FontWeight.SemiBold, fontSize = 13.sp), color = TextPrimary)
    }
}
