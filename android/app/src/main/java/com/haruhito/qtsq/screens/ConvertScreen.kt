package com.haruhito.qtsq.screens

import android.graphics.Bitmap
import android.net.Uri
import android.util.Log
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.*
import androidx.compose.material.icons.rounded.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.haruhito.qtsq.QtsqEngine
import com.haruhito.qtsq.VideoConverter
import com.haruhito.qtsq.ui.theme.*
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.io.InputStream

enum class ImageQualityMode { HD, COMPACT, LOSSLESS, SPLAT_HD, SPLAT_COMPACT, SPLAT_LOSSLESS }
enum class AudioMethodMode { GRAVITATIONAL, PHOTON_HQ }

// Pixel-path video tiers. BudgetScale multiplies the source-size byte budget
// per frame, so LOW spends less and HIGH spends more than the original file.
enum class VideoQualityMode(val label: String, val budgetScale: Double) {
    PIXEL_HIGH("Pixel High", 1.5),
    PIXEL_MEDIUM("Pixel Medium", 1.0),
    PIXEL_LOW("Pixel Low", 0.6),
    PIXEL_LOSSLESS("Pixel Lossless", 0.0)
}

// Data class for compression result with preview info
data class CompressionResult(
    val outputFile: File,
    val originalSize: Long,
    val compressedSize: Long,
    val fileType: String,       // "image", "audio", "generic"
    val previewBitmap: Bitmap? = null,
    val imageWidth: Int = 0,
    val imageHeight: Int = 0,
    val qualityMode: String = "",
    val vertexCount: Int = 0,
    val splatCount: Int = 0,
    val encoding: String = ""
)

