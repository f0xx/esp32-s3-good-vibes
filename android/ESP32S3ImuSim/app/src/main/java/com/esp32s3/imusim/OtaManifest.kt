package com.esp32s3.imusim

import org.json.JSONObject

/** Cloud builder manifest (`imu.ota.v1`) — APK first, then signed ESP32 slot image. */
data class OtaManifest(
    val available: Boolean,
    val publishedAtMs: Long,
    val apk: Apk?,
    val fw: Fw?,
) {
    data class Apk(
        val versionCode: Int,
        val versionName: String,
        val sha256: String,
        val size: Long,
        val url: String,
    )

    data class Fw(
        val versionCode: Int,
        val version: String,
        val sha256: String,
        val size: Long,
        val url: String,
        val minApkVersionCode: Int,
    )

    companion object {
        fun parse(text: String): OtaManifest? {
            val o = try {
                JSONObject(text)
            } catch (_: Exception) {
                return null
            }
            if (o.optString("schema") != "imu.ota.v1") {
                return OtaManifest(false, 0L, null, null)
            }
            val apkObj = o.optJSONObject("apk")
            val fwObj = o.optJSONObject("fw")
            val apk = apkObj?.let {
                Apk(
                    versionCode = it.optInt("versionCode"),
                    versionName = it.optString("versionName"),
                    sha256 = it.optString("sha256").lowercase(),
                    size = it.optLong("size"),
                    url = it.optString("url", "artifacts/apk"),
                )
            }
            val fw = fwObj?.let {
                val version = it.optString("version")
                val parsed = it.optInt("versionCode")
                Fw(
                    versionCode = if (parsed > 0) parsed else OtaSettings.parseVersionCode(version),
                    version = version,
                    sha256 = it.optString("sha256").lowercase(),
                    size = it.optLong("size"),
                    url = it.optString("url", "artifacts/fw"),
                    minApkVersionCode = it.optInt("min_apk_versionCode"),
                )
            }
            val available = o.optBoolean("available", apk != null || fw != null)
            return OtaManifest(available, o.optLong("published_at_ms"), apk, fw)
        }
    }
}
