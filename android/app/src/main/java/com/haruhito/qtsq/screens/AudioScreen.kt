package com.haruhito.qtsq.screens

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.AudioTrack
import android.media.MediaRecorder
import android.util.Log
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.core.*
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
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
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import com.haruhito.qtsq.QtsqEngine
import com.haruhito.qtsq.ui.theme.*
import kotlinx.coroutines.*
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

// ---------------- Data Models ----------------

private data class CapturedAudio(
    val pcmShorts: ShortArray,
    val sampleRate: Int,
    val channels: Int,     // 1=mono, 2=stereo
    val bitDepth: Int,     // 16 or 8
    val durationSeconds: Int
)

private data class AudioQuality(
    val label: String,
    val sampleRate: Int,
    val bitDepth: Int
)

// Available quality options
private val QUALITY_OPTIONS = listOf(
    AudioQuality("8 kHz / 16-bit",   8000,  16),
    AudioQuality("16 kHz / 16-bit",  16000, 16),
    AudioQuality("22 kHz / 16-bit",  22050, 16),
    AudioQuality("44.1 kHz / 16-bit", 44100, 16),
    AudioQuality("48 kHz / 16-bit",  48000, 16),
    AudioQuality("96 kHz / 16-bit",  96000, 16)
)

// ============================================================
//  MAIN COMPOSABLE
// ============================================================

