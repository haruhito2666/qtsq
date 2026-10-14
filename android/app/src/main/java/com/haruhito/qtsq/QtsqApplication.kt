package com.haruhito.qtsq

import android.app.Application
import android.util.Log
import com.clerk.api.Clerk

class QtsqApplication : Application() {
    override fun onCreate() {
        super.onCreate()

        // ── QTSQ device keystore (must run before ANY compress/save) ──
        // The C engine is fail-closed (Phase 1): context_to_bytes refuses to
        // serialize an unencrypted context. ensureKeystoreReady also pins
        // HOME to app-private storage — without it the native keystore
        // writes to a read-only relative path and every save fails.
        try {
            val engine = QtsqEngine()
            val ok = engine.ensureKeystoreReady(applicationContext)
            Log.i("QtsqApplication", "QTSQ keystore ready=$ok")
            // Phase 7: apply the persisted codec preference (qtc default) so it
            // is active process-wide before any compress/save runs.
            engine.setCodec(QtsqEngine.loadCodecPref(applicationContext))
        } catch (e: Exception) {
            Log.e("QtsqApplication", "QTSQ keystore init failed: ${e.message}")
        }

        // Connected to getqtsq.xyz live Clerk instance
        val clerkPublishableKey = "pk_live_Y2xlcmsuZ2V0cXRzcS54eXok"

        try {
            Clerk.initialize(
                this,
                clerkPublishableKey
            )
            Log.i("QtsqApplication", "Clerk initialized successfully")
        } catch (e: Exception) {
            Log.e("QtsqApplication", "Failed to initialize Clerk: ${e.message}")
        }
    }
}
