package com.esp32s3.imusim

import android.app.Application
import android.content.Intent
import android.util.Log
import androidx.work.Configuration
import androidx.work.WorkManager

/** Start cloud/bridge schedulers without opening MainActivity. */
class ImuApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        if (isServiceProcess()) {
            /* Startup provider only runs in the default process. The BLE process
             * still enqueues uploads, so initialize WorkManager here and do not
             * schedule from Application.onCreate (that killed :imu on launch). */
            if (!WorkManager.isInitialized()) {
                WorkManager.initialize(this, Configuration.Builder().build())
            }
            return
        }
        AutopilotRelay.bootstrap(this)
        ServiceKeepAliveWorker.schedule(this)
        /* PackageInstaller may restart the process without delivering MY_PACKAGE_REPLACED
         * to a live receiver on some OEMs — prefs flag covers that path. */
        OtaInstallActivity.finishPendingRestart(this)
    }

    private fun isServiceProcess(): Boolean {
        val name = if (android.os.Build.VERSION.SDK_INT >= 28) {
            getProcessName()
        } else {
            val am = getSystemService(android.app.ActivityManager::class.java)
            am?.runningAppProcesses?.firstOrNull { it.pid == android.os.Process.myPid() }?.processName
        }
        return name?.endsWith(":imu") == true
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
        if (!BlePermissionGate.canUseBle(context)) {
            Log.w("AutopilotRelay", "BLE permissions are not granted; opening permission gate")
            PermissionGateActivity.launch(context)
            return
        }
        if (PermissionGateActivity.needsAttention(context)) {
            PermissionGateActivity.launch(context)
        }
        val intent = Intent(context, ImuBleForegroundService::class.java).apply {
            action = ImuBleForegroundService.ACTION_BLE_RELAY
        }
        context.startForegroundService(intent)
    }

    fun startConnectRelayService(context: android.content.Context) {
        if (!BlePermissionGate.canUseBle(context)) return
        val intent = Intent(context, ImuBleForegroundService::class.java).apply {
            action = ImuBleForegroundService.ACTION_CONNECT_RELAY
        }
        context.startForegroundService(intent)
    }

    fun startAutopilotService(context: android.content.Context) {
        if (!BlePermissionGate.canUseBle(context)) return
        val intent = Intent(context, ImuBleForegroundService::class.java).apply {
            action = ImuBleForegroundService.ACTION_AUTOPILOT
        }
        context.startForegroundService(intent)
    }

    /** After APK OTA / package replace — open the launcher activity. */
    fun launchMainUi(context: android.content.Context) {
        val app = context.applicationContext
        val launch = app.packageManager.getLaunchIntentForPackage(app.packageName) ?: Intent(
            app,
            MainActivity::class.java,
        )
        launch.addFlags(
            Intent.FLAG_ACTIVITY_NEW_TASK or
                Intent.FLAG_ACTIVITY_CLEAR_TOP or
                Intent.FLAG_ACTIVITY_SINGLE_TOP,
        )
        try {
            app.startActivity(launch)
            Log.i("AutopilotRelay", "launched MainActivity after package replace")
        } catch (e: Exception) {
            Log.w("AutopilotRelay", "launch MainActivity failed: ${e.message}")
        }
    }

    object BlePermissionGate {
        fun canUseBle(context: android.content.Context): Boolean {
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.S) {
                return androidx.core.content.ContextCompat.checkSelfPermission(
                    context,
                    android.Manifest.permission.BLUETOOTH_SCAN,
                ) == android.content.pm.PackageManager.PERMISSION_GRANTED &&
                    androidx.core.content.ContextCompat.checkSelfPermission(
                        context,
                        android.Manifest.permission.BLUETOOTH_CONNECT,
                    ) == android.content.pm.PackageManager.PERMISSION_GRANTED
            }
            return androidx.core.content.ContextCompat.checkSelfPermission(
                context,
                android.Manifest.permission.ACCESS_FINE_LOCATION,
            ) == android.content.pm.PackageManager.PERMISSION_GRANTED
        }
    }
}
