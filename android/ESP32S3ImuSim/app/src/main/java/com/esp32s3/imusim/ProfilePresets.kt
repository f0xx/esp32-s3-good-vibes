package com.esp32s3.imusim

import java.nio.ByteBuffer
import java.nio.ByteOrder

/** M-profile wizard presets — maps use-case → DeviceConfigV1 fields + BLE mode. */
object ProfilePresets {
    enum class Id(val label: String) {
        BODY_COMPUTED("Body sensor (computed)"),
        BODY_SCENE("Body sensor (scene mirror)"),
        VIBRO_NORMAL("Vibro: normal"),
        VIBRO_LOW_RPM("Vibro: low RPM diagnosis"),
        VIBRO_ULTRA_LOW_RPM("Vibro: ultra-low RPM"),
        VIBRO_INTERMITTENT("Vibro: intermittent machine"),
        VIBRO_MACHINE("Vibro: machine monitor (mix)"),
        BATTERY_BALANCED("Battery balanced"),
        DC_SAVE("DC power save"),
    }

    data class ApplyResult(
        val preset: Id,
        val bleMode: Int,
        val pollMs: Int,
        val blob: ByteArray,
    )

    /** Human-readable summary shown in profile wizard before push. */
    fun describe(preset: Id): String = when (preset) {
        Id.BODY_COMPUTED -> "Live IMU, step counting, performance power. Requires BLE."
        Id.BODY_SCENE -> "Mirrors on-device scene mode. Requires BLE."
        Id.VIBRO_NORMAL -> VibroDiagnosisMode.describe(VibroDiagnosisMode.Id.NORMAL)
        Id.VIBRO_LOW_RPM -> VibroDiagnosisMode.describe(VibroDiagnosisMode.Id.LOW_RPM)
        Id.VIBRO_ULTRA_LOW_RPM -> VibroDiagnosisMode.describe(VibroDiagnosisMode.Id.ULTRA_LOW_RPM)
        Id.VIBRO_INTERMITTENT -> VibroDiagnosisMode.describe(VibroDiagnosisMode.Id.INTERMITTENT) +
            " Mix 6:1 full/sub capture windows."
        Id.VIBRO_MACHINE -> "Hourly random capture, deep sleep, mix 6:4 + dyn/2 + nest/2 sub-windows."
        Id.BATTERY_BALANCED -> "Lower rate, battery-friendly. Requires BLE."
        Id.DC_SAVE -> "USB power save profile. Requires BLE."
    }

    fun isVibrationPreset(preset: Id): Boolean = when (preset) {
        Id.VIBRO_NORMAL, Id.VIBRO_LOW_RPM, Id.VIBRO_ULTRA_LOW_RPM,
        Id.VIBRO_INTERMITTENT, Id.VIBRO_MACHINE,
        -> true
        else -> false
    }

    private fun vibroSpec(mode: VibroDiagnosisMode.Id): Spec {
        val v = VibroDiagnosisMode.spec(mode)
        val mix = when (mode) {
            VibroDiagnosisMode.Id.INTERMITTENT -> Triple(6, 4, 2)
            VibroDiagnosisMode.Id.LOW_RPM -> Triple(4, 2, 0)
            VibroDiagnosisMode.Id.ULTRA_LOW_RPM -> Triple(6, 3, 0)
            else -> Triple(0, 0, 0)
        }
        val nested = if (mode == VibroDiagnosisMode.Id.INTERMITTENT) 2 else 0
        return Spec(
            ImuProtocol.MODE_COMPUTED, v.pollMs, 1.0f, 1.0f,
            if (v.deepSleep != 0) ImuProtocol.PROFILE_DEEP_SLEEP else ImuProtocol.PROFILE_DC_FULL,
            if (v.deepSleep != 0) 80 else 240,
            v.imuHz, if (v.deepSleep != 0) 0 else 2,
            v.wakeSec, v.activeSec, 0, v.deepSleep,
            v.scheduleMode, v.intervalSec, v.windowSec, v.jitterSec, v.tier, 0,
            mix.first, mix.second, mix.third, nested,
        )
    }

