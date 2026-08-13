package com.esp32s3.imusim

import org.json.JSONArray
import org.json.JSONObject
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.UUID

object ImuProtocol {
    const val DEVICE_NAME = "ESP32S3 IMU sim"

    /** Match firmware BLE_IMU_CONNECT_GRACE_MS + BLE_IMU_POST_GRACE_MS (+500 ms margin).
     *  Phone GATT writes (MODE/poll/TIME/crash/offload) before this elapses stack against
     *  ESP connect handling and have triggered TG0WDT_SYS_RST (task_wdt, 2 s HW fallback). */
    const val ESP_CONNECT_SETTLE_MS = 14_500L

    val SERVICE_UUID: UUID = UUID.fromString("4a6e0001-0000-1000-8000-00805f9b34fb")
    val CHAR_MODE_UUID: UUID = UUID.fromString("4a6e0002-0000-1000-8000-00805f9b34fb")
    val CHAR_STATUS_UUID: UUID = UUID.fromString("4a6e0003-0000-1000-8000-00805f9b34fb")
    val CHAR_DATA_UUID: UUID = UUID.fromString("4a6e0004-0000-1000-8000-00805f9b34fb")
    val CHAR_POLL_MS_UUID: UUID = UUID.fromString("4a6e0005-0000-1000-8000-00805f9b34fb")
    val CHAR_NOTIFY_UUID: UUID = UUID.fromString("4a6e0006-0000-1000-8000-00805f9b34fb")
    val CHAR_TIME_UUID: UUID = UUID.fromString("4a6e0007-0000-1000-8000-00805f9b34fb")
    val CHAR_CAPS_UUID: UUID = UUID.fromString("4a6e0008-0000-1000-8000-00805f9b34fb")
    val CHAR_SCREEN_UUID: UUID = UUID.fromString("4a6e0009-0000-1000-8000-00805f9b34fb")
    /** Manual CPU clock override, 1 byte: 0 = auto, else explicit target MHz. */
    val CHAR_CPU_MHZ_UUID: UUID = UUID.fromString("4a6e000a-0000-1000-8000-00805f9b34fb")
    /** Manual IMU sample-rate override, 1 byte: 0 = auto, else explicit Hz (1-120). */
    val CHAR_IMU_HZ_UUID: UUID = UUID.fromString("4a6e000b-0000-1000-8000-00805f9b34fb")

    const val CAP_IMU = 1 shl 0
    const val CAP_TFT = 1 shl 1
    const val CAP_BLE_CONFIG = 1 shl 2
    const val CAP_CHIP_TEMP = 1 shl 3
    const val CAP_VIBRO = 1 shl 4
    const val CAP_WIFI = 1 shl 5
    const val CAP_OTA = 1 shl 6
    const val CAP_CRASH_DEBUG = 1 shl 7

    const val MODE_RAW = 0
    const val MODE_COMPUTED = 1
    const val MODE_SCENE = 2

    const val POWER_UNKNOWN = 0
    const val POWER_BATTERY = 1
    const val POWER_DC_USB = 2


    const val VERDICT_OK = 0
    const val VERDICT_WARN = 1
    const val VERDICT_ALERT = 2

    const val DEFAULT_POLL_MS = 33
    /** Matches esp32 RENDER_HZ (30 fps). */
    const val LIVE_POLL_MS = 33
    const val MIN_POLL_MS = 33
    const val SCREEN_W = 172
    const val SCREEN_H = 320

    const val PROFILE_DEEP_SLEEP = 1
    const val PROFILE_BALANCED = 2
    const val PROFILE_PERFORMANCE = 3
    const val PROFILE_DC_SAVE = 4
    const val PROFILE_DC_FULL = 5

