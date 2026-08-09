package com.esp32s3.imusim

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.net.HttpURLConnection
import java.net.URL

/** POST imu.ingest.v1 verdict envelope to backend (M-cloud). */
class CloudUploader(private val context: Context) {
    private val settings = CloudSettings(context)
    private val offload = OffloadExporter(context)
    private val verdictStore = VerdictStore(context)

    data class Result(val ok: Boolean, val message: String, val accepted: Int = 0)

    data class BatchResult(
        val verdicts: Result,
        val spectra: Result,
        val crashes: Result,
    ) {
        val totalAccepted: Int
            get() = verdicts.accepted + spectra.accepted + crashes.accepted

        val summary: String
            get() = buildList {
                if (verdicts.accepted > 0) add("${verdicts.accepted} verdicts")
                if (spectra.accepted > 0) add("${spectra.accepted} spectra")
                if (crashes.accepted > 0) add("${crashes.accepted} crashes")
            }.joinToString(", ").ifEmpty {
                when {
                    verdicts.message.contains("nothing", ignoreCase = true) &&
                        spectra.message.contains("no spectra", ignoreCase = true) &&
                        crashes.message.contains("no crashes", ignoreCase = true) -> "nothing pending"
                    else -> "no new records accepted"
                }
            }
    }

    fun uploadAll(maxVerdicts: Int = 200): BatchResult =
        BatchResult(
            verdicts = uploadPending(maxVerdicts),
            spectra = uploadPendingSpectra(20),
            crashes = uploadPendingCrashes(10),
        )

    fun localHistoryCount(): Int = verdictStore.count()