@Composable
fun ConvertScreen(
    qtsqEngine: QtsqEngine,
    onClose: () -> Unit,
    /** When set, the file picker opens once with this MIME filter applied. */
    initialPickerFilter: String? = null,
    onPickerFilterConsumed: () -> Unit = {}
) {
    val context = LocalContext.current
    val coroutineScope = rememberCoroutineScope()
    var isConverting by remember { mutableStateOf(false) }
    var selectedUri by remember { mutableStateOf<Uri?>(null) }
    var password by remember { mutableStateOf("") }
    var errorMessage by remember { mutableStateOf<String?>(null) }

    // Quality picker states
    var showImageQualityPicker by remember { mutableStateOf(false) }
    var showAudioMethodPicker by remember { mutableStateOf(false) }
    var showVideoQualityPicker by remember { mutableStateOf(false) }
    var pendingVideoMode by remember { mutableStateOf<VideoQualityMode?>(null) }
    var pendingImageQuality by remember { mutableStateOf<ImageQualityMode?>(null) }
    var pendingAudioMethod by remember { mutableStateOf<AudioMethodMode?>(null) }
    var pendingBytes by remember { mutableStateOf<ByteArray?>(null) }
    var pendingMimeType by remember { mutableStateOf("") }
    var pendingOriginalSize by remember { mutableStateOf(0L) }

    // Preview state
    var showPreview by remember { mutableStateOf(false) }
    var previewResult by remember { mutableStateOf<CompressionResult?>(null) }

    val filePickerLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocument(),
        onResult = { uri: Uri? ->
            selectedUri = uri
            errorMessage = null
            showPreview = false
            previewResult = null
        }
    )

    // A "Video" (or future per-type) entry point asks us to open the picker
    // straight away, already filtered, instead of making the user tap the
    // file card a second time.
    LaunchedEffect(initialPickerFilter) {
        if (initialPickerFilter != null) {
            filePickerLauncher.launch(arrayOf(initialPickerFilter))
            onPickerFilterConsumed()
        }
    }

    // Two-step method picker state
    var showMethodPicker by remember { mutableStateOf(false) }
    var selectedTier by remember { mutableStateOf<String?>(null) }  // "hd", "compact", "lossless"

    // ── Step 1: Image Quality Tier Picker Dialog ──
    if (showImageQualityPicker) {
        AlertDialog(
            onDismissRequest = {
                showImageQualityPicker = false
                pendingBytes = null
            },
            title = {
                Text(
                    "Image Quality",
                    style = Typography.titleMedium.copy(fontWeight = FontWeight.Bold),
                    color = TextPrimary
                )
            },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Surface(
                        modifier = Modifier
                            .fillMaxWidth()
                            .clip(RoundedCornerShape(20.dp))
                            .clickable {
                                selectedTier = "hd"
                                showImageQualityPicker = false
                                showMethodPicker = true
                            },
                        color = AccentPrimaryLight,
                        shape = RoundedCornerShape(20.dp)
                    ) {
                        Row(
                            modifier = Modifier.padding(16.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Icon(Icons.Rounded.HighQuality, null, tint = AccentPrimary, modifier = Modifier.size(28.dp))
                            Spacer(modifier = Modifier.width(14.dp))
                            Column {
                                Text("HD Quality", style = Typography.bodyLarge.copy(fontWeight = FontWeight.SemiBold), color = TextPrimary)
                                Text("Best quality, ~20-30% smaller", style = Typography.bodySmall, color = TextSecondary)
                            }
                        }
                    }
                    Surface(
                        modifier = Modifier
                            .fillMaxWidth()
                            .clip(RoundedCornerShape(20.dp))
                            .clickable {
                                selectedTier = "compact"
                                showImageQualityPicker = false
                                showMethodPicker = true
                            },
                        color = SurfaceCard,
                        shape = RoundedCornerShape(20.dp)
                    ) {
                        Row(
                            modifier = Modifier.padding(16.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Icon(Icons.Rounded.Compress, null, tint = TextSecondary, modifier = Modifier.size(28.dp))
                            Spacer(modifier = Modifier.width(14.dp))
                            Column {
                                Text("Compact", style = Typography.bodyLarge.copy(fontWeight = FontWeight.SemiBold), color = TextPrimary)
                                Text("Smallest file size", style = Typography.bodySmall, color = TextSecondary)
                            }
                        }
                    }
                    Surface(
                        modifier = Modifier
                            .fillMaxWidth()
                            .clip(RoundedCornerShape(20.dp))
                            .clickable {
                                selectedTier = "lossless"
                                showImageQualityPicker = false
                                showMethodPicker = true
                            },
                        color = Color(0xFFFFF8E1),
                        shape = RoundedCornerShape(20.dp)
                    ) {
                        Row(
                            modifier = Modifier.padding(16.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Icon(Icons.Rounded.Diamond, null, tint = Color(0xFFFFB300), modifier = Modifier.size(28.dp))
                            Spacer(modifier = Modifier.width(14.dp))
                            Column {
                                Text("Lossless", style = Typography.bodyLarge.copy(fontWeight = FontWeight.SemiBold), color = TextPrimary)
                                Text("Maximum quality · 6M vertices", style = Typography.bodySmall, color = TextSecondary)
                            }
                        }
                    }
                }
            },
            confirmButton = {},
            dismissButton = {
                TextButton(onClick = { showImageQualityPicker = false; pendingBytes = null }) {
                    Text("Cancel", color = TextSecondary)
                }
            },
            containerColor = BackgroundWhite,
            shape = RoundedCornerShape(20.dp)
        )
    }

    // ── Step 2: Compression Method Picker (Hybrid vs Pure Splats) ──
    if (showMethodPicker && selectedTier != null) {
        AlertDialog(
            onDismissRequest = {
                showMethodPicker = false
                selectedTier = null
                pendingBytes = null
            },
            title = {
                Text(
                    "Compression Method",
                    style = Typography.titleMedium.copy(fontWeight = FontWeight.Bold),
                    color = TextPrimary
                )
            },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    // Hybrid (Triangle + Splats)
                    Surface(
                        modifier = Modifier
                            .fillMaxWidth()
                            .clip(RoundedCornerShape(20.dp))
                            .clickable {
                                pendingImageQuality = when (selectedTier) {
                                    "hd" -> ImageQualityMode.HD
                                    "compact" -> ImageQualityMode.COMPACT
                                    else -> ImageQualityMode.LOSSLESS
                                }
                                showMethodPicker = false
                                selectedTier = null
                            },
                        color = AccentPrimaryLight,
                        shape = RoundedCornerShape(20.dp)
                    ) {
                        Row(
                            modifier = Modifier.padding(16.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Icon(Icons.Rounded.ChangeHistory, null, tint = AccentPrimary, modifier = Modifier.size(28.dp))
                            Spacer(modifier = Modifier.width(14.dp))
                            Column {
                                Text("Triangle + Splats", style = Typography.bodyLarge.copy(fontWeight = FontWeight.SemiBold), color = TextPrimary)
                                Text("Hybrid mesh compression (current)", style = Typography.bodySmall, color = TextSecondary)
                            }
                        }
                    }
                    // Pure Splats Only
                    Surface(
                        modifier = Modifier
                            .fillMaxWidth()
                            .clip(RoundedCornerShape(20.dp))
                            .clickable {
                                pendingImageQuality = when (selectedTier) {
                                    "hd" -> ImageQualityMode.SPLAT_HD
                                    "compact" -> ImageQualityMode.SPLAT_COMPACT
                                    else -> ImageQualityMode.SPLAT_LOSSLESS
                                }
                                showMethodPicker = false
                                selectedTier = null
                            },
                        color = Color(0xFFFCE4EC),
                        shape = RoundedCornerShape(20.dp)
                    ) {
                        Row(
                            modifier = Modifier.padding(16.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Icon(Icons.Rounded.BlurOn, null, tint = Color(0xFFE91E63), modifier = Modifier.size(28.dp))
                            Spacer(modifier = Modifier.width(14.dp))
                            Column {
                                Text("Pure Splats Only", style = Typography.bodyLarge.copy(fontWeight = FontWeight.SemiBold), color = TextPrimary)
                                Text("Gaussian splat-only mode (new)", style = Typography.bodySmall, color = TextSecondary)
                            }
                        }
                    }
                }
            },
            confirmButton = {},
            dismissButton = {
                TextButton(onClick = { showMethodPicker = false; selectedTier = null; pendingBytes = null }) {
                    Text("Cancel", color = TextSecondary)
                }
            },
            containerColor = BackgroundWhite,
            shape = RoundedCornerShape(20.dp)
        )
    }

    // ── Audio Method Picker Dialog ──
    if (showAudioMethodPicker) {
        AlertDialog(
            onDismissRequest = {
                showAudioMethodPicker = false
                pendingBytes = null
            },
            title = {
                Text(
                    "Audio Compression",
                    style = Typography.titleMedium.copy(fontWeight = FontWeight.Bold),
                    color = TextPrimary
                )
            },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Surface(
                        modifier = Modifier
                            .fillMaxWidth()
                            .clip(RoundedCornerShape(20.dp))
                            .clickable {
                                pendingAudioMethod = AudioMethodMode.GRAVITATIONAL
                                showAudioMethodPicker = false
                            },
                        color = AccentPrimaryLight,
                        shape = RoundedCornerShape(20.dp)
                    ) {
                        Row(
                            modifier = Modifier.padding(16.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Icon(Icons.Rounded.GraphicEq, null, tint = AccentPrimary, modifier = Modifier.size(28.dp))
                            Spacer(modifier = Modifier.width(14.dp))
                            Column {
                                Text("Gravitational", style = Typography.bodyLarge.copy(fontWeight = FontWeight.SemiBold), color = TextPrimary)
                                Text("Classic wavelet compression", style = Typography.bodySmall, color = TextSecondary)
                            }
                        }
                    }
                    Surface(
                        modifier = Modifier
                            .fillMaxWidth()
                            .clip(RoundedCornerShape(20.dp))
                            .clickable {
                                pendingAudioMethod = AudioMethodMode.PHOTON_HQ
                                showAudioMethodPicker = false
                            },
                        color = SurfaceCard,
                        shape = RoundedCornerShape(20.dp)
                    ) {
                        Row(
                            modifier = Modifier.padding(16.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Icon(Icons.Rounded.SpatialAudio, null, tint = TextSecondary, modifier = Modifier.size(28.dp))
                            Spacer(modifier = Modifier.width(14.dp))
                            Column {
                                Text("Photon HQ", style = Typography.bodyLarge.copy(fontWeight = FontWeight.SemiBold), color = TextPrimary)
                                Text("Gabor spatial atoms, higher fidelity", style = Typography.bodySmall, color = TextSecondary)
                            }
                        }
                    }
                }
            },
            confirmButton = {},
            dismissButton = {
                TextButton(onClick = { showAudioMethodPicker = false; pendingBytes = null }) {
                    Text("Cancel", color = TextSecondary)
                }
            },
            containerColor = BackgroundWhite,
            shape = RoundedCornerShape(20.dp)
        )
    }

    // ── Trigger compression after quality selection ──
    LaunchedEffect(pendingImageQuality) {
        val quality = pendingImageQuality ?: return@LaunchedEffect
        val bytes = pendingBytes ?: return@LaunchedEffect
        val origSize = pendingOriginalSize
        val mime = pendingMimeType
        // Clear pendingBytes but NOT the key — nulling the key cancels this coroutine!
        pendingBytes = null

        isConverting = true
        errorMessage = null
        try {
            val result = withContext(Dispatchers.IO) {
                try {
                    compressImageWithPreview(context, qtsqEngine, bytes, quality, password, origSize, mime)
                } catch (e: Exception) {
                    Log.e("ConvertScreen", "Compress error", e)
                    null
                }
            }
            if (result != null) {
                previewResult = result
                showPreview = true
            } else {
                errorMessage = "Compression failed."
            }
        } finally {
            isConverting = false
            pendingImageQuality = null  // Safe to null AFTER work is done
        }
    }

    // ── Video Quality Tier Picker ──
    if (showVideoQualityPicker) {
        AlertDialog(
            onDismissRequest = { showVideoQualityPicker = false; pendingBytes = null },
            title = {
                Text(
                    "Video Compression",
                    style = Typography.titleMedium.copy(fontWeight = FontWeight.Bold),
                    color = TextPrimary
                )
            },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    Text(
                        "Pixel codec — GOP I/P frames with motion compensation. " +
                        "The source audio track is carried with Classic Gravitational.",
                        style = Typography.bodySmall,
                        color = TextSecondary
                    )
                    VideoQualityMode.entries.forEach { mode ->
                        Surface(
                            onClick = {
                                pendingVideoMode = mode
                                showVideoQualityPicker = false
                            },
                            shape = RoundedCornerShape(12.dp),
                            color = SurfaceCard,
                            modifier = Modifier.fillMaxWidth()
                        ) {
                            Column(Modifier.padding(14.dp)) {
                                Text(mode.label, color = TextPrimary, fontWeight = FontWeight.SemiBold)
                                Text(
                                    when (mode) {
                                        VideoQualityMode.PIXEL_HIGH -> "More bytes than the source — best quality"
                                        VideoQualityMode.PIXEL_MEDIUM -> "Match the source file size"
                                        VideoQualityMode.PIXEL_LOW -> "Smaller than the source"
                                        VideoQualityMode.PIXEL_LOSSLESS -> "No rate limit (large)"
                                    },
                                    style = Typography.bodySmall,
                                    color = TextSecondary
                                )
                            }
                        }
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = { showVideoQualityPicker = false; pendingBytes = null }) {
                    Text("Cancel", color = TextSecondary)
                }
            }
        )
    }

    // ── Video encode: demux/decode the source, then hand RGB frames and PCM
    //    to the native pixel encoder, which attaches Classic Gravitational audio.
    LaunchedEffect(pendingVideoMode) {
        val mode = pendingVideoMode ?: return@LaunchedEffect
        val bytes = pendingBytes ?: return@LaunchedEffect
        val origSize = pendingOriginalSize
        val uri = selectedUri ?: return@LaunchedEffect
        pendingBytes = null
        pendingVideoMode = null

        isConverting = true
        errorMessage = null
        try {
            val result = withContext(Dispatchers.IO) {
                // One streaming transcode. Frames go from the decoder straight
                // into the native encoder, so the JVM never holds the whole
                // clip (300 x 640x360x3 = 207 MB, which OOM'd on a 256 MB heap).
                val target = if (mode.budgetScale > 0.0) {
                    (kotlin.math.max(256L, origSize / 300) * mode.budgetScale).toLong()
                } else 0L
                val res = VideoConverter.transcode(
                    qtsqEngine, context, uri, target,
                    maxFrames = 300, maxDimension = 640
                )
                Log.i(
                    "ConvertScreen",
                    "video encoded ${res.width}x${res.height} fps=${res.fps} " +
                        "frames=${res.frameCount} audio=${res.audioSamples} " +
                        "bytes=${res.qtsqBytes.size}"
                )
                if (res.frameCount <= 0) {
                    return@withContext "Video track produced no frames."
                }
                val outFile = File(context.filesDir, "video_${System.currentTimeMillis()}.qtsq")
                outFile.writeBytes(res.qtsqBytes)
                withContext(Dispatchers.Main) {
                    previewResult = CompressionResult(
                        outputFile = outFile,
                        originalSize = origSize,
                        compressedSize = res.qtsqBytes.size.toLong(),
                        fileType = "video",
                        imageWidth = res.width,
                        imageHeight = res.height,
                        qualityMode = mode.label,
                        encoding = if (res.audioSamples > 0) "video+gfc-audio" else "video"
                    )
                    showPreview = true
                }
                "OK"
            }
            isConverting = false
            if (result != "OK" && result != null) errorMessage = result
        } catch (e: Exception) {
            Log.e("ConvertScreen", "Video conversion failed", e)
            isConverting = false
            errorMessage = "Video conversion failed: ${e.message}"
        }
    }

    LaunchedEffect(pendingAudioMethod) {
        val method = pendingAudioMethod ?: return@LaunchedEffect
        val bytes = pendingBytes ?: return@LaunchedEffect
        val origSize = pendingOriginalSize
        pendingBytes = null

        isConverting = true
        errorMessage = null
        try {
            val result = withContext(Dispatchers.IO) {
                try {
                    compressAudioWithPreview(context, qtsqEngine, bytes, method, password, origSize)
                } catch (e: Exception) {
                    Log.e("ConvertScreen", "Audio compress error", e)
                    null
                }
            }
            if (result != null) {
                previewResult = result
                showPreview = true
            } else {
                errorMessage = "Compression failed."
            }
        } finally {
            isConverting = false
            pendingAudioMethod = null
        }
    }

    // ══════════════════════════════════════════════
    // PREVIEW SCREEN (shown after compression)
    // ══════════════════════════════════════════════
    if (showPreview && previewResult != null) {
        val pr = previewResult!!
        Column(
            modifier = Modifier
                .fillMaxSize()
                .background(BackgroundWhite)
                .statusBarsPadding()
                .navigationBarsPadding()
                .verticalScroll(rememberScrollState()),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            // Top Bar
            Row(
                modifier = Modifier.fillMaxWidth().padding(top = 0.dp, bottom = 8.dp, start = 8.dp, end = 8.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                IconButton(onClick = { showPreview = false; previewResult = null }) {
                    Icon(Icons.AutoMirrored.Rounded.ArrowBack, "Back", tint = TextPrimary)
                }
                Text("Compression Result", style = Typography.titleLarge, color = TextPrimary, modifier = Modifier.padding(start = 8.dp))
            }

            // Phase 7: show which container codec produced this file (qtc default / zlib opt-out).
            Text(
                text = "Compressed via ${QtsqEngine.codecName(context)}",
                style = Typography.bodySmall,
                color = TextTertiary,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(start = 20.dp, end = 20.dp)
            )

            Spacer(modifier = Modifier.height(4.dp))

            // ── Image Preview ──
            if (pr.fileType == "image" && pr.previewBitmap != null) {
                Box(
                    modifier = Modifier
                        .padding(horizontal = 20.dp)
                        .fillMaxWidth()
                        .clip(RoundedCornerShape(20.dp))
                        .background(SurfaceCard)
                        .border(1.dp, Divider, RoundedCornerShape(20.dp))
                ) {
                    Image(
                        bitmap = pr.previewBitmap.asImageBitmap(),
                        contentDescription = "Compressed preview",
                        modifier = Modifier
                            .fillMaxWidth()
                            .clip(RoundedCornerShape(20.dp)),
                        contentScale = ContentScale.FillWidth
                    )
                }
                Spacer(modifier = Modifier.height(4.dp))
                Text(
                    "${pr.imageWidth} × ${pr.imageHeight}",
                    style = Typography.bodySmall,
                    color = TextTertiary,
                    modifier = Modifier.padding(horizontal = 20.dp)
                )
            }

            // ── Audio/Generic Icon ──
            if (pr.fileType == "audio") {
                Box(
                    modifier = Modifier
                        .padding(horizontal = 20.dp)
                        .fillMaxWidth()
                        .height(140.dp)
                        .clip(RoundedCornerShape(20.dp))
                        .background(AccentPrimaryLight),
                    contentAlignment = Alignment.Center
                ) {
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        Icon(Icons.Rounded.GraphicEq, null, tint = AccentPrimary, modifier = Modifier.size(48.dp))
                        Spacer(modifier = Modifier.height(8.dp))
                        Text("Audio compressed", style = Typography.bodyMedium, color = AccentPrimary)
                    }
                }
            }

            if (pr.fileType == "video") {
                Box(
                    modifier = Modifier
                        .padding(horizontal = 20.dp)
                        .fillMaxWidth()
                        .height(140.dp)
                        .clip(RoundedCornerShape(20.dp))
                        .background(SurfaceCard),
                    contentAlignment = Alignment.Center
                ) {
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        Icon(Icons.Rounded.Movie, null, tint = AccentPrimary, modifier = Modifier.size(48.dp))
                        Spacer(modifier = Modifier.height(8.dp))
                        Text("Video compressed", style = Typography.bodyMedium, color = AccentPrimary)
                        if (pr.imageWidth > 0) {
                            Text(
                                "${pr.imageWidth}×${pr.imageHeight}" +
                                    if (pr.encoding.contains("audio")) " · audio attached" else "",
                                style = Typography.bodySmall,
                                color = TextSecondary
                            )
                        }
                    }
                }
            }

            if (pr.fileType == "generic") {
                Box(
                    modifier = Modifier
                        .padding(horizontal = 20.dp)
                        .fillMaxWidth()
                        .height(140.dp)
                        .clip(RoundedCornerShape(20.dp))
                        .background(SurfaceCard),
                    contentAlignment = Alignment.Center
                ) {
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        Icon(Icons.AutoMirrored.Rounded.InsertDriveFile, null, tint = TextSecondary, modifier = Modifier.size(48.dp))
                        Spacer(modifier = Modifier.height(8.dp))
                        Text("File compressed", style = Typography.bodyMedium, color = TextSecondary)
                    }
                }
            }

            Spacer(modifier = Modifier.height(20.dp))

            // ── Size Comparison Card ──
            Box(
                modifier = Modifier
                    .padding(horizontal = 20.dp)
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(20.dp))
                    .background(SurfaceCard)
                    .padding(20.dp)
            ) {
                Column {
                    Text("Size Comparison", style = Typography.titleSmall.copy(fontWeight = FontWeight.Bold), color = TextPrimary)
                    Spacer(modifier = Modifier.height(16.dp))

                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceEvenly,
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        // Original
                        Column(horizontalAlignment = Alignment.CenterHorizontally) {
                            Text("Original", style = Typography.bodySmall, color = TextTertiary)
                            Spacer(modifier = Modifier.height(4.dp))
                            Text(
                                formatSizeKt(pr.originalSize),
                                style = Typography.titleMedium.copy(fontWeight = FontWeight.Bold),
                                color = TextPrimary
                            )
                        }

                        // Arrow
                        Icon(
                            Icons.AutoMirrored.Rounded.ArrowForward,
                            null,
                            tint = AccentPrimary,
                            modifier = Modifier.size(24.dp)
                        )

                        // Compressed
                        Column(horizontalAlignment = Alignment.CenterHorizontally) {
                            Text("Compressed", style = Typography.bodySmall, color = TextTertiary)
                            Spacer(modifier = Modifier.height(4.dp))
                            Text(
                                formatSizeKt(pr.compressedSize),
                                style = Typography.titleMedium.copy(fontWeight = FontWeight.Bold),
                                color = AccentPrimary
                            )
                        }
                    }

                    Spacer(modifier = Modifier.height(16.dp))

                    // Ratio / Savings bar
                    val savings = if (pr.originalSize > 0) {
                        ((pr.originalSize - pr.compressedSize).toFloat() / pr.originalSize * 100).coerceIn(-999f, 100f)
                    } else 0f

                    val isSmaller = pr.compressedSize <= pr.originalSize

                    Box(
                        modifier = Modifier
                            .fillMaxWidth()
                            .height(40.dp)
                            .clip(RoundedCornerShape(10.dp))
                            .background(if (isSmaller) Color(0xFFE8F5E9) else Color(0xFFFFF3E0)),
                        contentAlignment = Alignment.Center
                    ) {
                        Text(
                            text = if (isSmaller) {
                                "%.1f%% smaller".format(savings)
                            } else {
                                "Quality enhanced (+${formatSizeKt(pr.compressedSize - pr.originalSize)})"
                            },
                            style = Typography.bodyMedium.copy(fontWeight = FontWeight.SemiBold),
                            color = if (isSmaller) Color(0xFF2E7D32) else Color(0xFFE65100)
                        )
                    }
                }
            }

            Spacer(modifier = Modifier.height(16.dp))

            // ── Technical Details Card ──
            if (pr.encoding.isNotEmpty() || pr.vertexCount > 0) {
                Box(
                    modifier = Modifier
                        .padding(horizontal = 20.dp)
                        .fillMaxWidth()
                        .clip(RoundedCornerShape(20.dp))
                        .background(SurfaceCard)
                        .padding(20.dp)
                ) {
                    Column {
                        Text("Technical Details", style = Typography.titleSmall.copy(fontWeight = FontWeight.Bold), color = TextPrimary)
                        Spacer(modifier = Modifier.height(12.dp))
                        if (pr.qualityMode.isNotEmpty()) {
                            DetailRow("Mode", pr.qualityMode)
                        }
                        if (pr.encoding.isNotEmpty()) {
                            DetailRow("Encoding", pr.encoding)
                        }
                        if (pr.vertexCount > 0) {
                            DetailRow("Vertices", "%,d".format(pr.vertexCount))
                        }
                        if (pr.splatCount > 0) {
                            DetailRow("Splats", "%,d".format(pr.splatCount))
                        }
                        val ratio = if (pr.compressedSize > 0) {
                            "%.1f:1".format(pr.originalSize.toFloat() / pr.compressedSize)
                        } else "—"
                        DetailRow("Ratio", ratio)
                    }
                }
            }

            Spacer(modifier = Modifier.height(24.dp))

            // ── Done Button ──
            Button(
                onClick = { showPreview = false; previewResult = null; selectedUri = null },
                colors = ButtonDefaults.buttonColors(
                    containerColor = InkBlack,
                    contentColor = Color.White
                ),
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 20.dp)
                    .height(56.dp),
                shape = RoundedCornerShape(20.dp)
            ) {
                Icon(Icons.Rounded.CheckCircle, null, modifier = Modifier.size(20.dp))
                Spacer(modifier = Modifier.width(8.dp))
                Text("Done", style = Typography.bodyLarge.copy(fontWeight = FontWeight.SemiBold))
            }

            Spacer(modifier = Modifier.height(40.dp))
        }
        return // Don't render the main screen behind it
    }

    // ══════════════════════════════════════════════
    // MAIN COMPRESSION SCREEN
    // ══════════════════════════════════════════════
    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(BackgroundWhite)
            .statusBarsPadding()
            .navigationBarsPadding(),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        // ── Top Bar ──
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(top = 0.dp, bottom = 8.dp, start = 8.dp, end = 8.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            IconButton(onClick = onClose) {
                Icon(Icons.AutoMirrored.Rounded.ArrowBack, contentDescription = "Back", tint = TextPrimary)
            }
            Text("Compress File", style = Typography.titleLarge, color = TextPrimary, modifier = Modifier.padding(start = 8.dp))
        }

        Spacer(modifier = Modifier.height(16.dp))

        // ── File Selection Card ──
        Box(
            modifier = Modifier
                .padding(horizontal = 20.dp)
                .fillMaxWidth()
                .clip(RoundedCornerShape(20.dp))
                .background(SurfaceCard)
                .clickable { filePickerLauncher.launch(arrayOf("image/*", "text/*", "video/*", "audio/*")) }
                .padding(40.dp),
            contentAlignment = Alignment.Center
        ) {
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Box(
                    modifier = Modifier
                        .size(56.dp)
                        .background(AccentPrimaryLight, RoundedCornerShape(20.dp)),
                    contentAlignment = Alignment.Center
                ) {
                    Icon(Icons.Rounded.FileOpen, null, tint = AccentPrimary, modifier = Modifier.size(28.dp))
                }
                Spacer(modifier = Modifier.height(16.dp))
                Text(
                    text = if (selectedUri == null) "Select a file to compress" else "File selected ✓",
                    style = Typography.bodyLarge,
                    color = if (selectedUri == null) TextSecondary else Success
                )
                if (selectedUri == null) {
                    Text("Supports images, video, audio and text files", style = Typography.bodySmall, color = TextTertiary, modifier = Modifier.padding(top = 4.dp))
                }
            }
        }

        Spacer(modifier = Modifier.height(20.dp))

        // ── Password Input ──
        OutlinedTextField(
            value = password,
            onValueChange = { password = it },
            label = { Text("Password (Optional)") },
            visualTransformation = PasswordVisualTransformation(),
            singleLine = true,
            modifier = Modifier.fillMaxWidth().padding(horizontal = 20.dp),
            shape = RoundedCornerShape(20.dp),
            colors = OutlinedTextFieldDefaults.colors(
                focusedBorderColor = AccentPrimary,
                unfocusedBorderColor = Divider,
                focusedLabelColor = AccentPrimary
            )
        )

        Spacer(modifier = Modifier.height(28.dp))

        // ── Compress Button ──
        Button(
            onClick = {
                val uri = selectedUri ?: return@Button
                isConverting = true
                errorMessage = null
                coroutineScope.launch {
                    val resultOrNull = withContext(Dispatchers.IO) {
                        try {
                            val inputStream: InputStream? = context.contentResolver.openInputStream(uri)
                            val bytes = inputStream?.readBytes()
                            inputStream?.close()

                            if (bytes != null) {
                                val mimeType = context.contentResolver.getType(uri) ?: ""
                                when {
                                    mimeType.startsWith("image/") -> {
                                        pendingBytes = bytes
                                        pendingMimeType = mimeType
                                        pendingOriginalSize = bytes.size.toLong()
                                        withContext(Dispatchers.Main) {
                                            isConverting = false
                                            showImageQualityPicker = true
                                        }
                                        return@withContext "PICKER"
                                    }
                                    mimeType.startsWith("video/") -> {
                                        pendingBytes = bytes
                                        pendingMimeType = mimeType
                                        pendingOriginalSize = bytes.size.toLong()
                                        withContext(Dispatchers.Main) {
                                            isConverting = false
                                            showVideoQualityPicker = true
                                        }
                                        return@withContext "PICKER"
                                    }
                                    mimeType.startsWith("audio/") -> {
                                        pendingBytes = bytes
                                        pendingMimeType = mimeType
                                        pendingOriginalSize = bytes.size.toLong()
                                        withContext(Dispatchers.Main) {
                                            isConverting = false
                                            showAudioMethodPicker = true
                                        }
                                        return@withContext "PICKER"
                                    }
                                    mimeType.startsWith("text/") -> {
                                        val compressData = qtsqEngine.compressBytes(bytes, "raw", password.ifEmpty { null })
                                        if (compressData != null) {
                                            val outFile = File(context.filesDir, "converted_${System.currentTimeMillis()}.qtsq")
                                            outFile.writeBytes(compressData)
                                            withContext(Dispatchers.Main) {
                                                previewResult = CompressionResult(
                                                    outputFile = outFile,
                                                    originalSize = bytes.size.toLong(),
                                                    compressedSize = compressData.size.toLong(),
                                                    fileType = "generic",
                                                    qualityMode = "Text"
                                                )
                                                showPreview = true
                                            }
                                            "OK"
                                        } else "ERROR"
                                    }
                                    else -> {
                                        withContext(Dispatchers.Main) {
                                            isConverting = false
                                            errorMessage = "Unsupported file type. Images, video, audio and text are supported."
                                        }
                                        "PICKER"
                                    }
                                }
                            } else "ERROR"
                        } catch (e: Exception) {
                            Log.e("ConvertScreen", "Error", e)
                            "ERROR"
                        }
                    }
                    if (resultOrNull == "ERROR") {
                        errorMessage = "Compression failed."
                    }
                    isConverting = false
                }
            },
            enabled = selectedUri != null && !isConverting,
            colors = ButtonDefaults.buttonColors(
                containerColor = InkBlack,
                contentColor = Color.White,
                disabledContainerColor = Divider,
                disabledContentColor = TextSecondary
            ),
            elevation = ButtonDefaults.buttonElevation(defaultElevation = 0.dp, pressedElevation = 0.dp),
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 20.dp)
                .height(56.dp),
            shape = RoundedCornerShape(20.dp)
        ) {
            if (isConverting) {
                CircularProgressIndicator(color = AccentPrimary, modifier = Modifier.size(24.dp), strokeWidth = 2.dp)
                Spacer(modifier = Modifier.width(12.dp))
                Text("Compressing...", style = Typography.bodyLarge.copy(fontWeight = FontWeight.SemiBold))
            } else {
                Icon(Icons.Rounded.Compress, null, modifier = Modifier.size(20.dp))
                Spacer(modifier = Modifier.width(8.dp))
                Text("Compress to .qtsq", style = Typography.bodyLarge.copy(fontWeight = FontWeight.SemiBold))
            }
        }

        // ── Error ──
        if (errorMessage != null) {
            Spacer(modifier = Modifier.height(24.dp))
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 20.dp)
                    .clip(RoundedCornerShape(20.dp))
                    .background(CategoryRedBg)
                    .padding(16.dp)
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Icon(Icons.Rounded.Error, null, tint = Error, modifier = Modifier.size(20.dp))
                    Spacer(modifier = Modifier.width(12.dp))
                    Text(errorMessage!!, style = Typography.bodyMedium, color = TextPrimary)
                }
            }
        }
    }
}

