package com.esp32s3.imusim

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.Locale

object ConfigSummary {
    /** Packed layout must match `DeviceConfigV1` in esp32_s3_imu_basics/config/device_config.h */
    fun format(blob: ByteArray): String {
        if (blob.size < ConfigProtocol.BLOB_SIZE) {
            return "cfg short ${blob.size}B (need ${ConfigProtocol.BLOB_SIZE})"
        }
        val bb = ByteBuffer.wrap(blob).order(ByteOrder.LITTLE_ENDIAN)
        val magic = bb.int
        val version = bb.short.toInt() and 0xFFFF
        if (magic != 0x31494D55) {
            return "cfg bad magic 0x${Integer.toHexString(magic)}"
        }

        bb.position(12)
        val batFull = bb.float
        val batEmpty = bb.float
        val batOffset = bb.float
        val dcMargin = bb.float

        bb.position(52)
        val zoomX = bb.float
        val zoomY = bb.float
        val zoomZ = bb.float

        bb.position(88)
        val gyroX = bb.float
        val gyroY = bb.float
        val gyroZ = bb.float

        bb.position(116)
        val walkH = bb.float
        val stepMin = bb.float
        val stepMax = bb.float

        bb.position(136)
        val pollMs = bb.short.toInt() and 0xFFFF
        bb.get() // ble_default_mode
        bb.get() // pad
        val accelScale = bb.float
        val gyroScale = bb.float

        bb.position(148)
        val powerProfile = bb.get().toInt() and 0xFF
        val tftPolicy = bb.get().toInt() and 0xFF
        val wakeSec = bb.short.toInt() and 0xFFFF
        val activeSec = bb.get().toInt() and 0xFF
        val cpuMhz = bb.get().toInt() and 0xFF
        val imuHz = bb.get().toInt() and 0xFF

        bb.position(165)
        val vibroMode = bb.get().toInt() and 0xFF
        val vibroInterval = bb.get().toInt() and 0xFF
        val vibroWindow = bb.get().toInt() and 0xFF
        bb.get() // vibro_jitter_sec
        val vibroTier = bb.get().toInt() and 0xFF
        val wifiUpload = bb.get().toInt() and 0xFF
        val vibroMixEvery = bb.get().toInt() and 0xFF
        val vibroMixRatio = bb.get().toInt() and 0xFF
        val vibroDynShort = bb.get().toInt() and 0xFF
        val vibroDynNested = bb.get().toInt() and 0xFF

        val vibroLabel = when (vibroMode) {
            1 -> "interval ${vibroWindow}s/${vibroInterval}s"
            2 -> "random ${vibroWindow}s/${vibroInterval}s"
            else -> "always"
        }
        val tierLabel = VibroDiagnosisMode.fromTier(vibroTier)?.label?.let { " tier=$it" } ?: ""
        val mixLabel = if (vibroMixEvery >= 2 && vibroMixRatio >= 2) {
            " mix=$vibroMixEvery:$vibroMixRatio" +
                (if (vibroDynShort >= 2) " dyn/$vibroDynShort" else "") +
                (if (vibroDynNested >= 2) " nest/$vibroDynNested" else "")
        } else {
            ""
        }
        val cloudLabel = if (wifiUpload != 0) " wifi-up" else ""

        return String.format(
            Locale.US,
            "cfg v%d | bat %.2f-%.2fV | zoom %.2f/%.2f/%.2f | gyro %.3f/%.3f/%.3f | walk %.0fcm step %.2f-%.2fm | imu a=%.2f g=%.2f poll %dms | pp=%d cpu=%d imu=%dHz tft=%d wake=%ds active=%ds | cap %s%s%s",
            version,
            batEmpty,
            batFull,
            zoomX,
            zoomY,
            zoomZ,
            gyroX,
            gyroY,
            gyroZ,
            walkH * 100f,
            stepMin,
            stepMax,
            accelScale,
            gyroScale,
            pollMs,
            powerProfile,
            cpuMhz,
            imuHz,
            tftPolicy,
            wakeSec,
            activeSec,
            vibroLabel + tierLabel + mixLabel,
            cloudLabel,
        )
    }
}
