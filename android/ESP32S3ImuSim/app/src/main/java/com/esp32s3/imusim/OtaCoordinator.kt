package com.esp32s3.imusim

import android.content.Context
import android.os.Build
import android.util.Log

/**
 * Poll cloud OTA: APK first, then signed firmware. Caller supplies BLE connected
 * state; this class never flashes USB.
 *
 * Firmware compare is numeric `versionCode` (live STATUS `fwc`, else parsed
 * `handshake vN`). Same or older is never offered; Check for OTA only re-shows
 * a declined newer build.
 */
class OtaCoordinator(
    private val context: Context,
    private val bleConnected: () -> Boolean,
    private val otaCapable: () -> Boolean,
    private val liveFwCode: () -> Int = { 0 },
) {
    private val repo = OtaRepository(context)
    private val ota = OtaSettings(context)
    private val cloud = CloudSettings(context)

    fun poll(force: Boolean) {
        if (!cloud.enabled) {
            Log.i(TAG, "skip — cloud disabled")
            return
        }
        val manifest = repo.fetchManifest()
        if (manifest == null || !manifest.available) {
            Log.i(TAG, "no ota artifacts")
            return
        }
        val apkCode = installedApkVersionCode(context)
        val apk = manifest.apk
        if (apk != null && apk.versionCode > apkCode &&
            (force || apk.versionCode != ota.declinedApkVersionCode)
        ) {
            val file = repo.download(OtaOffer.Kind.APK, apk.url, apk.size, apk.sha256) ?: return
            OtaOfferHub.publish(
                context,
                OtaOffer(
                    kind = OtaOffer.Kind.APK,
                    title = context.getString(R.string.ota_prompt_apk_title),
                    body = context.getString(R.string.ota_prompt_apk_body, apk.versionName, apk.versionCode),
                    file = file,
                    versionLabel = apk.versionName,
                    apkVersionCode = apk.versionCode,
                ),
            )
            return
        }
        val fw = manifest.fw ?: return
        if (fw.minApkVersionCode > apkCode) {
            Log.i(TAG, "fw ${fw.version} needs apk ${fw.minApkVersionCode} (have $apkCode)")
            return
        }
        val offerCode = if (fw.versionCode > 0) fw.versionCode else OtaSettings.parseVersionCode(fw.version)
        val live = maxOf(ota.liveVersionCode(), liveFwCode())
        if (offerCode > 0 && live > 0 && offerCode <= live) {
            Log.i(TAG, "fw ${fw.version} ($offerCode) not newer than live $live")
            return
        }
        if (!force && offerCode > 0 && offerCode == ota.declinedFwVersionCode) return
        if (!bleConnected()) {
            Log.i(TAG, "fw ${fw.version} waiting for BLE")
            return
        }
        if (!otaCapable()) {
            Log.i(TAG, "fw ${fw.version} — device has no OTA cap")
            return
        }
        val file = repo.download(OtaOffer.Kind.FW, fw.url, fw.size, fw.sha256) ?: return
        OtaOfferHub.publish(
            context,
            OtaOffer(
                kind = OtaOffer.Kind.FW,
                title = context.getString(R.string.ota_prompt_fw_title),
                body = context.getString(
                    R.string.ota_prompt_fw_body,
                    fw.version,
                    ota.lastFwVersion.ifBlank { "unknown" },
                ),
                file = file,
                versionLabel = fw.version,
                fwVersionCode = offerCode,
            ),
        )
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
    }
}
