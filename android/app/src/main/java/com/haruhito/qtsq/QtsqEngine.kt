package com.haruhito.qtsq

/**
 * Kotlin wrapper for the QTSQ C Engine via JNI.
 */
class QtsqEngine {

    companion object {
        // Bumped every sync round so the installed build is identifiable
        // from inside the app (shown in diagnostics).
        const val BUILD_TAG = "0.1.0-beta+sync7-video"

        // Phase 7: container codec preference (qtc default, zlib legacy
        // opt-out). Values mirror QTSQ_CODEC_* in include/qtsq_format.h.
        const val CODEC_QTC = 1
        const val CODEC_ZLIB = 0

        // Strategy flags mirroring include/qtsq.h
        const val VIDEO_PIXEL = 0x20000
        const val VIDEO_CONTINUOUS = 0x10000
        private const val PREFS_NAME = "qtsq_settings"
        private const val KEY_CODEC = "codec"

        fun loadCodecPref(context: android.content.Context): Int =
            context.getSharedPreferences(PREFS_NAME, android.content.Context.MODE_PRIVATE)
                .getInt(KEY_CODEC, CODEC_QTC)

        fun saveCodecPref(context: android.content.Context, codec: Int) {
            context.getSharedPreferences(PREFS_NAME, android.content.Context.MODE_PRIVATE)
                .edit().putInt(KEY_CODEC, codec).apply()
        }

        /** Human-readable name of the currently selected container codec. */
        fun codecName(context: android.content.Context): String =
            if (loadCodecPref(context) == CODEC_QTC) "qtc" else "zlib"

        init {
            System.loadLibrary("qtsq")
        }
    }

    /**
     * Compress raw byte array directly to QTSQ using the C engine.
     * @param input Raw bytes (e.g. from a File or CameraX proxy buffer)
     * @param type "text" selects the text codec; any other value is stored as raw bytes
     * @param password Optional encryption password
     * @return Byte array of the .qtsq file content
     */
    external fun compressBytes(input: ByteArray, type: String, password: String? = null): ByteArray?

    /**
     * Compress structured image data (Accretion Engine).
     * @param pixels Raw pixel buffer
     * @param width Image width
     * @param height Image height
     * @param channels Image channels (e.g., 3 for RGB, 4 for RGBA)
     * @return Byte array of the .qtsq file content
     */
    external fun compressImage(pixels: ByteArray, width: Int, height: Int, channels: Int, password: String? = null, sourceSize: Long = 0L): ByteArray?

    /**
     * Compress image in HD mode (up to 3M vertices, best quality).
     * Same args as compressImage but uses Gravitational HD engine.
     */
    external fun compressImageHD(
        pixels: ByteArray,
        width: Int,
        height: Int,
        channels: Int,
        password: String? = null,
        sourceSize: Long = 0L
    ): ByteArray?

    /**
     * Triangle Only Lossless mode.
     * Uses Adaptive Recursive Subdivision for extreme detail without noise.
     */
    external fun compressImageLosslessTriangle(
        pixels: ByteArray,
        width: Int,
        height: Int,
        channels: Int,
        password: String? = null,
        sourceSize: Long = 0L
    ): ByteArray?

    /**
     * Pixel-based format encapsulating pre-compressed JPEG bytes for smaller file size on Android.
     */
    external fun compressPixelJpeg(
        jpegBytes: ByteArray,
        password: String? = null
    ): ByteArray?

    /**
     * Mathematical quality upscale for lossy inputs (JPEG/WebP).
     * Uses dense triangle mesh to reconstruct clean edges from compression artifacts.
     * Output is LARGER but HIGHER QUALITY than input.
     */
    external fun compressImageUpscale(
        pixels: ByteArray,
        width: Int,
        height: Int,
        channels: Int,
        password: String? = null,
        sourceSize: Long = 0L
    ): ByteArray?

