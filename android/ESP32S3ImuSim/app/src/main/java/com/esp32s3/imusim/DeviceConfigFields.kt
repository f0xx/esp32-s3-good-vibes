package com.esp32s3.imusim

import java.nio.ByteBuffer
import java.nio.ByteOrder

/** Parsed schedule/power fields from DeviceConfigV1 blob (matches device_config.h). */
data class DeviceConfigFields(
    val powerProfile: Int,
    val wakeIntervalSec: Int,
    val activeWindowSec: Int,
    val deepSleepEnable: Int,
    val vibroScheduleMode: Int,
    val vibroIntervalSec: Int,
    val vibroWindowSec: Int,
    val vibroJitterSec: Int,
    val vibroTier: Int,
    val vibroMixEvery: Int,
    val vibroMixRatio: Int,
    val vibroDynShortRatio: Int,
    val vibroDynNestedRatio: Int,
) {
    companion object {
        fun fromBlob(blob: ByteArray?): DeviceConfigFields? {
            if (blob == null || blob.size < ConfigProtocol.BLOB_SIZE) {
                return null
            }
            val bb = ByteBuffer.wrap(blob).order(ByteOrder.LITTLE_ENDIAN)
            if (bb.int != 0x31494D55) {
                return null
            }
            bb.position(148)
            val powerProfile = bb.get().toInt() and 0xFF
            bb.get() // tft policy
            val wakeIntervalSec = bb.short.toInt() and 0xFFFF
            val activeWindowSec = bb.get().toInt() and 0xFF
            bb.get() // cpu
            bb.get() // imu hz
            bb.get() // auto dc
            val deepSleepEnable = bb.get().toInt() and 0xFF
            bb.position(165)
            val vibroScheduleMode = bb.get().toInt() and 0xFF
            val vibroIntervalSec = bb.get().toInt() and 0xFF
            val vibroWindowSec = bb.get().toInt() and 0xFF
            val vibroJitterSec = bb.get().toInt() and 0xFF
            val vibroTier = bb.get().toInt() and 0xFF
            bb.get() // wifi_upload_enable
            val vibroMixEvery = bb.get().toInt() and 0xFF
            val vibroMixRatio = bb.get().toInt() and 0xFF
            val vibroDynShortRatio = bb.get().toInt() and 0xFF
            val vibroDynNestedRatio = bb.get().toInt() and 0xFF
            return DeviceConfigFields(
                powerProfile = powerProfile,
                wakeIntervalSec = wakeIntervalSec.coerceAtLeast(0),
                activeWindowSec = activeWindowSec.coerceAtLeast(0),
                deepSleepEnable = deepSleepEnable,
                vibroScheduleMode = vibroScheduleMode,
                vibroIntervalSec = vibroIntervalSec,
                vibroWindowSec = vibroWindowSec,
                vibroJitterSec = vibroJitterSec,
                vibroTier = vibroTier,
                vibroMixEvery = vibroMixEvery,
                vibroMixRatio = vibroMixRatio,
                vibroDynShortRatio = vibroDynShortRatio,
                vibroDynNestedRatio = vibroDynNestedRatio,
            )
        }
    }
}
