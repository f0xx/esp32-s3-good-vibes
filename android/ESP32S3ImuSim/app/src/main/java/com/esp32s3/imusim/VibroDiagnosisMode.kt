package com.esp32s3.imusim

/** Vibro diagnosis capture tiers — maps to DeviceConfigV1 vibro_capture_tier + schedule. */
object VibroDiagnosisMode {
    enum class Id(val label: String, val tier: Int) {
        NORMAL("Normal", 0),
        LOW_RPM("Low RPM diagnosis", 1),
        ULTRA_LOW_RPM("Ultra-low RPM diagnosis", 2),
        INTERMITTENT("Intermittent machine", 3),
    }

    data class Spec(
        val tier: Int,
        val scheduleMode: Int,
        val intervalSec: Int,
        val windowSec: Int,
        val jitterSec: Int,
        val imuHz: Int,
        val pollMs: Int,
        val deepSleep: Int = 0,
        val wakeSec: Int = 0,
        val activeSec: Int = 0,
    )

    fun spec(mode: Id): Spec = when (mode) {
        Id.NORMAL -> Spec(
            tier = 0,
            scheduleMode = 0,
            intervalSec = 60,
            windowSec = 15,
            jitterSec = 0,
            imuHz = 100,
            pollMs = 33,
        )
        Id.LOW_RPM -> Spec(
            tier = 1,
            scheduleMode = 1,
            intervalSec = 120,
            windowSec = 30,
            jitterSec = 0,
            imuHz = 50,
            pollMs = 50,
        )
        Id.ULTRA_LOW_RPM -> Spec(
            tier = 2,
            scheduleMode = 1,
            intervalSec = 300,
            windowSec = 60,
            jitterSec = 0,
            imuHz = 25,
            pollMs = 100,
        )
        Id.INTERMITTENT -> Spec(
            tier = 3,
            scheduleMode = 2,
            intervalSec = 3600,
            windowSec = 90,
            jitterSec = 17,
            imuHz = 25,
            pollMs = 100,
            deepSleep = 1,
            wakeSec = 3600,
            activeSec = 90,
        )
    }

    fun describe(mode: Id): String = when (mode) {
        Id.NORMAL -> "Continuous capture, full 256-sample ref, ~10 Hz effective."
        Id.LOW_RPM -> "2× decimation, 128-sample ref, 120 s interval / 30 s window."
        Id.ULTRA_LOW_RPM -> "4× decimation, 64-sample ref, 5 min interval / 60 s window."
        Id.INTERMITTENT -> "Random slot in hourly window, deep-sleep between captures."
    }

    fun fromTier(tier: Int?): Id? = Id.values().find { it.tier == tier }

    fun applyToBlob(blob: ByteArray, mode: Id): ByteArray {
        val out = if (blob.size >= ConfigProtocol.BLOB_SIZE) {
            blob.copyOf(ConfigProtocol.BLOB_SIZE)
        } else {
            ProfilePresets.apply(null, ProfilePresets.Id.VIBRO_NORMAL).blob
        }
        val s = spec(mode)
        val bb = java.nio.ByteBuffer.wrap(out).order(java.nio.ByteOrder.LITTLE_ENDIAN)
        bb.putShort(136, s.pollMs.toShort())
        bb.put(154, s.imuHz.toByte())
        if (s.deepSleep != 0) {
            bb.put(156, s.deepSleep.toByte())
            bb.putShort(150, s.wakeSec.toShort())
            bb.put(152, s.activeSec.toByte())
        }
        bb.position(165)
        bb.put(s.scheduleMode.toByte())
        bb.put(s.intervalSec.toByte())
        bb.put(s.windowSec.toByte())
        bb.put(s.jitterSec.toByte())
        bb.put(s.tier.toByte())
        val wifiUpload = bb.get(170).toInt() and 0xFF
        bb.put(170, wifiUpload.toByte())
        when (mode) {
            Id.INTERMITTENT -> {
                bb.put(6)
                bb.put(4)
                bb.put(2)
            }
            Id.LOW_RPM -> {
                bb.put(4)
                bb.put(2)
                bb.put(0)
            }
            Id.ULTRA_LOW_RPM -> {
                bb.put(6)
                bb.put(3)
                bb.put(0)
            }
            else -> {
                bb.put(0)
                bb.put(0)
                bb.put(0)
            }
        }
        return out
    }
}