    /**
     * Lossless HD mode — maximum fidelity with up to 6M vertices.
     * Output caps: <5MB input → ≤10MB, ≥5MB input → ≤30MB.
     */
    external fun compressImageLossless(
        pixels: ByteArray,
        width: Int,
        height: Int,
        channels: Int,
        password: String? = null,
        sourceSize: Long = 0L
    ): ByteArray?

    /**
     * Pure Splat Only — Compact mode (50K-500K Gaussian splats).
     * Smallest file size using splat-only representation.
     */
    external fun compressSplatCompact(
        pixels: ByteArray,
        width: Int,
        height: Int,
        channels: Int,
        password: String? = null,
        sourceSize: Long = 0L
    ): ByteArray?

    /**
     * Pure Splat Only — HD mode (400K-1M Gaussian splats).
     * Best quality splat-only representation.
     */
    external fun compressSplatHD(
        pixels: ByteArray,
        width: Int,
        height: Int,
        channels: Int,
        password: String? = null,
        sourceSize: Long = 0L
    ): ByteArray?

    /**
     * Pure Splat Only — Lossless mode (1M-2M Gaussian splats).
     * Maximum fidelity splat-only representation.
     */
    external fun compressSplatLossless(
        pixels: ByteArray,
        width: Int,
        height: Int,
        channels: Int,
        password: String? = null,
        sourceSize: Long = 0L
    ): ByteArray?

    /**
     * Compress structured audio data (Lensing Engine).
     * @param pcm 16-bit PCM audio samples
     * @param sampleRate Sample rate (e.g., 44100)
     * @param channels Audio channels (e.g., 1 for Mono, 2 for Stereo)
     * @return Byte array of the .qtsq file content
     */
    external fun compressAudio(pcm: ShortArray, sampleRate: Int, channels: Int, password: String? = null): ByteArray?

    /**
     * Compress audio in HQ mode (Photon Gabor spatial atoms, higher fidelity).
     * @param pcm 16-bit PCM audio samples
     * @param sampleRate Sample rate (e.g., 44100)
     * @param channels Audio channels
     * @return Byte array of the .qtsq file content
     */
    external fun compressAudioHQ(pcm: ShortArray, sampleRate: Int, channels: Int, password: String? = null): ByteArray?

    /**
     * Decompress a .qtsq file back to its original format.
     * @param qtsqBytes The bytes of the .qtsq file
     * @param password Optional decryption password
     * @return QtsqResult containing the raw bytes and metadata
     */
    external fun decompressBytes(qtsqBytes: ByteArray, password: String? = null): QtsqResult?

    /**
     * Convert Android ARGB_8888 pixels (from Bitmap.getPixels) to packed RGB
     * bytes for the compress path. Native fast path replacing the per-pixel
     * Kotlin loops.
     * @param argb ARGB pixel ints (one per pixel)
     * @return RGB bytes (3 per pixel), or null on failure
     */
    external fun argbToRgb(argb: IntArray): ByteArray?

    /**
     * Toggles between GPU (Vulkan) and CPU (rasterize_strip) rendering engines.
     * @param useGpu True for GPU, False for CPU.
     */
    external fun setUseGpu(useGpu: Boolean)

    /**
     * Sets the container codec for subsequent compressions (process-wide).
     * Decode always sniffs the stream magic, so this only affects new writes.
     * @param codec CODEC_QTC (default) or CODEC_ZLIB (legacy opt-out).
     */
    external fun setCodec(codec: Int)

    // --- Phase C: Live ARCore surfel capture (posed-frame keyframe buffer) ---

    /**
     * Initialize a live capture session. Returns a native handle (0 on failure).
     * @param width Training resolution width (downscaled camera frame)
     * @param height Training resolution height
     * @param keyframeCap Bounded keyframe window size (older frames evicted)
     * @param translationThresh World-units of camera motion to admit a keyframe
     * @param cosThresh Forward-axis dot below which rotation admits a keyframe
     */
    external fun captureInit(
        width: Int,
        height: Int,
        keyframeCap: Int,
        translationThresh: Float,
        cosThresh: Float
    ): Long