    fun testConnection(): Result {
        val base = settings.baseUrl.trim().trimEnd('/')
        if (base.isEmpty()) {
            return Result(false, "set cloud URL first")
        }
        val paths = listOf("$base/v1/health", "$base/health")
        var lastErr = "connection failed"
        for (path in paths) {
            val r = tryGet(path, requireKey = false)
            if (r.ok) {
                return Result(true, "reachable via ${path.substringAfter("://").substringBefore("/")}")
            }
            lastErr = r.message
        }
        return Result(false, lastErr)
    }

    fun uploadPending(maxLines: Int = 200): Result {
        if (!settings.enabled) {
            return Result(false, "cloud disabled")
        }
        val queued = offload.drainPendingLines(maxLines)
        val backfill = if (queued.isEmpty()) linesFromHistory(maxLines) else emptyList()
        val lines = queued.ifEmpty { backfill }
        if (lines.isEmpty()) {
            return Result(true, "nothing pending", 0)
        }

        val records = JSONArray()
        for (line in lines) {
            val row = JSONObject(line)
            records.put(
                JSONObject().apply {
                    put("type", "verdict")
                    put("ts_ms", row.optLong("ts_ms"))
                    put("seq", row.optLong("seq"))
                    put("level", row.optInt("level"))
                    put("rms", row.optDouble("rms"))
                    put("peak", row.optDouble("peak"))
                    put("corr", row.optDouble("corr"))
                    put("rms_delta", row.optDouble("rms_delta"))
                    put("pct", row.optInt("pct"))
                    put("voltage", row.optDouble("voltage"))
                    if (row.has("power_profile")) put("power_profile", row.optInt("power_profile"))
                    if (row.has("chip_temp_c")) put("chip_temp_c", row.optDouble("chip_temp_c"))
                    if (row.has("band_corr")) put("band_corr", row.optDouble("band_corr"))
                    if (row.has("band_delta_max")) put("band_delta_max", row.optDouble("band_delta_max"))
                    if (row.has("edge_crest")) put("edge_crest", row.optDouble("edge_crest"))
                    if (row.has("edge_zcr_hz")) put("edge_zcr_hz", row.optDouble("edge_zcr_hz"))
                    if (row.has("edge_hf_ratio")) put("edge_hf_ratio", row.optDouble("edge_hf_ratio"))
                    row.optJSONArray("bands")?.let { put("bands", it) }
                },
            )
        }

        val body = JSONObject().apply {
            put("schema", "imu.ingest.v1")
            put("device_id", settings.deviceId)
            put("group_id", settings.groupId)
            put("phone_id", settings.phoneId(context))
            put("sent_at_ms", System.currentTimeMillis())
            put("records", records)
        }.toString()

        val result = try {
            postJson("${settings.baseUrl}/v1/ingest/verdicts", body)
        } catch (e: Exception) {
            if (queued.isNotEmpty()) {
                offload.restoreLines(queued)
            }
            return Result(false, e.message ?: "upload failed")
        }
        if (!result.ok && queued.isNotEmpty()) {
            offload.restoreLines(queued)
        }
        return result
    }

    private fun linesFromHistory(maxLines: Int): List<String> =
        verdictStore.forCloudExport(maxLines).map { row ->
            JSONObject().apply {
                put("ts_ms", row.tsMs)
                put("seq", row.seq)
                put("level", row.level)
                put("rms", row.rmsG.toDouble())
                put("peak", row.peakG.toDouble())
                put("corr", row.corr.toDouble())
                put("rms_delta", row.rmsDelta.toDouble())
                put("pct", row.pct)
                put("voltage", row.voltageV.toDouble())
                row.powerProfile?.let { put("power_profile", it) }
                row.chipTempC?.let { put("chip_temp_c", it.toDouble()) }
            }.toString()
        }

    fun uploadPendingSpectra(maxLines: Int = 20): Result {
        if (!settings.enabled) {
            return Result(false, "cloud disabled")
        }
        val lines = offload.drainPendingSpectra(maxLines)
        if (lines.isEmpty()) {
            return Result(true, "no spectra pending", 0)
        }
        val records = JSONArray()
        for (line in lines) {
            val row = JSONObject(line)
            records.put(
                JSONObject().apply {
                    put("type", "spectrum")
                    put("ts_ms", row.optLong("ts_ms"))
                    put("seq", row.optLong("seq"))
                    put("sample_hz", row.optDouble("sample_hz"))
                    put("bin_hz", row.optDouble("bin_hz"))
                    put("bins", row.optJSONArray("bins"))
                    put("peak_hz", row.optDouble("peak_hz"))
                    put("peak_mag", row.optDouble("peak_mag"))
                    put("axis", row.optString("axis", "mag"))
                },
            )
        }
        val body = JSONObject().apply {
            put("schema", "imu.ingest.v1")
            put("device_id", settings.deviceId)
            put("group_id", settings.groupId)
            put("phone_id", settings.phoneId(context))
            put("sent_at_ms", System.currentTimeMillis())
            put("records", records)
        }.toString()
        val result = try {
            postJson("${settings.baseUrl}/v1/ingest/spectra", body)
        } catch (e: Exception) {
            offload.restoreSpectra(lines)
            return Result(false, e.message ?: "spectrum upload failed")
        }
        if (!result.ok) {
            offload.restoreSpectra(lines)
        }
        return result
    }

    fun uploadPendingCrashes(maxLines: Int = 10): Result {
        if (!settings.enabled) {
            return Result(false, "cloud disabled")
        }
        val lines = offload.drainPendingCrashes(maxLines)
        if (lines.isEmpty()) {
            return Result(true, "no crashes pending", 0)
        }
        val records = JSONArray()
        for (line in lines) {
            val row = JSONObject(line)
            records.put(
                JSONObject().apply {
                    put("type", "crash")
                    put("ts_ms", row.optLong("ts_ms"))
                    put("seq", row.optLong("seq"))
                    put("reason", row.optString("reason"))
                    if (row.has("pc")) put("pc", row.optLong("pc"))
                    if (row.has("exccause")) put("exccause", row.optInt("exccause"))
                    if (row.has("excvaddr")) put("excvaddr", row.optLong("excvaddr"))
                    if (row.has("thread_name")) put("thread_name", row.optString("thread_name"))
                    if (row.has("fw_version")) put("fw_version", row.optString("fw_version"))
                    if (row.has("reset_reason")) put("reset_reason", row.optInt("reset_reason"))
                    if (row.has("uptime_ms")) put("uptime_ms", row.optLong("uptime_ms"))
                    if (row.has("backtrace")) put("backtrace", row.optJSONArray("backtrace"))
                    if (row.has("detail")) put("detail", row.optJSONObject("detail"))
                },
            )
        }
        val body = JSONObject().apply {
            put("schema", "imu.ingest.v1")
            put("device_id", settings.deviceId)
            put("group_id", settings.groupId)
            put("phone_id", settings.phoneId(context))
            put("sent_at_ms", System.currentTimeMillis())
            put("records", records)
        }.toString()
        val result = try {
            postJson("${settings.baseUrl}/v1/ingest/crashes", body)
        } catch (e: Exception) {
            offload.restoreCrashes(lines)
            return Result(false, e.message ?: "crash upload failed")
        }
        if (!result.ok) {
            offload.restoreCrashes(lines)
        }
        return result
    }

    /** Clock correction event from ESP STATUS (backend clock ingest TBD). */
    fun uploadClockEvent(
        src: Int,
        driftMs: Long,
        corrMs: Long,
        tzMin: Int,
        unixSec: Long,
    ): Result {
        if (!settings.enabled) {
            return Result(false, "cloud disabled")
        }
        android.util.Log.i(
            "CloudUploader",
            "clock_sync device=${settings.deviceId} src=$src drift=${driftMs}ms " +
                "corr=${corrMs}ms tz=${tzMin}min unix=$unixSec",
        )
        return Result(true, "clock event logged locally", 1)
    }

    private fun postJson(urlStr: String, body: String): Result {
        return try {
            postJsonInner(urlStr, body)
        } catch (e: javax.net.ssl.SSLException) {
            Result(
                false,
                "TLS error — use https://apps.f0xx.org/app/good_vibes " +
                    "or http://artc0.f0xx.org:8090 (not https on :8090). ${e.message}",
            )
        } catch (e: java.io.IOException) {
            val msg = e.message.orEmpty()
            if (msg.contains("tls", ignoreCase = true) || msg.contains("ssl", ignoreCase = true)) {
                Result(
                    false,
                    "TLS/HTTP mismatch — check cloud URL (https for apps.f0xx.org, http for :8090)",
                )
            } else {
                Result(false, msg.ifEmpty { "upload failed" })
            }
        }
    }

    private fun tryGet(urlStr: String, requireKey: Boolean): Result {
        return try {
            val conn = openConnection(urlStr, "GET", requireKey)
            try {
                val code = conn.responseCode
                if (code in 200..299) Result(true, "HTTP $code") else Result(false, "HTTP $code")
            } finally {
                conn.disconnect()
            }
        } catch (e: Exception) {
            Result(false, e.message ?: "connection failed")
        }
    }

    private fun postJsonInner(urlStr: String, body: String): Result {
        val conn = openConnection(urlStr, "POST", requireKey = true)
        try {
            OutputStreamWriter(conn.outputStream, Charsets.UTF_8).use { it.write(body) }
            val code = conn.responseCode
            val stream = if (code in 200..299) conn.inputStream else conn.errorStream
            val text = BufferedReader(InputStreamReader(stream, Charsets.UTF_8)).use { it.readText() }
            if (code !in 200..299) {
                return Result(false, "HTTP $code: $text")
            }
            val resp = JSONObject(text)
            val accepted = resp.optInt("accepted", linesAcceptedFallback(text))
            return Result(true, "uploaded $accepted", accepted)
        } finally {
            conn.disconnect()
        }
    }

    private fun openConnection(urlStr: String, method: String, requireKey: Boolean): HttpURLConnection {
        val url = URL(urlStr)
        return (url.openConnection() as HttpURLConnection).apply {
            requestMethod = method
            connectTimeout = 8_000
            readTimeout = 20_000
            if (method == "POST") {
                doOutput = true
                setRequestProperty("Content-Type", "application/json; charset=utf-8")
            }
            if (requireKey) {
                setRequestProperty("X-API-Key", settings.apiKey)
            }
        }
    }

    private fun linesAcceptedFallback(text: String): Int {
        return runCatching { JSONObject(text).optInt("accepted") }.getOrDefault(0)
    }
}
