package com.esp32s3.imusim

import android.content.Context
import android.util.Base64

class DeviceConfigRepository(context: Context) {
    private val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    fun loadLocal(): ByteArray? {
        val encoded = prefs.getString(KEY_BLOB, null) ?: return null
        return Base64.decode(encoded, Base64.DEFAULT)
    }

    fun saveLocal(blob: ByteArray) {
        prefs.edit()
            .putString(KEY_BLOB, Base64.encodeToString(blob, Base64.NO_WRAP))
            .apply()
    }

    fun clearLocal() {
        prefs.edit().remove(KEY_BLOB).apply()
    }

    companion object {
        private const val PREFS = "esp32_imu_device_config"
        private const val KEY_BLOB = "blob_v1"
    }
}