    /**
     * Offer one posed frame to the capture session.
     * @param handle Native handle from captureInit
     * @param poseMatrix float[16] column-major camera→world (ARCore Pose.toMatrix)
     * @param intrinsics float[4] = {fx, fy, cx, cy} scaled into width×height space
     * @param rgb byte[width*height*3] R,G,B (0..255)
     * @return 1 if admitted as a keyframe (kept), 0 if discarded
     */
    external fun captureObserve(
        handle: Long,
        poseMatrix: FloatArray,
        intrinsics: FloatArray,
        rgb: ByteArray
    ): Int

    /** Returns int[3] = {live keyframe count, capacity, total frames seen}. */
    external fun captureStats(handle: Long): IntArray

    /** Release the capture session and its keyframe window. */
    external fun captureFree(handle: Long)

    // --- Keystore (device keypair for encrypted saves) ---

    /** Register the AndroidKeyStore JNI bridge (encrypts the C keystore key file). */
    external fun nativeInitKeystore()

    /** Initialize the device keystore (first run generates the keypair). Returns 0 on success. */
    external fun nativeKeystoreInit(): Int

    /**
     * Run the exact camera-save pipeline in miniature (compress → keystore
     * encrypt → serialize → parse → keystore decrypt → decompress) and return
     * a diagnostic string: "HOME=…; ks=ok; enc=v3-pq; OK roundtrip" or
     * "FAIL@<stage> rc=<code> (<message>)".
     */
    external fun nativeSelfTest(): String

    /** Latest on-device save-pipeline self-test result (for diagnostics UI). */
    var lastSelfTest: String = "not run"
        private set

    /**
     * Ensure the device keystore is ready before any compress/save call.
     * Both natives are idempotent (re-init loads existing keys), so this is
     * cheap to call before every save. The C engine is fail-closed: without
     * a keystore (and without a password) every serialization is refused,
     * which surfaced as "Compression failed" / photos never saving.
     *
     * CRITICAL: the native keystore resolves its key directory as
     * `$HOME/.qtsq/keys`, but Android apps have no HOME env var — the C
     * code then falls back to a RELATIVE path (CWD = "/", read-only) and
     * every keystore write fails. We therefore pin HOME to the app's
     * private files dir before touching the keystore.
     */
    fun ensureKeystoreReady(context: android.content.Context? = null): Boolean {
        return try {
            if (context != null) {
                try {
                    android.system.Os.setenv("HOME", context.filesDir.absolutePath, true)
                } catch (_: Exception) { /* best effort */ }
            }
            nativeInitKeystore()
            val rc = nativeKeystoreInit()
            if (rc != 0) android.util.Log.e("QtsqEngine", "nativeKeystoreInit failed rc=$rc")
            if (rc == 0) {
                // Prove the full save pipeline works on THIS device right now.
                try {
                    lastSelfTest = nativeSelfTest()
                    android.util.Log.i("QtsqEngine", "selftest: $lastSelfTest")
                } catch (e: Exception) {
                    android.util.Log.e("QtsqEngine", "selftest threw: ${e.message}")
                }
            }
            rc == 0
        } catch (e: Exception) {
            android.util.Log.e("QtsqEngine", "ensureKeystoreReady failed: ${e.message}")
            false
        } catch (e: UnsatisfiedLinkError) {
            android.util.Log.e("QtsqEngine", "native lib not loaded: ${e.message}")
            false
        }
    }

    // --- Phase D: GPU Surfel Trainer ---

    /**
     * Start the GPU trainer on a capture session. Returns trainer handle (0 on failure).
     * @param pointsXyz ARCore sparse point cloud (float[n*3], world coords) used to seed
     *                  the surfel map; pass null to fall back to random seeding.
     * @param pointsRgb Optional per-point color (float[n*3] in [0,1]); null = neutral.
     */
    external fun captureTrainStart(
        captureHandle: Long,
        pointsXyz: FloatArray?,
        pointsRgb: FloatArray?
    ): Long

