package com.haruhito.qtsq.screens

import android.Manifest
import android.app.Activity
import android.content.pm.PackageManager
import android.opengl.GLSurfaceView
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Check
import androidx.compose.material.icons.rounded.Close
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.ContextCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import com.google.ar.core.ArCoreApk
import com.google.ar.core.Config
import com.google.ar.core.Session
import com.haruhito.qtsq.QtsqEngine
import com.haruhito.qtsq.ar.ARRenderer
import kotlinx.coroutines.*
import java.io.File

/* Training resolution + keyframe-window tunables (Phase C). */
private const val TRAIN_W = 128
private const val TRAIN_H = 96
private const val KF_CAP = 24
private const val T_THRESH = 0.04f       // ~4 cm of camera motion admits a keyframe
private const val COS_THRESH = 0.996f    // ~5° of view rotation admits a keyframe
private const val TRAIN_START_KF = 3     // start training after this many keyframes

/**
 * Phase C — live ARCore scene-capture screen.
 *
 * Owns an ARCore Session (exclusive camera; separate from the CameraX
 * CameraScreen), renders the camera feed + feature points, and streams posed
 * frames into the native keyframe buffer. Non-keyframes are discarded natively —
 * no video is stored, nothing is written to disk in Phase C. Training + save
 * arrive in Phase D on top of this same capture handle.
 */