    fun apply(base: ByteArray?, preset: Id): ApplyResult {
        val blob = if (base != null && base.size >= ConfigProtocol.BLOB_SIZE) {
            base.copyOf(ConfigProtocol.BLOB_SIZE)
        } else {
            defaultBlob()
        }
        val bb = ByteBuffer.wrap(blob).order(ByteOrder.LITTLE_ENDIAN)
        val spec = when (preset) {
            Id.BODY_COMPUTED -> Spec(
                ImuProtocol.MODE_COMPUTED, 100, 0.35f, 1.0f,
                ImuProtocol.PROFILE_PERFORMANCE, 160, 50, 2, 0, 0, 0, 0,
                0, 60, 10, 0, 0, 1, 0, 0, 0, 0,
            )
            Id.BODY_SCENE -> Spec(
                ImuProtocol.MODE_SCENE, 50, 0.35f, 1.0f,
                ImuProtocol.PROFILE_PERFORMANCE, 160, 50, 2, 0, 0, 0, 0,
                0, 60, 10, 0, 0, 1, 0, 0, 0, 0,
            )
            Id.VIBRO_NORMAL -> vibroSpec(VibroDiagnosisMode.Id.NORMAL)
            Id.VIBRO_LOW_RPM -> vibroSpec(VibroDiagnosisMode.Id.LOW_RPM)
            Id.VIBRO_ULTRA_LOW_RPM -> vibroSpec(VibroDiagnosisMode.Id.ULTRA_LOW_RPM)
            Id.VIBRO_INTERMITTENT -> vibroSpec(VibroDiagnosisMode.Id.INTERMITTENT)
            Id.VIBRO_MACHINE -> vibroSpec(VibroDiagnosisMode.Id.INTERMITTENT)
            Id.BATTERY_BALANCED -> Spec(
                ImuProtocol.MODE_COMPUTED, 200, 0.35f, 1.0f,
                ImuProtocol.PROFILE_BALANCED, 80, 10, 0, 0, 0, 1, 0,
                0, 60, 10, 0, 0, 0, 0, 0, 0, 0,
            )
            Id.DC_SAVE -> Spec(
                ImuProtocol.MODE_COMPUTED, 100, 1.0f, 1.0f,
                ImuProtocol.PROFILE_DC_SAVE, 80, 25, 1, 0, 0, 1, 0,
                0, 60, 10, 0, 0, 0, 0, 0, 0, 0,
            )
        }
        bb.putShort(136, spec.poll.toShort())
        bb.put(138, spec.mode.toByte())
        bb.putFloat(140, spec.accelScale)
        bb.putFloat(144, spec.gyroScale)
        putProfile(bb, spec)
        return ApplyResult(preset, spec.mode, spec.poll, blob)
    }

    private data class Spec(
        val mode: Int,
        val poll: Int,
        val accelScale: Float,
        val gyroScale: Float,
        val powerProfile: Int,
        val cpuMhz: Int,
        val imuHz: Int,
        val tftPolicy: Int,
        val wakeSec: Int,
        val activeSec: Int,
        val autoDc: Int,
        val deepSleepEnable: Int,
        val vibroScheduleMode: Int,
        val vibroIntervalSec: Int,
        val vibroWindowSec: Int,
        val vibroJitterSec: Int,
        val vibroCaptureTier: Int,
        val wifiUploadEnable: Int,
        val mixEvery: Int,
        val mixRatio: Int,
        val dynShortRatio: Int,
        val dynNestedRatio: Int,
    )

    private fun putProfile(bb: ByteBuffer, spec: Spec) {
        bb.put(148, spec.powerProfile.toByte())
        bb.put(149, spec.tftPolicy.toByte())
        bb.putShort(150, spec.wakeSec.toShort())
        bb.put(152, spec.activeSec.toByte())
        bb.put(153, spec.cpuMhz.toByte())
        bb.put(154, spec.imuHz.toByte())
        bb.put(155, spec.autoDc.toByte())
        bb.put(156, spec.deepSleepEnable.toByte())
        bb.position(165)
        bb.put(spec.vibroScheduleMode.toByte())
        bb.put(spec.vibroIntervalSec.toByte())
        bb.put(spec.vibroWindowSec.toByte())
        bb.put(spec.vibroJitterSec.toByte())
        bb.put(spec.vibroCaptureTier.toByte())
        bb.put(spec.wifiUploadEnable.toByte())
        bb.put(spec.mixEvery.toByte())
        bb.put(spec.mixRatio.toByte())
        bb.put(spec.dynShortRatio.toByte())
        bb.put(spec.dynNestedRatio.toByte())
    }

    private fun defaultBlob(): ByteArray {
        val blob = ByteArray(ConfigProtocol.BLOB_SIZE)
        val bb = ByteBuffer.wrap(blob).order(ByteOrder.LITTLE_ENDIAN)
        bb.putInt(0x31494D55)
        bb.putShort(4, 1.toShort())
        bb.putShort(6, ConfigProtocol.BLOB_SIZE.toShort())
        bb.position(136)
        bb.putShort(ImuProtocol.DEFAULT_POLL_MS.toShort())
        bb.put(138, ImuProtocol.MODE_COMPUTED.toByte())
        bb.putFloat(140, 1.0f)
        bb.putFloat(144, 1.0f)
        putProfile(
            bb,
            Spec(
                ImuProtocol.MODE_COMPUTED, ImuProtocol.DEFAULT_POLL_MS, 1.0f, 1.0f,
                ImuProtocol.PROFILE_DC_FULL, 240, 100, 2, 0, 0, 1, 0,
                0, 60, 10, 0, 0, 0, 0, 0, 0, 0,
            ),
        )
        return blob
    }
}
