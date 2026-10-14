package com.haruhito.qtsq.screens

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.net.Uri
import android.util.Log
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.camera.core.*
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.core.UseCase
import androidx.camera.view.PreviewView
import androidx.camera.video.FileOutputOptions
import androidx.camera.video.Quality
import androidx.camera.video.FallbackStrategy
import androidx.camera.video.QualitySelector
import androidx.camera.video.Recorder
import androidx.camera.video.PendingRecording
import androidx.camera.video.Recording
import androidx.camera.video.VideoCapture
import androidx.camera.video.VideoRecordEvent
import androidx.compose.material.icons.rounded.Movie
import com.haruhito.qtsq.VideoConverter
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.CameraAlt
import androidx.compose.material.icons.rounded.Close
import androidx.compose.material.icons.rounded.Check
import androidx.compose.material.icons.rounded.Delete
import androidx.compose.material.icons.rounded.FlashOn
import androidx.compose.material.icons.rounded.FlashOff
import androidx.compose.material.icons.rounded.FlipCameraIos
import androidx.compose.material.icons.rounded.Lock
import androidx.compose.material.icons.rounded.LockOpen
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.ContextCompat
import com.haruhito.qtsq.QtsqEngine
import com.haruhito.qtsq.ui.theme.*
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.util.concurrent.Executors
import kotlin.math.sqrt

// Captured photo data
private data class CapturedPhoto(
    val rgbBytes: ByteArray,
    val width: Int,
    val height: Int,
    val bitmap: Bitmap
)

/**
 * Fixed capture target for the beta: 13 MP. Captures at other hardware
 * resolutions are scaled to this before encoding.
 */
private const val TARGET_W = 4096
private const val TARGET_H = 3072
private val TARGET_MP = TARGET_W.toLong() * TARGET_H.toLong() / 1_000_000f

enum class EncodingMode {
    Triangle, Splat, Pixel
}

/** Whether the shutter takes a still or records a clip. */
enum class CaptureType { PHOTO, VIDEO }

