package com.esp32s3.imusim

import android.util.Base64
import org.json.JSONObject
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** Logical JSON view of DeviceConfigV1 — matches backend/schema/device_config.v1.json */
object DeviceConfigJson {
    const val SCHEMA = "device.config.v1"
    const val LOCAL_REV_FLAG = 0x80000000.toInt()
    const val LOCAL_TFT_OFF = 0x01

    data class Doc(
        val revision: Long,
        val localRevision: Long,
        val source: String,
        val profile: JSONObject,
        val vibro: JSONObject,
        val mix: JSONObject,
        val local: JSONObject,
        val blobB64: String?,
    )

    fun fromBlob(blob: ByteArray, source: String = "esp"): Doc {
        require(blob.size >= ConfigProtocol.BLOB_SIZE) { "blob too short" }
        val bb = ByteBuffer.wrap(blob).order(ByteOrder.LITTLE_ENDIAN)
        check(bb.int == 0x31494D55) { "bad magic" }
        bb.position(136)
        val pollMs = bb.short.toInt() and 0xFFFF
        val bleMode = bb.get().toInt() and 0xFF
        bb.position(148)
        val powerProfile = bb.get().toInt() and 0xFF
        val tftPolicy = bb.get().toInt() and 0xFF
        val wakeSec = bb.short.toInt() and 0xFFFF
        val activeSec = bb.get().toInt() and 0xFF
        val cpuMhz = bb.get().toInt() and 0xFF
        val imuHz = bb.get().toInt() and 0xFF
        bb.get()
        val deepSleep = bb.get().toInt() and 0xFF
        bb.int // created
        val revision = bb.int.toLong() and 0xFFFFFFFFL
        val vibroMode = bb.get().toInt() and 0xFF
        val interval = bb.get().toInt() and 0xFF
        val window = bb.get().toInt() and 0xFF
        val jitter = bb.get().toInt() and 0xFF
        val tier = bb.get().toInt() and 0xFF
        val wifiUp = bb.get().toInt() and 0xFF
        val mixEvery = bb.get().toInt() and 0xFF
        val mixRatio = bb.get().toInt() and 0xFF
        val dynShort = bb.get().toInt() and 0xFF
        val dynNested = bb.get().toInt() and 0xFF
        val localRevision = bb.int.toLong() and 0xFFFFFFFFL
        val localFlags = bb.get().toInt() and 0xFF

        val profile = JSONObject().apply {
            put("power_profile", powerProfile)
            put("tft_policy", tftPolicy)
            put("wake_interval_sec", wakeSec)
            put("active_window_sec", activeSec)
            put("cpu_mhz", cpuMhz)
            put("imu_sample_hz", imuHz)
            put("deep_sleep_enable", deepSleep)
            put("ble_poll_ms", pollMs)
            put("ble_default_mode", bleMode)
        }
        val vibro = JSONObject().apply {
            put("schedule_mode", vibroMode)
            put("interval_sec", interval)
            put("window_sec", window)
            put("jitter_sec", jitter)
            put("capture_tier", tier)
            put("wifi_upload_enable", wifiUp)
        }
        val mix = JSONObject().apply {
            put("every", mixEvery)
            put("ratio", mixRatio)
            put("dyn_short", dynShort)
            put("dyn_nested", dynNested)
        }
        val local = JSONObject().apply {
            put("tft_user_off", (localFlags and LOCAL_TFT_OFF) != 0)
        }
        return Doc(
            revision = revision,
            localRevision = localRevision,
            source = source,
            profile = profile,
            vibro = vibro,
            mix = mix,
            local = local,
            blobB64 = Base64.encodeToString(blob.copyOf(ConfigProtocol.BLOB_SIZE), Base64.NO_WRAP),
        )
    }

    fun toJson(doc: Doc, appVersion: String? = null): JSONObject =
        JSONObject().apply {
            put("schema", SCHEMA)
            put("revision", doc.revision)
            put("local_revision", doc.localRevision)
            put("source", doc.source)
            put("profile", doc.profile)
            put("vibro", doc.vibro)
            put("mix", doc.mix)
            put("local", doc.local)
            doc.blobB64?.let { put("blob_b64", it) }
            appVersion?.let { put("app_version", it) }
        }

    fun mergeIntoBlob(base: ByteArray, doc: Doc, newRevision: Long): ByteArray {
        val blob = base.copyOf(ConfigProtocol.BLOB_SIZE)
        val bb = ByteBuffer.wrap(blob).order(ByteOrder.LITTLE_ENDIAN)
        val p = doc.profile
        val v = doc.vibro
        val m = doc.mix
        bb.putShort(136, p.optInt("ble_poll_ms", 33).toShort())
        bb.put(138, p.optInt("ble_default_mode", 1).toByte())
        bb.put(148, p.optInt("power_profile", 2).toByte())
        bb.put(149, p.optInt("tft_policy", 2).toByte())
        bb.putShort(150, p.optInt("wake_interval_sec", 0).toShort())
        bb.put(152, p.optInt("active_window_sec", 0).toByte())
        bb.put(153, p.optInt("cpu_mhz", 80).toByte())
        bb.put(154, p.optInt("imu_sample_hz", 10).toByte())
        bb.put(156, p.optInt("deep_sleep_enable", 0).toByte())
        bb.putInt(161, newRevision.toInt())
        bb.put(165, v.optInt("schedule_mode", 0).toByte())
        bb.put(166, v.optInt("interval_sec", 60).toByte())
        bb.put(167, v.optInt("window_sec", 15).toByte())
        bb.put(168, v.optInt("jitter_sec", 0).toByte())
        bb.put(169, v.optInt("capture_tier", 0).toByte())
        bb.put(170, v.optInt("wifi_upload_enable", 0).toByte())
        bb.put(171, m.optInt("every", 0).toByte())
        bb.put(172, m.optInt("ratio", 0).toByte())
        bb.put(173, m.optInt("dyn_short", 0).toByte())
        bb.put(174, m.optInt("dyn_nested", 0).toByte())
        return blob
    }

    fun nextCloudRevision(current: Long): Long {
        val now = System.currentTimeMillis() / 1000L
        return maxOf(current + 1L, now)
    }
}
