package com.haruhito.qtsq.screens

import androidx.compose.runtime.Composable
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.haruhito.qtsq.ui.theme.*

import androidx.camera.video.FallbackStrategy
import androidx.camera.video.Quality
import androidx.camera.video.QualitySelector

/**
 * Capture settings for the camera's VIDEO mode.
 *
 * [targetLongEdge] is the long edge the clip is encoded at; the recorder is
 * asked for a quality at least that large and the transcode downscales to it.
 * "Native" keeps the camera's own frame size and is capped only by memory.
 */
enum class VideoResolution(val label: String, val targetLongEdge: Int) {
    HD("720p", 720),
    FHD("1080p", 1080),
    QHD("1440p", 1440),
    UHD("4K", 2160),
    /** Whatever the camera actually produces, bounded by MAX_NATIVE_LONG_EDGE. */
    NATIVE("Native", 0);

    companion object {
        /**
         * One decoded RGB frame is longEdge*aspect*3 bytes. 4K is ~25 MB and
         * 8K would be ~100 MB, which does not fit beside the rest of the
         * pipeline on a 256 MB heap, so native capture is capped here.
         */
        const val MAX_NATIVE_LONG_EDGE = 2160

        fun longEdgeFor(res: VideoResolution, srcW: Int, srcH: Int): Int {
            if (res != NATIVE) return res.targetLongEdge
            val nativeLong = maxOf(srcW, srcH)
            return minOf(nativeLong, MAX_NATIVE_LONG_EDGE)
        }
    }
}

/**
 * Requested capture rate.
 *
 * CameraX's Recorder exposes no frame-rate selector in this version, so this
 * is a *request*: we record at whatever rate the device delivers for the
 * chosen quality and then report the real rate read from the container. We do
 * not resample or duplicate frames to fake a rate, because that would change
 * motion timing and inflate the file.
 */
enum class VideoFramerate(val label: String, val value: Int) {
    FPS_24("24 fps", 24),
    FPS_30("30 fps", 30),
    FPS_60("60 fps", 60);

    companion object {
        fun nearest(rates: List<Int>, target: Int): Int {
            if (rates.isEmpty()) return target
            return rates.minByOrNull { kotlin.math.abs(it - target) } ?: target
        }
    }
}

/** Ask CameraX for a recording quality that can deliver [resolution]. */
fun qualitySelectorFor(resolution: VideoResolution): QualitySelector {
    val lower = FallbackStrategy.lowerQualityOrHigherThan(Quality.SD)
    return when (resolution) {
        // Request a tier at or above the target, falling back rather than
        // failing if the device cannot produce it.
        VideoResolution.HD ->
            QualitySelector.fromOrderedList(listOf(Quality.FHD, Quality.HD, Quality.SD), lower)
        VideoResolution.FHD ->
            QualitySelector.fromOrderedList(listOf(Quality.UHD, Quality.FHD, Quality.HD), lower)
        // There is no QHD tier in CameraX, so record UHD and downscale.
        VideoResolution.QHD ->
            QualitySelector.fromOrderedList(listOf(Quality.UHD, Quality.FHD), lower)
        VideoResolution.UHD ->
            QualitySelector.fromOrderedList(listOf(Quality.UHD, Quality.FHD), lower)
        VideoResolution.NATIVE ->
            QualitySelector.fromOrderedList(
                listOf(Quality.UHD, Quality.FHD, Quality.HD), lower
            )
    }
}

/** Small selectable pill used by the camera's video settings sheet. */
@Composable
fun FilterPill(
    label: String,
    selected: Boolean,
    enabled: Boolean = true,
    onClick: () -> Unit
) {
    Box(
        modifier = Modifier
            .height(38.dp)
            .clip(RoundedCornerShape(19.dp))
            .background(if (selected) AccentPrimary else SurfaceCard)
            .clickable(enabled = enabled, onClick = onClick)
            .padding(horizontal = 14.dp),
        contentAlignment = Alignment.Center
    ) {
        Text(
            text = label,
            color = if (selected) Color.White else TextSecondary,
            fontSize = 12.sp,
            fontWeight = FontWeight.SemiBold,
            textAlign = TextAlign.Center,
            maxLines = 1
        )
    }
}
