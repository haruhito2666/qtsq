package com.haruhito.qtsq.screens

import android.graphics.Bitmap
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.util.Log
import androidx.compose.animation.core.*
import androidx.compose.foundation.Image
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
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import android.util.LruCache
import com.haruhito.qtsq.QtsqEngine
import com.haruhito.qtsq.ui.theme.*
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

// LRU cache for decoded QTSQ results — avoids re-decompression on revisit.
// Max 5 entries (~400MB for 12MP images). Keyed by absolute file path.
private object DecodedImageCache {
    private val cache = LruCache<String, QtsqEngine.QtsqResult>(5)

    fun get(path: String): QtsqEngine.QtsqResult? = cache.get(path)
    fun put(path: String, result: QtsqEngine.QtsqResult) { cache.put(path, result) }
}

// QTSQ data type constants (from qtsq_format.h)
private object QtsqType {
    const val RAW = 0x00
    const val IMAGE_GRAY = 0x10
    const val IMAGE_RGB = 0x11
    const val IMAGE_RGBA = 0x12
    const val AUDIO_MONO = 0x20
    const val AUDIO_STEREO = 0x21
    const val AUDIO_MULTI = 0x22
    const val VIDEO_FRAMES = 0x30
    const val TEXT_ASCII = 0x40
    const val TEXT_UTF8 = 0x41
    const val TABLE_INT = 0x50
    const val TABLE_FLOAT = 0x51
    const val JSON = 0x52
    const val CSV = 0x53
}