@Composable
fun AudioScreen(
    qtsqEngine: QtsqEngine,
    onClose: () -> Unit,
    onRecorded: (File) -> Unit
) {
    val context = LocalContext.current
    val coroutineScope = rememberCoroutineScope()

    // Recording state
    var isRecording by remember { mutableStateOf(false) }
    var statusMessage by remember { mutableStateOf<String?>(null) }
    var recordingTime by remember { mutableStateOf(0L) }
    var recordingJobRef by remember { mutableStateOf<Job?>(null) }
    val timeFormatted = String.format("%02d:%02d", recordingTime / 60, recordingTime % 60)

    // Quality selector
    var selectedQualityIndex by remember { mutableIntStateOf(3) }  // default: 44.1 kHz
    var showQualitySelector by remember { mutableStateOf(false) }

    // Password
    var password by remember { mutableStateOf("") }
    var showPasswordDialog by remember { mutableStateOf(false) }
    var isLocked by remember { mutableStateOf(false) }

    // Preview state
    var capturedAudio by remember { mutableStateOf<CapturedAudio?>(null) }
    var isSaving by remember { mutableStateOf(false) }

    // Playback
    var audioTrack by remember { mutableStateOf<AudioTrack?>(null) }
    var isPlaying by remember { mutableStateOf(false) }

    // Permission
    var hasPermission by remember {
        mutableStateOf(
            ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED
        )
    }
    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted -> hasPermission = granted }

    LaunchedEffect(Unit) {
        if (!hasPermission) permissionLauncher.launch(Manifest.permission.RECORD_AUDIO)
    }

    // Timer
    LaunchedEffect(isRecording) {
        if (isRecording) {
            recordingTime = 0
            while (isRecording) { delay(1000); recordingTime++ }
        }
    }

    // Cleanup
    DisposableEffect(Unit) {
        onDispose {
            audioTrack?.stop()
            audioTrack?.release()
        }
    }

    // Animations
    val infiniteTransition = rememberInfiniteTransition(label = "pulse")
    val pulseAlpha by infiniteTransition.animateFloat(
        initialValue = 0.2f, targetValue = 0.6f,
        animationSpec = infiniteRepeatable(tween(1000, easing = LinearEasing), RepeatMode.Reverse),
        label = "pulseAlpha"
    )

    // ================= PREVIEW MODE =================
    if (capturedAudio != null) {
        val audio = capturedAudio!!
        val durationFmt = String.format("%02d:%02d", audio.durationSeconds / 60, audio.durationSeconds % 60)
        val dataSizeKb = (audio.pcmShorts.size * 2) / 1024
        val channelLabel = if (audio.channels == 2) "Stereo" else "Mono"

        val previewTransition = rememberInfiniteTransition(label = "pp")
        val previewPulse by previewTransition.animateFloat(
            initialValue = 1f, targetValue = 1.1f,
            animationSpec = infiniteRepeatable(tween(600, easing = EaseInOutCubic), RepeatMode.Reverse),
            label = "pps"
        )

        Column(
            modifier = Modifier.fillMaxSize().background(BackgroundOffWhite)
                .statusBarsPadding().navigationBarsPadding(),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Row(
                modifier = Modifier.fillMaxWidth().padding(24.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text("Preview Recording", style = Typography.titleLarge, color = TextPrimary)
            }

            Spacer(Modifier.weight(1f))

            // Playback button
            Box(
                modifier = Modifier.size(160.dp)
                    .background(
                        if (isPlaying) AccentSecondary.copy(alpha = 0.15f) else SurfaceWhite, CircleShape
                    ).padding(if (isPlaying) (4 * previewPulse).dp else 0.dp),
                contentAlignment = Alignment.Center
            ) {
                Box(
                    modifier = Modifier.size(100.dp)
                        .background(
                            if (isPlaying) AccentSecondary.copy(alpha = 0.3f) else AccentPrimary.copy(alpha = 0.1f),
                            CircleShape
                        ).clip(CircleShape)
                        .clickable {
                            if (isPlaying) {
                                audioTrack?.pause(); isPlaying = false
                            } else {
                                try {
                                    audioTrack?.stop(); audioTrack?.release()

                                    val channelConfig = if (audio.channels == 2)
                                        AudioFormat.CHANNEL_OUT_STEREO else AudioFormat.CHANNEL_OUT_MONO
                                    val byteData = ByteArray(audio.pcmShorts.size * 2)
                                    ByteBuffer.wrap(byteData).order(ByteOrder.LITTLE_ENDIAN)
                                        .asShortBuffer().put(audio.pcmShorts)

                                    val track = AudioTrack.Builder()
                                        .setAudioAttributes(
                                            AudioAttributes.Builder()
                                                .setUsage(AudioAttributes.USAGE_MEDIA)
                                                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                                                .build()
                                        ).setAudioFormat(
                                            AudioFormat.Builder()
                                                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                                                .setSampleRate(audio.sampleRate)
                                                .setChannelMask(channelConfig)
                                                .build()
                                        ).setBufferSizeInBytes(byteData.size)
                                        .setTransferMode(AudioTrack.MODE_STATIC)
                                        .build()

                                    track.write(byteData, 0, byteData.size)
                                    track.play()
                                    audioTrack = track; isPlaying = true
                                } catch (e: Exception) {
                                    Log.e("AudioScreen", "Playback failed", e)
                                }
                            }
                        },
                    contentAlignment = Alignment.Center
                ) {
                    Icon(
                        if (isPlaying) Icons.Rounded.Pause else Icons.Rounded.PlayArrow,
                        contentDescription = null,
                        tint = if (isPlaying) AccentSecondary else AccentPrimary,
                        modifier = Modifier.size(48.dp)
                    )
                }
            }

            Spacer(Modifier.height(24.dp))

            Text(durationFmt,
                style = Typography.titleLarge.copy(fontSize = 48.sp, fontWeight = FontWeight.Light),
                color = TextPrimary)

            Spacer(Modifier.height(8.dp))

            Text("${audio.sampleRate / 1000} kHz • ${audio.bitDepth}-bit • $channelLabel • ${dataSizeKb} KB",
                style = Typography.bodyMedium, color = TextSecondary)

            Spacer(Modifier.weight(1f))

            // Save / Discard
            Row(
                modifier = Modifier.fillMaxWidth().padding(horizontal = 48.dp, vertical = 32.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    IconButton(
                        onClick = {
                            audioTrack?.stop(); audioTrack?.release()
                            audioTrack = null; isPlaying = false
                            capturedAudio = null; recordingTime = 0
                        },
                        enabled = !isSaving,
                        modifier = Modifier.size(64.dp)
                            .background(Color(0xFFE74C3C).copy(alpha = 0.9f), CircleShape)
                    ) {
                        Icon(Icons.Rounded.Delete, "Discard", tint = Color.White, modifier = Modifier.size(28.dp))
                    }
                    Spacer(Modifier.height(8.dp))
                    Text("Discard", fontSize = 12.sp, color = TextSecondary)
                }

                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    IconButton(
                        onClick = {
                            isSaving = true
                            audioTrack?.stop(); audioTrack?.release()
                            audioTrack = null; isPlaying = false

                            coroutineScope.launch {
                                val file = withContext(Dispatchers.IO) {
                                    compressAndSaveAudio(context, audio.pcmShorts,
                                        audio.sampleRate, audio.channels, qtsqEngine,
                                        password.ifEmpty { null })
                                }
                                isSaving = false
                                if (file != null) {
                                    // Phase 7: confirm which container codec was used.
                                    withContext(Dispatchers.Main) {
                                        android.widget.Toast.makeText(context, "Saved via ${QtsqEngine.codecName(context)} codec", android.widget.Toast.LENGTH_SHORT).show()
                                    }
                                    capturedAudio = null
                                    onRecorded(file)
                                } else {
                                    withContext(Dispatchers.Main) {
                                        android.widget.Toast.makeText(context,
                                            "Save failed — check logs", android.widget.Toast.LENGTH_LONG).show()
                                    }
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
                    Text(if (isSaving) "Saving..." else "Save .qtsq", fontSize = 12.sp, color = TextSecondary)
                }
            }
        }
        return
    }

    // ================= RECORDING MODE =================
    Column(
        modifier = Modifier.fillMaxSize().background(BackgroundOffWhite)
            .statusBarsPadding().navigationBarsPadding(),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        // Top bar
        Row(
            modifier = Modifier.fillMaxWidth().padding(24.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            IconButton(onClick = onClose) {
                Icon(Icons.Rounded.Close, "Close", tint = TextPrimary)
            }
            Text("Audio Capture", style = Typography.titleLarge, color = TextPrimary)
            Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                IconButton(onClick = { showPasswordDialog = true }) {
                    Icon(
                        if (isLocked) Icons.Rounded.Lock else Icons.Rounded.LockOpen,
                        "Lock", tint = if (isLocked) AccentSecondary else TextPrimary
                    )
                }
            }
        }

        // Quality selector chip
        Box(
            modifier = Modifier
                .clip(RoundedCornerShape(20.dp))
                .background(AccentPrimary.copy(alpha = 0.1f))
                .clickable(enabled = !isRecording) { showQualitySelector = !showQualitySelector }
                .padding(horizontal = 16.dp, vertical = 8.dp)
        ) {
            Text(
                QUALITY_OPTIONS[selectedQualityIndex].label + " • Stereo",
                color = AccentPrimary, fontWeight = FontWeight.Medium, fontSize = 13.sp
            )
        }

        // Quality dropdown
        if (showQualitySelector && !isRecording) {
            Column(
                modifier = Modifier.padding(horizontal = 32.dp, vertical = 8.dp)
                    .clip(RoundedCornerShape(20.dp))
                    .background(SurfaceWhite)
                    .padding(8.dp)
            ) {
                QUALITY_OPTIONS.forEachIndexed { index, option ->
                    val isSelected = index == selectedQualityIndex
                    Row(
                        modifier = Modifier.fillMaxWidth()
                            .clip(RoundedCornerShape(12.dp))
                            .background(if (isSelected) AccentPrimary.copy(alpha = 0.15f) else Color.Transparent)
                            .clickable { selectedQualityIndex = index; showQualitySelector = false }
                            .padding(horizontal = 16.dp, vertical = 12.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Icon(Icons.Rounded.GraphicEq, null,
                            tint = if (isSelected) AccentPrimary else TextSecondary,
                            modifier = Modifier.size(18.dp))
                        Spacer(Modifier.width(12.dp))
                        Text(option.label,
                            color = if (isSelected) AccentPrimary else TextPrimary,
                            fontWeight = if (isSelected) FontWeight.Bold else FontWeight.Normal,
                            fontSize = 14.sp)
                    }
                }
            }
        }

        Spacer(Modifier.weight(1f))

        // Timer
        Text(timeFormatted,
            style = Typography.titleLarge.copy(fontSize = 72.sp, fontWeight = FontWeight.Light),
            color = if (isRecording) AccentSecondary else TextSecondary)

        Spacer(Modifier.height(48.dp))

        // Waveform bars
        Row(
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier.height(64.dp)
        ) {
            if (isRecording) {
                repeat(7) { i ->
                    Box(modifier = Modifier.width(6.dp)
                        .height(if (i % 3 == 0) 48.dp else if (i % 2 == 0) 32.dp else 40.dp)
                        .background(AccentSecondary.copy(alpha = pulseAlpha), CircleShape))
                }
            } else {
                Box(modifier = Modifier.size(8.dp).background(Color.LightGray, CircleShape))
            }
        }

        Spacer(Modifier.weight(1f))

        if (!hasPermission) {
            Text("Audio permission is required.", color = Color.Red)
            Spacer(Modifier.height(16.dp))
        }

        if (statusMessage != null) {
            Text(statusMessage!!, style = Typography.bodyMedium, color = TextSecondary,
                modifier = Modifier.padding(horizontal = 24.dp, vertical = 8.dp))
        }

        // Record button
        Box(
            modifier = Modifier.padding(bottom = 64.dp).size(96.dp)
                .background(
                    if (isRecording) AccentSecondary.copy(alpha = 0.2f)
                    else AccentPrimary.copy(alpha = 0.1f), CircleShape
                ).padding(12.dp).clip(CircleShape)
                .background(if (isRecording) AccentSecondary else AccentPrimary)
                .clickable(enabled = hasPermission) {
                    if (isRecording) {
                        isRecording = false
                        recordingJobRef?.cancel()
                    } else {
                        isRecording = true
                        statusMessage = null
                        val quality = QUALITY_OPTIONS[selectedQualityIndex]
                        recordingJobRef = coroutineScope.launch(Dispatchers.IO) {
                            recordAudioStereo(
                                sampleRate = quality.sampleRate,
                                onResult = { audio, msg ->
                                    isRecording = false
                                    statusMessage = msg
                                    if (audio != null) capturedAudio = audio
                                }
                            )
                        }
                    }
                },
            contentAlignment = Alignment.Center
        ) {
            Icon(
                if (isRecording) Icons.Rounded.Stop else Icons.Rounded.Mic,
                contentDescription = null, tint = SurfaceWhite,
                modifier = Modifier.size(48.dp)
            )
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

// ============================================================
//  RECORDING — STEREO
// ============================================================

private suspend fun recordAudioStereo(
    sampleRate: Int,
    onResult: suspend (CapturedAudio?, String) -> Unit
) {
    val channelConfig = AudioFormat.CHANNEL_IN_STEREO
    val audioFormat = AudioFormat.ENCODING_PCM_16BIT
    val minBuf = AudioRecord.getMinBufferSize(sampleRate, channelConfig, audioFormat)

    // Fallback to mono if stereo not supported
    val actualChannelConfig: Int
    val actualChannels: Int
    if (minBuf == AudioRecord.ERROR_BAD_VALUE || minBuf == AudioRecord.ERROR) {
        val monoBuf = AudioRecord.getMinBufferSize(sampleRate, AudioFormat.CHANNEL_IN_MONO, audioFormat)
        if (monoBuf == AudioRecord.ERROR_BAD_VALUE || monoBuf == AudioRecord.ERROR) {
            withContext(Dispatchers.Main) { onResult(null, "Audio hardware error.") }
            return
        }
        actualChannelConfig = AudioFormat.CHANNEL_IN_MONO
        actualChannels = 1
    } else {
        actualChannelConfig = channelConfig
        actualChannels = 2
    }

    val bufferSize = AudioRecord.getMinBufferSize(sampleRate, actualChannelConfig, audioFormat)

    val recorder = try {
        AudioRecord(
            MediaRecorder.AudioSource.MIC,
            sampleRate, actualChannelConfig, audioFormat,
            bufferSize * 4
        )
    } catch (e: SecurityException) {
        withContext(Dispatchers.Main) { onResult(null, "Audio permission denied.") }
        return
    }

    if (recorder.state != AudioRecord.STATE_INITIALIZED) {
        withContext(Dispatchers.Main) { onResult(null, "Failed to init AudioRecord.") }
        recorder.release()
        return
    }

    val pcmStream = ByteArrayOutputStream()
    val buffer = ShortArray(bufferSize)

    try {
        recorder.startRecording()

        while (true) {
            val readSize = recorder.read(buffer, 0, buffer.size)
            if (readSize > 0) {
                val byteBuffer = ByteBuffer.allocate(readSize * 2)
                byteBuffer.order(ByteOrder.LITTLE_ENDIAN)
                for (i in 0 until readSize) byteBuffer.putShort(buffer[i])
                pcmStream.write(byteBuffer.array(), 0, readSize * 2)
            }
            yield()
        }
    } catch (_: CancellationException) {
        // User pressed stop
    } catch (e: Exception) {
        Log.e("AudioScreen", "Recording error", e)
    } finally {
        try { recorder.stop() } catch (_: Exception) {}
        recorder.release()

        // NonCancellable ensures this runs even after coroutine cancellation (user pressed Stop)
        withContext(NonCancellable) {
            try {
                val rawBytes = pcmStream.toByteArray()
                if (rawBytes.isNotEmpty()) {
                    val shortCount = rawBytes.size / 2
                    val shortBuf = ByteBuffer.wrap(rawBytes).order(ByteOrder.LITTLE_ENDIAN).asShortBuffer()
                    val shortArray = ShortArray(shortCount)
                    shortBuf.get(shortArray)

                    val durationSec = shortCount / (sampleRate * actualChannels)
                    val audio = CapturedAudio(shortArray, sampleRate, actualChannels, 16, durationSec)

                    withContext(Dispatchers.Main) {
                        onResult(audio, "Recording complete (${durationSec}s, $actualChannels ch)")
                    }
                } else {
                    withContext(Dispatchers.Main) { onResult(null, "No audio recorded.") }
                }
            } catch (e: Exception) {
                Log.e("AudioScreen", "Processing error", e)
                withContext(Dispatchers.Main) { onResult(null, "Error: ${e.message}") }
            }
        }
    }
}

// ============================================================
//  COMPRESS & SAVE
// ============================================================

private fun compressAndSaveAudio(
    context: Context,
    pcmShorts: ShortArray,
    sampleRate: Int,
    channels: Int,
    qtsqEngine: QtsqEngine,
    password: String?
): File? {
    return try {
        Log.i("AudioScreen", "Compressing ${pcmShorts.size} samples, ${sampleRate}Hz, ${channels}ch")

        // Fail-closed engine: keystore must be ready (or a password must be set)
        if (!qtsqEngine.ensureKeystoreReady(context)) {
            Log.e("AudioScreen", "Keystore unavailable and no password set — refusing to save")
            return null
        }

        // Try compressAudio first
        var qtsqData = qtsqEngine.compressAudio(pcmShorts, sampleRate, channels, password)

        if (qtsqData == null) {
            Log.w("AudioScreen", "compressAudio returned null, trying compressBytes fallback")
            // Fallback: convert shorts to byte array and compress as raw bytes
            val byteData = ByteArray(pcmShorts.size * 2)
            ByteBuffer.wrap(byteData).order(ByteOrder.LITTLE_ENDIAN).asShortBuffer().put(pcmShorts)
            qtsqData = qtsqEngine.compressBytes(byteData, "pcm_audio", password)
        }

        if (qtsqData != null) {
            val file = File(context.filesDir, "audio_${System.currentTimeMillis()}.qtsq")
            file.writeBytes(qtsqData)
            Log.i("AudioScreen", "Saved: ${file.name} (${qtsqData.size} bytes, " +
                    "ratio: ${String.format("%.1f", qtsqData.size * 100.0 / (pcmShorts.size * 2))}%)")
            file
        } else {
            Log.e("AudioScreen", "Both compression methods returned null")
            null
        }
    } catch (e: Exception) {
        Log.e("AudioScreen", "Save failed: ${e.message}", e)
        null
    }
}
