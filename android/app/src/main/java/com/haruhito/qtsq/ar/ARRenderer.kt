package com.haruhito.qtsq.ar

import android.opengl.GLES20
import android.opengl.GLSurfaceView
import android.opengl.Matrix
import com.google.ar.core.Session
import com.google.ar.core.TrackingFailureReason
import com.google.ar.core.TrackingState
import com.google.ar.core.exceptions.NotYetAvailableException
import com.haruhito.qtsq.QtsqEngine
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

/**
 * GLSurfaceView renderer for the Phase C live capture screen.
 *
 * Per frame: draws the ARCore camera background + feature-point cloud, then
 * extracts the camera pose + intrinsics + a downscaled RGB frame and offers it
 * to the native keyframe buffer (QtsqEngine.captureObserve). Non-keyframes are
 * dropped natively — nothing is stored.
 */
class ARRenderer(
    private val engine: QtsqEngine,
    private val trainW: Int,
    private val trainH: Int,
    private val sessionProvider: () -> Session?,
    private val captureHandleProvider: () -> Long,
    private val displayRotationProvider: () -> Int,
    private val trainerHandleProvider: () -> Long,
    private val surfelProvider: () -> FloatArray?,
    private val onStats: (tracking: Boolean, keyframes: Int, framesSeen: Int, hint: String) -> Unit
) : GLSurfaceView.Renderer {

    private val background = BackgroundRenderer()
    private val pointCloud = PointCloudRenderer()
    private val surfelPreview = SurfelPreviewRenderer()

    /**
     * Latest ARCore sparse point cloud (float[n*3] world coords), harvested on
     * the GL thread each tracking frame. The Phase D trainer seeds its surfel
     * map from this. Read the reference once from the UI thread when starting
     * the trainer (a stale snapshot is fine for seeding).
     */
    @Volatile var latestPointsXyz: FloatArray? = null

    // Viewport, pushed to ARCore via setDisplayGeometry so it can compute the
    // camera-image → screen texture transform. Without this the background quad
    // has no valid texture coordinates and renders as a single flat color.
    private var viewportWidth = 0
    private var viewportHeight = 0
    @Volatile private var viewportChanged = false

    private val projMatrix = FloatArray(16)
    private val viewMatrix = FloatArray(16)
    private val mvpMatrix = FloatArray(16)
    private val poseMatrix = FloatArray(16)
    private val intrinsics = FloatArray(4)
    private val rgb = ByteArray(trainW * trainH * 3)

    @Volatile private var textureBound = false

    override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
        GLES20.glClearColor(0f, 0f, 0f, 1f)
        background.createOnGlThread()
        pointCloud.createOnGlThread()
        surfelPreview.createOnGlThread()
        textureBound = false
    }

    override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
        GLES20.glViewport(0, 0, width, height)
        viewportWidth = width
        viewportHeight = height
        viewportChanged = true
    }

    override fun onDrawFrame(gl: GL10?) {
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT or GLES20.GL_DEPTH_BUFFER_BIT)
        val session = sessionProvider() ?: return

        try {
            if (!textureBound) {
                session.setCameraTextureName(background.textureId)
                textureBound = true
            }
            // Tell ARCore the current viewport so it can map the camera image
            // onto the screen. Must happen before update(), and again whenever
            // the surface size changes.
            if (viewportChanged && viewportWidth > 0 && viewportHeight > 0) {
                session.setDisplayGeometry(displayRotationProvider(), viewportWidth, viewportHeight)
                viewportChanged = false
            }
            val frame = session.update()
            background.draw(frame)

            val camera = frame.camera
            val tracking = camera.trackingState == TrackingState.TRACKING
            val hint = if (tracking) "" else trackingHint(camera.trackingFailureReason)

            if (tracking) {
                camera.getProjectionMatrix(projMatrix, 0, 0.1f, 100f)
                camera.getViewMatrix(viewMatrix, 0)
                Matrix.multiplyMM(mvpMatrix, 0, projMatrix, 0, viewMatrix, 0)

                val pc = frame.acquirePointCloud()
                try {
                    pointCloud.draw(pc, mvpMatrix)
                    harvestPoints(pc)
                } finally {
                    pc.release()
                }

                // Phase D §5: live surfel preview — draw the trainer's model
                // on top once training is running (replaces the point cloud
                // role; we keep both on for now so the points seed is visible).
                val th = trainerHandleProvider()
                if (th != 0L) {
                    val surfels = surfelProvider()
                    if (surfels != null && surfels.size >= 13) {
                        surfelPreview.draw(surfels, mvpMatrix, viewMatrix)
                    }
                }

                offerFrame(frame, camera)
            }

            val handle = captureHandleProvider()
            var kf = 0
            var seen = 0
            if (handle != 0L) {
                val stats = engine.captureStats(handle)
                if (stats.size >= 3) { kf = stats[0]; seen = stats[2] }
            }
            onStats(tracking, kf, seen, hint)
        } catch (t: Throwable) {
            // Never let a transient AR error crash the GL thread.
        }
    }

    /** Copy the sparse point cloud (x,y,z,confidence) → xyz array for seeding. */
    private fun harvestPoints(pc: com.google.ar.core.PointCloud) {
        val points = pc.points
        val n = points.remaining() / 4
        if (n <= 0) return
        val xyz = FloatArray(n * 3)
        points.position(0)
        var i = 0
        while (i < n) {
            xyz[i * 3] = points.get()
            xyz[i * 3 + 1] = points.get()
            xyz[i * 3 + 2] = points.get()
            points.get() // confidence
            i++
        }
        latestPointsXyz = xyz
    }

    /** Human-readable guidance for why ARCore isn't tracking yet. */
    private fun trackingHint(reason: TrackingFailureReason?): String = when (reason) {
        TrackingFailureReason.INSUFFICIENT_LIGHT -> "Too dark — find more light"
        TrackingFailureReason.EXCESSIVE_MOTION -> "Slow down"
        TrackingFailureReason.INSUFFICIENT_FEATURES -> "Point at a textured object"
        TrackingFailureReason.CAMERA_UNAVAILABLE -> "Camera unavailable"
        else -> "Move the phone slowly to start"
    }

    private fun offerFrame(frame: com.google.ar.core.Frame, camera: com.google.ar.core.Camera) {
        val handle = captureHandleProvider()
        if (handle == 0L) return
        val image = try {
            frame.acquireCameraImage()
        } catch (e: NotYetAvailableException) {
            return
        } catch (e: Throwable) {
            return
        }
        try {
            YuvToRgb.downscale(image, trainW, trainH, rgb)

            val intr = camera.imageIntrinsics
            val focal = intr.focalLength           // [fx, fy]
            val principal = intr.principalPoint     // [cx, cy]
            val dims = intr.imageDimensions         // [w, h]
            val sxr = trainW.toFloat() / dims[0]
            val syr = trainH.toFloat() / dims[1]
            intrinsics[0] = focal[0] * sxr
            intrinsics[1] = focal[1] * syr
            intrinsics[2] = principal[0] * sxr
            intrinsics[3] = principal[1] * syr

            camera.pose.toMatrix(poseMatrix, 0)     // column-major camera→world

            engine.captureObserve(handle, poseMatrix, intrinsics, rgb)
        } finally {
            image.close()
        }
    }
}