@Composable
private fun DetailRow(label: String, value: String) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp),
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        Text(label, style = Typography.bodyMedium, color = TextSecondary)
        Text(value, style = Typography.bodyMedium.copy(fontWeight = FontWeight.Medium), color = TextPrimary)
    }
}

private fun formatSizeKt(size: Long): String {
    if (size < 1024) return "$size B"
    if (size < 1048576) return "%.1f KB".format(size / 1024.0)
    return "%.1f MB".format(size / 1048576.0)
}

// ── Helper: Compress Image with Preview ──
private fun isLossyMime(mimeType: String): Boolean {
    val m = mimeType.lowercase()
    return m.contains("jpeg") || m.contains("jpg") || m.contains("webp")
}

private fun compressImageWithPreview(
    context: android.content.Context,
    qtsqEngine: QtsqEngine,
    bytes: ByteArray,
    quality: ImageQualityMode,
    password: String,
    originalSize: Long,
    mimeType: String = ""
): CompressionResult? {
    // Fail-closed engine: keystore must be ready (or a password must be set)
    if (password.isEmpty() && !qtsqEngine.ensureKeystoreReady(context)) {
        Log.e("ConvertScreen", "Keystore unavailable and no password set — refusing to save")
        return null
    }
    // ── Step 1: Decode bitmap with downsampling for phone (cap 2048px) ──
    val opts = android.graphics.BitmapFactory.Options()
    opts.inJustDecodeBounds = true
    android.graphics.BitmapFactory.decodeByteArray(bytes, 0, bytes.size, opts)
    val maxDim = maxOf(opts.outWidth, opts.outHeight)
    opts.inSampleSize = 1
    val targetMax = when (quality) {
        ImageQualityMode.COMPACT, ImageQualityMode.SPLAT_COMPACT -> 4096
        ImageQualityMode.HD, ImageQualityMode.SPLAT_HD -> 4096
        ImageQualityMode.LOSSLESS, ImageQualityMode.SPLAT_LOSSLESS -> 8192
    }
    while (maxDim / opts.inSampleSize > targetMax) {
        opts.inSampleSize *= 2
    }
    opts.inJustDecodeBounds = false
    opts.inPreferredConfig = android.graphics.Bitmap.Config.ARGB_8888
    val bitmap = android.graphics.BitmapFactory.decodeByteArray(bytes, 0, bytes.size, opts)
        ?: return null
    val w = bitmap.width
    val h = bitmap.height
    Log.d("ConvertScreen", "Decoded ${opts.outWidth}x${opts.outHeight} → ${w}x${h} (inSampleSize=${opts.inSampleSize})")

    // ── Step 2: Fast ARGB→RGB via IntArray + native conversion ──
    val argbPixels = IntArray(w * h)
    bitmap.getPixels(argbPixels, 0, w, 0, 0, w, h)
    val rgb = qtsqEngine.argbToRgb(argbPixels) ?: return null

    /* HD routing:
     *  - Small files (< 500KB): Upscale — reconstructs edges from compression artifacts
     *  - Lossy formats (JPEG/WebP): Upscale — same treatment, lossy always benefits
     *  - Large lossless (PNG/BMP/TIFF ≥ 500KB): HD — gentle quality-preserving compression */
    val useUpscale = bytes.size < 500_000 || isLossyMime(mimeType)

    val compressData = when (quality) {
        ImageQualityMode.HD -> {
            if (useUpscale) {
                qtsqEngine.compressImageUpscale(rgb, w, h, 3, password.ifEmpty { null }, bytes.size.toLong())
            } else {
                qtsqEngine.compressImageHD(rgb, w, h, 3, password.ifEmpty { null }, bytes.size.toLong())
            }
        }
        ImageQualityMode.COMPACT -> {
            qtsqEngine.compressImage(rgb, w, h, 3, password.ifEmpty { null }, bytes.size.toLong())
        }
        ImageQualityMode.LOSSLESS -> {
            qtsqEngine.compressImageLossless(rgb, w, h, 3, password.ifEmpty { null }, bytes.size.toLong())
        }
        ImageQualityMode.SPLAT_COMPACT -> {
            qtsqEngine.compressSplatCompact(rgb, w, h, 3, password.ifEmpty { null }, bytes.size.toLong())
        }
        ImageQualityMode.SPLAT_HD -> {
            qtsqEngine.compressSplatHD(rgb, w, h, 3, password.ifEmpty { null }, bytes.size.toLong())
        }
        ImageQualityMode.SPLAT_LOSSLESS -> {
            qtsqEngine.compressSplatLossless(rgb, w, h, 3, password.ifEmpty { null }, bytes.size.toLong())
        }
    } ?: run { bitmap.recycle(); return null }

    val outFile = File(context.filesDir, "converted_${System.currentTimeMillis()}.qtsq")
    outFile.writeBytes(compressData)

    val previewBitmap = bitmap

    val modeName = when (quality) {
        ImageQualityMode.HD -> if (useUpscale) "HD (Upscale)" else "HD"
        ImageQualityMode.COMPACT -> "Compact"
        ImageQualityMode.LOSSLESS -> "Lossless"
        ImageQualityMode.SPLAT_COMPACT -> "Splat Compact"
        ImageQualityMode.SPLAT_HD -> "Splat HD"
        ImageQualityMode.SPLAT_LOSSLESS -> "Splat Lossless"
    }

    return CompressionResult(
        outputFile = outFile,
        originalSize = originalSize,
        compressedSize = compressData.size.toLong(),
        fileType = "image",
        previewBitmap = previewBitmap,
        imageWidth = w,
        imageHeight = h,
        qualityMode = modeName
    )
}

