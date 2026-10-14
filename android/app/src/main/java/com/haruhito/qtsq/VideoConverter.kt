package com.haruhito.qtsq

import android.content.Context
import android.media.AudioFormat
import android.media.Image
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaExtractor
import android.media.MediaFormat
import android.media.MediaMetadataRetriever
import android.net.Uri
import android.util.Log
import com.haruhito.qtsq.screens.VideoResolution
import java.nio.ByteOrder
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/**
 * Demuxes a video URI and feeds RGB frames straight into the native QTSQ pixel
 * encoder, then attaches the audio track as Classic Gravitational.
 *
 * MediaExtractor/MediaCodec are used only to *decode* the source; all
 * compression is done by libqtsq.
 *
 * MEMORY: frames are streamed one at a time into a single reused buffer. The
 * previous design buffered every frame in one ByteArray (300 x 640x360x3 =
 * 207 MB) and then copied it again in toByteArray() — 415 MB against a 256 MB
 * Android heap, which reliably threw OutOfMemoryError. Nothing here may
 * accumulate per-frame data.
 */
object VideoConverter {

    private const val TAG = "VideoConverter"

    /** Upper bound on buffered audio, so a long recording cannot exhaust the heap. */
    private const val MAX_AUDIO_SECONDS = 180

    /** Cap on a single decoded RGB frame (24 MB ~= 2160p RGB). */
    private const val MAX_FRAME_BYTES = 24L * 1024 * 1024

    /** Largest 16:9 long edge whose RGB frame fits MAX_FRAME_BYTES. */
    private fun maxLongEdgeFor(maxFrameBytes: Long): Int {
        val pixels = maxFrameBytes / 3
        return (Math.sqrt(pixels * 9.0 / 16.0) / 2).toInt() * 2
    }

    data class Result(
        val qtsqBytes: ByteArray,
        val frameCount: Int,
        val width: Int,
        val height: Int,
        val fps: Int,
        val audioSamples: Int
    )

    class TranscodeException(message: String) : Exception(message)

    /**
     * Encode [uri] into a .qtsq payload.
     *
     * [targetPayload] is the per-frame byte budget (0 = uncapped). Frames are
     * decoded at up to [maxDimension] on the long edge and capped at
     * [maxFrames].
     */
    fun transcode(
        engine: QtsqEngine,
        context: Context,
        uri: Uri,
        targetPayload: Long,
        maxFrames: Int = 300,
        maxDimension: Int = 640,
        withAudio: Boolean = true,
        quality: Float = 0.75f,
        progress: ((Int) -> Unit)? = null
    ): Result {
        val meta = probe(context, uri)
        val (outW, outH) = scaledSize(meta.srcW, meta.srcH, meta.rotation, maxDimension)
        val frameBytes = outW * outH * 3
        // One frame is the only large allocation, but it still has to fit.
        if (frameBytes > MAX_FRAME_BYTES) {
            val safe = maxLongEdgeFor(MAX_FRAME_BYTES)
            return transcode(engine, context, uri, targetPayload, maxFrames, safe, withAudio, quality, progress)
        }

        val handle = engine.videoEncodeCreate(
            outW, outH, 3, meta.fps, QtsqEngine.VIDEO_PIXEL, targetPayload, quality
        )
        if (handle == 0L) throw TranscodeException("Video encoder could not be created.")

        // One reusable RGB buffer for the whole transcode — the native side
        // copies out of it per call, so this is the only frame-sized allocation.
        val frame = ByteArray(frameBytes)
        var encoded = 0
        try {
            encoded = streamViaCodec(
                context, uri, outW, outH, meta.rotation, maxFrames, frame
            ) { n, data -> if (engine.videoEncodeAddFrame(handle, data) != 0) throw TranscodeException("Encoding failed at frame $n.") }

            if (encoded == 0) {
                Log.i(TAG, "MediaCodec yielded no frames — falling back to retriever sampling")
                encoded = streamViaRetriever(
                    context, uri, outW, outH, maxFrames, meta.fps, meta.durationUs, frame
                ) { n, data -> if (engine.videoEncodeAddFrame(handle, data) != 0) throw TranscodeException("Encoding failed at frame $n.") }
                // getFrameAtTime() already returns an upright bitmap, so the
                // retriever path needs no rotation. Nothing else to do here.
            }

            if (encoded == 0) {
                throw TranscodeException(
                    "Could not decode any frames. The file may be truncated or use an unsupported codec."
                )
            }

            var audioSamples = 0
            if (withAudio) {
                val audio = runCatching { extractAudio(context, uri) }.getOrNull()
                if (audio != null && audio.samples.isNotEmpty()) {
                    audioSamples = audio.samples.size
                    val rc = engine.videoEncodeSetAudio(
                        handle, audio.samples, audio.sampleRate, audio.channels
                    )
                    if (rc != 0) Log.w(TAG, "audio attach rc=$rc")
                }
            }

            val bytes = engine.videoEncodeFinish(handle)
                ?: throw TranscodeException("Video encoding failed.")
            return Result(bytes, encoded, outW, outH, meta.fps, audioSamples)
        } finally {
            // Always release the native encoder, even on the failure paths.
            engine.videoEncoderFree(handle)
        }
    }