@Composable
fun ViewerScreen(
    qtsqEngine: QtsqEngine,
    filePath: String,
    onClose: () -> Unit
) {
    val coroutineScope = rememberCoroutineScope()
    var isLoading by remember { mutableStateOf(true) }
    var errorMessage by remember { mutableStateOf<String?>(null) }
    var decompressedResult by remember { mutableStateOf<QtsqEngine.QtsqResult?>(null) }
    var password by remember { mutableStateOf("") }
    var showPasswordDialog by remember { mutableStateOf(false) }
    var needsPassword by remember { mutableStateOf(false) }
    var showDownloadDialog by remember { mutableStateOf(false) }
    val context = androidx.compose.ui.platform.LocalContext.current

    // Audio playback state
    var isPlaying by remember { mutableStateOf(false) }
    var audioTrack by remember { mutableStateOf<AudioTrack?>(null) }
    // Video has no single-image payload, so detect it from the stored QTSQ
    // type and hand it to VideoViewer before decompressBytes runs.
    var isVideoFile by remember { mutableStateOf(false) }
    
    fun exportFile(asNative: Boolean) {
        val currentResult = decompressedResult
        if (asNative && currentResult == null) {
            android.widget.Toast.makeText(context, "Wait for decryption to complete", android.widget.Toast.LENGTH_SHORT).show()
            return
        }
        coroutineScope.launch(Dispatchers.IO) {
            try {
                val baseName = File(filePath).nameWithoutExtension
                val (outputBytes, ext, mime) = if (asNative && currentResult != null) {
                    when (currentResult.type) {
                        QtsqType.IMAGE_GRAY, QtsqType.IMAGE_RGB, QtsqType.IMAGE_RGBA -> {
                            val w = currentResult.width
                            val h = currentResult.height
                            if (w > 0 && h > 0) {
                                val bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
                                bmp.copyPixelsFromBuffer(java.nio.ByteBuffer.wrap(currentResult.data))
                                val outStream = java.io.ByteArrayOutputStream()
                                bmp.compress(Bitmap.CompressFormat.PNG, 100, outStream)
                                Triple(outStream.toByteArray(), "png", "image/png")
                            } else {
                                Triple(currentResult.data, "raw", "application/octet-stream")
                            }
                        }
                        QtsqType.TEXT_ASCII, QtsqType.TEXT_UTF8 -> Triple(currentResult.data, "txt", "text/plain")
                        QtsqType.JSON -> Triple(currentResult.data, "json", "application/json")
                        QtsqType.CSV -> Triple(currentResult.data, "csv", "text/csv")
                        QtsqType.AUDIO_MONO, QtsqType.AUDIO_STEREO, QtsqType.AUDIO_MULTI -> Triple(currentResult.data, "pcm", "audio/raw")
                        else -> Triple(currentResult.data, "bin", "application/octet-stream")
                    }
                } else {
                    Triple(File(filePath).readBytes(), "qtsq", "application/octet-stream")
                }

                val exportFilename = if (asNative) "${baseName}_exported.$ext" else "${baseName}.$ext"

                if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.Q) {
                    val resolver = context.contentResolver
                    val contentValues = android.content.ContentValues().apply {
                        put(android.provider.MediaStore.MediaColumns.DISPLAY_NAME, exportFilename)
                        put(android.provider.MediaStore.MediaColumns.MIME_TYPE, mime)
                        put(android.provider.MediaStore.MediaColumns.RELATIVE_PATH, android.os.Environment.DIRECTORY_DOWNLOADS + "/QTSQ_Export")
                    }
                    val uri = resolver.insert(android.provider.MediaStore.Downloads.EXTERNAL_CONTENT_URI, contentValues)
                    if (uri != null) {
                        resolver.openOutputStream(uri)?.use { it.write(outputBytes) }
                    }
                } else {
                    val dir = File(android.os.Environment.getExternalStoragePublicDirectory(android.os.Environment.DIRECTORY_DOWNLOADS), "QTSQ_Export")
                    if (!dir.exists()) dir.mkdirs()
                    File(dir, exportFilename).writeBytes(outputBytes)
                }

                withContext(Dispatchers.Main) {
                    android.widget.Toast.makeText(context, "Saved as $exportFilename", android.widget.Toast.LENGTH_LONG).show()
                }
            } catch (e: Exception) {
                Log.e("ViewerScreen", "Export failed: ${e.message}", e)
                withContext(Dispatchers.Main) {
                    android.widget.Toast.makeText(context, "Export failed: ${e.message}", android.widget.Toast.LENGTH_SHORT).show()
                }
            }
        }
    }

    // Load file on first composition
    fun loadFile(pw: String?) {
        isLoading = true
        errorMessage = null
        coroutineScope.launch {
            val result = withContext(Dispatchers.IO) {
                try {
                    // Check LRU cache first — instant if previously decoded
                    val cached = DecodedImageCache.get(filePath)
                    if (cached != null && pw == null) {
                        Log.d("ViewerScreen", "Cache HIT for $filePath — skipping decompression")
                        decompressedResult = cached
                        return@withContext null
                    }

                    val file = File(filePath)
                    if (!file.exists()) return@withContext "File not found."
                    val fileBytes = file.readBytes()

                    val peeked = qtsqEngine.peekDataType(fileBytes, pw)
                    if (peeked == QtsqType.VIDEO_FRAMES) {
                        withContext(Dispatchers.Main) { isVideoFile = true }
                        return@withContext null
                    }

                    // First attempt without password to detect if encryption is needed
                    val qtsqResult = qtsqEngine.decompressBytes(fileBytes, pw)
                    if (qtsqResult == null) {
                        // Check if the file header has the encrypted flag
                        return@withContext if (pw == null) {
                            "This file is encrypted. Enter a password to decrypt."
                        } else {
                            "Decompression failed. Wrong password or corrupted file."
                        }
                    }
                    decompressedResult = qtsqResult

                    // Store to cache for future instant access
                    DecodedImageCache.put(filePath, qtsqResult)
                    Log.d("ViewerScreen", "Cache MISS for $filePath — stored to cache")

                    null // no error
                } catch (e: Exception) {
                    Log.e("ViewerScreen", "Load error", e)
                    "Error: ${e.message}"
                }
            }
            if (result != null) {
                errorMessage = result
                if (result.contains("password", ignoreCase = true) ||
                    result.contains("encrypted", ignoreCase = true)) {
                    needsPassword = true
                }
            }
            isLoading = false
        }
    }

    LaunchedEffect(filePath) {
        loadFile(null)
    }

    // Clean up audio on dispose
    DisposableEffect(Unit) {
        onDispose {
            audioTrack?.stop()
            audioTrack?.release()
        }
    }

    if (isVideoFile) {
        VideoViewer(qtsqEngine = qtsqEngine, filePath = filePath, onClose = onClose)
        return
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(BackgroundWhite)
            .statusBarsPadding()
            .navigationBarsPadding()
    ) {
        // ── Top Bar ──
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 8.dp, vertical = 8.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            IconButton(onClick = {
                audioTrack?.stop()
                audioTrack?.release()
                onClose()
            }) {
                Icon(Icons.AutoMirrored.Rounded.ArrowBack, "Back", tint = TextPrimary)
            }

            Text(
                text = File(filePath).name,
                style = Typography.titleMedium,
                color = TextPrimary,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f).padding(horizontal = 12.dp)
            )

            // Download to Downloads/QTSQ button
            IconButton(onClick = { showDownloadDialog = true }) {
                Icon(Icons.Rounded.Download, "Download", tint = AccentPrimary)
            }

            // Password retry button
            if (needsPassword || errorMessage != null) {
                IconButton(onClick = { showPasswordDialog = true }) {
                    Icon(Icons.Rounded.Key, "Enter Password", tint = AccentPrimary)
                }
            }
        }

        // Content Area
        Box(
            modifier = Modifier
                .fillMaxSize()
                .weight(1f),
            contentAlignment = Alignment.Center
        ) {
            when {
                isLoading -> {
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        CircularProgressIndicator(
                            color = AccentPrimary,
                            modifier = Modifier.size(48.dp)
                        )
                        Spacer(modifier = Modifier.height(16.dp))
                        Text("Decompressing...", style = Typography.bodyMedium, color = TextSecondary)
                    }
                }

                errorMessage != null -> {
                    Column(
                        horizontalAlignment = Alignment.CenterHorizontally,
                        modifier = Modifier.padding(32.dp)
                    ) {
                        Icon(
                            Icons.Rounded.ErrorOutline,
                            contentDescription = null,
                            tint = Color(0xFFE74C3C),
                            modifier = Modifier.size(64.dp)
                        )
                        Spacer(modifier = Modifier.height(16.dp))
                        Text(
                            text = errorMessage!!,
                            style = Typography.bodyLarge,
                            color = TextPrimary,
                            textAlign = TextAlign.Center
                        )
                        Spacer(modifier = Modifier.height(24.dp))
                        Button(
                            onClick = { showPasswordDialog = true },
                            elevation = ButtonDefaults.buttonElevation(defaultElevation = 0.dp, pressedElevation = 0.dp),
                            colors = ButtonDefaults.buttonColors(containerColor = Color.White, contentColor = AccentPrimary),
                            shape = RoundedCornerShape(20.dp)
                        ) {
                            Icon(Icons.Rounded.Key, null, modifier = Modifier.size(18.dp))
                            Spacer(modifier = Modifier.width(8.dp))
                            Text("Try with Password")
                        }
                    }
                }

                decompressedResult != null -> {
                    val result = decompressedResult!!
                    ViewContent(
                        result = result,
                        isPlaying = isPlaying,
                        onPlayToggle = {
                            if (isPlaying) {
                                audioTrack?.pause()
                                isPlaying = false
                            } else {
                                playAudio(result) { track ->
                                    audioTrack = track
                                    isPlaying = true
                                }
                            }
                        },
                        onStop = {
                            audioTrack?.stop()
                            isPlaying = false
                        }
                    )
                }
            }
        }

        // File Info Footer
        if (decompressedResult != null) {
            val result = decompressedResult!!
            FileInfoFooter(result = result, filePath = filePath)
        }
    }

    // Password Dialog
    if (showPasswordDialog) {
        AlertDialog(
            onDismissRequest = { showPasswordDialog = false },
            title = { Text("Enter Password", style = Typography.titleMedium) },
            text = {
                OutlinedTextField(
                    value = password,
                    onValueChange = { password = it },
                    label = { Text("Password") },
                    singleLine = true,
                    shape = RoundedCornerShape(20.dp),
                    colors = OutlinedTextFieldDefaults.colors(
                        focusedBorderColor = AccentPrimary,
                        unfocusedBorderColor = Divider
                    )
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    showPasswordDialog = false
                    loadFile(password.ifEmpty { null })
                }) {
                    Text("Decrypt", color = AccentPrimary)
                }
            },
            dismissButton = {
                TextButton(onClick = { showPasswordDialog = false }) {
                    Text("Cancel")
                }
            }
        )
    }

    if (showDownloadDialog) {
        AlertDialog(
            onDismissRequest = { showDownloadDialog = false },
            title = {
                Text(
                    text = "Save Options",
                    style = Typography.titleMedium.copy(fontWeight = FontWeight.Bold),
                    color = TextPrimary
                )
            },
            text = {
                Text(
                    "Do you want to export the decompressed native file (PNG, TXT, etc.) or save the original end-to-end encrypted QTSQ container?",
                    style = Typography.bodyMedium,
                    color = TextSecondary
                )
            },
            containerColor = Color.White,
            confirmButton = {
                Button(
                    onClick = {
                        showDownloadDialog = false
                        exportFile(asNative = true)
                    },
                    colors = ButtonDefaults.buttonColors(containerColor = InkBlack)
                ) {
                    Text("Export as Native", color = Color.White)
                }
            },
            dismissButton = {
                TextButton(
                    onClick = {
                        showDownloadDialog = false
                        exportFile(asNative = false)
                    }
                ) {
                    Text("Save Original (.qtsq)", color = PrimaryCyan)
                }
            }
        )
    }
}

