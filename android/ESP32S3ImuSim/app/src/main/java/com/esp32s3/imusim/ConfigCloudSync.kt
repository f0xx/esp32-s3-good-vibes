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
