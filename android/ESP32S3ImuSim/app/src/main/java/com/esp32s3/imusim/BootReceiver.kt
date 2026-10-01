package com.esp32s3.imusim

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/** Re-arm WorkManager + autopilot service after phone reboot or self-update. */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent?) {
        val action = intent?.action ?: return
        if (action != Intent.ACTION_BOOT_COMPLETED &&
            action != Intent.ACTION_MY_PACKAGE_REPLACED &&
            action != "android.intent.action.QUICKBOOT_POWERON" &&
            action != "com.htc.intent.action.QUICKBOOT_POWERON"
        ) {
            return
        }
        android.util.Log.i(TAG, "onReceive action=$action")
        AutopilotRelay.bootstrap(context)
        if (action == Intent.ACTION_MY_PACKAGE_REPLACED) {
            OtaInstallActivity.finishPendingRestart(context)
        }
    }

    companion object {
        private const val TAG = "BootReceiver"
    }
}