@Composable
private fun ViewContent(
    result: QtsqEngine.QtsqResult,
    isPlaying: Boolean,
    onPlayToggle: () -> Unit,
    onStop: () -> Unit
) {
    when (result.type) {
        QtsqType.IMAGE_GRAY, QtsqType.IMAGE_RGB, QtsqType.IMAGE_RGBA -> {
            ImageViewer(result)
        }
        QtsqType.AUDIO_MONO, QtsqType.AUDIO_STEREO, QtsqType.AUDIO_MULTI -> {
            AudioViewer(result, isPlaying, onPlayToggle, onStop)
        }
        QtsqType.TEXT_ASCII, QtsqType.TEXT_UTF8, QtsqType.JSON, QtsqType.CSV -> {
            TextViewer(result)
        }
        else -> {
            RawDataViewer(result)
        }
    }
}

@Composable
private fun ImageViewer(result: QtsqEngine.QtsqResult) {
    var scale by remember { mutableFloatStateOf(1f) }
    var offset by remember { mutableStateOf(androidx.compose.ui.geometry.Offset.Zero) }

    val bitmapState = produceState<Bitmap?>(initialValue = null, result) {
        value = withContext(Dispatchers.IO) {
            try {
                val w = result.width
                val h = result.height

                /* Pixel JPEG/PNG passthrough: dims are 0 — decode using BitmapFactory */
                if (w <= 0 || h <= 0) {
                    val bmp = android.graphics.BitmapFactory.decodeByteArray(result.data, 0, result.data.size)
                    if (bmp != null) {
                        Log.d("ViewerScreen", "JPEG/PNG passthrough decoded: ${bmp.width}x${bmp.height}")
                    } else {
                        Log.e("ViewerScreen", "BitmapFactory failed to decode passthrough image")
                    }
                    return@withContext bmp
                }

                val bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
                val data = result.data

                /* JNI now returns pre-packed ARGB_8888 bytes for all image types.
                 * Zero-copy: directly load into Bitmap without any Kotlin-side iteration. */
                bmp.copyPixelsFromBuffer(java.nio.ByteBuffer.wrap(data))
                bmp
            } catch (e: Exception) {
                Log.e("ViewerScreen", "Bitmap creation failed", e)
                null
            }
        }
    }
    val bitmap = bitmapState.value

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center
    ) {
        if (bitmap != null) {
            Box(
                modifier = Modifier
                    .weight(1f)
                    .fillMaxWidth()
                    .shadow(8.dp, RoundedCornerShape(20.dp))
                    .clip(RoundedCornerShape(20.dp))
                    .background(Color.Black)
                    .pointerInput(Unit) {
                        detectDragGestures { change, dragAmount ->
                            change.consume()
                            if (scale > 1f) {
                                offset += dragAmount
                            }
                        }
                    }
            ) {
                Image(
                    bitmap = bitmap.asImageBitmap(),
                    contentDescription = "Decompressed image",
                    contentScale = ContentScale.Fit,
                    filterQuality = androidx.compose.ui.graphics.FilterQuality.High,
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(2.dp)
                        .graphicsLayer {
                            scaleX = scale
                            scaleY = scale
                            translationX = offset.x
                            translationY = offset.y
                        }
                )

                // Zoom control overlay
                Row(
                    modifier = Modifier
                        .align(Alignment.BottomEnd)
                        .padding(16.dp)
                        .background(Color.Black.copy(alpha = 0.6f), RoundedCornerShape(8.dp))
                        .padding(horizontal = 8.dp, vertical = 4.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    IconButton(onClick = {
                        scale = (scale / 1.5f).coerceAtLeast(1f)
                        if (scale <= 1f) offset = androidx.compose.ui.geometry.Offset.Zero
                    }) {
                        Icon(Icons.Rounded.Remove, "Zoom Out", tint = Color.White)
                    }
                    Text(
                        text = String.format("%.1fx", scale),
                        color = Color.White,
                        style = Typography.bodyMedium,
                        fontFamily = FontFamily.Monospace
                    )
                    IconButton(onClick = {
                        scale = (scale * 1.5f).coerceAtMost(50f)
                    }) {
                        Icon(Icons.Rounded.Add, "Zoom In", tint = Color.White)
                    }
                }
            }
            Spacer(modifier = Modifier.height(12.dp))
            Text(
                text = "${bitmap.width} × ${bitmap.height} px",
                style = Typography.bodyMedium,
                color = TextSecondary
            )
        } else {
            Icon(
                Icons.Rounded.BrokenImage,
                contentDescription = null,
                tint = TextSecondary,
                modifier = Modifier.size(64.dp)
            )
            Spacer(modifier = Modifier.height(16.dp))
            Text("Could not render image", style = Typography.bodyLarge, color = TextSecondary)
        }
    }
}

