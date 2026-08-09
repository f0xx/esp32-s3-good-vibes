package com.esp32s3.imusim

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/** Re-arm WorkManager + autopilot service after phone reboot. */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent?) {
        if (intent?.action != Intent.ACTION_BOOT_COMPLETED &&
            intent?.action != Intent.ACTION_MY_PACKAGE_REPLACED
        ) {
            return
        }
        AutopilotRelay.bootstrap(context)
    }
}
