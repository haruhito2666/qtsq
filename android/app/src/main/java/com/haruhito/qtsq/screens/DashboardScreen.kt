package com.haruhito.qtsq.screens

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
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
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.haruhito.qtsq.ui.theme.*
import java.io.File

@Suppress("UNUSED_PARAMETER")
@Composable
fun DashboardScreen(
    qtsqEngine: com.haruhito.qtsq.QtsqEngine,
    onCameraClick: () -> Unit,
    onAudioClick: () -> Unit = {},
    onVideoClick: () -> Unit = {},
    onSceneCaptureClick: () -> Unit = {},
    onConvertClick: () -> Unit,
    onDecryptClick: () -> Unit,
    onFileClick: (String) -> Unit = {}
) {
    val context = LocalContext.current

    val recentFiles = remember {
        val files = context.filesDir.listFiles { file -> file.extension == "qtsq" } ?: emptyArray()
        files.sortedByDescending { it.lastModified() }.take(5)
    }
    val totalFiles = remember {
        (context.filesDir.listFiles { file -> file.extension == "qtsq" } ?: emptyArray()).size
    }
    val totalSizeKb = remember {
        val files = context.filesDir.listFiles { file -> file.extension == "qtsq" } ?: emptyArray()
        files.sumOf { it.length() } / 1024
    }
    val sizeLabel = if (totalSizeKb >= 1024) "${"%.1f".format(totalSizeKb / 1024f)} MB" else "$totalSizeKb KB"

    LazyColumn(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.White)
            .statusBarsPadding(),
        contentPadding = PaddingValues(bottom = 110.dp)
    ) {
        // ── Storage header ──
        item {
            Spacer(Modifier.height(28.dp))
            QtsqStatHeader(
                label = "Storage Used",
                valueMain = if (totalSizeKb >= 1024)
                    "${"%.0f".format(totalSizeKb / 1024f)}" else "$totalSizeKb",
                valueSuffix = if (totalSizeKb >= 1024) ".${"%02d".format(((totalSizeKb % 1024) * 100 / 1024).toInt())}" else ".00",
                deltaText = "$totalFiles files"
            )
        }

        // ── Quick Actions (2×2 grid, reference style) ──
        item {
            Spacer(Modifier.height(22.dp))
            QtsqSectionTitle("Quick Actions")
            Spacer(Modifier.height(12.dp))
            Row(
                modifier = Modifier.fillMaxWidth().padding(horizontal = ScreenHPadding),
                horizontalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                QtsqActionCard(
                    title = "Camera", subtitle = "Send to .qtsq",
                    icon = Icons.Rounded.NorthEast, tileColor = TileBlue,
                    modifier = Modifier.weight(1f), onClick = onCameraClick
                )
                QtsqActionCard(
                    title = "Convert", subtitle = "Files, audio…",
                    icon = Icons.Rounded.AttachMoney, tileColor = TileOrange,
                    modifier = Modifier.weight(1f), onClick = onConvertClick
                )
            }
            Spacer(Modifier.height(12.dp))
            Row(
                modifier = Modifier.fillMaxWidth().padding(horizontal = ScreenHPadding),
                horizontalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                QtsqActionCard(
                    title = "3D Scene", subtitle = "Live capture",
                    icon = Icons.Rounded.ViewInAr, tileColor = TilePurple,
                    modifier = Modifier.weight(1f), onClick = onSceneCaptureClick
                )
                QtsqActionCard(
                    title = "More", subtitle = "Video, decrypt…",
                    icon = Icons.Rounded.GridView, tileColor = TileDark,
                    modifier = Modifier.weight(1f), onClick = onDecryptClick
                )
            }
        }

        // ── Secondary actions row (audio / video shortcuts) ──
        item {
            Spacer(Modifier.height(12.dp))
            Row(
                modifier = Modifier.fillMaxWidth().padding(horizontal = ScreenHPadding),
                horizontalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                QtsqActionCard(
                    title = "Audio", subtitle = "Record sound",
                    icon = Icons.Rounded.Mic, tileColor = TileGreen,
                    modifier = Modifier.weight(1f), onClick = onAudioClick
                )
                QtsqActionCard(
                    title = "Video", subtitle = "Clip to .qtsq",
                    icon = Icons.Rounded.Movie, tileColor = TilePink,
                    modifier = Modifier.weight(1f), onClick = onVideoClick
                )
            }
        }

        // ── Recent captures (mirrors Transaction History) ──
        item {
            Spacer(Modifier.height(24.dp))
            QtsqSectionTitle("Recent captures")
            Spacer(Modifier.height(6.dp))
        }

        if (recentFiles.isEmpty()) {
            item {
                Spacer(Modifier.height(6.dp))
                QtsqEmptyCard(
                    icon = Icons.Rounded.Inbox,
                    title = "No files yet",
                    subtitle = "Capture or convert to create .qtsq files"
                )
            }
        } else {
            items(recentFiles) { file ->
                val sizeKb = file.length() / 1024
                val isAudio = file.name.contains("audio", ignoreCase = true)
                val isVideo = file.name.startsWith("video_")
                DashboardRow(
                    file = file,
                    icon = when {
                        isVideo -> Icons.Rounded.Movie
                        isAudio -> Icons.Rounded.GraphicEq
                        else -> Icons.Rounded.Image
                    },
                    tile = when {
                        isVideo -> TileOrange
                        isAudio -> TilePurple
                        else -> TileBlue
                    },
                    amount = if (sizeKb >= 1024) "${"%.1f".format(sizeKb / 1024f)} MB" else "$sizeKb KB",
                    onClick = { onFileClick(file.absolutePath) }
                )
            }
        }

        // ── Footer link (mirrors "View Today's Analytics") ──
        item {
            Spacer(Modifier.height(18.dp))
            Box(modifier = Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
                Text(
                    text = "View Library →",
                    style = Typography.labelLarge.copy(fontSize = 16.sp, fontWeight = FontWeight.Bold),
                    color = LinkBlue,
                    modifier = Modifier.clickable { onConvertClick() }.padding(8.dp)
                )
            }
        }
    }
}

@Composable
private fun DashboardRow(
    file: File,
    icon: androidx.compose.ui.graphics.vector.ImageVector,
    tile: Color,
    amount: String,
    onClick: () -> Unit
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable { onClick() }
            .padding(horizontal = ScreenHPadding, vertical = 9.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Box(
            modifier = Modifier.size(38.dp).clip(CircleShape).background(tile.copy(alpha = 0.12f)),
            contentAlignment = Alignment.Center
        ) {
            Icon(
                if (icon == Icons.Rounded.Image) Icons.Rounded.NorthEast else icon,
                null, tint = tile, modifier = Modifier.size(17.dp)
            )
        }
        Spacer(Modifier.width(12.dp))
        Text(
            text = file.name,
            style = Typography.bodyMedium.copy(fontSize = 16.sp, fontWeight = FontWeight.SemiBold),
            color = TextPrimary,
            modifier = Modifier.weight(1f),
            maxLines = 1,
            overflow = TextOverflow.Ellipsis
        )
        Text(
            text = amount,
            style = Typography.bodyMedium.copy(fontWeight = FontWeight.Bold, fontSize = 16.sp),
            color = TextPrimary
        )
    }
}
