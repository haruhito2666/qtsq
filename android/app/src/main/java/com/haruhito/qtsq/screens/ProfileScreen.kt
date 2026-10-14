package com.haruhito.qtsq.screens

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.*
import androidx.compose.material.icons.automirrored.rounded.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.foundation.clickable
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.haruhito.qtsq.QtsqEngine
import com.haruhito.qtsq.ui.theme.*

import android.util.Log
import kotlinx.coroutines.launch
import com.clerk.api.Clerk

private const val TAG = "QtsqProfile"

@Composable
fun ProfileScreen(
    qtsqEngine: QtsqEngine
) {
    val context = LocalContext.current
    val uriHandler = LocalUriHandler.current
    val coroutineScope = rememberCoroutineScope()
    val clerkUser by Clerk.userFlow.collectAsState()
    var isGpuEnabled by remember { mutableStateOf(true) }
    var codec by remember { mutableIntStateOf(QtsqEngine.loadCodecPref(context)) }

    val displayName = clerkUser?.let { user ->
        val first = user.firstName ?: ""
        val last = user.lastName ?: ""
        "$first $last".trim().ifEmpty { user.username ?: "QTSQ User" }
    } ?: "QTSQ User"
    val email = clerkUser?.primaryEmailAddress?.emailAddress ?: clerkUser?.emailAddresses?.firstOrNull()?.emailAddress ?: "Quantum Tensor User"

    LaunchedEffect(Unit) {
        try {
            isGpuEnabled = true
            qtsqEngine.setUseGpu(true)
        } catch (e: Exception) { e.printStackTrace() }
    }

    val (fileCount, totalSizeKb) = remember {
        val files = context.filesDir.listFiles { f -> f.extension == "qtsq" } ?: emptyArray()
        Pair(files.size, files.sumOf { it.length() } / 1024)
    }
    val usageStr = if (totalSizeKb >= 1024) "${"%.1f".format(totalSizeKb / 1024f)} MB" else "$totalSizeKb KB"

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.White)
            .verticalScroll(rememberScrollState())
    ) {
        QtsqTopBar(title = "Profile")

        Spacer(Modifier.height(10.dp))

        // ── User hero card — soft gray, matte dark avatar ──
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = ScreenHPadding)
                .clip(RoundedCornerShape(CardRadius))
                .background(CardGray)
                .padding(18.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Box(
                modifier = Modifier.size(52.dp).clip(CircleShape).background(TileDark),
                contentAlignment = Alignment.Center
            ) {
                Icon(Icons.Rounded.Person, null, tint = Color.White, modifier = Modifier.size(26.dp))
            }
            Spacer(Modifier.width(14.dp))
            Column(Modifier.weight(1f)) {
                Text(displayName, style = Typography.titleSmall.copy(fontSize = 16.sp, fontWeight = FontWeight.SemiBold), color = TextPrimary)
                Text(email, style = Typography.bodySmall.copy(fontSize = 12.sp), color = TextTertiary, modifier = Modifier.padding(top = 2.dp))
            }
        }

        Spacer(Modifier.height(22.dp))
        QtsqSectionTitle("Settings")

        Spacer(Modifier.height(10.dp))
        ProfileGroup {
            ProfileRow(
                icon = Icons.Rounded.Memory, tile = TileBlue,
                title = "GPU Acceleration", subtitle = if (isGpuEnabled) "Vulkan compute enabled" else "CPU fallback mode"
            ) {
                Switch(
                    checked = isGpuEnabled,
                    onCheckedChange = { checked ->
                        isGpuEnabled = checked
                        qtsqEngine.setUseGpu(checked)
                        coroutineScope.launch { }
                    },
                    colors = SwitchDefaults.colors(
                        checkedThumbColor = Color.White,
                        checkedTrackColor = InkBlack,
                        uncheckedThumbColor = Color.White,
                        uncheckedTrackColor = Divider
                    )
                )
            }
            HorizontalDivider(color = Divider, thickness = 0.75.dp, modifier = Modifier.padding(horizontal = 16.dp))
            ProfileRow(icon = Icons.Rounded.Compress, tile = TilePurple, title = "Compression Codec", subtitle = if (codec == QtsqEngine.CODEC_QTC) "qtc — smaller, default" else "zlib — legacy") {
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    CodecChip(label = "qtc", selected = codec == QtsqEngine.CODEC_QTC, onClick = {
                        codec = QtsqEngine.CODEC_QTC
                        QtsqEngine.saveCodecPref(context, codec)
                        qtsqEngine.setCodec(codec)
                    })
                    CodecChip(label = "zlib", selected = codec == QtsqEngine.CODEC_ZLIB, onClick = {
                        codec = QtsqEngine.CODEC_ZLIB
                        QtsqEngine.saveCodecPref(context, codec)
                        qtsqEngine.setCodec(codec)
                    })
                }
            }
        }

        Spacer(Modifier.height(20.dp))
        QtsqSectionTitle("Storage")
        Spacer(Modifier.height(10.dp))
        ProfileGroup {
            ProfileRow(icon = Icons.Rounded.Storage, tile = TileGreen, title = "Total Files", subtitle = "$fileCount .qtsq files stored") {
                Text("$fileCount", style = Typography.titleSmall.copy(fontWeight = FontWeight.Bold), color = TextPrimary)
            }
            HorizontalDivider(color = Divider, thickness = 0.75.dp, modifier = Modifier.padding(horizontal = 16.dp))
            ProfileRow(icon = Icons.Rounded.SdStorage, tile = TileOrange, title = "Disk Usage", subtitle = usageStr) {
                Text(usageStr, style = Typography.titleSmall.copy(fontWeight = FontWeight.Bold, fontSize = 14.sp), color = TextPrimary)
            }
        }

        Spacer(Modifier.height(20.dp))
        QtsqSectionTitle("About")
        Spacer(Modifier.height(10.dp))
        ProfileGroup {
            ProfileRow(icon = Icons.Rounded.Info, tile = TileDark, title = "App Version", subtitle = "QTSQ v0.1.0-beta") {
                Text("0.1.0-beta", style = Typography.bodyMedium.copy(fontSize = 13.sp), color = TextTertiary)
            }
        }

        Spacer(Modifier.height(20.dp))
        QtsqSectionTitle("Legal")
        Spacer(Modifier.height(10.dp))
        ProfileGroup {
            ProfileRow(
                icon = Icons.Rounded.PrivacyTip, tile = TileBlue,
                title = "Privacy Policy", subtitle = "Read our privacy guidelines",
                modifier = Modifier.clickable { uriHandler.openUri("https://getqtsq.xyz/#privacy") }
            ) {
                Icon(Icons.AutoMirrored.Rounded.OpenInNew, null, tint = TextTertiary, modifier = Modifier.size(16.dp))
            }
            HorizontalDivider(color = Divider, thickness = 0.75.dp, modifier = Modifier.padding(horizontal = 16.dp))
            ProfileRow(
                icon = Icons.Rounded.Description, tile = TilePink,
                title = "Terms of Service", subtitle = "Read our terms & conditions",
                modifier = Modifier.clickable { uriHandler.openUri("https://getqtsq.xyz/#terms") }
            ) {
                Icon(Icons.AutoMirrored.Rounded.OpenInNew, null, tint = TextTertiary, modifier = Modifier.size(16.dp))
            }
        }

        Spacer(Modifier.height(32.dp))

        // Sign out — black pill like reference Continue button
        Box(Modifier.padding(horizontal = ScreenHPadding)) {
            QtsqPrimaryButton(text = "Sign Out", onClick = {
                coroutineScope.launch {
                    try { Clerk.signOut() } catch (e: Exception) { Log.e(TAG, "signout failed", e) }
                }
            })
        }
        Spacer(Modifier.height(110.dp))
    }
}

