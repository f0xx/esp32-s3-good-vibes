package com.esp32s3.imusim

import android.content.Context

/** Last-seen firmware + declined OTA versions (re-prompt only when the offer changes). */
class OtaSettings(context: Context) {
    private val prefs = context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    var lastFwVersion: String
        get() = prefs.getString(KEY_FW, "") ?: ""
        set(value) = prefs.edit().putString(KEY_FW, value.trim()).apply()

    var lastFwVersionCode: Int
        get() = prefs.getInt(KEY_FW_CODE, 0)
        set(value) = prefs.edit().putInt(KEY_FW_CODE, value).apply()

    var declinedApkVersionCode: Int
        get() = prefs.getInt(KEY_DECLINED_APK, 0)
        set(value) = prefs.edit().putInt(KEY_DECLINED_APK, value).apply()

    var declinedFwVersionCode: Int
        get() = prefs.getInt(KEY_DECLINED_FW_CODE, 0)
        set(value) = prefs.edit().putInt(KEY_DECLINED_FW_CODE, value).apply()

    fun liveVersionCode(): Int {
        if (lastFwVersionCode > 0) return lastFwVersionCode
        return parseVersionCode(lastFwVersion)
    }

    /**
     * Record a firmware identity. Older crash-ring strings must not clobber a
     * newer live STATUS `fwc`.
     */
    fun noteFw(version: String, versionCode: Int = 0) {
        val name = version.trim()
        val code = if (versionCode > 0) versionCode else parseVersionCode(name)
        if (code > 0 && lastFwVersionCode > 0 && code < lastFwVersionCode) {
            return
        }
        if (code > 0) lastFwVersionCode = code
        if (name.isNotEmpty() && name != "zephyr") lastFwVersion = name
    }

    fun noteFwVersion(version: String) = noteFw(version, 0)

    companion object {
        private const val PREFS = "ota_settings"
        private const val KEY_FW = "last_fw_version"
        private const val KEY_FW_CODE = "last_fw_version_code"
        private const val KEY_DECLINED_APK = "declined_apk_version_code"
        private const val KEY_DECLINED_FW_CODE = "declined_fw_version_code"

        fun parseVersionCode(name: String): Int {
            val m = Regex("""v(\d+)""", RegexOption.IGNORE_CASE).find(name.trim())
            return m?.groupValues?.get(1)?.toIntOrNull() ?: 0
        }
    }
}