@Composable
private fun AudioViewer(
    result: QtsqEngine.QtsqResult,
    isPlaying: Boolean,
    onPlayToggle: () -> Unit,
    onStop: () -> Unit
) {
    val channels = when (result.type) {
        QtsqType.AUDIO_MONO -> 1
        QtsqType.AUDIO_STEREO -> 2
        else -> 1
    }
    val sampleRate = if (result.audioSampleRate > 0) result.audioSampleRate else 44100
    val durationSeconds = if (result.data.size > 0) result.data.size / (2 * channels * sampleRate) else 0
    val durationFormatted = String.format("%02d:%02d", durationSeconds / 60, durationSeconds % 60)

    // Pulse animation
    val infiniteTransition = rememberInfiniteTransition(label = "audio_pulse")
    val pulseScale by infiniteTransition.animateFloat(
        initialValue = 1f,
        targetValue = 1.15f,
        animationSpec = infiniteRepeatable(
            animation = tween(600, easing = EaseInOutCubic),
            repeatMode = RepeatMode.Reverse
        ),
        label = "pulse_scale"
    )

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(32.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center
    ) {
        // Waveform visualization
        Box(
            modifier = Modifier
                .size(180.dp)
                .shadow(
                    elevation = if (isPlaying) 16.dp else 4.dp,
                    shape = CircleShape,
                    spotColor = AccentSecondary.copy(alpha = 0.4f)
                )
                .background(
                    if (isPlaying) AccentSecondary.copy(alpha = 0.15f) else SurfaceWhite,
                    CircleShape
                )
                .padding(if (isPlaying) (4 * pulseScale).dp else 0.dp),
            contentAlignment = Alignment.Center
        ) {
            Box(
                modifier = Modifier
                    .size(120.dp)
                    .background(
                        if (isPlaying) AccentSecondary.copy(alpha = 0.3f)
                        else AccentPrimary.copy(alpha = 0.1f),
                        CircleShape
                    ),
                contentAlignment = Alignment.Center
            ) {
                Icon(
                    if (isPlaying) Icons.Rounded.Pause else Icons.Rounded.PlayArrow,
                    contentDescription = if (isPlaying) "Pause" else "Play",
                    tint = if (isPlaying) AccentSecondary else AccentPrimary,
                    modifier = Modifier.size(56.dp)
                )
            }
        }

        Spacer(modifier = Modifier.height(32.dp))

        Text(
            text = durationFormatted,
            style = Typography.displayLarge.copy(fontSize = 48.sp),
            color = if (isPlaying) AccentSecondary else TextPrimary
        )

        Spacer(modifier = Modifier.height(8.dp))

        Text(
            text = "${sampleRate / 1000}kHz • ${channels}ch • ${result.data.size / 1024} KB",
            style = Typography.bodyMedium,
            color = TextSecondary
        )

        Spacer(modifier = Modifier.height(48.dp))

        // Playback Controls
        Row(
            horizontalArrangement = Arrangement.spacedBy(24.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            // Stop
            IconButton(
                onClick = onStop,
                modifier = Modifier
                    .size(56.dp)
                    .background(SurfaceWhite, CircleShape)
                    .shadow(4.dp, CircleShape)
            ) {
                Icon(Icons.Rounded.Stop, "Stop", tint = TextSecondary, modifier = Modifier.size(28.dp))
            }

            // Play / Pause
            IconButton(
                onClick = onPlayToggle,
                modifier = Modifier
                    .size(72.dp)
                    .background(
                        if (isPlaying) AccentSecondary else AccentPrimary,
                        CircleShape
                    )
                    .shadow(8.dp, CircleShape)
            ) {
                Icon(
                    if (isPlaying) Icons.Rounded.Pause else Icons.Rounded.PlayArrow,
                    contentDescription = if (isPlaying) "Pause" else "Play",
                    tint = SurfaceWhite,
                    modifier = Modifier.size(36.dp)
                )
            }
        }
    }
}

@Composable
private fun TextViewer(result: QtsqEngine.QtsqResult) {
    var isTruncated by remember { mutableStateOf(false) }
    val maxLines = 1000

    val text = remember(result) {
        var lines = 0
        var endIndex = result.data.size
        
        for (i in 0 until result.data.size) {
            if (result.data[i] == 10.toByte()) {
                lines++
                if (lines >= maxLines) {
                    endIndex = i
                    isTruncated = true
                    break
                }
            }
        }
        
        try {
            String(result.data, 0, endIndex, Charsets.UTF_8)
        } catch (e: Exception) {
            "Could not decode text."
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp)
    ) {
        // Type label
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(bottom = 12.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Icon(
                Icons.Rounded.Description,
                contentDescription = null,
                tint = AccentPrimary,
                modifier = Modifier.size(20.dp)
            )
            Spacer(modifier = Modifier.width(8.dp))
            val baseType = when (result.type) {
                QtsqType.JSON -> "JSON"
                QtsqType.CSV -> "CSV"
                QtsqType.TEXT_ASCII -> "ASCII Text"
                else -> "UTF-8 Text"
            }
            Text(
                text = baseType + if (isTruncated) " (Truncated at ${maxLines} lines)" else "",
                style = Typography.bodyMedium.copy(fontWeight = FontWeight.SemiBold),
                color = AccentPrimary
            )
        }

        Box(
            modifier = Modifier
                .fillMaxWidth()
                .weight(1f)
                .shadow(4.dp, RoundedCornerShape(20.dp))
                .clip(RoundedCornerShape(20.dp))
                .background(SurfaceWhite)
                .padding(16.dp)
        ) {
            Text(
                text = text,
                style = Typography.bodyMedium.copy(
                    fontFamily = FontFamily.Monospace,
                    fontSize = 13.sp,
                    lineHeight = 20.sp
                ),
                color = TextPrimary,
                modifier = Modifier.verticalScroll(rememberScrollState())
            )
        }
    }
}

@Composable
private fun RawDataViewer(result: QtsqEngine.QtsqResult) {
    val hexPreview = remember(result) {
        result.data.take(512).joinToString(" ") { "%02X".format(it) }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        // Header
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(bottom = 12.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Icon(
                Icons.Rounded.DataObject,
                contentDescription = null,
                tint = AccentPrimary,
                modifier = Modifier.size(20.dp)
            )
            Spacer(modifier = Modifier.width(8.dp))
            Text(
                text = "Raw Binary (Type: 0x${"%02X".format(result.type)})",
                style = Typography.bodyMedium.copy(fontWeight = FontWeight.SemiBold),
                color = AccentPrimary
            )
        }

        Box(
            modifier = Modifier
                .fillMaxWidth()
                .weight(1f)
                .shadow(4.dp, RoundedCornerShape(20.dp))
                .clip(RoundedCornerShape(20.dp))
                .background(Color(0xFF1E1E24))
                .padding(16.dp)
        ) {
            Text(
                text = hexPreview,
                style = Typography.bodyMedium.copy(
                    fontFamily = FontFamily.Monospace,
                    fontSize = 12.sp,
                    lineHeight = 20.sp,
                    color = Color(0xFF00FF88)
                ),
                modifier = Modifier.verticalScroll(rememberScrollState())
            )
        }

        Spacer(modifier = Modifier.height(12.dp))
        Text(
            "Showing first ${minOf(512, result.data.size)} of ${result.data.size} bytes",
            style = Typography.bodyMedium,
            color = TextSecondary
        )
    }
}

@Composable
private fun FileInfoFooter(result: QtsqEngine.QtsqResult, filePath: String) {
    val file = File(filePath)
    val compressedSize = file.length()
    val originalSize = result.originalSize
    val ratio = if (originalSize > 0) (compressedSize.toFloat() / originalSize * 100) else 0f

    val typeLabel = when (result.type) {
        QtsqType.IMAGE_GRAY -> "Grayscale Image"
        QtsqType.IMAGE_RGB -> "RGB Image"
        QtsqType.IMAGE_RGBA -> "RGBA Image"
        QtsqType.AUDIO_MONO -> "Mono Audio"
        QtsqType.AUDIO_STEREO -> "Stereo Audio"
        QtsqType.TEXT_ASCII -> "ASCII Text"
        QtsqType.TEXT_UTF8 -> "UTF-8 Text"
        QtsqType.JSON -> "JSON Data"
        QtsqType.CSV -> "CSV Data"
        QtsqType.RAW -> "Raw Binary"
        else -> "Unknown (0x${"%02X".format(result.type)})"
    }

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .background(SurfaceCard)
            .padding(top = 1.dp)
    ) {
        HorizontalDivider(color = Divider)
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 20.dp, vertical = 16.dp),
            horizontalArrangement = Arrangement.SpaceEvenly
        ) {
            InfoChip(label = "Type", value = typeLabel)
            InfoChip(label = "Size", value = "${compressedSize / 1024} KB")
            InfoChip(label = "Ratio", value = "${"%.1f".format(ratio)}%")
        }

        if (result.vertexCount > 0) {
            val formatStr = if (result.encoding.isNotEmpty()) result.encoding else "QTSQ HD"
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 20.dp)
                    .padding(bottom = 16.dp),
                horizontalArrangement = Arrangement.SpaceEvenly
            ) {
                InfoChip(label = "Engine", value = formatStr)
                InfoChip(label = "Mesh", value = "${result.vertexCount}v / ${result.faceCount}f")
                InfoChip(label = "Splats", value = "${result.splatCount}")
            }
        }
    }
}