@Composable
fun CameraScreen(
    qtsqEngine: QtsqEngine,
    onClose: () -> Unit,
    onCaptured: (File) -> Unit
) {
    val context = LocalContext.current
    val lifecycleOwner = LocalLifecycleOwner.current
    val coroutineScope = rememberCoroutineScope()

    var imageCapture by remember { mutableStateOf<ImageCapture?>(null) }
    var isCapturing by remember { mutableStateOf(false) }
    var lensFacing by remember { mutableIntStateOf(CameraSelector.LENS_FACING_BACK) }
    var flashMode by remember { mutableIntStateOf(ImageCapture.FLASH_MODE_OFF) }
    var password by remember { mutableStateOf("") }
    var showPasswordDialog by remember { mutableStateOf(false) }
    var isLocked by remember { mutableStateOf(false) }
    var cameraControl by remember { mutableStateOf<androidx.camera.core.CameraControl?>(null) }
    
    var encodingMode by remember { mutableStateOf(EncodingMode.Triangle) }
    var modeMenuExpanded by remember { mutableStateOf(false) }

    // ── Video capture ──
    var captureType by remember { mutableStateOf(CaptureType.PHOTO) }
    var videoCapture by remember { mutableStateOf<VideoCapture<Recorder>?>(null) }
    var activeRecording by remember { mutableStateOf<Recording?>(null) }
    var recordingFile by remember { mutableStateOf<File?>(null) }
    var isRecording by remember { mutableStateOf(false) }
    var recordSeconds by remember { mutableIntStateOf(0) }
    var isProcessingVideo by remember { mutableStateOf(false) }
    var videoResolution by remember { mutableStateOf(VideoResolution.FHD) }
    var videoFramerate by remember { mutableStateOf(VideoFramerate.FPS_30) }
    var showVideoSettings by remember { mutableStateOf(false) }
    var capturedSummary by remember { mutableStateOf<String?>(null) }


    // ── Video recording ──────────────────────────────────────────────
    // CameraX records to a plain file; we then run it through the same
    // VideoConverter → native pixel encoder path used for imported clips, so
    // camera footage and imported footage produce identical .qtsq output.
    fun compressRecordedVideo(context: android.content.Context, file: File) {
        isProcessingVideo = true
        // Ask for the selected long edge, but never upscale: a 720p source
        // stays 720p even when the user picks 1080p.
        val resolutionForTranscode = VideoConverter.longEdgeForFile(
            context, Uri.fromFile(file), videoResolution
        )
        coroutineScope.launch {
            val err = withContext(Dispatchers.IO) {
                try {
                    // One streaming transcode: frames go straight from the
                    // decoder into the native encoder, so nothing large is
                    // ever held in the JVM heap.
                    // Budget the encode against the clip we just recorded so
                    // the rate controller has something to aim at, and start
                    // from a high quality ceiling it can trim if needed.
                    val sourceBytes = file.length()
                    val approxFrames = 300
                    val target = if (sourceBytes > 0) {
                        kotlin.math.max(256L, sourceBytes / approxFrames)
                    } else 0L
                    val res = VideoConverter.transcode(
                        qtsqEngine, context, Uri.fromFile(file),
                        targetPayload = target,
                        maxFrames = 300,
                        maxDimension = resolutionForTranscode,
                        quality = 0.80f
                    )
                    file.delete()
                    Log.i(
                        "CameraScreen",
                        "recording encoded: ${res.width}x${res.height} fps=${res.fps} " +
                            "frames=${res.frameCount} audio=${res.audioSamples} " +
                            "bytes=${res.qtsqBytes.size}"
                    )
                    capturedSummary =
                        "${res.width}x${res.height} • ${res.fps} fps • ${res.frameCount} frames" +
                            if (res.audioSamples > 0) " • audio" else " • no audio"
                    val out = File(context.filesDir, "video_${System.currentTimeMillis()}.qtsq")
                    out.writeBytes(res.qtsqBytes)
                    withContext(Dispatchers.Main) {
                        isProcessingVideo = false
                        onCaptured(out)
                    }
                    null
                } catch (e: OutOfMemoryError) {
                    Log.e("CameraScreen", "out of memory transcoding", e)
                    "Out of memory — try a shorter recording."
                } catch (e: Exception) {
                    Log.e("CameraScreen", "Video compression failed", e)
                    "Video compression failed: ${e.message}"
                }
            }
            isProcessingVideo = false
            if (err != null) {
                android.widget.Toast.makeText(context, err, android.widget.Toast.LENGTH_LONG).show()
            }
        }
    }

    fun startRecording(context: android.content.Context) {
        val vc = videoCapture
        if (vc == null) {
            android.widget.Toast.makeText(context, "Camera not ready", android.widget.Toast.LENGTH_SHORT).show()
            return
        }
        val outFile = File(context.cacheDir, "qtsq_rec_${System.currentTimeMillis()}.mp4")
        // prepareRecording / withAudioEnabled / start all throw on unsupported
        // combinations, so the whole sequence is guarded.
        var pending: PendingRecording
        try {
            pending = vc.output.prepareRecording(
                context, FileOutputOptions.Builder(outFile).build()
            )
            // Audio is optional: without the mic permission we still get video.
            if (ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO)
                == PackageManager.PERMISSION_GRANTED
            ) {
                pending = pending.withAudioEnabled()
            }
        } catch (t: Throwable) {
            Log.e("CameraScreen", "prepareRecording failed", t)
            runCatching { outFile.delete() }
            android.widget.Toast.makeText(context, "Cannot start recording: ${t.message}",
                android.widget.Toast.LENGTH_LONG).show()
            return
        }
        recordingFile = outFile
        recordSeconds = 0
        isRecording = true
        activeRecording = pending.start(ContextCompat.getMainExecutor(context)) { event ->
          try {
            if (event is VideoRecordEvent.Finalize) {
                activeRecording = null
                isRecording = false
                val file = recordingFile
                recordingFile = null
                val err = event.hasError()
                if (err || file == null || !file.exists() || file.length() == 0L) {
                    isProcessingVideo = false
                    android.widget.Toast.makeText(
                        context,
                        if (err) "Recording failed (code ${event.error})" else "Recording produced no data",
                        android.widget.Toast.LENGTH_LONG
                    ).show()
                    return@start
                }
                compressRecordedVideo(context, file)
            }
          } catch (t: Throwable) {
            activeRecording = null
            isRecording = false
            isProcessingVideo = false
            Log.e("CameraScreen", "record finalize failed", t)
            android.widget.Toast.makeText(context, "Recording failed: ${t.message}",
                android.widget.Toast.LENGTH_LONG).show()
          }
        }
    }

    fun stopRecording() {
        activeRecording?.stop()
        activeRecording = null
    }

    // Preview state
    var capturedPhoto by remember { mutableStateOf<CapturedPhoto?>(null) }
    var isSaving by remember { mutableStateOf(false) }
    var saveStatus by remember { mutableStateOf("") }
    var saveErrorDiag by remember { mutableStateOf<String?>(null) }

    // Permissions
    var hasCameraPermission by remember {
        mutableStateOf(
            ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
        )
    }
    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted -> hasCameraPermission = granted }
    LaunchedEffect(Unit) {
        if (!hasCameraPermission) permissionLauncher.launch(Manifest.permission.CAMERA)
    }

    val cameraExecutor = remember { Executors.newSingleThreadExecutor() }
    DisposableEffect(Unit) { onDispose { cameraExecutor.shutdown() } }

    if (!hasCameraPermission) {
        Box(Modifier.fillMaxSize().background(Color.Black), contentAlignment = Alignment.Center) {
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Text("Camera permission is required", color = Color.White)
                Spacer(Modifier.height(16.dp))
                Button(onClick = { permissionLauncher.launch(Manifest.permission.CAMERA) }) {
                    Text("Grant Permission")
                }
                Spacer(Modifier.height(16.dp))
                TextButton(onClick = onClose) { Text("Go Back", color = Color.White) }
            }
        }
        return
    }

    // =========== PREVIEW MODE ===========
    if (capturedPhoto != null) {
        val photo = capturedPhoto!!
        Box(Modifier.fillMaxSize().background(Color.Black)) {
            Image(
                bitmap = photo.bitmap.asImageBitmap(),
                contentDescription = "Captured photo",
                contentScale = ContentScale.Fit,
                modifier = Modifier.fillMaxSize()
            )

            Row(
                modifier = Modifier
                    .align(Alignment.TopCenter).fillMaxWidth()
                    .statusBarsPadding().padding(horizontal = 24.dp, vertical = 16.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text("Preview", style = MaterialTheme.typography.titleMedium, color = Color.White)
                Text(
                    "${photo.width} × ${photo.height}",
                    style = MaterialTheme.typography.bodyMedium,
                    color = Color.White.copy(alpha = 0.7f)
                )
            }

            Row(
                modifier = Modifier
                    .align(Alignment.BottomCenter).fillMaxWidth()
                    .navigationBarsPadding().padding(bottom = 32.dp, start = 48.dp, end = 48.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    IconButton(
                        onClick = { capturedPhoto?.bitmap?.recycle(); capturedPhoto = null },
                        enabled = !isSaving,
                        modifier = Modifier.size(64.dp)
                            .background(Color(0xFFE74C3C).copy(alpha = 0.9f), CircleShape)
                    ) {
                        Icon(Icons.Rounded.Delete, "Discard", tint = Color.White, modifier = Modifier.size(28.dp))
                    }
                    Spacer(Modifier.height(8.dp))
                    Text("Discard", color = Color.White, fontSize = 12.sp)
                }

                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    IconButton(
                        onClick = {
                            isSaving = true
                            saveStatus = "Starting…"
                            val startedAt = android.os.SystemClock.elapsedRealtime()
                            // Live progress: stage text + elapsed seconds so long
                            // encodes (triangle/splat) never look frozen.
                            val ticker = coroutineScope.launch {
                                while (true) {
                                    kotlinx.coroutines.delay(1000)
                                    if (isSaving) {
                                        val s = (android.os.SystemClock.elapsedRealtime() - startedAt) / 1000
                                        saveStatus = "$saveStatus".let { it.substringBefore(" (") } + " (${s}s)"
                                    }
                                }
                            }
                            coroutineScope.launch {
                                val file = withContext(Dispatchers.IO) {
                                    compressAndSave(context, photo.rgbBytes, photo.bitmap, photo.width, photo.height,
                                        3, qtsqEngine, password.ifEmpty { null }, encodingMode,
                                        onProgress = { saveStatus = it })
                                }
                                ticker.cancel()
                                isSaving = false
                                saveStatus = ""
                                if (file != null) {
                                    // Phase 7: confirm which container codec was used.
                                    android.widget.Toast.makeText(context, "Saved via ${QtsqEngine.codecName(context)} codec", android.widget.Toast.LENGTH_SHORT).show()
                                    capturedPhoto?.bitmap?.recycle(); capturedPhoto = null
                                    onCaptured(file)
                                } else {
                                    // Full on-device diagnostic in a dismissible
                                    // dialog — names the exact failing stage.
                                    val diag = try { qtsqEngine.nativeSelfTest() } catch (e: Exception) { "selftest unavailable: ${e.message}" }
                                    Log.e("CameraScreen", "Save failed. build=${QtsqEngine.BUILD_TAG} diag=$diag")
                                    saveErrorDiag = "Save failed — build ${QtsqEngine.BUILD_TAG}\n$diag"
                                }
                            }
                        },
                        enabled = !isSaving,
                        modifier = Modifier.size(64.dp)
                            .background(Color(0xFF10B981).copy(alpha = 0.9f), CircleShape)
                    ) {
                        if (isSaving) CircularProgressIndicator(color = Color.White, modifier = Modifier.size(28.dp), strokeWidth = 3.dp)
                        else Icon(Icons.Rounded.Check, "Save", tint = Color.White, modifier = Modifier.size(28.dp))
                    }
                    Spacer(Modifier.height(8.dp))
                    Text(
                        if (isSaving) saveStatus else "Save · ${encodingMode.name}",
                        color = Color.White, fontSize = 12.sp
                    )
                }

            // ── Save-failure diagnostic dialog ──
            saveErrorDiag?.let { diag ->
                androidx.compose.material3.AlertDialog(
                    onDismissRequest = { saveErrorDiag = null },
                    confirmButton = {
                        TextButton(onClick = { saveErrorDiag = null }) { Text("OK") }
                    },
                    title = { Text("Compression failed", style = MaterialTheme.typography.titleMedium) },
                    text = {
                        Column {
                            Text(diag, style = MaterialTheme.typography.bodySmall)
                            Spacer(Modifier.height(8.dp))
                            Text("Send a screenshot of this dialog to support — it names the exact failing stage.",
                                 style = MaterialTheme.typography.labelSmall,
                                 color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                    }
                )
            }
            }
        }
        return
    }

    // =========== LIVE CAMERA MODE ===========
    Box(Modifier.fillMaxSize().background(Color.Black)) {
        // Camera preview + capture
        key(lensFacing) {
            // AndroidView's factory runs once, so keying on the capture mode
            // tears the preview down and rebinds when switching photo/video.
            key(captureType, videoResolution) {
            AndroidView(
                factory = { ctx ->
                    val previewView = PreviewView(ctx)
                    val cameraProviderFuture = ProcessCameraProvider.getInstance(ctx)

                    cameraProviderFuture.addListener({
                        val cameraProvider = cameraProviderFuture.get()
                        val preview = Preview.Builder().build().also {
                            it.setSurfaceProvider(previewView.surfaceProvider)
                        }

                        val cameraSelector = CameraSelector.Builder()
                            .requireLensFacing(lensFacing).build()

                        // Bind either a still capture or a recorder — the two
                        // are mutually exclusive here so the shutter has one
                        // unambiguous meaning.
                        //
                        // Building the recorder can throw (QualitySelector.from
                        // rejects a quality the device cannot produce, and a
                        // Recorder already bound to a live VideoCapture throws
                        // on build), so construction and binding share one
                        // guard. This runs inside a provider listener, where an
                        // uncaught exception crashes the process.
                        try {
                            val useCase: UseCase = if (captureType == CaptureType.VIDEO) {
                                val recorder = Recorder.Builder()
                                    .setQualitySelector(qualitySelectorFor(videoResolution))
                                    .build()
                                val vc = VideoCapture.withOutput(recorder)
                                videoCapture = vc
                                imageCapture = null
                                vc
                            } else {
                                // Capture at maximum hardware quality
                                val capture = ImageCapture.Builder()
                                    .setCaptureMode(ImageCapture.CAPTURE_MODE_MAXIMIZE_QUALITY)
                                    .setFlashMode(flashMode)
                                    .build()
                                imageCapture = capture
                                videoCapture = null
                                capture
                            }

                            cameraProvider.unbindAll()
                            val camera = cameraProvider.bindToLifecycle(
                                lifecycleOwner, cameraSelector, preview, useCase
                            )
                            cameraControl = camera.cameraControl
                            cameraControl?.enableTorch(flashMode == ImageCapture.FLASH_MODE_ON)
                        } catch (exc: Throwable) {
                            Log.e("CameraScreen", "Camera binding failed", exc)
                            videoCapture = null
                            imageCapture = null
                            android.widget.Toast.makeText(
                                ctx, "Camera unavailable: ${exc.message}",
                                android.widget.Toast.LENGTH_LONG
                            ).show()
                        }
                    }, ContextCompat.getMainExecutor(ctx))

                    previewView
                },
                modifier = Modifier.fillMaxSize()
            )
            }
        }

        // Elapsed recording timer
        LaunchedEffect(isRecording) {
            recordSeconds = 0
            while (isRecording) {
                kotlinx.coroutines.delay(1000)
                if (isRecording) recordSeconds += 1
            }
        }

        // ── Video resolution / framerate sheet ──
    if (showVideoSettings) {
        AlertDialog(
            onDismissRequest = { showVideoSettings = false },
            title = {
                Text(
                    "Video Settings",
                    style = Typography.titleMedium.copy(fontWeight = FontWeight.Bold),
                    color = TextPrimary
                )
            },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    Text("Resolution", color = TextSecondary, fontSize = 12.sp, fontWeight = FontWeight.Bold)
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        VideoResolution.entries.forEach { r ->
                            FilterPill(
                                label = r.label,
                                selected = videoResolution == r,
                                enabled = !isRecording && !isProcessingVideo
                            ) { videoResolution = r }
                        }
                    }
                    Spacer(Modifier.height(4.dp))
                    Text("Frame rate", color = TextSecondary, fontSize = 12.sp, fontWeight = FontWeight.Bold)
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        VideoFramerate.entries.forEach { f ->
                            FilterPill(
                                label = f.label,
                                selected = videoFramerate == f,
                                enabled = !isRecording && !isProcessingVideo
                            ) { videoFramerate = f }
                        }
                    }
                    Spacer(Modifier.height(4.dp))
                    Text(
                        "The clip is encoded at the selected resolution and never " +
                            "upscaled above what the camera recorded. Frame rate is " +
                            "what the device supports for that resolution — the " +
                            "captured rate is reported after recording.",
                        style = Typography.bodySmall,
                        color = TextTertiary
                    )
                }
            },
            confirmButton = {
                TextButton(onClick = { showVideoSettings = false }) {
                    Text("Done", color = AccentPrimary)
                }
            }
        )
    }

    // What the last recording actually became.
    capturedSummary?.let { summary ->
        Row(
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .padding(bottom = 190.dp)
                .clip(RoundedCornerShape(20.dp))
                .background(Color.Black.copy(alpha = 0.7f))
                .padding(horizontal = 12.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Icon(Icons.Rounded.Movie, null, tint = AccentPrimary, modifier = Modifier.size(16.dp))
            Spacer(Modifier.width(6.dp))
            Text(summary, color = Color.White, fontSize = 12.sp)
        }
    }

    // Top Controls — split into two rows. Cramming the capture toggle, the
    // setting chips, lock and flash into one Row overflowed the width and
    // pushed the trailing buttons off-screen.
    Column(
        modifier = Modifier
            .align(Alignment.TopCenter)
            .fillMaxWidth()
            .statusBarsPadding()
            .padding(horizontal = 16.dp, vertical = 8.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp)
    ) {
        // Row 1: close on the left, lock + flash on the right.
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            CircleControl(
                icon = Icons.Rounded.Close,
                contentDescription = "Close",
                onClick = onClose
            )

            Row(
                horizontalArrangement = Arrangement.spacedBy(10.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                CircleControl(
                    icon = if (isLocked) Icons.Rounded.Lock else Icons.Rounded.LockOpen,
                    contentDescription = "Lock",
                    tint = if (isLocked) AccentPrimary else Color.White,
                    onClick = { showPasswordDialog = true }
                )
                CircleControl(
                    icon = if (flashMode == ImageCapture.FLASH_MODE_ON) Icons.Rounded.FlashOn
                           else Icons.Rounded.FlashOff,
                    contentDescription = "Flash",
                    tint = if (flashMode == ImageCapture.FLASH_MODE_ON) AccentPrimary else Color.White,
                    onClick = {
                        val newMode = if (flashMode == ImageCapture.FLASH_MODE_OFF) {
                            ImageCapture.FLASH_MODE_ON
                        } else ImageCapture.FLASH_MODE_OFF
                        flashMode = newMode
                        imageCapture?.flashMode = newMode
                        cameraControl?.enableTorch(newMode == ImageCapture.FLASH_MODE_ON)
                    }
                )
            }
        }

        // Row 2: capture mode + settings, centred and scrollable so a long set
        // of chips scrolls instead of overlapping the edges.
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .horizontalScroll(rememberScrollState()),
            horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.CenterHorizontally),
            verticalAlignment = Alignment.CenterVertically
        ) {
            // PHOTO / VIDEO segmented control.
            Row(
                modifier = Modifier
                    .height(40.dp)
                    .clip(RoundedCornerShape(20.dp))
                    .background(Color.Black.copy(alpha = 0.55f))
                    .padding(3.dp),
                horizontalArrangement = Arrangement.spacedBy(2.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                CaptureType.entries.forEach { type ->
                    val selected = captureType == type
                    Box(
                        modifier = Modifier
                            .height(34.dp)
                            .clip(RoundedCornerShape(17.dp))
                            .background(if (selected) AccentPrimary else Color.Transparent)
                            .clickable(enabled = !isRecording && !isProcessingVideo) {
                                captureType = type
                            }
                            .padding(horizontal = 16.dp),
                        contentAlignment = Alignment.Center
                    ) {
                        Text(
                            text = if (type == CaptureType.PHOTO) "PHOTO" else "VIDEO",
                            color = if (selected) Color.White else Color.White.copy(alpha = 0.75f),
                            fontWeight = FontWeight.SemiBold,
                            fontSize = 12.sp,
                            textAlign = TextAlign.Center
                        )
                    }
                }
            }

            if (captureType == CaptureType.VIDEO) {
                CameraChip(
                    label = videoResolution.label,
                    onClick = { showVideoSettings = true },
                    enabled = !isRecording && !isProcessingVideo
                )
                CameraChip(
                    label = videoFramerate.label,
                    onClick = { showVideoSettings = true },
                    enabled = !isRecording && !isProcessingVideo
                )
            } else {
                CameraChip(label = "${TARGET_MP.toInt()} MP")
            }

            // Encoding mode is image-only (Triangle/Splat have no video
            // equivalent), so it is hidden in VIDEO mode and forced to Pixel.
            if (captureType == CaptureType.PHOTO) Box {
                CameraChip(
                    label = encodingMode.name,
                    onClick = { modeMenuExpanded = true }
                )
                DropdownMenu(
                    expanded = modeMenuExpanded,
                    onDismissRequest = { modeMenuExpanded = false }
                ) {
                    EncodingMode.entries.forEach { mode ->
                        DropdownMenuItem(
                            text = { Text(mode.name) },
                            onClick = {
                                encodingMode = mode
                                modeMenuExpanded = false
                            }
                        )
                    }
                }
            }
        }

        // Recording / processing banner. It lives inside this column so it
        // flows below the controls instead of overlapping them, whatever
        // height the control rows end up being.
        if (isRecording || isProcessingVideo) {
            Row(
                modifier = Modifier
                    .align(Alignment.CenterHorizontally)
                    .clip(RoundedCornerShape(20.dp))
                    .background(Color.Black.copy(alpha = 0.7f))
                    .padding(horizontal = 14.dp, vertical = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                if (isRecording) {
                    Box(Modifier.size(10.dp).background(Color(0xFFE53935), CircleShape))
                    Text(
                        "REC  ${recordSeconds / 60}:" +
                            (recordSeconds % 60).toString().padStart(2, '0'),
                        color = Color.White,
                        fontWeight = FontWeight.Bold,
                        fontSize = 13.sp
                    )
                } else {
                    androidx.compose.material3.CircularProgressIndicator(
                        modifier = Modifier.size(14.dp),
                        color = AccentPrimary,
                        strokeWidth = 2.dp
                    )
                    Text("Compressing to .qtsq…", color = Color.White, fontSize = 13.sp)
                }
            }
        }
    }

        // Bottom: Capture + Swap
        Row(
            modifier = Modifier.align(Alignment.BottomCenter).fillMaxWidth()
                .navigationBarsPadding().padding(bottom = 32.dp, start = 32.dp, end = 32.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Spacer(Modifier.size(48.dp))

            // Capture button
            Box(
                modifier = Modifier.size(80.dp)
                    .background(Color.White.copy(alpha = 0.3f), CircleShape)
                    .padding(8.dp).clip(CircleShape)
                    .background(
                        when {
                            isRecording -> Color(0xFFE53935)
                            isCapturing || isProcessingVideo -> Color.Gray
                            else -> SurfaceWhite
                        }
                    )
                    .clickable(enabled = !isCapturing && !isProcessingVideo) {

                    if (captureType == CaptureType.VIDEO) {
                        if (isRecording) stopRecording() else startRecording(context)
                        return@clickable
                    }

                    val currentCapture = imageCapture
                        if (currentCapture == null) {
                            android.widget.Toast.makeText(context, "Camera not ready",
                                android.widget.Toast.LENGTH_SHORT).show()
                            return@clickable
                        }
                        isCapturing = true

                        currentCapture.takePicture(cameraExecutor,
                            object : ImageCapture.OnImageCapturedCallback() {
                                override fun onCaptureSuccess(image: ImageProxy) {
                                    try {
                                        val rawBitmapUnrotated = image.toBitmap()
                                        val rotation = image.imageInfo.rotationDegrees
                                        
                                        val rawBitmap = if (rotation != 0) {
                                            val matrix = android.graphics.Matrix()
                                            matrix.postRotate(rotation.toFloat())
                                            val rotated = Bitmap.createBitmap(rawBitmapUnrotated, 0, 0, rawBitmapUnrotated.width, rawBitmapUnrotated.height, matrix, true)
                                            if (rotated != rawBitmapUnrotated) rawBitmapUnrotated.recycle()
                                            rotated
                                        } else {
                                            rawBitmapUnrotated
                                        }
                                        image.close()

                                        val capturedW = rawBitmap.width
                                        val capturedH = rawBitmap.height
                                        val capturedMp = Math.round((capturedW.toLong() * capturedH.toLong()) / 1_000_000f)

                                        Log.i("CameraScreen", "Captured: ${capturedW}x${capturedH} (${capturedMp} MP)")

                                        // Get the fixed beta target size
                                        var targetW = TARGET_W
                                        var targetH = TARGET_H

                                        // Ensure scaling target orientation matches the true captured hardware orientation
                                        if ((capturedW > capturedH && targetW < targetH) || 
                                            (capturedW < capturedH && targetW > targetH)) {
                                            val temp = targetW
                                            targetW = targetH
                                            targetH = temp
                                        }

                                        // Scale to match selected resolution (up or down)
                                        val bitmap = if (targetW != capturedW || targetH != capturedH) {
                                            val direction = if (targetW > capturedW) "Upscaling" else "Downscaling"
                                            Log.i("CameraScreen", "$direction: ${capturedW}x${capturedH} → ${targetW}x${targetH}")
                                            val scaled = Bitmap.createScaledBitmap(rawBitmap, targetW, targetH, true)
                                            if (scaled != rawBitmap) rawBitmap.recycle()
                                            scaled
                                        } else {
                                            rawBitmap
                                        }

                                        val w = bitmap.width
                                        val h = bitmap.height
                                        Log.i("CameraScreen", "Final output: ${w}x${h} (${Math.round((w.toLong() * h.toLong()) / 1_000_000f)} MP)")

                                        // Extract RGB (native ARGB→RGB conversion)
                                        val pixels = IntArray(w * h)
                                        bitmap.getPixels(pixels, 0, w, 0, 0, w, h)

                                        val rgbBytes = qtsqEngine.argbToRgb(pixels) ?: run {
                                            throw java.io.IOException("ARGB→RGB conversion failed")
                                        }

                                        val photo = CapturedPhoto(rgbBytes, w, h, bitmap)
                                        android.os.Handler(android.os.Looper.getMainLooper()).post {
                                            capturedPhoto = photo
                                            isCapturing = false
                                        }
                                    } catch (e: Exception) {
                                        Log.e("CameraScreen", "Processing failed: ${e.message}", e)
                                        try { image.close() } catch (_: Exception) {}
                                        android.os.Handler(android.os.Looper.getMainLooper()).post {
                                            isCapturing = false
                                            android.widget.Toast.makeText(context,
                                                "Capture failed: ${e.message}",
                                                android.widget.Toast.LENGTH_LONG).show()
                                        }
                                    }
                                }

                                override fun onError(exception: ImageCaptureException) {
                                    Log.e("CameraScreen", "Capture error: ${exception.message}", exception)
                                    android.os.Handler(android.os.Looper.getMainLooper()).post {
                                        isCapturing = false
                                        android.widget.Toast.makeText(context,
                                            "Capture error: ${exception.message}",
                                            android.widget.Toast.LENGTH_LONG).show()
                                    }
                                }
                            }
                        )
                    },
                contentAlignment = Alignment.Center
            ) {
                if (isCapturing) CircularProgressIndicator(color = AccentPrimary, modifier = Modifier.size(32.dp))
                else Icon(Icons.Rounded.CameraAlt, "Capture", tint = AccentPrimary, modifier = Modifier.size(36.dp))
            }

            // Camera swap
            IconButton(
                onClick = { lensFacing = if (lensFacing == CameraSelector.LENS_FACING_BACK)
                    CameraSelector.LENS_FACING_FRONT else CameraSelector.LENS_FACING_BACK },
                modifier = Modifier.size(48.dp).background(Color.Black.copy(alpha = 0.5f), CircleShape)
            ) { Icon(Icons.Rounded.FlipCameraIos, "Swap Camera", tint = Color.White) }
        }

        // Password Dialog
        if (showPasswordDialog) {
            AlertDialog(
                onDismissRequest = { showPasswordDialog = false },
                title = { Text("Session Password") },
                text = {
                    OutlinedTextField(value = password, onValueChange = { password = it },
                        label = { Text("Password (Optional)") }, singleLine = true)
                },
                confirmButton = {
                    TextButton(onClick = { isLocked = password.isNotEmpty(); showPasswordDialog = false }) {
                        Text("Save") }
                },
                dismissButton = {
                    TextButton(onClick = { password = ""; isLocked = false; showPasswordDialog = false }) {
                        Text("Clear") }
                }
            )
        }
    }
}