    data class Status(
        val seq: Long,
        val mode: Int,
        val count: Int,
        val bytes: Int,
        val powerSource: Int = POWER_UNKNOWN,
        val voltageV: Float = 0f,
        val percent: Int = 0,
        val trendV: Float = 0f,
        val chipTempC: Float? = null,
        val vibroRmsG: Float? = null,
        val vibroPeakG: Float? = null,
        val vibroRefReady: Boolean = false,
        val vibroVerdictLevel: Int? = null,
        val vibroCorr: Float? = null,
        val vibroRmsDelta: Float? = null,
        val offloadAckSeq: Long? = null,
        val offloadPending: Int? = null,
        val powerProfile: Int? = null,
        val awakeSecondsRemaining: Int? = null,
        val captureActive: Boolean? = null,
        val vibroTier: Int? = null,
        val captureWindowSecRemaining: Int? = null,
        val captureWindowSecUntil: Int? = null,
        val screenOn: Boolean? = null,
        val edgeCrest: Float? = null,
        val edgeZcrHz: Float? = null,
        val edgeHfRatio: Float? = null,
        val radioMode: String? = null,
        val bandRms: FloatArray? = null,
        val pendingSessionSeq: Long? = null,
        val bandCorr: Float? = null,
        val bandDeltaMax: Float? = null,
        val captureMixWindowSec: Int? = null,
        val crashDebugEnabled: Boolean = false,
        val bistStatus: String? = null,
        val resetReason: String? = null,
        val configSeq: Long? = null,
        val localConfigRev: Long? = null,
        val clockSynced: Boolean? = null,
        val clockSource: Int? = null,
        val clockTzMin: Int? = null,
        val clockDriftMs: Long? = null,
        val clockCorrMs: Long? = null,
        val clockUnixSec: Long? = null,
        /** Mode/override-derived MHz before the BLE-linked 240 MHz floor. */
        val cpuMhzDesired: Int? = null,
        /** 0 = auto (mode-derived); nonzero = manual override currently in effect. */
        val cpuMhzOverride: Int? = null,
        /** Actually-applied CPU MHz (settled). */
        val cpuMhzApplied: Int? = null,
        /** True while BT/BLE forces the 240 MHz safety floor. */
        val cpuBleClamped: Boolean = false,
        val imuHzTarget: Int? = null,
        val imuHzOverride: Int? = null,
    )

    data class PowerStatus(
        val source: Int,
        val voltageV: Float,
        val percent: Int,
        val valid: Boolean,
        val trendV: Float = 0f,
    ) {
        fun caption(): String {
            if (!valid) return "p/s:--"
            val effectiveSource = effectiveSource()
            return when (effectiveSource) {
                POWER_DC_USB ->
                    if (voltageV >= 3.25f) {
                        String.format(java.util.Locale.US, "p/s:DC %.2fV", voltageV)
                    } else {
                        "p/s:DC ext"
                    }
                POWER_BATTERY -> String.format(java.util.Locale.US, "p/s:BAT %d%%", percent)
                else -> "p/s:--"
            }
        }

        /** USB float-charge reads flat near 4.2 V while firmware still says BAT. */
        private fun effectiveSource(): Int {
            if (source == POWER_DC_USB) return POWER_DC_USB
            if (source == POWER_BATTERY && voltageV >= 4.15f && trendV >= -0.004f) {
                return POWER_DC_USB
            }
            return source
        }
    }

    data class RawRecord(
        val tMs: Long,
        val ax: Float,
        val ay: Float,
        val az: Float,
        val gx: Float,
        val gy: Float,
        val gz: Float,
        val distanceM: Float = 0f,
    )

    data class ComputedRecord(
        val tMs: Long,
        val distanceM: Float,
        val footerX: Float,
        val footerY: Float,
        val footerZ: Float,
        val zoomX: Float,
        val zoomY: Float,
        val zoomZ: Float,
        val rot: FloatArray,
        val axes: FloatArray,
    )

    data class SceneRecord(
        val tMs: Long,
        val distanceM: Float,
        val footerX: Float,
        val footerY: Float,
        val footerZ: Float,
        val axes: FloatArray,
        val corners: FloatArray,
    )

    data class Batch(
        val seq: Long,
        val mode: Int,
        val screenW: Int,
        val screenH: Int,
        val powerSource: Int = POWER_UNKNOWN,
        val voltageV: Float = 0f,
        val percent: Int = 0,
        val trendV: Float = 0f,
        val raw: List<RawRecord> = emptyList(),
        val computed: List<ComputedRecord> = emptyList(),
        val scene: List<SceneRecord> = emptyList(),
    )

    fun parseStatus(json: String): Status {
        val o = JSONObject(json.trim())
        return parseStatusObject(o)
    }

    fun parseStatusLenient(json: String): Status? = try {
        parseStatus(json)
    } catch (_: Exception) {
        null
    }

    /** Reject torn/truncated ATT long-read or NOTIFY payloads before JSONObject tries. */
    fun looksLikeCompleteJson(json: String): Boolean {
        val t = json.trim()
        if (t.length < 2 || t[0] != '{') return false
        var i = t.length - 1
        while (i >= 0 && t[i].isWhitespace()) i--
        return i > 0 && t[i] == '}'
    }

