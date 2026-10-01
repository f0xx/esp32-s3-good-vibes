package com.esp32s3.imusim

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.net.Uri
import android.os.Build
import android.provider.Settings
import android.util.Log
import java.io.File

object ApkInstaller {
    private const val TAG = "ApkInstaller"

    fun canInstall(context: Context): Boolean =
        context.packageManager.canRequestPackageInstalls()

    fun openUnknownSourcesSettings(context: Context) {
        val uri = Uri.parse("package:${context.packageName}")
        val intent = Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, uri).apply {
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }
        context.startActivity(intent)
    }

    /**
     * Stage the APK on a background-safe call (do not run on the UI thread) and
     * commit. System confirmation / success / failure arrives at [OtaInstallActivity].
     */
    fun install(context: Context, apk: File): Boolean {
        if (!apk.isFile || apk.length() < 1024L) {
            Log.w(TAG, "apk missing or tiny: ${apk.absolutePath} len=${apk.length()}")
            return false
        }
        val app = context.applicationContext
        val installer = app.packageManager.packageInstaller
        val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL)
        params.setAppPackageName(app.packageName)
        /* Do not setDontKillApp — old process must die so MY_PACKAGE_REPLACED + new code load. */
        OtaSettings(app).pendingAppRestart = true
        val sessionId = installer.createSession(params)
        installer.openSession(sessionId).use { session ->
            val len = apk.length()
            session.openWrite("imu-apk", 0, len).use { out ->
                apk.inputStream().use { input ->
                    val buf = ByteArray(64 * 1024)
                    var copied = 0L
                    while (true) {
                        val n = input.read(buf)
                        if (n <= 0) break
                        out.write(buf, 0, n)
                        copied += n
                    }
                    session.fsync(out)
                    Log.i(TAG, "staged $copied bytes session=$sessionId")
                }
            }
            val trampoline = Intent(app, OtaInstallActivity::class.java).apply {
                putExtra(OtaInstallActivity.EXTRA_SESSION_ID, sessionId)
            }
            val flags = PendingIntent.FLAG_UPDATE_CURRENT or
                (if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0)
            val pending = PendingIntent.getActivity(app, sessionId, trampoline, flags)
            session.commit(pending.intentSender)
        }
        return true
    }
}
