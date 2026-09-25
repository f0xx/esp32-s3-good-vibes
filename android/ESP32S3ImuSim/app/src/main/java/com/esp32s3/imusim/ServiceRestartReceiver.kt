package com.esp32s3.imusim

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log

/** Restarts the always-on service after the process is removed by the system/OEM manager. */
class ServiceRestartReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent?) {
        if (intent?.action != ACTION_RESTART) return
        Log.i(TAG, "restarting BLE service after process removal")
        AutopilotRelay.bootstrap(context)
    }

    companion object {
        const val ACTION_RESTART = "com.esp32s3.imusim.RESTART_BLE_SERVICE"
        private const val TAG = "ServiceRestartReceiver"
    }
}