    /**
     * RAW coded dimensions plus the container rotation. These must stay raw:
     * MediaCodec emits the un-rotated frame, so swapping them here (as we
     * used to) made the scaler squeeze a 1920x1080 frame into a 608x1080
     * buffer and the picture came out stretched and sideways.
     */
    private data class Meta(
        val srcW: Int, val srcH: Int, val rotation: Int,
        val durationUs: Long, val fps: Int
    )

    private fun probe(context: Context, uri: Uri): Meta {
        val r = MediaMetadataRetriever()
        try {
            openRetriever(r, context, uri)
            var w = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH)?.toIntOrNull() ?: 0
            var h = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT)?.toIntOrNull() ?: 0
            val rot = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_ROTATION)?.toIntOrNull() ?: 0
            val dur = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION)?.toLongOrNull() ?: 0L
            if (w <= 0 || h <= 0) throw TranscodeException("Not a decodable video.")
            return Meta(w, h, ((rot % 360) + 360) % 360, dur, 30)
        } finally {
            r.release()
        }
    }

    /**
     * Long edge to encode at for a recorded file: the user's choice, clamped to
     * what the clip actually contains so we never upscale.
     */
    fun longEdgeForFile(context: Context, uri: Uri, res: VideoResolution): Int {
        if (res != VideoResolution.NATIVE) return res.targetLongEdge
        val r = MediaMetadataRetriever()
        return try {
            openRetriever(r, context, uri)
            var w = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH)?.toIntOrNull() ?: 0
            var h = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT)?.toIntOrNull() ?: 0
            val rot = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_ROTATION)?.toIntOrNull() ?: 0
            if (rot == 90 || rot == 270) { val t = w; w = h; h = t }
            maxOf(w, h).coerceIn(2, VideoResolution.MAX_NATIVE_LONG_EDGE)
        } catch (e: Exception) {
            VideoResolution.FHD.targetLongEdge
        } finally {
            r.release()
        }
    }

    /**
     * Output (display) size for a raw frame of [srcW]x[srcH] with [rotation].
     * The long edge of the *upright* picture is what [maxDim] refers to, so the
     * requested resolution means the same thing in portrait and landscape.
     */
    private fun scaledSize(srcW: Int, srcH: Int, rotation: Int, maxDim: Int): Pair<Int, Int> {
        val quarterTurn = rotation == 90 || rotation == 270
        val dispRawW = if (quarterTurn) srcH else srcW
        val dispRawH = if (quarterTurn) srcW else srcH
        val s = min(1.0, maxDim.toDouble() / max(dispRawW, dispRawH).toDouble())
        val w = max(2, ((dispRawW * s).roundToInt() and 1.inv()))
        val h = max(2, ((dispRawH * s).roundToInt() and 1.inv()))
        return w to h
    }

    // ── Demux helpers ───────────────────────────────────────────────

    private fun openRetriever(r: MediaMetadataRetriever, context: Context, uri: Uri) {
        val p = uri.path
        if (uri.scheme == "file" && p != null && java.io.File(p).exists()) {
            r.setDataSource(p)
        } else {
            r.setDataSource(context, uri)
        }
    }

    private fun openExtractor(e: MediaExtractor, context: Context, uri: Uri) {
        val p = uri.path
        if (uri.scheme == "file" && p != null && java.io.File(p).exists()) {
            e.setDataSource(p)
        } else {
            e.setDataSource(context, uri, null)
        }
    }

    // ── Fast path: MediaCodec in ByteBuffer mode ────────────────────
    //
    // The decoder MUST be configured with the *source* format. Passing a
    // fabricated output size makes many decoders switch to Surface output,
    // where getOutputImage() returns null and no frames ever decode. Scaling
    // is done in the YUV conversion instead.
    private fun streamViaCodec(
        context: Context, uri: Uri, outW: Int, outH: Int, rotation: Int, maxFrames: Int,
        frame: ByteArray, onFrame: (Int, ByteArray) -> Unit
    ): Int {
        val extractor = MediaExtractor()
        var codec: MediaCodec? = null
        try {
            openExtractor(extractor, context, uri)
            var track = -1
            var format: MediaFormat? = null
            for (i in 0 until extractor.trackCount) {
                val f = extractor.getTrackFormat(i)
                val m = f.getString(MediaFormat.KEY_MIME) ?: continue
                if (m.startsWith("video/")) { track = i; format = f; break }
            }
            if (track < 0 || format == null) return 0
            extractor.selectTrack(track)

            val mime = format.getString(MediaFormat.KEY_MIME)!!
            codec = MediaCodec.createDecoderByType(mime)
            runCatching {
                format.setInteger(
                    MediaFormat.KEY_COLOR_FORMAT,
                    MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible
                )
            }
            codec.configure(format, null, null, 0)
            codec.start()

            val info = MediaCodec.BufferInfo()
            var inputEos = false
            var decoded = 0
            var lastPts = Long.MIN_VALUE
            var idle = 0

            loop@ while (decoded < maxFrames) {
                if (!inputEos) {
                    val inIdx = codec.dequeueInputBuffer(10_000)
                    if (inIdx >= 0) {
                        val buf = codec.getInputBuffer(inIdx)
                        val size = if (buf != null) extractor.readSampleData(buf, 0) else -1
                        if (size < 0) {
                            codec.queueInputBuffer(inIdx, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                            inputEos = true
                        } else {
                            codec.queueInputBuffer(inIdx, 0, size, extractor.sampleTime, 0)
                            extractor.advance()
                        }
                    }
                }
                val outIdx = codec.dequeueOutputBuffer(info, 10_000)
                when {
                    outIdx >= 0 -> {
                        val image = codec.getOutputImage(outIdx)
                        if (image != null && info.presentationTimeUs != lastPts) {
                            Yuv420Image.rgb(image, outW, outH, rotation, frame)
                            onFrame(decoded, frame)
                            lastPts = info.presentationTimeUs
                            decoded++
                        }
                        image?.close()
                        codec.releaseOutputBuffer(outIdx, false)
                        idle = 0
                        if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) break@loop
                    }
                    outIdx == MediaCodec.INFO_TRY_AGAIN_LATER && inputEos -> {
                        if (++idle > 100) break@loop
                    }
                }
            }
            return decoded
        } catch (e: Exception) {
            Log.w(TAG, "codec decode failed: ${e.message}")
            return 0
        } finally {
            runCatching { codec?.stop() }
            runCatching { codec?.release() }
            extractor.release()
        }
    }

    // ── Robust fallback: sample frames with MediaMetadataRetriever ──
    private fun streamViaRetriever(
        context: Context, uri: Uri, outW: Int, outH: Int, maxFrames: Int,
        fps: Int, durationUs: Long, frame: ByteArray, onFrame: (Int, ByteArray) -> Unit
    ): Int {
        val r = MediaMetadataRetriever()
        var decoded = 0
        try {
            openRetriever(r, context, uri)
            val stepUs = if (fps > 0) 1_000_000L / fps else 33_333L
            val totalUs = if (durationUs > 0) durationUs else stepUs * maxFrames
            val pixels = IntArray(outW * outH)
            var t = 0L
            while (decoded < maxFrames && t < totalUs) {
                val bmp = r.getFrameAtTime(t * 1000, MediaMetadataRetriever.OPTION_CLOSEST_SYNC)
                    ?: r.getFrameAtTime(t * 1000)
                if (bmp != null) {
                    val scaled = android.graphics.Bitmap.createScaledBitmap(bmp, outW, outH, true)
                    scaled.getPixels(pixels, 0, outW, 0, 0, outW, outH)
                    var o = 0
                    for (p in pixels) {
                        frame[o++] = ((p shr 16) and 0xFF).toByte()
                        frame[o++] = ((p shr 8) and 0xFF).toByte()
                        frame[o++] = (p and 0xFF).toByte()
                    }
                    onFrame(decoded, frame)
                    if (scaled !== bmp) scaled.recycle()
                    bmp.recycle()
                    decoded++
                }
                t += stepUs
            }
        } catch (e: Exception) {
            Log.w(TAG, "retriever decode failed: ${e.message}")
        } finally {
            r.release()
        }
        return decoded
    }

    // ── Audio ───────────────────────────────────────────────────────

    private data class Audio(val sampleRate: Int, val channels: Int, val samples: ShortArray)

    /** Decode the first audio track to interleaved Int16, bounded in size. */
    private fun extractAudio(context: Context, uri: Uri): Audio {
        val extractor = MediaExtractor()
        var codec: MediaCodec? = null
        try {
            openExtractor(extractor, context, uri)
            var track = -1
            var format: MediaFormat? = null
            for (i in 0 until extractor.trackCount) {
                val f = extractor.getTrackFormat(i)
                val m = f.getString(MediaFormat.KEY_MIME) ?: continue
                if (m.startsWith("audio/")) { track = i; format = f; break }
            }
            if (track < 0 || format == null) return Audio(0, 0, ShortArray(0))
            extractor.selectTrack(track)

            val mime = format.getString(MediaFormat.KEY_MIME)!!
            val rate = format.getInteger(MediaFormat.KEY_SAMPLE_RATE)
            val channels = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
            // Hard cap so a long recording cannot grow the PCM array without bound.
            val capBytes = rate * channels * 2 * MAX_AUDIO_SECONDS

            codec = MediaCodec.createDecoderByType(mime)
            val outFmt = MediaFormat.createAudioFormat(mime, rate, channels).apply {
                setInteger(MediaFormat.KEY_PCM_ENCODING, AudioFormat.ENCODING_PCM_16BIT)
            }
            codec.configure(outFmt, null, null, 0)
            codec.start()

            val sink = java.io.ByteArrayOutputStream(1 shl 16)
            val info = MediaCodec.BufferInfo()
            var eos = false
            while (!eos && sink.size() < capBytes) {
                val inIdx = codec.dequeueInputBuffer(10_000)
                if (inIdx >= 0) {
                    val inBuf = codec.getInputBuffer(inIdx)
                    val size = if (inBuf != null) extractor.readSampleData(inBuf, 0) else -1
                    if (size < 0) {
                        codec.queueInputBuffer(inIdx, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                    } else {
                        codec.queueInputBuffer(inIdx, 0, size, extractor.sampleTime, 0)
                        extractor.advance()
                    }
                }
                val outIdx = codec.dequeueOutputBuffer(info, 10_000)
                if (outIdx >= 0) {
                    val buf = codec.getOutputBuffer(outIdx)
                    if (buf != null && info.size > 0) {
                        val room = capBytes - sink.size()
                        val take = min(info.size, room)
                        val chunk = ByteArray(take)
                        buf.position(info.offset)
                        buf.limit(info.offset + take)
                        buf.get(chunk)
                        sink.write(chunk)
                    }
                    codec.releaseOutputBuffer(outIdx, false)
                    if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) eos = true
                }
            }
            val raw = sink.toByteArray()
            val shorts = ShortArray(raw.size / 2)
            java.nio.ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN).asShortBuffer().get(shorts)
            return Audio(rate, channels, shorts)
        } finally {
            runCatching { codec?.stop() }
            runCatching { codec?.release() }
            extractor.release()
        }
    }

    /** BT.601 YUV_420_888 -> RGB888, nearest-neighbour scaled to outW x outH. */
    private object Yuv420Image {
        /**
         * BT.601 YUV_420_888 -> RGB888 into an *upright* [outW]x[outH] buffer.
         *
         * The decoder emits raw, un-rotated pixels, so the container rotation
         * has to be applied here. Each output pixel maps back to a source
         * coordinate according to the rotation, which also keeps the aspect
         * ratio correct — the previous version scaled a landscape frame into a
         * portrait buffer and produced a stretched, sideways picture.
         */
        fun rgb(image: Image, outW: Int, outH: Int, rotation: Int, out: ByteArray) {
            val srcW = image.width
            val srcH = image.height
            val yP = image.planes[0]
            val uP = image.planes[1]
            val vP = image.planes[2]
            val yB = yP.buffer; val uB = uP.buffer; val vB = vP.buffer
            val yStride = yP.rowStride; val yPix = yP.pixelStride
            val uStride = uP.rowStride; val uPix = uP.pixelStride
            val vStride = vP.rowStride; val vPix = vP.pixelStride

            val rot = ((rotation % 360) + 360) % 360
            var o = 0
            for (y in 0 until outH) {
                for (x in 0 until outW) {
                    val sx: Int
                    val sy: Int
                    when (rot) {
                        // After a quarter turn the rotated frame is srcH wide
                        // and srcW tall, so output x advances along the
                        // source's *rows* and output y along its *columns*.
                        // 90° clockwise: x' = srcH-1-sy, y' = sx.
                        90 -> {
                            sx = (y.toLong() * srcW / outH).toInt().coerceIn(0, srcW - 1)
                            sy = (srcH - 1 - (x.toLong() * srcH / outW).toInt()).coerceIn(0, srcH - 1)
                        }
                        // 180°: both axes reversed.
                        180 -> {
                            sx = (srcW - 1 - (x.toLong() * srcW / outW).toInt()).coerceIn(0, srcW - 1)
                            sy = (srcH - 1 - (y.toLong() * srcH / outH).toInt()).coerceIn(0, srcH - 1)
                        }
                        // 270°: x' = sy, y' = srcW-1-sx.
                        270 -> {
                            sx = (srcW - 1 - (y.toLong() * srcW / outH).toInt()).coerceIn(0, srcW - 1)
                            sy = (x.toLong() * srcH / outW).toInt().coerceIn(0, srcH - 1)
                        }
                        else -> {
                            sx = (x.toLong() * srcW / outW).toInt().coerceIn(0, srcW - 1)
                            sy = (y.toLong() * srcH / outH).toInt().coerceIn(0, srcH - 1)
                        }
                    }
                    val cy = sy / 2
                    val cx = sx / 2
                    val yy = yB.get(sy * yStride + sx * yPix).toInt() and 0xFF
                    val uu = (uB.get(cy * uStride + cx * uPix).toInt() and 0xFF) - 128
                    val vv = (vB.get(cy * vStride + cx * vPix).toInt() and 0xFF) - 128
                    var r = yy + (1.370705f * vv).toInt()
                    var g = yy - (0.337633f * uu).toInt() - (0.698001f * vv).toInt()
                    var b = yy + (1.732446f * uu).toInt()
                    if (r < 0) r = 0 else if (r > 255) r = 255
                    if (g < 0) g = 0 else if (g > 255) g = 255
                    if (b < 0) b = 0 else if (b > 255) b = 255
                    out[o++] = r.toByte(); out[o++] = g.toByte(); out[o++] = b.toByte()
                }
            }
        }
    }
}