    private fun parseStatusObject(o: JSONObject): Status = Status(
            seq = o.optLong("s"),
            mode = o.optInt("m"),
            count = o.optInt("n"),
            bytes = o.optInt("b"),
            powerSource = o.optInt("p", POWER_UNKNOWN),
            voltageV = o.optDouble("v", 0.0).toFloat(),
            percent = o.optInt("pct", 0),
            trendV = if (o.has("tr")) o.optDouble("tr").toFloat() else 0f,
            chipTempC = if (o.has("tc")) o.optDouble("tc").toFloat() else null,
            vibroRmsG = if (o.has("vrms")) o.optDouble("vrms").toFloat() else null,
            vibroPeakG = if (o.has("vpeak")) o.optDouble("vpeak").toFloat() else null,
            vibroRefReady = o.optInt("ref", 0) != 0,
            vibroVerdictLevel = if (o.has("vd")) o.optInt("vd") else null,
            vibroCorr = if (o.has("vcorr")) o.optDouble("vcorr").toFloat() else null,
            vibroRmsDelta = if (o.has("vrmsd")) o.optDouble("vrmsd").toFloat() else null,
            offloadAckSeq = if (o.has("ack")) o.optLong("ack") else null,
            offloadPending = if (o.has("opend")) o.optInt("opend") else null,
            powerProfile = if (o.has("pp")) o.optInt("pp") else null,
            awakeSecondsRemaining = if (o.has("awake")) o.optInt("awake") else null,
            captureActive = if (o.has("cap")) o.optInt("cap") != 0 else null,
            vibroTier = if (o.has("vt")) o.optInt("vt") else null,
            captureWindowSecRemaining = if (o.has("capwin")) o.optInt("capwin") else null,
            captureWindowSecUntil = if (o.has("capuntil")) o.optInt("capuntil") else null,
            screenOn = if (o.has("scr")) o.optInt("scr") != 0 else null,
            edgeCrest = if (o.has("cr")) o.optDouble("cr").toFloat() else null,
            edgeZcrHz = if (o.has("zcr")) o.optDouble("zcr").toFloat() else null,
            edgeHfRatio = if (o.has("hfr")) o.optDouble("hfr").toFloat() else null,
            radioMode = if (o.has("radio")) o.optString("radio") else null,
            bandRms = o.optJSONArray("bnd")?.let { arr ->
                FloatArray(arr.length()) { i -> arr.optDouble(i).toFloat() }
            },
            pendingSessionSeq = if (o.has("psess")) o.optLong("psess") else null,
            bandCorr = if (o.has("bcorr")) o.optDouble("bcorr").toFloat() else null,
            bandDeltaMax = if (o.has("bdmax")) o.optDouble("bdmax").toFloat() else null,
            captureMixWindowSec = if (o.has("capmix")) o.optInt("capmix") else null,
            crashDebugEnabled = o.optInt("dbg", 0) != 0,
            bistStatus = if (o.has("bist")) o.optString("bist") else null,
            resetReason = if (o.has("rr")) o.optString("rr") else null,
            configSeq = if (o.has("cfgseq")) o.optLong("cfgseq") else null,
            localConfigRev = if (o.has("locrev")) o.optLong("locrev") else null,
            clockSynced = if (o.has("clks")) o.optInt("clks") != 0 else null,
            clockSource = if (o.has("clksrc")) o.optInt("clksrc") else null,
            clockTzMin = if (o.has("tz")) o.optInt("tz") else null,
            clockDriftMs = if (o.has("clkd")) o.optLong("clkd") else null,
            clockCorrMs = if (o.has("clkc")) o.optLong("clkc") else null,
            clockUnixSec = if (o.has("clku")) o.optLong("clku") else null,
            cpuMhzDesired = if (o.has("cpumhz")) o.optInt("cpumhz") else null,
            cpuMhzOverride = if (o.has("cpuov")) o.optInt("cpuov") else null,
            cpuMhzApplied = if (o.has("cpuact")) o.optInt("cpuact") else null,
            cpuBleClamped = o.optInt("cpuclamp", 0) != 0,
            imuHzTarget = if (o.has("imuhz")) o.optInt("imuhz") else null,
            imuHzOverride = if (o.has("imuov")) o.optInt("imuov") else null,
        )

    fun crashDebugFromCaps(caps: Int): Boolean = (caps and CAP_CRASH_DEBUG) != 0