@Composable
private fun InfoChip(label: String, value: String) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        Text(
            text = label,
            style = Typography.bodyMedium.copy(fontSize = 11.sp),
            color = TextSecondary
        )
        Text(
            text = value,
            style = Typography.bodyMedium.copy(fontWeight = FontWeight.SemiBold, fontSize = 13.sp),
            color = TextPrimary
        )
    }
}

// Audio playback helper
private fun playAudio(result: QtsqEngine.QtsqResult, onTrackReady: (AudioTrack) -> Unit) {
    val channels = when (result.type) {
        QtsqType.AUDIO_STEREO -> 2
        else -> 1
    }
    val sampleRate = if (result.audioSampleRate > 0) result.audioSampleRate else 44100
    val channelConfig = if (channels == 2) AudioFormat.CHANNEL_OUT_STEREO else AudioFormat.CHANNEL_OUT_MONO

    try {
        val track = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build()
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(sampleRate)
                    .setChannelMask(channelConfig)
                    .build()
            )
            .setBufferSizeInBytes(result.data.size)
            .setTransferMode(AudioTrack.MODE_STATIC)
            .build()

        track.write(result.data, 0, result.data.size)
        track.play()
        onTrackReady(track)
    } catch (e: Exception) {
        Log.e("ViewerScreen", "Audio playback failed", e)
    }
}