@Composable
private fun ProfileGroup(content: @Composable ColumnScope.() -> Unit) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = ScreenHPadding)
            .clip(RoundedCornerShape(CardRadius))
            .background(CardGray)
            .padding(vertical = 4.dp),
        content = content
    )
}

@Composable
private fun ProfileRow(
    icon: ImageVector,
    tile: Color,
    title: String,
    subtitle: String,
    modifier: Modifier = Modifier,
    trailing: @Composable () -> Unit
) {
    Row(
        modifier = modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 13.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Box(
            modifier = Modifier.size(38.dp).clip(RoundedCornerShape(12.dp)).background(tile),
            contentAlignment = Alignment.Center
        ) { Icon(icon, null, tint = Color.White, modifier = Modifier.size(19.dp)) }
        Column(Modifier.weight(1f).padding(horizontal = 13.dp)) {
            Text(title, style = Typography.labelLarge.copy(fontSize = 14.sp), color = TextPrimary)
            Text(subtitle, style = Typography.bodySmall.copy(fontSize = 12.sp), color = TextTertiary, modifier = Modifier.padding(top = 1.dp))
        }
        trailing()
    }
}

@Composable
private fun CodecChip(label: String, selected: Boolean, onClick: () -> Unit) {
    Box(
        modifier = Modifier
            .clip(RoundedCornerShape(20.dp))
            .background(if (selected) InkBlack else Color.White)
            .clickable(onClick = onClick)
            .padding(horizontal = 14.dp, vertical = 7.dp),
        contentAlignment = Alignment.Center
    ) {
        Text(
            label,
            style = Typography.labelMedium.copy(fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Normal),
            color = if (selected) Color.White else TextSecondary
        )
    }
}
