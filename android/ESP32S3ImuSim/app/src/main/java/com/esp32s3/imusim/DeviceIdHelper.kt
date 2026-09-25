package com.esp32s3.imusim

/** Stable cloud device_id from BLE MAC (e.g. esp-e8f60a9251f0). */
object DeviceIdHelper {
    private val autoMacId = Regex("^esp-[0-9a-f]{12}$")

    fun fromBleAddress(address: String?): String? {
        val hex = address?.trim()?.lowercase()?.replace(":", "").orEmpty()
        if (hex.length != 12 || !hex.all { it.isDigit() || it in 'a'..'f' }) {
            return null
        }
        return "esp-$hex"
    }

    /**
     * Auto-fill cloud device_id from the connected board MAC only when unset, still the BLE
     * advertising name default, or already a previous auto MAC id. Never clobber a custom id
     * such as `esp-new1` (anything `esp-*` used to be overwritten on every connect).
     */
    fun maybeSyncCloudDeviceId(cloud: CloudSettings, bleAddress: String?) {
        val id = fromBleAddress(bleAddress) ?: return
        val cur = cloud.deviceId.trim()
        if (cur.isEmpty() ||
            cur == CloudSettings.DEFAULT_DEVICE_ID ||
            autoMacId.matches(cur.lowercase())
        ) {
            cloud.deviceId = id
        }
    }
}