    fun verdictCaption(level: Int, corr: Float?): String {
        val tag = when (level) {
            VERDICT_ALERT -> "ALERT"
            VERDICT_WARN -> "WARN"
            else -> "OK"
        }
        return if (corr != null) {
            String.format(java.util.Locale.US, "vib:%s c=%.2f", tag, corr)
        } else {
            "vib:$tag"
        }
    }

    fun parseCaps(data: ByteArray): Int {
        if (data.size < 4) return 0
        val bb = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
        return bb.int
    }

    fun capsCaption(caps: Int): String {
        val parts = mutableListOf<String>()
        if (caps and CAP_IMU != 0) parts.add("IMU")
        if (caps and CAP_TFT != 0) parts.add("TFT")
        if (caps and CAP_VIBRO != 0) parts.add("VIB")
        if (caps and CAP_CHIP_TEMP != 0) parts.add("TEMP")
        if (caps and CAP_WIFI != 0) parts.add("WiFi")
        if (caps and CAP_OTA != 0) parts.add("OTA")
        if (caps and CAP_CRASH_DEBUG != 0) parts.add("DBG")
        return if (parts.isEmpty()) "caps:--" else "caps:${parts.joinToString("+")}"
    }

    fun powerFromStatus(status: Status): PowerStatus =
        powerFromFields(status.powerSource, status.voltageV, status.percent, status.trendV)

    fun powerFromFields(source: Int, voltageV: Float, percent: Int, trendV: Float = 0f): PowerStatus {
        if (source == POWER_UNKNOWN && voltageV <= 0f) {
            return PowerStatus(POWER_UNKNOWN, 0f, 0, false, trendV)
        }
        return PowerStatus(
            source = source,
            voltageV = voltageV,
            percent = percent,
            valid = source != POWER_UNKNOWN || voltageV >= 0.40f,
            trendV = trendV,
        )
    }

    /** Scene row: [t,dm,fx,fy,fz, axes×12, corners×16] → 33 fields. */
    private const val SCENE_ROW_MIN_LEN = 33

    /** Lightweight header for dedup/stats without parsing the full `d` payload. */
    data class BatchHeader(val seq: Long, val mode: Int, val recordCount: Int)

    /** Parse only batch metadata (seq/mode/record count) — skips row arrays. */
    fun peekBatchHeader(json: String): BatchHeader? {
        val trimmed = json.trim()
        if (trimmed.length < 8 || !trimmed.startsWith("{")) {
            return null
        }
        return try {
            val o = org.json.JSONObject(trimmed)
            val mode = o.optInt("m")
            val seq = o.optLong("s")
            val recordCount = o.optJSONArray("d")?.length() ?: 0
            BatchHeader(seq, mode, recordCount)
        } catch (_: Exception) {
            null
        }
    }

    fun parseBatch(json: String): Batch {
        val trimmed = json.trim()
        if (trimmed.length < 8 || !trimmed.startsWith("{")) {
            throw IllegalArgumentException("truncated JSON (${trimmed.length} B)")
        }
        val o = JSONObject(trimmed)
        val mode = o.optInt("m")
        val seq = o.optLong("s")
        val w = o.optInt("w", SCREEN_W)
        val h = o.optInt("h", SCREEN_H)
        val powerSource = o.optInt("p", POWER_UNKNOWN)
        val voltageV = o.optDouble("v", 0.0).toFloat()
        val percent = o.optInt("pct", 0)
        val trendV = if (o.has("tr")) o.optDouble("tr").toFloat() else 0f
        val d = o.optJSONArray("d") ?: JSONArray()

        val base = Batch(seq, mode, w, h, powerSource, voltageV, percent, trendV)
        return when (mode) {
            MODE_RAW -> base.copy(raw = parseRawArray(d))
            MODE_COMPUTED -> base.copy(computed = parseComputedArray(d))
            MODE_SCENE -> base.copy(scene = parseSceneArray(d))
            else -> base
        }
    }

    fun parseBatchLenient(json: String): Batch? = try {
        parseBatch(json)
    } catch (_: Exception) {
        null
    }

    private fun parseRawArray(arr: JSONArray): List<RawRecord> {
        val out = ArrayList<RawRecord>(arr.length())
        for (i in 0 until arr.length()) {
            val row = arr.getJSONArray(i)
            out.add(
                RawRecord(
                    tMs = row.getLong(0),
                    ax = row.getDouble(1).toFloat(),
                    ay = row.getDouble(2).toFloat(),
                    az = row.getDouble(3).toFloat(),
                    gx = row.getDouble(4).toFloat(),
                    gy = row.getDouble(5).toFloat(),
                    gz = row.getDouble(6).toFloat(),
                    distanceM = if (row.length() > 7) row.getDouble(7).toFloat() else 0f,
                )
            )
        }
        return out
    }

