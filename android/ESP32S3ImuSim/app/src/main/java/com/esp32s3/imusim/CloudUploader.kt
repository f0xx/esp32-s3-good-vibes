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
    private val batteryBenchStore = BatteryBenchStore(context)

    data class Result(
        val ok: Boolean,
        val message: String,
        val accepted: Int = 0,
        val duplicates: Int = 0,
        val maxSeq: Long = 0L,
    )

    data class BatchResult(
        val verdicts: Result,
        val spectra: Result,
        val crashes: Result,
        val batteryBench: Result = Result(true, "no bench pending", 0),
        val telemetry: Result = Result(true, "no telemetry pending", 0),
    ) {
        val totalAccepted: Int
            get() = verdicts.accepted + spectra.accepted + crashes.accepted + batteryBench.accepted +
                telemetry.accepted

        val summary: String
            get() = buildList {
                if (verdicts.accepted > 0) add("${verdicts.accepted} verdicts")
                if (spectra.accepted > 0) add("${spectra.accepted} spectra")
                if (crashes.accepted > 0) add("${crashes.accepted} crashes")
                if (batteryBench.accepted > 0) add("${batteryBench.accepted} bench samples")
            }.joinToString(", ").ifEmpty {
                when {
                    verdicts.message.contains("nothing", ignoreCase = true) &&
                        spectra.message.contains("no spectra", ignoreCase = true) &&
                        crashes.message.contains("no crashes", ignoreCase = true) &&
                        batteryBench.message.contains("no bench", ignoreCase = true) &&
                        telemetry.message.contains("no telemetry", ignoreCase = true) -> "up to date"
                    else -> "no new records"
                }
            }
    }

    fun uploadAll(maxVerdicts: Int = 200): BatchResult {
        val telemetry = uploadPendingTelemetry(maxVerdicts)
        return BatchResult(
            verdicts = uploadPending(maxVerdicts),
            spectra = uploadPendingSpectra(20),
            crashes = uploadPendingCrashes(10),
            batteryBench = uploadPendingBatteryBench(500),
            telemetry = telemetry,
        )
    }

    fun localHistoryCount(): Int = verdictStore.count()

    /** Cheap pre-check so callers on a hot path (BLE notify callback) can skip work entirely
     *  when cloud upload isn't configured, without needing to know about CloudSettings. */
    fun isEnabledForAhrs(): Boolean = settings.enabled

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
        var maxSeq = 0L
        for (line in lines) {
            val row = JSONObject(line)
            val seq = row.optLong("seq")
            val sessionSeq = row.optLong("session_seq")
            if (seq > maxSeq) maxSeq = seq
            if (sessionSeq > maxSeq) maxSeq = sessionSeq
            records.put(
                JSONObject().apply {
                    put("type", "verdict")
                    put("ts_ms", row.optLong("ts_ms"))
                    put("seq", seq)
                    put("level", row.optInt("level"))
                    put("rms", row.optDouble("rms"))
                    put("peak", row.optDouble("peak"))
                    put("corr", row.optDouble("corr"))
                    put("rms_delta", row.optDouble("rms_delta"))
                    put("pct", row.optInt("pct"))
                    put("voltage", row.optDouble("voltage"))
                    if (row.has("power_profile")) put("power_profile", row.optInt("power_profile"))
                    if (row.has("chip_temp_c")) put("chip_temp_c", row.optDouble("chip_temp_c"))
                    row.optInt("cpu_mhz").takeIf { row.has("cpu_mhz") }?.let { put("cpu_mhz", it) }
                    row.optInt("apb_mhz").takeIf { row.has("apb_mhz") }?.let { put("apb_mhz", it) }
                    row.optInt("spool_free_b").takeIf { row.has("spool_free_b") }?.let { put("spool_free_b", it) }
                    row.optInt("spool_cap_b").takeIf { row.has("spool_cap_b") }?.let { put("spool_cap_b", it) }
                    row.optInt("spool_pending").takeIf { row.has("spool_pending") }?.let { put("spool_pending", it) }
                    if (row.has("band_corr")) put("band_corr", row.optDouble("band_corr"))
                    if (row.has("band_delta_max")) put("band_delta_max", row.optDouble("band_delta_max"))
                    if (row.has("edge_crest")) put("edge_crest", row.optDouble("edge_crest"))
                    if (row.has("edge_zcr_hz")) put("edge_zcr_hz", row.optDouble("edge_zcr_hz"))
                    if (row.has("edge_hf_ratio")) put("edge_hf_ratio", row.optDouble("edge_hf_ratio"))
                    row.optJSONArray("bands")?.let { put("bands", it) }
                    row.optLong("session_seq").takeIf { row.has("session_seq") }?.let { put("session_seq", it) }
                    row.optInt("cap_mix_sec").takeIf { row.has("cap_mix_sec") }?.let { put("cap_mix_sec", it) }
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
        if (result.ok && maxSeq > 0L) {
            OffloadAckStore.noteCloudAcked(context, maxSeq)
        }
        if (result.ok && result.accepted > 0) {
            reportIngestBatch("verdict", result.accepted, body.length)
        }
        return result.copy(maxSeq = maxSeq)
    }

    fun uploadPendingTelemetry(maxLines: Int = 200): Result {
        if (!settings.enabled) {
            return Result(false, "cloud disabled")
        }
        val lines = offload.drainPendingTelemetry(maxLines)
        if (lines.isEmpty()) {
            return Result(true, "no telemetry pending", 0)
        }
        val records = JSONArray()
        for (line in lines) {
            val row = JSONObject(line)
            records.put(
                JSONObject().apply {
                    put("type", "telemetry")
                    put("ts_ms", row.optLong("ts_ms"))
                    row.optDouble("chip_temp_c").takeIf { row.has("chip_temp_c") }?.let { put("chip_temp_c", it) }
                    row.optInt("cpu_mhz").takeIf { row.has("cpu_mhz") }?.let { put("cpu_mhz", it) }
                    row.optInt("apb_mhz").takeIf { row.has("apb_mhz") }?.let { put("apb_mhz", it) }
                    row.optInt("spool_free_b").takeIf { row.has("spool_free_b") }?.let { put("spool_free_b", it) }
                    row.optInt("spool_cap_b").takeIf { row.has("spool_cap_b") }?.let { put("spool_cap_b", it) }
                    row.optInt("spool_pending").takeIf { row.has("spool_pending") }?.let { put("spool_pending", it) }
                    row.optInt("dram_free_kb").takeIf { row.has("dram_free_kb") }?.let { put("dram_free_kb", it) }
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
            postJson("${settings.baseUrl}/v1/ingest/telemetry", body)
        } catch (e: Exception) {
            offload.restoreTelemetry(lines)
            return Result(false, e.message ?: "telemetry upload failed")
        }
        if (!result.ok) {
            offload.restoreTelemetry(lines)
        } else if (result.accepted > 0) {
            reportIngestBatch("telemetry", result.accepted, body.length)
        }
        return result
    }

    private fun reportIngestBatch(kind: String, records: Int, bytes: Int) {
        if (records <= 0) return
        val batch = JSONObject().apply {
            put("ts_ms", System.currentTimeMillis())
            put("kind", kind)
            put("records", records)
            put("bytes", bytes)
        }
        val body = JSONObject().apply {
            put("schema", "imu.ingest.v1")
            put("device_id", settings.deviceId)
            put("group_id", settings.groupId)
            put("phone_id", settings.phoneId(context))
            put("sent_at_ms", System.currentTimeMillis())
            put("batches", JSONArray().put(batch))
        }.toString()
        postJson("${settings.baseUrl}/v1/ingest/batches", body)
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
                    if (row.has("fw_version")) {
                        val fw = row.optString("fw_version")
                        put("fw_version", fw)
                        if (fw.isNotBlank()) OtaSettings(context).noteFw(fw)
                    }
                    if (row.has("reset_reason")) put("reset_reason", row.optInt("reset_reason"))
                    if (row.has("soft_reboot_reason")) {
                        put("soft_reboot_reason", row.optString("soft_reboot_reason"))
                    }
                    if (row.has("is_fatal")) put("is_fatal", row.optBoolean("is_fatal"))
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

    fun uploadPendingBatteryBench(maxLines: Int = 500): Result {
        if (!settings.enabled) {
            return Result(false, "cloud disabled")
        }
        val lines = batteryBenchStore.drain(maxLines)
        if (lines.isEmpty()) {
            return Result(true, "no bench pending", 0)
        }
        val records = JSONArray()
        for (line in lines) {
            val row = JSONObject(line)
            records.put(
                JSONObject().apply {
                    put("type", "battery_bench_sample")
                    put("session_id", row.optLong("session_id"))
                    put("seq", row.optLong("seq"))
                    put("ts_ms", row.optLong("ts_ms"))
                    put("voltage", row.optDouble("voltage"))
                    put("pct", row.optInt("pct"))
                    put("trend_v", row.optDouble("trend_v"))
                    put("src", row.optInt("src"))
                    row.optInt("cpu_mhz").takeIf { row.has("cpu_mhz") }?.let { put("cpu_mhz", it) }
                    row.optInt("imu_hz").takeIf { row.has("imu_hz") }?.let { put("imu_hz", it) }
                    row.optInt("render_hz").takeIf { row.has("render_hz") }?.let { put("render_hz", it) }
                    row.optDouble("chip_temp_c").takeIf { row.has("chip_temp_c") }?.let { put("chip_temp_c", it) }
                    row.optLong("uptime_ms").takeIf { row.has("uptime_ms") }?.let { put("uptime_ms", it) }
                    put("session_started_ms", row.optLong("session_started_ms"))
                    put("session_stopped", row.optBoolean("session_stopped"))
                    row.optString("label").takeIf { row.has("label") && it.isNotBlank() }?.let { put("label", it) }
                    row.optJSONObject("profile_snapshot")?.let { put("profile_snapshot", it) }
                    put("cell_mah", 500)
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
            postJson("${settings.baseUrl}/v1/ingest/battery_bench", body)
        } catch (e: Exception) {
            batteryBenchStore.restoreLines(lines)
            return Result(false, e.message ?: "battery bench upload failed")
        }
        if (!result.ok) {
            batteryBenchStore.restoreLines(lines)
        }
        return result
    }

    /** Clock correction event from ESP STATUS. */
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
                        put("type", "clock")
                        put("ts_ms", System.currentTimeMillis())
                        put("drift_ms", driftMs)
                        put("corr_ms", corrMs)
                        put("tz_min", tzMin)
                        put("unix_sec", unixSec)
                        put("src", src)
                    },
                ),
            )
        }.toString()
        return try {
            postJson("${settings.baseUrl}/v1/ingest/clock", body)
        } catch (e: Exception) {
            Result(false, e.message ?: "clock upload failed")
        }
    }

    /** Live AHRS orientation relay (phone -> backend -> web debug page). Deliberately no
     *  offload/retry queue here — see AhrsSampleIn on the backend: this is ephemeral, best-
     *  effort debug telemetry, not durable data like crashes/verdicts. Dropping a sample on a
     *  flaky network is fine; the next one supersedes it within ~200ms anyway. */
    /** Live MT200 wearable relay (phone sees piggybacked DATA fields, POSTs /v1/ingest/wearable).
     *  Same best-effort / no-retry path as AHRS: next sample supersedes a dropped one. */
    fun uploadWearableSamples(
        seq: Long,
        unixMs: Long,
        hr: Int?,
        spo2: Int?,
        steps: Int?,
        batteryPct: Int?,
        walkCm: Int? = null,
        rssiEsp: Int? = null,
        rssiMt200: Int? = null,
    ): Result {
        if (!settings.enabled) {
            return Result(false, "cloud disabled")
        }
        val records = JSONArray()
        fun add(kind: String, value: Int, source: String = "mt200") {
            records.put(
                JSONObject().apply {
                    put("type", "wearable")
                    put("ts_ms", unixMs)
                    put("seq", seq)
                    put("source", source)
                    put("kind", kind)
                    put("value", value)
                },
            )
        }
        if (hr != null) add("hr", hr)
        if (spo2 != null) add("spo2", spo2)
        if (steps != null) add("steps", steps)
        if (batteryPct != null) add("battery_pct", batteryPct)
        if (walkCm != null) add("walk_cm", walkCm, source = "esp32")
        if (rssiEsp != null) add("rssi_esp", rssiEsp, source = "phone")
        if (rssiMt200 != null) add("rssi_mt200", rssiMt200, source = "esp32")
        if (records.length() == 0) {
            return Result(true, "no wearable", 0)
        }
        val body = JSONObject().apply {
            put("schema", "imu.ingest.v1")
            put("device_id", settings.deviceId)
            put("group_id", settings.groupId)
            put("phone_id", settings.phoneId(context))
            put("sent_at_ms", System.currentTimeMillis())
            put("records", records)
        }.toString()
        return try {
            postJson("${settings.baseUrl}/v1/ingest/wearable", body)
        } catch (e: Exception) {
            Result(false, e.message ?: "wearable upload failed")
        }
    }

    fun uploadAhrsSample(seq: Long, unixMs: Long, rot: DoubleArray): Result {
        if (!settings.enabled) {
            return Result(false, "cloud disabled")
        }
        val body = JSONObject().apply {
            put("device_id", settings.deviceId)
            put("unix_ms", unixMs)
            put("seq", seq)
            put("rot", JSONArray(rot.toList()))
        }.toString()
        return try {
            postJson("${settings.baseUrl}/v1/ingest/ahrs", body)
        } catch (e: Exception) {
            Result(false, e.message ?: "ahrs upload failed")
        }
    }

    /** GPS anchor / IMU dead-reckoning point relay (Phase 3 — see GeoTracker). Same best-effort,
     *  no-retry philosophy as uploadAhrsSample: this is preprod-demo route-comparison debug
     *  data, not a durable trip log. */
    fun uploadGeoPoint(kind: String, lat: Double, lon: Double, unixMs: Long, accuracyM: Double?): Result {
        if (!settings.enabled) {
            return Result(false, "cloud disabled")
        }
        val body = JSONObject().apply {
            put("device_id", settings.deviceId)
            put("kind", kind)
            put("unix_ms", unixMs)
            put("lat", lat)
            put("lon", lon)
            if (accuracyM != null) put("accuracy_m", accuracyM)
        }.toString()
        return try {
            postJson("${settings.baseUrl}/v1/ingest/geo", body)
        } catch (e: Exception) {
            Result(false, e.message ?: "geo upload failed")
        }
    }

    fun uploadReferenceProfiles(refListJson: String): Result {
        if (!settings.enabled) {
            return Result(false, "cloud disabled")
        }
        val root = JSONObject(refListJson)
        val slots = root.optJSONArray("slots") ?: return Result(true, "no slots", 0)
        val records = JSONArray()
        for (i in 0 until slots.length()) {
            val s = slots.optJSONObject(i) ?: continue
            if (!s.optBoolean("valid", false)) continue
            records.put(
                JSONObject().apply {
                    put("type", "reference_profile")
                    put("slot", s.optInt("slot", i))
                    put("name", s.optString("name", ""))
                    put("duration_ms", s.optInt("duration_ms", 0))
                    put("sample_hz", s.optDouble("sample_hz", 0.0).takeIf { s.has("sample_hz") })
                    s.optJSONArray("bands")?.let { put("bands", it) }
                    put("active", s.optInt("slot", i) == root.optInt("active", -1))
                },
            )
        }
        if (records.length() == 0) {
            return Result(true, "no valid refs", 0)
        }
        val body = JSONObject().apply {
            put("schema", "imu.ingest.v1")
            put("device_id", settings.deviceId)
            put("group_id", settings.groupId)
            put("phone_id", settings.phoneId(context))
            put("sent_at_ms", System.currentTimeMillis())
            put("records", records)
        }.toString()
        return try {
            postJson("${settings.baseUrl}/v1/ingest/reference_profiles", body)
        } catch (e: Exception) {
            Result(false, e.message ?: "reference upload failed")
        }
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
            val duplicates = resp.optInt("duplicates", 0)
            return Result(true, "uploaded $accepted", accepted, duplicates)
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