    /** Start (true) or stop (false) the background GPU train loop (Phase D §3). */
    external fun captureTrainSetRunning(trainerHandle: Long, running: Boolean)

    /** Get trainer stats. Returns float[4] = {step, loss, psnr, surfel_count}. */
    external fun captureTrainStats(trainerHandle: Long): FloatArray?

    /** Get surfel data for GL preview. Returns float[n*13] or null. */
    external fun captureGetSurfels(trainerHandle: Long): FloatArray?

    /** Save the trained model to a .qtsq file. Returns 0 on success. */
    external fun captureTrainSave(trainerHandle: Long, path: String): Int

    /** Destroy the trainer and free GPU resources. */
    external fun captureTrainFree(trainerHandle: Long)

    // ── Video (pixel path) ────────────────────────────────────────────
    /** Create a video encoder. Returns a handle, or 0 on failure. */
    external fun videoEncodeCreate(
        width: Int, height: Int, channels: Int, fps: Int,
        flags: Int, targetPayload: Long,
        quality: Float = 0.75f
    ): Long

    /** Append one RGB frame. Returns an error code (0 = ok). */
    external fun videoEncodeAddFrame(handle: Long, pixels: ByteArray): Int

    /**
     * Attach an audio track encoded with Classic Gravitational
     * (1D GFC + Wendland). Deliberately not Photon/HQ and not Opus.
     * pcm is interleaved Int16; sampleCount counts all values.
     */
    external fun videoEncodeSetAudio(
        handle: Long, pcm: ShortArray, sampleRate: Int, channels: Int
    ): Int

    /** Finish the encode and serialize to .qtsq bytes. Call once. */
    external fun videoEncodeFinish(handle: Long): ByteArray?

    /** Release the encoder handle. */
    external fun videoEncoderFree(handle: Long)

    /** Open a .qtsq for video playback. Returns a decoder handle, or 0. */
    external fun videoDecoderOpen(bytes: ByteArray, password: String? = null): Long

    /** Returns int[4] = {frameCount, width, height, fps}. */
    external fun videoDecoderInfo(handle: Long): IntArray?

    /** Encoded file size in bytes, or -1. */
    external fun videoDecoderFileSize(handle: Long): Long

    /** Codec label from the schema, e.g. "PIXEL" or "4DGS". */
    external fun videoDecoderEncoding(handle: Long): String?

    /** Frame type at an index: 'K' keyframe, 'P'/'p'/'q' predicted, 0 if none. */
    external fun videoDecoderFrameType(handle: Long, index: Int): Byte

    /** Decode one frame as ARGB_8888 bytes, ready for Bitmap.copyPixelsFromBuffer. */
    external fun videoDecoderFrame(handle: Long, index: Int): ByteArray?

    /** True when the file carries a Classic Gravitational audio track. */
    external fun videoDecoderHasAudio(handle: Long): Boolean

    /**
     * Decode the audio track to a complete mono WAV.
     * Returns arrayOf(wavBytes, sampleRate, channels), or null.
     */
    external fun videoDecoderAudio(handle: Long): Array<Any>?

    /** Release the decoder handle. */
    external fun videoDecoderFree(handle: Long)

    /**
     * Read only the stored QTSQ data type (0x30 = video frames, 0x10..0x12 =
     * image, 0x20..0x22 = audio) without decompressing. Returns -1 if the
     * bytes cannot be parsed or decrypted. Lets the viewer pick the right
     * surface before attempting a decode.
     */
    external fun peekDataType(bytes: ByteArray, password: String? = null): Int

    data class QtsqResult(
        val data: ByteArray,
        val type: Int,
        val originalSize: Long,
        val width: Int = 0,
        val height: Int = 0,
        val audioSampleRate: Int = 0,
        val encoding: String = "",
        val vertexCount: Int = 0,
        val faceCount: Int = 0,
        val splatCount: Int = 0
    )
}