@Composable
fun SceneCaptureScreen(
    qtsqEngine: QtsqEngine,
    onClose: () -> Unit
) {
    val context = LocalContext.current
    val activity = context as? Activity
    val lifecycleOwner = LocalLifecycleOwner.current

    // ── Permission ──
    var hasCameraPermission by remember {
        mutableStateOf(
            ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) ==
                PackageManager.PERMISSION_GRANTED
        )
    }
    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted -> hasCameraPermission = granted }
    LaunchedEffect(Unit) {
        if (!hasCameraPermission) permissionLauncher.launch(Manifest.permission.CAMERA)
    }

    // ── Capture / session state ──
    val sessionHolder = remember { mutableStateOf<Session?>(null) }
    var captureHandle by remember { mutableStateOf(0L) }
    var statusMessage by remember { mutableStateOf("Preparing AR…") }
    var installRequested by remember { mutableStateOf(false) }

    // HUD state (written from the GL thread; snapshot state is thread-safe).
    var tracking by remember { mutableStateOf(false) }
    var keyframes by remember { mutableIntStateOf(0) }
    var framesSeen by remember { mutableIntStateOf(0) }

    // Phase D: trainer state
    var trainerHandle by remember { mutableStateOf(0L) }
    var trainStep by remember { mutableIntStateOf(0) }
    var trainLoss by remember { mutableStateOf(0f) }
    var trainPsnr by remember { mutableStateOf(0f) }
    var trainSurfelCount by remember { mutableIntStateOf(0) }
    // Phase G: extended trainer stats — pixel coverage, keyframe view-angle
    // spread, and the native inter-step sleep (adaptive thermal throttle).
    var coveragePct by remember { mutableStateOf(0f) }
    var angleSpreadDeg by remember { mutableStateOf(0f) }
    var thermalSleepMs by remember { mutableIntStateOf(0) }
    var isTraining by remember { mutableStateOf(false) }
    val trainScope = rememberCoroutineScope()

    // Native capture handle: created once, freed on dispose.
    DisposableEffect(Unit) {
        captureHandle = qtsqEngine.captureInit(TRAIN_W, TRAIN_H, KF_CAP, T_THRESH, COS_THRESH)
        onDispose {
            if (trainerHandle != 0L) {
                qtsqEngine.captureTrainSetRunning(trainerHandle, false)
                qtsqEngine.captureTrainFree(trainerHandle)
                trainerHandle = 0L
            }
            if (captureHandle != 0L) {
                qtsqEngine.captureFree(captureHandle)
                captureHandle = 0L
            }
        }
    }

    var hint by remember { mutableStateOf("Move the phone slowly to start") }

    // Current display rotation (Surface.ROTATION_0/90/180/270) for ARCore's
    // setDisplayGeometry. Read on the GL thread each time the viewport changes.
    fun currentDisplayRotation(): Int {
        val display = if (android.os.Build.VERSION.SDK_INT >= 30) {
            context.display
        } else {
            @Suppress("DEPRECATION")
            (context.getSystemService(android.content.Context.WINDOW_SERVICE)
                as android.view.WindowManager).defaultDisplay
        }
        return display?.rotation ?: android.view.Surface.ROTATION_0
    }

    // Phase D: save state
    var saveStatus by remember { mutableStateOf<String?>(null) }
    var isSaving by remember { mutableStateOf(false) }

    val renderer = remember {
        ARRenderer(
            engine = qtsqEngine,
            trainW = TRAIN_W,
            trainH = TRAIN_H,
            sessionProvider = { sessionHolder.value },
            captureHandleProvider = { captureHandle },
            displayRotationProvider = { currentDisplayRotation() },
            trainerHandleProvider = { trainerHandle },
            surfelProvider = {
                if (trainerHandle != 0L) qtsqEngine.captureGetSurfels(trainerHandle) else null
            },
            onStats = { t, kf, seen, h -> tracking = t; keyframes = kf; framesSeen = seen; hint = h }
        )
    }

    val glView = remember {
        GLSurfaceView(context).apply {
            preserveEGLContextOnPause = true
            setEGLContextClientVersion(2)
            setEGLConfigChooser(8, 8, 8, 8, 16, 0)
            setRenderer(renderer)
            renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
        }
    }

    // Tracks whether the live session is currently resumed, so resume()/pause()
    // are never called twice in a row (ARCore throws on that).
    var sessionResumed by remember { mutableStateOf(false) }

    // Creates the ARCore session if needed, then resumes it. Safe to call
    // repeatedly. Returns true when a live, resumed session is running.
    fun startSession(): Boolean {
        if (!hasCameraPermission || activity == null) return false
        if (sessionHolder.value == null) {
            try {
                when (ArCoreApk.getInstance().requestInstall(activity, !installRequested)) {
                    ArCoreApk.InstallStatus.INSTALL_REQUESTED -> {
                        installRequested = true
                        statusMessage = "Installing AR services…"
                        return false
                    }
                    ArCoreApk.InstallStatus.INSTALLED -> { /* proceed */ }
                }
                val session = Session(activity)
                val config = Config(session).apply {
                    focusMode = Config.FocusMode.AUTO
                    updateMode = Config.UpdateMode.LATEST_CAMERA_IMAGE
                    depthMode = Config.DepthMode.DISABLED
                    planeFindingMode = Config.PlaneFindingMode.DISABLED
                }
                session.configure(config)
                sessionHolder.value = session
                statusMessage = "Move slowly around the object"
            } catch (e: Exception) {
                statusMessage = "AR unavailable: ${e.javaClass.simpleName}"
                return false
            }
        }
        if (sessionResumed) return true
        return try {
            sessionHolder.value?.resume()
            sessionResumed = true
            glView.onResume()
            true
        } catch (e: Exception) {
            statusMessage = "AR resume failed: ${e.javaClass.simpleName}"
            sessionHolder.value?.close()
            sessionHolder.value = null
            sessionResumed = false
            false
        }
    }

    fun pauseSession() {
        if (!sessionResumed) return
        glView.onPause()
        sessionHolder.value?.pause()
        sessionResumed = false
    }

    // Start the session as soon as the screen is shown and permission is
    // granted. Compose navigation does NOT fire an Activity ON_RESUME, so we
    // cannot rely on the lifecycle observer alone to bring the camera up.
    LaunchedEffect(hasCameraPermission) {
        if (hasCameraPermission) startSession()
    }

    // ── ARCore session lifecycle: pause/resume the existing session ──
    DisposableEffect(lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_RESUME -> startSession()
                Lifecycle.Event.ON_PAUSE -> pauseSession()
                else -> {}
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose {
            lifecycleOwner.lifecycle.removeObserver(observer)
            pauseSession()
            sessionHolder.value?.close()
            sessionHolder.value = null
        }
    }

    // Phase D: Start the GPU trainer (seeded from the ARCore sparse point
    // cloud) once enough keyframes are in, then run the native train loop.
    LaunchedEffect(keyframes) {
        if (keyframes >= TRAIN_START_KF && trainerHandle == 0L && captureHandle != 0L) {
            val th = qtsqEngine.captureTrainStart(captureHandle, renderer.latestPointsXyz, null)
            if (th != 0L) {
                trainerHandle = th
                isTraining = true
                qtsqEngine.captureTrainSetRunning(th, true)
            } else {
                statusMessage = "GPU trainer failed to start"
            }
        }
    }

    // HUD polling: read trainer stats while the native loop runs.
    LaunchedEffect(isTraining) {
        while (isActive && isTraining && trainerHandle != 0L) {
            try {
                val result = qtsqEngine.captureTrainStats(trainerHandle)
                if (result != null && result.size >= 7) {
                    trainStep = result[0].toInt()
                    trainLoss = result[1]
                    trainPsnr = result[2]
                    trainSurfelCount = result[3].toInt()
                    coveragePct = result[4]
                    angleSpreadDeg = result[5]
                    thermalSleepMs = result[6].toInt()
                }
                delay(300)
            } catch (e: Exception) {
                break
            }
        }
        isTraining = false
    }

    // Save the trained model via the fail-closed secure write path.
    // Requires the device keystore (device keypair) — initialized on demand,
    // then captured as an encrypted .qtsq container in the app files dir.
    fun saveAndClose() {
        if (isSaving) return
        val th = trainerHandle
        if (th == 0L || captureHandle == 0L) {
            onClose()
            return
        }
        isSaving = true
        saveStatus = null
        trainScope.launch(Dispatchers.Default) {
            try {
                qtsqEngine.captureTrainSetRunning(th, false)
                // Ensure the hardware-backed keystore (JNI bridge + keypair).
                // Also pins HOME to app-private storage (required on Android).
                if (!qtsqEngine.ensureKeystoreReady(context)) {
                    withContext(Dispatchers.Main) {
                        isSaving = false
                        saveStatus = "Keystore unavailable — cannot save encrypted scene"
                    }
                    return@launch
                }

                val dir = File(context.filesDir, "captures").apply { mkdirs() }
                val out = File(dir, "scene_${System.currentTimeMillis()}.qtsq")
                val rc = qtsqEngine.captureTrainSave(th, out.absolutePath)
                withContext(Dispatchers.Main) {
                    isSaving = false
                    isTraining = false
                    saveStatus = if (rc == 0) "Saved encrypted: ${out.name}" else "Save failed (rc=$rc)"
                }
            } catch (e: Exception) {
                withContext(Dispatchers.Main) {
                    isSaving = false
                    saveStatus = "Save failed: ${e.javaClass.simpleName}"
                }
            }
        }
    }

    // Phase G: sweep guidance driven by the live coverage / angle metrics.
    // Coverage builds by moving around the object; angle spread grows by
    // sweeping to distinct view directions. Once both are healthy, hold still.
    fun trainingGuidance(): String = when {
        coveragePct < 50f -> "Move slowly around the object to build coverage"
        coveragePct < 75f -> "Keep moving — coverage building (add low angles)"
        angleSpreadDeg < 60f -> "Good coverage — sweep for more view angles"
        else -> "Looks good — hold still, refining…"
    }

    // Phase G: a failed save keeps the button in "Retry Save" mode (label +
    // click both re-save); only a successful save turns it into "Close".
    val saveFailed = saveStatus?.startsWith("Save failed") == true

    // ── UI ──
    Box(Modifier.fillMaxSize().background(Color.Black)) {
        if (hasCameraPermission) {
            AndroidView(factory = { glView }, modifier = Modifier.fillMaxSize())
        } else {
            Column(
                Modifier.fillMaxSize(),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.Center
            ) {
                Text("Camera permission is required", color = Color.White)
                Spacer(Modifier.height(16.dp))
                Button(onClick = { permissionLauncher.launch(Manifest.permission.CAMERA) }) {
                    Text("Grant Permission")
                }
            }
        }

        // Top bar: close + live status
        Row(
            Modifier.fillMaxWidth().statusBarsPadding().padding(16.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Box(
                Modifier.size(44.dp).clip(CircleShape)
                    .background(Color.Black.copy(alpha = 0.45f)),
                contentAlignment = Alignment.Center
            ) {
                IconButton(onClick = onClose) {
                    Icon(Icons.Rounded.Close, contentDescription = "Close", tint = Color.White)
                }
            }
            Spacer(Modifier.width(12.dp))
            val dot = if (tracking) Color(0xFF34D399) else Color(0xFFF59E0B)
            val sessionLive = sessionHolder.value != null
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(Modifier.size(10.dp).clip(CircleShape).background(dot))
                Spacer(Modifier.width(8.dp))
                Text(
                    when {
                        tracking -> "Tracking"
                        sessionLive -> hint
                        else -> statusMessage
                    },
                    color = Color.White, fontSize = 14.sp, fontWeight = FontWeight.Medium
                )
            }
        }

        // Bottom HUD: keyframe count + training stats + guidance + Done
        Column(
            Modifier.align(Alignment.BottomCenter).fillMaxWidth()
                .background(Color.Black.copy(alpha = 0.45f))
                .navigationBarsPadding().padding(20.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Text(
                "$keyframes / $KF_CAP views captured",
                color = Color.White, fontSize = 22.sp, fontWeight = FontWeight.Bold
            )
            Spacer(Modifier.height(10.dp))
            LinearProgressIndicator(
                progress = { keyframes.toFloat() / KF_CAP },
                modifier = Modifier.fillMaxWidth().height(6.dp).clip(RoundedCornerShape(3.dp)),
                color = Color(0xFF34D399),
                trackColor = Color.White.copy(alpha = 0.18f)
            )
            // Phase G: training-only coverage progress (fraction of training
            // pixels with alpha > 0.5 in the latest rendered frame).
            if (isTraining) {
                Spacer(Modifier.height(6.dp))
                LinearProgressIndicator(
                    progress = { coveragePct / 100f },
                    modifier = Modifier.fillMaxWidth().height(6.dp).clip(RoundedCornerShape(3.dp)),
                    color = Color(0xFFF59E0B),
                    trackColor = Color.White.copy(alpha = 0.18f)
                )
            }

            // Phase D: Training stats
            if (isTraining && trainStep > 0) {
                Spacer(Modifier.height(12.dp))
                Text(
                    "Training: step $trainStep · ${trainSurfelCount} surfels",
                    color = Color(0xFF34D399), fontSize = 14.sp, fontWeight = FontWeight.Medium
                )
                Text(
                    "PSNR: ${"%.1f".format(trainPsnr)} dB · Loss: ${"%.4f".format(trainLoss)}",
                    color = Color.White.copy(alpha = 0.8f), fontSize = 12.sp
                )
                // Phase G: live coverage + keyframe view-angle spread.
                Text(
                    "Coverage: ${"%.1f".format(coveragePct)}% · Angles: ${angleSpreadDeg.toInt()}°",
                    color = Color.White.copy(alpha = 0.8f), fontSize = 12.sp
                )
            }

            Spacer(Modifier.height(8.dp))
            Text(
                if (isTraining) trainingGuidance()
                else "Distinct camera angles kept for the 3D reconstruction.",
                color = Color.White.copy(alpha = 0.75f), fontSize = 12.sp
            )
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    "$framesSeen frames scanned · duplicates discarded, no video saved",
                    color = Color.White.copy(alpha = 0.55f), fontSize = 11.sp
                )
                // Phase G: native thermal throttle kicked in — note it in amber,
                // appended to the frames-scanned line.
                if (thermalSleepMs >= 40) {
                    Text(
                        " · thermal throttling",
                        color = Color(0xFFF59E0B), fontSize = 11.sp
                    )
                }
            }
            // Save status (encrypted .qtsq written via the secure path)
            if (saveStatus != null) {
                // Phase G: final summary on success (stats retained on failure).
                if (saveStatus!!.startsWith("Saved")) {
                    Spacer(Modifier.height(10.dp))
                    Text(
                        "Final: ${"%.1f".format(trainPsnr)} dB · $trainSurfelCount surfels · " +
                            "${"%.1f".format(coveragePct)}% covered",
                        color = Color.White.copy(alpha = 0.85f), fontSize = 13.sp,
                        fontWeight = FontWeight.Medium
                    )
                }
                Spacer(Modifier.height(10.dp))
                Text(
                    saveStatus!!,
                    color = if (saveStatus!!.startsWith("Saved")) Color(0xFF34D399) else Color(0xFFF59E0B),
                    fontSize = 12.sp
                )
            }

            Spacer(Modifier.height(16.dp))
            Button(
                onClick = {
                    when {
                        saveStatus == null -> saveAndClose()
                        saveFailed -> saveAndClose()   // retry the failed save
                        else -> onClose()
                    }
                },
                enabled = keyframes > 0 && !isSaving,
                modifier = Modifier.fillMaxWidth().height(52.dp),
                shape = RoundedCornerShape(20.dp)
            ) {
                if (isSaving) {
                    Text("Saving…", fontSize = 16.sp, fontWeight = FontWeight.SemiBold)
                } else {
                    Icon(Icons.Rounded.Check, contentDescription = null)
                    Spacer(Modifier.width(8.dp))
                    Text(
                        when {
                            saveStatus == null -> "Done – Save Model"
                            saveFailed -> "Retry Save"
                            else -> "Close"
                        },
                        fontSize = 16.sp, fontWeight = FontWeight.SemiBold
                    )
                }
            }
        }
    }
}
