package com.esp32s3.imusim

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/** Queues battery-bench samples for cloud upload (offline-safe). */
class BatteryBenchStore(context: Context) {
    private val file = File(context.filesDir, "battery_bench_pending.jsonl")

    data class Sample(
        val sessionId: Long,
        val seq: Long,
        val tsMs: Long,
        val voltageV: Float,
        val pct: Int,
        val trendV: Float,
        val src: Int,
        val cpuMhz: Int?,
        val imuHz: Int?,
        val renderHz: Int?,
        val chipTempC: Float?,
        val uptimeMs: Long?,
        val sessionStartedMs: Long,
        val sessionStopped: Boolean,
        val label: String?,
        val profileSnapshot: JSONObject?,
    )

    @Synchronized
    fun append(sample: Sample) {
        file.appendText(sampleToJson(sample).toString() + "\n")
    }

    @Synchronized
    fun drain(maxLines: Int = 500): List<String> {
        if (!file.exists() || file.length() == 0L) {
            return emptyList()
        }
        val lines = file.readLines().filter { it.isNotBlank() }
        if (lines.isEmpty()) {
            file.delete()
            return emptyList()
        }
        val take = lines.take(maxLines)
        val rest = lines.drop(maxLines)
        if (rest.isEmpty()) {
            file.delete()
        } else {
            file.writeText(rest.joinToString("\n") + "\n")
        }
        return take
    }

    @Synchronized
    fun pendingCount(): Int {
        if (!file.exists()) return 0
        return file.readLines().count { it.isNotBlank() }
    }

    @Synchronized
    fun restoreLines(lines: List<String>) {
        if (lines.isEmpty()) return
        val existing = if (file.exists()) file.readLines().filter { it.isNotBlank() } else emptyList()
        file.writeText((lines + existing).joinToString("\n") + "\n")
    }

    companion object {
        fun sampleToJson(s: Sample): JSONObject = JSONObject().apply {
            put("type", "battery_bench_sample")
            put("session_id", s.sessionId)
            put("seq", s.seq)
            put("ts_ms", s.tsMs)
            put("voltage", s.voltageV.toDouble())
            put("pct", s.pct)
            put("trend_v", s.trendV.toDouble())
            put("src", s.src)
            s.cpuMhz?.let { put("cpu_mhz", it) }
            s.imuHz?.let { put("imu_hz", it) }
            s.renderHz?.let { put("render_hz", it) }
            s.chipTempC?.let { put("chip_temp_c", it.toDouble()) }
            s.uptimeMs?.let { put("uptime_ms", it) }
            put("session_started_ms", s.sessionStartedMs)
            put("session_stopped", s.sessionStopped)
            s.label?.let { put("label", it) }
            s.profileSnapshot?.let { put("profile_snapshot", it) }
        }
    }
}
