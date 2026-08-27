package com.esp32s3.imusim

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.io.OutputStreamWriter
import java.net.HttpURLConnection
import java.net.URL

/** Upload device.config.v1 to Good Vibes backend after ESP sync/push. */
object ConfigCloudSync {
    private fun appVersion(context: Context): String =
        runCatching {
            context.packageManager.getPackageInfo(context.packageName, 0).versionName ?: "unknown"
        }.getOrDefault("unknown")

    fun upload(context: Context, doc: DeviceConfigJson.Doc): CloudUploader.Result {
        val settings = CloudSettings(context)
        if (!settings.enabled) {
            return CloudUploader.Result(false, "cloud disabled")
        }
        val body = JSONObject().apply {
            put("schema", "imu.ingest.v1")
            put("device_id", settings.deviceId)
            put("group_id", settings.groupId)
            put("phone_id", settings.phoneId(context))
            put("sent_at_ms", System.currentTimeMillis())
            put(
                "records",
                JSONArray().put(
                    JSONObject().apply {
                        put("type", "config")
                        put("ts_ms", System.currentTimeMillis())
                        put("revision", doc.revision)
                        put("local_revision", doc.localRevision)
                        put("source", doc.source)
                        put("app_version", appVersion(context))
                        put("config", DeviceConfigJson.toJson(doc, appVersion(context)))
                        doc.blobB64?.let { put("blob_b64", it) }
                    },
                ),
            )
        }
        return postJson(settings, "${settings.baseUrl.trim().trimEnd('/')}/v1/ingest/config", body.toString())
    }

    sealed class ReconcileResult {
        /** Cloud was strictly newer than the device's just-reported revision — `blob` is
         *  ready to push straight to pushConfigToDevice(). */
        data class PushToDevice(val blob: ByteArray, val cloudRevision: Long) : ReconcileResult()
        /** Device was equal-or-ahead — its current config was uploaded to the cloud. */
        data class UploadedToCloud(val deviceRevision: Long) : ReconcileResult()
    }

    /**
     * Handshake reconciliation, run every time the phone reads the ESP's live config (manual
     * "sync" and the periodic background bridge cycle — see ImuBleForegroundService).
     *
     * On-device config has priority: the ESP's own device_config_apply_remote() already
     * rejects any incoming push whose revision is older than what's currently running (see
     * device_config.c), so a stale cloud copy can never clobber a fresher on-device config.
     * This mirrors that same rule proactively on the phone side — only builds a cloud -> device
     * push when the cloud is *strictly newer* than what the device just reported; otherwise
     * (equal, or device ahead — e.g. reconfigured locally, or pushed from another phone
     * session) uploads the device's current config so the cloud copy catches up.
     *
     * `baseBlob` should be the raw blob just read from the device (same one `doc` was decoded
     * from) — it's the base that cloud profile/vibro/mix fields get merged on top of, so
     * anything the cloud JSON doesn't cover (or fields outside the profile/vibro/mix trio,
     * e.g. the ESP-local overlay bytes) is preserved unchanged.
     */
    fun reconcile(context: Context, doc: DeviceConfigJson.Doc, baseBlob: ByteArray): ReconcileResult {
        val (cloudRevision, cloudConfig) = fetchLatest(context)
        if (cloudRevision > doc.revision && cloudConfig != null) {
            val cloudDoc = doc.copy(
                revision = cloudRevision,
                profile = cloudConfig.optJSONObject("profile") ?: doc.profile,
                vibro = cloudConfig.optJSONObject("vibro") ?: doc.vibro,
                mix = cloudConfig.optJSONObject("mix") ?: doc.mix,
                source = "cloud",
            )
            val blob = DeviceConfigJson.mergeIntoBlob(baseBlob, cloudDoc, cloudRevision)
            return ReconcileResult.PushToDevice(blob, cloudRevision)
        }
        upload(context, doc)
        return ReconcileResult.UploadedToCloud(doc.revision)
    }

    fun fetchLatest(context: Context): Pair<Long, JSONObject?> {
        val settings = CloudSettings(context)
        if (!settings.enabled) return 0L to null
        val url = "${settings.baseUrl.trim().trimEnd('/')}/v1/devices/${settings.deviceId}/config"
        val conn = URL(url).openConnection() as HttpURLConnection
        conn.requestMethod = "GET"
        conn.setRequestProperty("X-API-Key", settings.apiKey)
        conn.connectTimeout = 8000
        return try {
            if (conn.responseCode == 404) return 0L to null
            if (conn.responseCode !in 200..299) return 0L to null
            val text = conn.inputStream.bufferedReader().readText()
            val root = JSONObject(text)
            root.optLong("revision", 0L) to root.optJSONObject("config")
        } catch (_: Exception) {
            0L to null
        } finally {
            conn.disconnect()
        }
    }

    private fun postJson(settings: CloudSettings, url: String, body: String): CloudUploader.Result {
        val conn = URL(url).openConnection() as HttpURLConnection
        conn.requestMethod = "POST"
        conn.setRequestProperty("Content-Type", "application/json")
        conn.setRequestProperty("X-API-Key", settings.apiKey)
        conn.doOutput = true
        conn.connectTimeout = 15000
        return try {
            OutputStreamWriter(conn.outputStream).use { it.write(body) }
            val code = conn.responseCode
            val msg = if (code in 200..299) {
                conn.inputStream.bufferedReader().readText()
            } else {
                conn.errorStream?.bufferedReader()?.readText() ?: "HTTP $code"
            }
            if (code in 200..299) {
                val accepted = runCatching { JSONObject(msg).optInt("accepted", 1) }.getOrDefault(1)
                CloudUploader.Result(true, "config uploaded", accepted)
            } else {
                CloudUploader.Result(false, msg)
            }
        } catch (e: Exception) {
            CloudUploader.Result(false, e.message ?: "upload failed")
        } finally {
            conn.disconnect()
        }
    }
}
