package com.esp32s3.imusim

import android.content.Context
import android.os.Build
import android.util.Log

/**
 * Inspect cloud OTA only — no download. App update is always offered first;
 * firmware is offered only when the installed APK already meets min_apk.
 */
class OtaCoordinator(
    private val context: Context,
    private val bleConnected: () -> Boolean,
    private val otaCapable: () -> Boolean,
    private val liveFwCode: () -> Int = { 0 },
    private val liveFwName: () -> String = { "" },
) {
    private val repo = OtaRepository(context)
    private val ota = OtaSettings(context)

    sealed class Result {
        data class App(val apk: OtaManifest.Apk, val currentName: String, val currentCode: Int) : Result()
        data class Firmware(
            val fw: OtaManifest.Fw,
            val currentName: String,
            val currentCode: Int,
        ) : Result()
        data class None(val reason: String) : Result()
    }

    fun inspect(force: Boolean): Result {
        val currentAppName = installedApkVersionName(context)
        val currentAppCode = installedApkVersionCode(context)
        val channel = CloudSettings(context).otaChannel
        val chTag = "ch=$channel"
        val manifest = repo.fetchManifest()
        if (manifest == null || !manifest.available) {
            return Result.None("No OTA on $chTag · app $currentAppName ($currentAppCode)")
        }
        val apk = manifest.apk
        if (apk != null && apk.versionCode > currentAppCode &&
            (force || apk.versionCode != ota.declinedApkVersionCode)
        ) {
            return Result.App(apk, currentAppName, currentAppCode)
        }
        val fw = manifest.fw ?: return Result.None(
            "App current ($currentAppName) · no firmware on $chTag",
        )
        if (fw.minApkVersionCode > currentAppCode) {
            return Result.None(
                "Firmware ${fw.version} needs app ${fw.minApkVersionCode} (have $currentAppCode)",
            )
        }
        val offerCode = if (fw.versionCode > 0) fw.versionCode else OtaSettings.parseVersionCode(fw.version)
        val statusCode = liveFwCode()
        val liveCode = if (statusCode > 0) statusCode else ota.liveVersionCode()
        val liveName = liveFwName().ifBlank { ota.lastFwVersion }.ifBlank { "unknown" }
        if (offerCode > 0 && liveCode > 0 && offerCode <= liveCode) {
            return Result.None("Firmware current · $liveName ($liveCode)")
        }
        if (!force && offerCode > 0 && offerCode == ota.declinedFwVersionCode) {
            return Result.None("Firmware OTA deferred · $liveName")
        }
        if (!bleConnected()) {
            return Result.None("Firmware ${fw.version} waiting for BLE · app $currentAppName")
        }
        if (!otaCapable() && !force) {
            return Result.None("Firmware ${fw.version} waiting for IMU BLE session")
        }
        return Result.Firmware(fw, liveName, liveCode)
    }

    companion object {
        private const val TAG = "OtaCoordinator"

        fun installedApkVersionCode(context: Context): Int {
            val info = context.packageManager.getPackageInfo(context.packageName, 0)
            return if (Build.VERSION.SDK_INT >= 28) {
                info.longVersionCode.toInt()
            } else {
                @Suppress("DEPRECATION")
                info.versionCode
            }
        }

        fun installedApkVersionName(context: Context): String {
            return try {
                context.packageManager.getPackageInfo(context.packageName, 0).versionName ?: "?"
            } catch (e: Exception) {
                Log.w(TAG, "versionName: ${e.message}")
                "?"
            }
        }
    }
}