// ── Helper: Compress Audio with Preview ──
private fun compressAudioWithPreview(
    context: android.content.Context,
    qtsqEngine: QtsqEngine,
    bytes: ByteArray,
    method: AudioMethodMode,
    password: String,
    originalSize: Long
): CompressionResult? {
    try {
        val tempFile = File(context.cacheDir, "temp_audio_${System.currentTimeMillis()}")
        tempFile.writeBytes(bytes)
        val extractor = android.media.MediaExtractor()
        extractor.setDataSource(tempFile.absolutePath)
        var audioTrack = -1
        var audioFormat: android.media.MediaFormat? = null
        for (t in 0 until extractor.trackCount) {
            val fmt = extractor.getTrackFormat(t)
            val mime = fmt.getString(android.media.MediaFormat.KEY_MIME) ?: ""
            if (mime.startsWith("audio/")) { audioTrack = t; audioFormat = fmt; break }
        }
        if (audioTrack >= 0 && audioFormat != null) {
            extractor.selectTrack(audioTrack)
            val sampleRate = audioFormat.getInteger(android.media.MediaFormat.KEY_SAMPLE_RATE)
            val channels = audioFormat.getInteger(android.media.MediaFormat.KEY_CHANNEL_COUNT)
            val codecMime = audioFormat.getString(android.media.MediaFormat.KEY_MIME) ?: ""
            val codec = android.media.MediaCodec.createDecoderByType(codecMime)
            codec.configure(audioFormat, null, null, 0)
            codec.start()
            val pcmBuffers = mutableListOf<ByteArray>()
            val bufInfo = android.media.MediaCodec.BufferInfo()
            var eos = false

            while (!eos) {
                val inIdx = codec.dequeueInputBuffer(10000)
                if (inIdx >= 0) {
                    val inBuf = codec.getInputBuffer(inIdx)!!
                    val read = extractor.readSampleData(inBuf, 0)
                    if (read < 0) {
                        codec.queueInputBuffer(inIdx, 0, 0, 0, android.media.MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                    } else {
                        codec.queueInputBuffer(inIdx, 0, read, extractor.sampleTime, 0)
                        extractor.advance()
                    }
                }
                var outIdx = codec.dequeueOutputBuffer(bufInfo, 10000)
                while (outIdx >= 0) {
                    if (bufInfo.size > 0) {
                        val outBuf = codec.getOutputBuffer(outIdx)!!
                        val chunk = ByteArray(bufInfo.size)
                        outBuf.position(bufInfo.offset)
                        outBuf.get(chunk)
                        pcmBuffers.add(chunk)
                    }
                    if (bufInfo.flags and android.media.MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) eos = true
                    codec.releaseOutputBuffer(outIdx, false)
                    outIdx = codec.dequeueOutputBuffer(bufInfo, 0)
                }
            }

            codec.stop(); codec.release(); extractor.release(); tempFile.delete()
            val totalBytes = pcmBuffers.sumOf { it.size }
            val pcmBytes = ByteArray(totalBytes)
            var off = 0
            for (chunk in pcmBuffers) {
                System.arraycopy(chunk, 0, pcmBytes, off, chunk.size)
                off += chunk.size
            }
            val shortBuf = java.nio.ByteBuffer.wrap(pcmBytes).order(java.nio.ByteOrder.LITTLE_ENDIAN).asShortBuffer()
            val pcm = ShortArray(shortBuf.remaining())
            shortBuf.get(pcm)

            val compressData = when (method) {
                AudioMethodMode.GRAVITATIONAL -> qtsqEngine.compressAudio(pcm, sampleRate, channels, password.ifEmpty { null })
                AudioMethodMode.PHOTON_HQ -> qtsqEngine.compressAudioHQ(pcm, sampleRate, channels, password.ifEmpty { null })
            } ?: return null

            val outFile = File(context.filesDir, "converted_${System.currentTimeMillis()}.qtsq")
            outFile.writeBytes(compressData)

            return CompressionResult(
                outputFile = outFile,
                originalSize = originalSize,
                compressedSize = compressData.size.toLong(),
                fileType = "audio",
                qualityMode = when (method) {
                    AudioMethodMode.GRAVITATIONAL -> "Gravitational"
                    AudioMethodMode.PHOTON_HQ -> "Photon HQ"
                }
            )
        } else {
            extractor.release(); tempFile.delete()
            val compressData = qtsqEngine.compressBytes(bytes, "raw", password.ifEmpty { null }) ?: return null
            val outFile = File(context.filesDir, "converted_${System.currentTimeMillis()}.qtsq")
            outFile.writeBytes(compressData)
            return CompressionResult(outFile, originalSize, compressData.size.toLong(), "audio", qualityMode = "Raw")
        }
    } catch (e: Exception) {
        Log.e("ConvertScreen", "Audio decode failed", e)
        val compressData = qtsqEngine.compressBytes(bytes, "raw", password.ifEmpty { null }) ?: return null
        val outFile = File(context.filesDir, "converted_${System.currentTimeMillis()}.qtsq")
        outFile.writeBytes(compressData)
        return CompressionResult(outFile, originalSize, compressData.size.toLong(), "audio", qualityMode = "Raw (fallback)")
    }
}