/** Compress raw RGB pixels to Lossless .qtsq and save.
 *
 * Performance notes (measured at 13MP):
 *  - Triangle: heavy but acceptable on-device.
 *  - Splat: refine cost scales with frame area — downscaled to ≤4 MP here
 *    (~3× faster, better coverage per splat, same visual quality).
 *  - Pixel: JPEG passthrough, near-instant in C; the only real cost is
 *    the Bitmap→JPEG encode.
 * The engine is fail-closed on encryption, so the device keystore is
 * verified BEFORE compressing — otherwise every save silently dead-ends
 * and surfaces as "Compression failed". */
private fun compressAndSave(
    context: Context, rgbBytes: ByteArray, bitmap: Bitmap, width: Int, height: Int,
    channels: Int, qtsqEngine: QtsqEngine, password: String?,
    mode: EncodingMode,
    onProgress: (String) -> Unit = {}
): File? {
    return try {
        // ── 0. Keystore must be ready or nothing can ever be saved ──
        onProgress("Preparing encryption…")
        if (!qtsqEngine.ensureKeystoreReady(context)) {
            Log.e("CameraScreen", "Keystore unavailable and no password set — refusing to save")
            return null
        }

        // ── 1. Mode-aware working resolution ──
        var workBitmap = bitmap
        var w = width
        var h = height
        if (mode == EncodingMode.Splat) {
            val mp = width.toLong() * height.toLong()
            val maxMp = 4_000_000L
            if (mp > maxMp) {
                val scale = kotlin.math.sqrt(maxMp.toDouble() / mp)
                val sw = maxOf(((width * scale).toInt() and 0x7FFFFFFC.toInt()), 16)
                val sh = maxOf(((height * scale).toInt() and 0x7FFFFFFC.toInt()), 16)
                Log.i("CameraScreen", "Splat mode: downscaling ${width}x${height} → ${sw}x${sh} (refine is O(area))")
                onProgress("Resizing…")
                workBitmap = Bitmap.createScaledBitmap(bitmap, sw, sh, true)
                w = sw; h = sh
            }
        }

        try {
            Log.i("CameraScreen", "Compressing [Mode: ${mode.name}] ${w}x${h} (${channels}ch)")

            // ── 2. RGB at working resolution ──
            // Pixel mode is JPEG passthrough and never touches raw pixels.
            // When the bitmap was not rescaled, the capture-time rgbBytes ARE
            // exactly these pixels — re-reading them was a full second
            // getPixels + conversion of the whole frame. Only a downscaled
            // Splat working bitmap needs a fresh extraction (native convert).
            var workRgb: ByteArray = rgbBytes
            if (mode != EncodingMode.Pixel && workBitmap !== bitmap) {
                onProgress("Reading pixels…")
                val pixels = IntArray(w * h)
                workBitmap.getPixels(pixels, 0, w, 0, 0, w, h)
                workRgb = qtsqEngine.argbToRgb(pixels) ?: return null
            }

            // ── 3. Encode ──
            var qtsqData: ByteArray? = null
            when (mode) {
                EncodingMode.Triangle -> {
                    onProgress("Triangle mesh…")
                    qtsqData = qtsqEngine.compressImageLosslessTriangle(workRgb, w, h, channels, password)
                }
                EncodingMode.Splat -> {
                    onProgress("Gaussian splats…")
                    qtsqData = qtsqEngine.compressSplatLossless(workRgb, w, h, channels, password)
                }
                EncodingMode.Pixel -> {
                    onProgress("Pixel encode…")
                    val outStream = java.io.ByteArrayOutputStream()
                    bitmap.compress(Bitmap.CompressFormat.JPEG, 95, outStream)
                    val jpegBytes = outStream.toByteArray()
                    Log.i("CameraScreen", "Pixel Mode: JPEG pre-compressed size: ${jpegBytes.size} bytes")
                    qtsqData = qtsqEngine.compressPixelJpeg(jpegBytes, password)
                }
            }

            // ── 4. Fallback: cheap JPEG wrap instead of multi-second raw zlib ──
            if (qtsqData == null) {
                Log.w("CameraScreen", "Primary ${mode.name} compress failed; falling back to Pixel-JPEG wrap")
                onProgress("Retrying (pixel)…")
                val jpg = java.io.ByteArrayOutputStream()
                if (workBitmap.compress(Bitmap.CompressFormat.JPEG, 90, jpg)) {
                    qtsqData = qtsqEngine.compressPixelJpeg(jpg.toByteArray(), password)
                }
            }

            // ── 5. Write ──
            if (qtsqData != null) {
                onProgress("Writing file…")
                val file = File(context.filesDir, "QTSQ_${System.currentTimeMillis()}.qtsq")
                file.writeBytes(qtsqData)
                Log.i("CameraScreen", "Saved: ${file.name} (${qtsqData.size} bytes)")
                file
            } else null
        } finally {
            if (workBitmap !== bitmap) workBitmap.recycle()
        }
    } catch (e: Exception) {
        Log.e("CameraScreen", "Save failed: ${e.message}", e)
        null
    }
}


