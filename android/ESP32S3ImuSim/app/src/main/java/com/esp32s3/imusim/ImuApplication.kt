package com.esp32s3.imusim

import android.app.Application
import android.content.Intent
import android.util.Log

/** Start cloud/bridge schedulers without opening MainActivity. */
class ImuApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        AutopilotRelay.bootstrap(this)
    }

    companion object {
        private const val TAG = "ImuApplication"
    }
}

object AutopilotRelay {
    fun bootstrap(context: android.content.Context) {
        val app = context.applicationContext
        startBleRelayService(app)
        val cloud = CloudSettings(app)
        if (cloud.enabled) {
            CloudUploadScheduler.schedulePeriodic(app)
        }
        if (cloud.enabled && BridgeSyncSettings(app).scheduled) {
            startAutopilotService(app)
            BridgeSyncScheduler.reschedule(app, firstSyncDelayMs = BridgeSyncScheduler.FIRST_SYNC_DELAY_MS)
        }
        Log.i("AutopilotRelay", "bootstrap cloud=${cloud.enabled} bridge=${BridgeSyncSettings(app).mode.id}")
    }

    /** Enable periodic bridge when cloud is turned on (unless user chose another mode explicitly). */
    fun onCloudEnabled(context: android.content.Context, bridge: BridgeSyncSettings) {
        if (bridge.mode == BridgeSyncSettings.Mode.MANUAL && !bridge.userDisabledBridge) {
            bridge.mode = BridgeSyncSettings.Mode.RENDEZVOUS
        }
        startConnectRelayService(context)
        startAutopilotService(context)
        BridgeSyncScheduler.reschedule(context, firstSyncDelayMs = BridgeSyncScheduler.FIRST_SYNC_DELAY_MS)
    }

    fun onCloudDisabled(context: android.content.Context) {
        CloudUploadScheduler.cancelAll(context)
        BridgeSyncScheduler.cancel(context)
        val intent = Intent(context, ImuBleForegroundService::class.java).apply {
            action = ImuBleForegroundService.ACTION_STOP_AUTOPILOT
        }
        context.startForegroundService(intent)
    }

    /** Always-on BLE relay: time sync + crash drain (upload when cloud on). */
    fun startBleRelayService(context: android.content.Context) {
        val intent = Intent(context, ImuBleForegroundService::class.java).apply {
            action = ImuBleForegroundService.ACTION_BLE_RELAY
        }
        context.startForegroundService(intent)
    }

    fun startConnectRelayService(context: android.content.Context) {
        val intent = Intent(context, ImuBleForegroundService::class.java).apply {
            action = ImuBleForegroundService.ACTION_CONNECT_RELAY
        }
        context.startForegroundService(intent)
    }

    fun startAutopilotService(context: android.content.Context) {
        val intent = Intent(context, ImuBleForegroundService::class.java).apply {
            action = ImuBleForegroundService.ACTION_AUTOPILOT
        }
        context.startForegroundService(intent)
    }
}
