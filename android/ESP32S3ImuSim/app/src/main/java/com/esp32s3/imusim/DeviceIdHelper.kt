package com.esp32s3.imusim

/** Stable cloud device_id from BLE MAC (e.g. esp-e8f60a9251f0). */
object DeviceIdHelper {
    fun fromBleAddress(address: String?): String? {
        val hex = address?.trim()?.lowercase()?.replace(":", "").orEmpty()
        if (hex.length != 12 || !hex.all { it.isDigit() || it in 'a'..'f' }) {
            return null
        }
        return "esp-$hex"
    }

    fun maybeSyncCloudDeviceId(cloud: CloudSettings, bleAddress: String?) {
        val id = fromBleAddress(bleAddress) ?: return
        val cur = cloud.deviceId
        if (cur == CloudSettings.DEFAULT_DEVICE_ID || cur.startsWith("esp-")) {
            cloud.deviceId = id
        }
    }
}
