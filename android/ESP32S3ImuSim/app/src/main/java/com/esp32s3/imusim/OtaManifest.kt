package com.esp32s3.imusim

import org.json.JSONArray
import org.json.JSONObject

/**
 * Cloud OTA: backend `imu.ota.v1` or CDN `schema=v0` product manifest
 * (`good_vibes/v0/ota/…`). APK first, then signed ESP32 slot image.
 */
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
        val mirrors: List<String> = emptyList(),
        val bundleUrl: String = "",
        val bundleSha256: String = "",
        val bundleSize: Long = 0,
        val bundleMirrors: List<String> = emptyList(),
    ) {
        fun downloadUrls(): List<String> {
            val out = linkedSetOf<String>()
            if (url.isNotBlank()) out.add(url)
            out.addAll(mirrors.filter { it.isNotBlank() })
            if (bundleUrl.isNotBlank()) out.add(bundleUrl)
            out.addAll(bundleMirrors.filter { it.isNotBlank() })
            return out.toList()
        }
    }

    data class Fw(
        val versionCode: Int,
        val version: String,
        val sha256: String,
        val size: Long,
        val url: String,
        val minApkVersionCode: Int,
        val mirrors: List<String> = emptyList(),
    ) {
        fun downloadUrls(): List<String> {
            val out = linkedSetOf<String>()
            if (url.isNotBlank()) out.add(url)
            out.addAll(mirrors.filter { it.isNotBlank() })
            return out.toList()
        }
    }

    companion object {
        fun parse(text: String): OtaManifest? {
            val o = try {
                JSONObject(text)
            } catch (_: Exception) {
                return null
            }
            val schema = o.optString("schema")
            if (schema != "imu.ota.v1" && schema != "v0") {
                return OtaManifest(false, 0L, null, null)
            }
            val apkObj = o.optJSONObject("apk")
            val fwObj = o.optJSONObject("fw")
            val apk = apkObj?.let { parseApk(it) }
            val fw = fwObj?.let { parseFw(it) }
            val available = o.optBoolean("available", apk != null || fw != null)
            return OtaManifest(available, o.optLong("published_at_ms"), apk, fw)
        }

        fun parseChannel(text: String): List<String> {
            val o = try {
                JSONObject(text)
            } catch (_: Exception) {
                return emptyList()
            }
            if (o.optString("schema") != "v0") return emptyList()
            val out = linkedSetOf<String>()
            // Backbone mirrors first — director 302s to cdnN and Android
            // HttpURLConnection can hang forever on that follow-up GET.
            stringList(o.optJSONArray("manifestMirrors")).forEach { out.add(it) }
            o.optString("manifestUrl").trim().takeIf { it.isNotEmpty() }?.let { out.add(it) }
            return out.toList()
        }

        private fun parseApk(it: JSONObject): Apk {
            val name = it.optString("versionName")
            val parsed = it.optInt("versionCode")
            val code = if (parsed > 0) parsed else GvAppVersion.parseCode(name)
            return Apk(
                versionCode = code,
                versionName = name,
                sha256 = it.optString("sha256").lowercase(),
                size = it.optLong("size").takeIf { s -> s > 0 } ?: it.optLong("sizeBytes"),
                url = it.optString("url").ifBlank { it.optString("apkUrl", "artifacts/apk") },
                mirrors = stringList(it.optJSONArray("mirrors")),
                bundleUrl = it.optString("bundleUrl"),
                bundleSha256 = it.optString("bundleSha256").lowercase(),
                bundleSize = it.optLong("bundleSizeBytes"),
                bundleMirrors = stringList(it.optJSONArray("bundleMirrors")),
            )
        }

        private fun parseFw(it: JSONObject): Fw {
            val version = it.optString("version").ifBlank { it.optString("versionName") }
            val parsed = it.optInt("versionCode")
            return Fw(
                versionCode = if (parsed > 0) parsed else OtaSettings.parseVersionCode(version),
                version = version,
                sha256 = it.optString("sha256").lowercase(),
                size = it.optLong("size").takeIf { s -> s > 0 } ?: it.optLong("sizeBytes"),
                url = it.optString("url", "artifacts/fw"),
                minApkVersionCode = it.optInt("min_apk_versionCode"),
                mirrors = stringList(it.optJSONArray("mirrors")),
            )
        }

        private fun stringList(arr: JSONArray?): List<String> {
            if (arr == null) return emptyList()
            val out = ArrayList<String>(arr.length())
            for (i in 0 until arr.length()) {
                val s = arr.optString(i).trim()
                if (s.isNotEmpty()) out.add(s)
            }
            return out
        }
    }
}
