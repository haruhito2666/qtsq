package com.haruhito.qtsq.screens

import android.graphics.Bitmap
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.util.Log
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Info
import androidx.compose.material.icons.rounded.Pause
import androidx.compose.material.icons.rounded.PlayArrow
import androidx.compose.material.icons.rounded.Stop
import androidx.compose.material.icons.rounded.VolumeUp
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.haruhito.qtsq.QtsqEngine
import com.haruhito.qtsq.ui.theme.*
import android.os.SystemClock
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import java.io.File
import kotlin.math.roundToInt

@Composable
private fun DetailRow(label: String, value: String) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        Text(label, color = TextTertiary, fontSize = 12.sp)
        Text(
            value,
            color = Color.White,
            fontSize = 12.sp,
            fontWeight = androidx.compose.ui.text.font.FontWeight.SemiBold
        )
    }
}

/**
 * Plays a QTSQ pixel-path video.
 *
 * Frames come from the native decoder and arrive as ARGB_8888 bytes, which is
 * exactly what Bitmap.copyPixelsFromBuffer consumes — no Kotlin pixel loop.
 * Pacing is wall-clock driven (elapsed × fps) rather than a fixed delay per
 * frame, so a slow decode shows late frames instead of permanently drifting
 * behind the audio.
 */
@Composable
fun VideoViewer(
    qtsqEngine: QtsqEngine,
    filePath: String,
    onClose: () -> Unit
) {
    val context = LocalContext.current

    var frameCount by remember { mutableIntStateOf(0) }
    var videoWidth by remember { mutableIntStateOf(0) }
    var videoHeight by remember { mutableIntStateOf(0) }
    var currentFrame by remember { mutableIntStateOf(0) }
    var bitmap by remember { mutableStateOf<Bitmap?>(null) }
    var isPlaying by remember { mutableStateOf(false) }
    var errorMessage by remember { mutableStateOf<String?>(null) }
    var hasAudio by remember { mutableStateOf(false) }
    var audioSampleRate by remember { mutableIntStateOf(0) }
    /** Real container rate. Assuming 30 played correct clips at the wrong speed. */
    var fps by remember { mutableIntStateOf(30) }
    var fileSize by remember { mutableLongStateOf(0L) }
    var encoding by remember { mutableStateOf("PIXEL") }
    var showDetails by remember { mutableStateOf(false) }
    var decodeFps by remember { mutableFloatStateOf(0f) }
    var audioMuted by remember { mutableStateOf(false) }
    var startedAt by remember { mutableStateOf(0L) }
    var startFrame by remember { mutableIntStateOf(0) }
    var decoder by remember { mutableStateOf(0L) }
    var position by remember { mutableFloatStateOf(0f) }

    // createBitmap throws on a non-positive dimension; every call site goes
    // through this so a malformed header cannot crash playback.
    fun frameBitmap(): Bitmap? {
        if (videoWidth <= 0 || videoHeight <= 0) return null
        bitmap?.let { return it }
        val bmp = runCatching {
            Bitmap.createBitmap(videoWidth, videoHeight, Bitmap.Config.ARGB_8888)
        }.getOrNull() ?: return null
        bitmap = bmp
        return bmp
    }

    // ── Open the decoder and show frame 0 ──
    LaunchedEffect(filePath) {
        try {
            val bytes = with(kotlin.io.path.Path(filePath)) {
                File(filePath).readBytes()
            }
            val h = qtsqEngine.videoDecoderOpen(bytes, null)
            if (h == 0L) {
                errorMessage = "Could not open this video."
                return@LaunchedEffect
            }
            decoder = h
            val info = qtsqEngine.videoDecoderInfo(h)
            if (info != null && info.size >= 4) {
                frameCount = info[0]
                videoWidth = info[1]
                videoHeight = info[2]
                fps = info[3]
            }
            fileSize = qtsqEngine.videoDecoderFileSize(h).coerceAtLeast(0L)
            encoding = qtsqEngine.videoDecoderEncoding(h) ?: "PIXEL"
            hasAudio = qtsqEngine.videoDecoderHasAudio(h)
            if (frameCount <= 0) {
                errorMessage = "This video has no frames."
                return@LaunchedEffect
            }
            if (videoWidth <= 0 || videoHeight <= 0) {
                errorMessage = "This video reports an invalid size."
                return@LaunchedEffect
            }
            val first = qtsqEngine.videoDecoderFrame(h, 0)
            if (first != null && first.size == videoWidth * videoHeight * 4) {
                frameBitmap()?.copyPixelsFromBuffer(java.nio.ByteBuffer.wrap(first))
            }
        } catch (e: Exception) {
            Log.e("VideoViewer", "open failed", e)
            errorMessage = "Failed to open video: ${e.message}"
        }
    }

    // ── Audio track from the embedded gravitational audio ──
    var audioTrack by remember { mutableStateOf<AudioTrack?>(null) }
    DisposableEffect(decoder, hasAudio) {
        if (decoder != 0L && hasAudio) {
            val decoded = qtsqEngine.videoDecoderAudio(decoder)
            val wav = decoded?.getOrNull(0) as? ByteArray
            val rate = (decoded?.getOrNull(1) as? Int) ?: 44100
            audioSampleRate = rate
            if (wav != null && wav.size > 44) {
                runCatching {
                    val t = AudioTrack.Builder()
                        .setAudioAttributes(
                            AudioAttributes.Builder()
                                .setUsage(AudioAttributes.USAGE_MEDIA)
                                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                                .build()
                        )
                        .setAudioFormat(
                            AudioFormat.Builder()
                                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                                .setSampleRate(rate)
                                .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                                .build()
                        )
                        .setBufferSizeInBytes(wav.size)
                        .setTransferMode(AudioTrack.MODE_STATIC)
                        .build()
                    // Skip the 44-byte RIFF header: AudioTrack wants raw PCM.
                    t.write(wav, 44, wav.size - 44)
                    audioTrack = t
                }.onFailure { Log.w("VideoViewer", "audio track failed", it) }
            }
        }
        onDispose {
            audioTrack?.let { runCatching { it.stop() }; runCatching { it.release() } }
            audioTrack = null
        }
    }

    // ── Playback loop ──
    // Decoding runs on a background dispatcher: doing it on the main thread
    // blocked the UI, which is what made playback look choppy. The target
    // frame comes from a monotonic clock so a slow decode shows a late frame
    // instead of permanently drifting behind.
    LaunchedEffect(isPlaying, decoder, frameCount, fps) {
        if (!isPlaying || decoder == 0L || frameCount <= 0) return@LaunchedEffect
        val rate = fps.toFloat().coerceAtLeast(1f)
        val dec = decoder
        // Loop bookkeeping stays local: writing it into Compose state on every
        // frame would trigger a recomposition per frame.
        var anchorMs = SystemClock.elapsedRealtime()
        var anchorFrame = startFrame
        var decoded = 0
        var windowStartMs = anchorMs
        while (isPlaying) {
            val nowMs = SystemClock.elapsedRealtime()
            val target = (anchorFrame + ((nowMs - anchorMs) / 1000f * rate).roundToInt()) % frameCount
            if (target != currentFrame) {
                val data = withContext(Dispatchers.Default) {
                    qtsqEngine.videoDecoderFrame(dec, target)
                }
                if (data != null && data.size == videoWidth * videoHeight * 4) {
                    frameBitmap()?.copyPixelsFromBuffer(java.nio.ByteBuffer.wrap(data))
                    currentFrame = target
                    position = target.toFloat()
                    decoded++
                    val t = SystemClock.elapsedRealtime()
                    if (t - windowStartMs >= 1000) {
                        decodeFps = decoded * 1000f / (t - windowStartMs)
                        decoded = 0
                        windowStartMs = t
                        // Re-anchor so the clock keeps tracking the picture
                        // even when decode is slower than real time.
                        anchorMs = t
                        anchorFrame = target
                        startedAt = t
                    }
                }
            } else {
                // Nothing new yet — sleep until the next frame is actually due.
                val nextDue = ((currentFrame + 1 - anchorFrame) * 1000f / rate)
                    .toLong() - (SystemClock.elapsedRealtime() - anchorMs)
                delay(nextDue.coerceIn(1L, 100L))
            }
        }
    }

    // Release the native decoder when we leave.
    DisposableEffect(Unit) {
        onDispose {
            audioTrack?.let { runCatching { it.stop() }; runCatching { it.release() } }
            if (decoder != 0L) qtsqEngine.videoDecoderFree(decoder)
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black)
    ) {
        // ── Picture ──
        Box(
            modifier = Modifier
                .weight(1f)
                .fillMaxWidth(),
            contentAlignment = Alignment.Center
        ) {
            val bmp = bitmap
            when {
                errorMessage != null -> Text(
                    errorMessage!!,
                    color = TextPrimary,
                    fontSize = 14.sp
                )
                bmp != null -> Image(
                    bitmap = bmp.asImageBitmap(),
                    contentDescription = "Video frame",
                    modifier = Modifier.fillMaxSize(),
                    contentScale = ContentScale.Fit
                )
                else -> Text("Loading…", color = TextPrimary, fontSize = 14.sp)
            }
        }

        // ── Details ──
        if (showDetails) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(Color(0xFF101010))
                    .padding(horizontal = 16.dp, vertical = 10.dp),
                verticalArrangement = Arrangement.spacedBy(4.dp)
            ) {
                DetailRow("Resolution", "$videoWidth × $videoHeight")
                DetailRow("Frame rate", "$fps fps")
                DetailRow("Frames", "$frameCount")
                DetailRow("Duration", "%.2f s".format(frameCount / fps.toFloat()))
                DetailRow("Codec", "$encoding (QTSQ pixel)")
                DetailRow("Audio", if (hasAudio) "Classic Gravitational, $audioSampleRate Hz" else "none")
                DetailRow("File size", "%.2f MB".format(fileSize / 1_000_000.0))
                DetailRow("Decode rate", "%.1f fps".format(decodeFps))
            }
        }

        // ── Transport ──
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .background(Color(0xFF141414))
                .padding(horizontal = 16.dp, vertical = 12.dp)
        ) {
            Slider(
                value = position,
                onValueChange = { v ->
                    val idx = v.roundToInt().coerceIn(0, (frameCount - 1).coerceAtLeast(0))
                    isPlaying = false
                    currentFrame = idx
                    position = idx.toFloat()
                    startFrame = idx
                    val data = if (decoder != 0L) qtsqEngine.videoDecoderFrame(decoder, idx) else null
                    if (data != null && data.size == videoWidth * videoHeight * 4) {
                        frameBitmap()?.copyPixelsFromBuffer(java.nio.ByteBuffer.wrap(data))
                    }
                    // 44100 Hz / 30 fps is not a safe constant; only nudge the
                    // audio head when we can derive a sane frame size.
                    if (hasAudio) {
                        val perFrame = if (frameCount > 0 && audioSampleRate > 0)
                            (audioSampleRate.toLong() / 30L).toInt() else 0
                        if (perFrame > 0) {
                            audioTrack?.let {
                                runCatching {
                                    it.setPlaybackHeadPosition((idx.toLong() * perFrame).toInt())
                                }
                            }
                        }
                    }
                },
                valueRange = 0f..(frameCount - 1).coerceAtLeast(1).toFloat(),
                colors = SliderDefaults.colors(
                    thumbColor = AccentPrimary,
                    activeTrackColor = AccentPrimary,
                    inactiveTrackColor = Color(0xFF444444)
                )
            )
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(4.dp)
            ) {
                IconButton(onClick = {
                    if (frameCount > 0) {
                        if (isPlaying) {
                            isPlaying = false
                            audioTrack?.let { runCatching { it.pause() } }
                        } else {
                            isPlaying = true
                            startFrame = currentFrame
                            if (!audioMuted) runCatching { audioTrack?.play() }
                        }
                    }
                }) {
                    Icon(
                        if (isPlaying) Icons.Rounded.Pause else Icons.Rounded.PlayArrow,
                        contentDescription = if (isPlaying) "Pause" else "Play",
                        tint = TextPrimary
                    )
                }
                IconButton(onClick = {
                    isPlaying = false
                    currentFrame = 0
                    startFrame = 0
                    position = 0f
                    audioTrack?.let { runCatching { it.pause() }; runCatching { it.setPlaybackHeadPosition(0) } }
                    val data = if (decoder != 0L) qtsqEngine.videoDecoderFrame(decoder, 0) else null
                    if (data != null && data.size == videoWidth * videoHeight * 4) {
                        frameBitmap()?.copyPixelsFromBuffer(java.nio.ByteBuffer.wrap(data))
                    }
                }) {
                    Icon(Icons.Rounded.Stop, contentDescription = "Stop", tint = TextPrimary)
                }
                if (hasAudio) {
                    IconButton(onClick = {
                        audioMuted = !audioMuted
                        audioTrack?.let {
                            if (audioMuted) runCatching { it.pause() }
                            else if (isPlaying) runCatching { it.play() }
                        }
                    }) {
                        Icon(
                            Icons.Rounded.VolumeUp,
                            contentDescription = if (audioMuted) "Unmute" else "Mute",
                            tint = if (audioMuted) Color(0xFF666666) else TextPrimary
                        )
                    }
                    Text(
                        "GFC audio",
                        color = TextTertiary,
                        fontSize = 11.sp,
                        modifier = Modifier.padding(start = 4.dp)
                    )
                }
                Spacer(Modifier.weight(1f))
                IconButton(onClick = { showDetails = !showDetails }) {
                    Icon(
                        Icons.Rounded.Info,
                        contentDescription = "Video details",
                        tint = if (showDetails) AccentPrimary else TextTertiary
                    )
                }
                Text(
                    "${currentFrame + 1} / $frameCount" +
                        if (videoWidth > 0) "  •  ${videoWidth}×${videoHeight}" else "",
                    color = TextTertiary,
                    fontSize = 12.sp
                )
            }
        }
    }
}
