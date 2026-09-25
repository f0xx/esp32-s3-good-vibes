package com.esp32s3.imusim

import android.app.Activity
import android.app.AlarmManager
import android.app.PendingIntent
import android.content.Intent
import android.content.pm.PackageInstaller
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.util.Log
import kotlin.system.exitProcess

/** PackageInstaller confirmation trampoline + status sink. */
class OtaInstallActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, Int.MIN_VALUE)
        val message = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE).orEmpty()
        Log.i(TAG, "status=$status msg=$message session=${intent.getIntExtra(EXTRA_SESSION_ID, -1)}")
        when (status) {
            PackageInstaller.STATUS_PENDING_USER_ACTION -> {
                val extra = confirmIntent()
                if (extra != null) {
                    extra.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    startActivity(extra)
                } else {
                    OtaSession.fail("Android install prompt missing")
                }
            }
            PackageInstaller.STATUS_SUCCESS -> {
                val appCtx = applicationContext
                OtaSettings(appCtx).pendingAppRestart = true
                OtaSession.complete("App OTA installed — restarting…")
                AutopilotRelay.bootstrap(appCtx)
                scheduleRelaunch(appCtx)
                AutopilotRelay.launchMainUi(appCtx)
                /* Force process death so the new APK is mapped; AlarmManager relaunches UI. */
                android.os.Handler(android.os.Looper.getMainLooper()).postDelayed({
                    Log.i(TAG, "exiting process after APK OTA for clean restart")
                    exitProcess(0)
                }, 700L)
            }
            PackageInstaller.STATUS_FAILURE_ABORTED -> {
                OtaSettings(applicationContext).pendingAppRestart = false
                OtaSession.fail("install cancelled")
            }
            Int.MIN_VALUE -> {
                val extra = confirmIntent()
                if (extra != null) {
                    extra.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    startActivity(extra)
                } else {
                    OtaSession.fail("install session started with no status")
                }
            }
            else -> {
                OtaSettings(applicationContext).pendingAppRestart = false
                val label = when (status) {
                    PackageInstaller.STATUS_FAILURE -> "install failed"
                    PackageInstaller.STATUS_FAILURE_BLOCKED -> "install blocked"
                    PackageInstaller.STATUS_FAILURE_CONFLICT -> "signature or package conflict"
                    PackageInstaller.STATUS_FAILURE_INCOMPATIBLE -> "incompatible APK"
                    PackageInstaller.STATUS_FAILURE_INVALID -> "invalid APK"
                    PackageInstaller.STATUS_FAILURE_STORAGE -> "not enough storage"
                    else -> "install status $status"
                }
                OtaSession.fail(if (message.isNotBlank()) "$label: $message" else label)
            }
        }
        finish()
    }

    private fun confirmIntent(): Intent? {
        return if (Build.VERSION.SDK_INT >= 33) {
            intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent::class.java)
        } else {
            @Suppress("DEPRECATION")
            intent.getParcelableExtra(Intent.EXTRA_INTENT)
        }
    }

    companion object {
        const val EXTRA_SESSION_ID = "ota_session_id"
        private const val TAG = "OtaInstall"
        private const val RELAUNCH_REQ = 0x07A1

        fun scheduleRelaunch(context: android.content.Context) {
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
            val flags = PendingIntent.FLAG_UPDATE_CURRENT or
                (if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_IMMUTABLE else 0)
            val pending = PendingIntent.getActivity(app, RELAUNCH_REQ, launch, flags)
            val am = app.getSystemService(AlarmManager::class.java) ?: return
            try {
                am.set(
                    AlarmManager.ELAPSED_REALTIME,
                    SystemClock.elapsedRealtime() + 1_500L,
                    pending,
                )
                Log.i(TAG, "scheduled post-OTA relaunch +1.5s")
            } catch (e: Exception) {
                Log.w(TAG, "schedule relaunch failed: ${e.message}")
            }
        }

        fun finishPendingRestart(context: android.content.Context) {
            val ota = OtaSettings(context)
            if (!ota.pendingAppRestart) return
            ota.pendingAppRestart = false
            OtaSession.complete("App OTA · restarted")
            AutopilotRelay.bootstrap(context)
            AutopilotRelay.launchMainUi(context)
            android.os.Handler(android.os.Looper.getMainLooper()).apply {
                postDelayed({ AutopilotRelay.launchMainUi(context.applicationContext) }, 800L)
                postDelayed({ AutopilotRelay.launchMainUi(context.applicationContext) }, 2_500L)
            }
        }
    }
}