/** Circular icon button used across the camera chrome. */
@Composable
private fun CircleControl(
    icon: androidx.compose.ui.graphics.vector.ImageVector,
    contentDescription: String,
    tint: Color = Color.White,
    onClick: () -> Unit
) {
    androidx.compose.material3.IconButton(
        onClick = onClick,
        modifier = Modifier
            .size(44.dp)
            .background(Color.Black.copy(alpha = 0.55f), CircleShape)
    ) {
        Icon(icon, contentDescription, tint = tint, modifier = Modifier.size(22.dp))
    }
}

/** Pill-shaped setting chip with vertically and horizontally centred text. */
@Composable
private fun CameraChip(
    label: String,
    onClick: (() -> Unit)? = null,
    enabled: Boolean = true
) {
    val shape = RoundedCornerShape(20.dp)
    val bg = Color.Black.copy(alpha = 0.55f)
    Box(
        modifier = Modifier
            .height(40.dp)
            .clip(shape)
            .background(bg)
            .then(
                if (onClick != null) {
                    Modifier.clickable(enabled = enabled, onClick = onClick)
                } else Modifier
            )
            .padding(horizontal = 16.dp),
        contentAlignment = Alignment.Center
    ) {
        Text(
            text = label,
            color = AccentPrimary,
            fontWeight = FontWeight.SemiBold,
            fontSize = 13.sp,
            textAlign = TextAlign.Center,
            maxLines = 1
        )
    }
}