    private fun parseComputedArray(arr: JSONArray): List<ComputedRecord> {
        val out = ArrayList<ComputedRecord>(arr.length())
        for (i in 0 until arr.length()) {
            val row = arr.getJSONArray(i)
            when {
                row.length() >= 29 -> out.add(parseComputedV3(row))
                row.length() >= 17 -> out.add(parseComputedMatrix(row))
            }
        }
        return out
    }

    private fun parseComputedV3(row: JSONArray): ComputedRecord {
        val rot = FloatArray(9)
        for (j in 0 until 9) {
            rot[j] = row.getDouble(8 + j).toFloat()
        }
        val axes = FloatArray(12)
        for (j in 0 until 12) {
            axes[j] = row.getDouble(17 + j).toFloat()
        }
        return ComputedRecord(
            tMs = row.getLong(0),
            distanceM = row.getDouble(1).toFloat(),
            footerX = row.getDouble(2).toFloat(),
            footerY = row.getDouble(3).toFloat(),
            footerZ = row.getDouble(4).toFloat(),
            zoomX = row.getDouble(5).toFloat(),
            zoomY = row.getDouble(6).toFloat(),
            zoomZ = row.getDouble(7).toFloat(),
            rot = rot,
            axes = axes,
        )
    }

    /** Legacy matrix batch: [t,m00..m22,dm,zx,zy,zz,wx,wy,wz] — axes projected on phone. */
    private fun parseComputedMatrix(row: JSONArray): ComputedRecord {
        val rot = FloatArray(9)
        for (j in 0 until 9) {
            rot[j] = row.getDouble(1 + j).toFloat()
        }
        return ComputedRecord(
            tMs = row.getLong(0),
            distanceM = row.getDouble(10).toFloat(),
            footerX = row.getDouble(14).toFloat(),
            footerY = row.getDouble(15).toFloat(),
            footerZ = row.getDouble(16).toFloat(),
            zoomX = row.getDouble(11).toFloat(),
            zoomY = row.getDouble(12).toFloat(),
            zoomZ = row.getDouble(13).toFloat(),
            rot = rot,
            axes = FloatArray(12),
        )
    }

    private fun parseSceneArray(arr: JSONArray): List<SceneRecord> {
        val out = ArrayList<SceneRecord>(arr.length())
        for (i in 0 until arr.length()) {
            val row = arr.optJSONArray(i) ?: continue
            if (row.length() < SCENE_ROW_MIN_LEN) {
                continue
            }
            try {
                val axes = FloatArray(12)
                for (j in 0 until 12) {
                    axes[j] = row.optDouble(5 + j).toFloat()
                }
                val corners = FloatArray(16)
                for (j in 0 until 16) {
                    corners[j] = row.optDouble(17 + j).toFloat()
                }
                out.add(
                    SceneRecord(
                        tMs = row.optLong(0),
                        distanceM = row.optDouble(1).toFloat(),
                        footerX = row.optDouble(2).toFloat(),
                        footerY = row.optDouble(3).toFloat(),
                        footerZ = row.optDouble(4).toFloat(),
                        axes = axes,
                        corners = corners,
                    ),
                )
            } catch (_: Exception) {
                // skip malformed row
            }
        }
        return out
    }

    fun pollMsToBytes(ms: Int): ByteArray {
        val bb = ByteBuffer.allocate(2).order(ByteOrder.LITTLE_ENDIAN)
        bb.putShort(ms.toShort())
        return bb.array()
    }

    fun bytesToPollMs(data: ByteArray): Int {
        if (data.size < 2) return DEFAULT_POLL_MS
        val bb = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
        return bb.short.toInt() and 0xFFFF
    }

    /** 8-byte Unix ms + 4-byte tz offset minutes (LE) for BLE TIME characteristic. */
    fun timeSyncPayload(unixMs: Long, tzOffsetMin: Int): ByteArray {
        val bb = ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN)
        bb.putLong(unixMs)
        bb.putInt(tzOffsetMin)
        return bb.array()
    }

    fun unixMsToBytes(ms: Long): ByteArray {
        val bb = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN)
        bb.putLong(ms)
        return bb.array()
    }
}
