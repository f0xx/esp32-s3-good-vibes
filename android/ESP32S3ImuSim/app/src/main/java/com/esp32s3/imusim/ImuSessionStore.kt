package com.esp32s3.imusim

import android.content.Context
import android.os.Bundle

/** Persists UI/session prefs owned by the BLE service (survives rotation & process relaunch). */
class ImuSessionStore(context: Context) {
    private val prefs = context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    var pollMs: Int
        get() = prefs.getInt(KEY_POLL_MS, ImuProtocol.DEFAULT_POLL_MS)
        set(value) = prefs.edit().putInt(KEY_POLL_MS, value.coerceIn(ImuProtocol.MIN_POLL_MS, 2000)).apply()

    var renderMode: Int
        get() = prefs.getInt(KEY_MODE, ImuProtocol.MODE_COMPUTED)
        set(value) = prefs.edit().putInt(KEY_MODE, value).apply()

    var lastStatus: String
        get() = prefs.getString(KEY_STATUS, "") ?: ""
        set(value) = prefs.edit().putString(KEY_STATUS, value).apply()

    fun saveLocalConfig(blob: ByteArray) {
        prefs.edit()
            .putString(KEY_CONFIG_BLOB, android.util.Base64.encodeToString(blob, android.util.Base64.NO_WRAP))
            .apply()
    }

    fun loadLocalConfig(): ByteArray? {
        val encoded = prefs.getString(KEY_CONFIG_BLOB, null) ?: return null
        return android.util.Base64.decode(encoded, android.util.Base64.DEFAULT)
    }

    var lastNetScanJson: String?
        get() = prefs.getString(KEY_LAST_NET_SCAN, null)
        set(value) = prefs.edit().putString(KEY_LAST_NET_SCAN, value).apply()

    var lastNetProfilesJson: String?
        get() = prefs.getString(KEY_LAST_NET_PROFILES, null)
        set(value) = prefs.edit().putString(KEY_LAST_NET_PROFILES, value).apply()

    var lastNetStatusJson: String?
        get() = prefs.getString(KEY_LAST_NET_STATUS, null)
        set(value) = prefs.edit().putString(KEY_LAST_NET_STATUS, value).apply()

    fun toSnapshotBundle(
        connected: Boolean,
        power: ImuProtocol.PowerStatus?,
        caps: Int,
        lastBatchJson: String?,
        crashDebug: Boolean = false,
        bistStatus: String? = null,
        relayState: Int = RelayFsmState.STARTING.id,
        relayCaption: String = "",
        showDisconnectButton: Boolean = false,
    ): Bundle = Bundle().apply {
        putBoolean(KEY_CONNECTED, connected)
        putInt(KEY_RELAY_STATE, relayState)
        putString(KEY_RELAY_CAPTION, relayCaption)
        putBoolean(KEY_SHOW_DISCONNECT, showDisconnectButton)
        putInt(KEY_POLL_MS, pollMs)
        putInt(KEY_MODE, renderMode)
        putString(KEY_STATUS, lastStatus)
        putInt(KEY_CAPS, caps)
        putBoolean(KEY_CRASH_DEBUG, crashDebug)
        bistStatus?.let { putString(KEY_BIST_STATUS, it) }
        if (power != null && power.valid) {
            putInt(KEY_POWER_SOURCE, power.source)
            putFloat(KEY_POWER_V, power.voltageV)
            putInt(KEY_POWER_PCT, power.percent)
            putBoolean(KEY_POWER_VALID, true)
        } else {
            putBoolean(KEY_POWER_VALID, false)
        }
        if (!lastBatchJson.isNullOrEmpty()) {
            putString(KEY_LAST_BATCH, lastBatchJson)
        }
        lastNetScanJson?.let { putString(KEY_LAST_NET_SCAN, it) }
        lastNetProfilesJson?.let { putString(KEY_LAST_NET_PROFILES, it) }
        lastNetStatusJson?.let { putString(KEY_LAST_NET_STATUS, it) }
        loadLocalConfig()?.let { putByteArray(KEY_CONFIG_BLOB_BYTES, it) }
    }

    companion object {
        const val PREFS = "esp32_imu_session"

        const val KEY_CONNECTED = "connected"
        const val KEY_RELAY_STATE = "relay_state"
        const val KEY_RELAY_CAPTION = "relay_caption"
        const val KEY_SHOW_DISCONNECT = "show_disconnect"
        const val KEY_POLL_MS = "poll_ms"
        const val KEY_MODE = "render_mode"
        const val KEY_STATUS = "status"
        const val KEY_CAPS = "caps"
        const val KEY_POWER_SOURCE = "power_source"
        const val KEY_POWER_V = "power_v"
        const val KEY_POWER_PCT = "power_pct"
        const val KEY_POWER_VALID = "power_valid"
        const val KEY_LAST_BATCH = "last_batch_json"
        const val KEY_LAST_NET_SCAN = "last_net_scan_json"
        const val KEY_LAST_NET_PROFILES = "last_net_profiles_json"
        const val KEY_LAST_NET_STATUS = "last_net_status_json"
        const val KEY_CONFIG_BLOB = "config_blob_b64"
        const val KEY_CONFIG_BLOB_BYTES = "config_blob"
        const val KEY_CRASH_DEBUG = "crash_debug"
        const val KEY_BIST_STATUS = "bist_status"

        private const val KEY_CONFIG_BLOB_LEGACY = KEY_CONFIG_BLOB
    }
}
