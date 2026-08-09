package com.esp32s3.imusim

import android.content.Context
import java.util.concurrent.TimeUnit

/**
 * Phone-side BLE bridge scheduling (Case C production relay).
 * ESP stays disconnected most of the time; phone connects briefly to drain buffers.
 */
class BridgeSyncSettings(context: Context) {
    private val prefs = context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    enum class Mode(val id: String, val label: String) {
        MANUAL("manual", "Manual (live session only)"),
        RENDEZVOUS("rendezvous", "ESP-aligned rendezvous"),
        PERIODIC_FIXED("periodic_fixed", "Periodic — fixed interval"),
        PERIODIC_ADAPTIVE("periodic_adaptive", "Periodic — adaptive fill"),
        ESP_INITIATED("esp_initiated", "ESP-initiated (future)"),
        ;

        companion object {
            fun fromId(id: String?): Mode =
                entries.firstOrNull { it.id == id } ?: MANUAL
        }
    }

    val scheduled: Boolean
        get() = mode == Mode.RENDEZVOUS ||
            mode == Mode.PERIODIC_FIXED ||
            mode == Mode.PERIODIC_ADAPTIVE

    /** User explicitly picked Manual in Cloud dialog. */
    var userDisabledBridge: Boolean
        get() = prefs.getBoolean(KEY_USER_DISABLED, false)
        set(value) = prefs.edit().putBoolean(KEY_USER_DISABLED, value).apply()

    var mode: Mode
        get() = Mode.fromId(prefs.getString(KEY_MODE, Mode.MANUAL.id))
        set(value) {
            prefs.edit().putString(KEY_MODE, value.id).apply()
            if (value == Mode.MANUAL) {
                userDisabledBridge = true
            } else if (value != Mode.ESP_INITIATED) {
                userDisabledBridge = false
            }
        }

    var intervalMinutes: Int
        get() = prefs.getInt(KEY_INTERVAL_MIN, DEFAULT_INTERVAL_MIN).coerceIn(1, 24 * 60)
        set(value) = prefs.edit().putInt(KEY_INTERVAL_MIN, value.coerceIn(1, 24 * 60)).apply()

    var minIntervalMinutes: Int
        get() = prefs.getInt(KEY_MIN_INTERVAL_MIN, 1).coerceIn(1, intervalMinutes)
        set(value) = prefs.edit().putInt(KEY_MIN_INTERVAL_MIN, value.coerceIn(1, 24 * 60)).apply()

    var estimatedVerdictCapacity: Int
        get() = prefs.getInt(KEY_VERDICT_CAPACITY, 32).coerceIn(4, 256)
        set(value) = prefs.edit().putInt(KEY_VERDICT_CAPACITY, value.coerceIn(4, 256)).apply()

    var dwellSeconds: Int
        get() = prefs.getInt(KEY_DWELL_SEC, DEFAULT_DWELL_SEC).coerceIn(8, 120)
        set(value) = prefs.edit().putInt(KEY_DWELL_SEC, value.coerceIn(8, 120)).apply()

    fun intervalMs(context: Context): Long = when (mode) {
        Mode.RENDEZVOUS -> EspRendezvous.nextConnectDelayMs(context)
        Mode.PERIODIC_ADAPTIVE -> {
            val baseMs = TimeUnit.MINUTES.toMillis(intervalMinutes.toLong())
            val pending = VerdictStore(context).pendingAckSeqs().size +
                OffloadExporter(context).pendingCrashCount()
            val fill = pending.toFloat() / estimatedVerdictCapacity.toFloat()
            val scale = (1f - fill.coerceIn(0f, 0.85f)).coerceAtLeast(0.15f)
            val adaptive = (baseMs * scale).toLong()
            val minMs = TimeUnit.MINUTES.toMillis(minIntervalMinutes.toLong())
            adaptive.coerceAtLeast(minMs)
        }
        Mode.PERIODIC_FIXED -> TimeUnit.MINUTES.toMillis(intervalMinutes.toLong())
        else -> TimeUnit.MINUTES.toMillis(intervalMinutes.toLong())
    }

    fun summary(context: Context): String = when (mode) {
        Mode.MANUAL -> "Bridge: manual (stay connected to relay)"
        Mode.RENDEZVOUS -> EspRendezvous.summary(context)
        Mode.PERIODIC_FIXED -> "Bridge: every ${intervalMinutes}m, dwell ${dwellSeconds}s"
        Mode.PERIODIC_ADAPTIVE -> {
            val nextMin = TimeUnit.MILLISECONDS.toMinutes(intervalMs(context)).coerceAtLeast(1)
            "Bridge: adaptive ~${nextMin}m (base ${intervalMinutes}m), dwell ${dwellSeconds}s"
        }
        Mode.ESP_INITIATED -> "Bridge: ESP-initiated (not implemented — use rendezvous)"
    }

    companion object {
        private const val PREFS = "bridge_sync_settings"
        private const val KEY_MODE = "mode"
        private const val KEY_USER_DISABLED = "user_disabled_bridge"
        private const val KEY_INTERVAL_MIN = "interval_min"
        private const val KEY_MIN_INTERVAL_MIN = "min_interval_min"
        private const val KEY_VERDICT_CAPACITY = "verdict_capacity"
        private const val KEY_DWELL_SEC = "dwell_sec"
        const val DEFAULT_INTERVAL_MIN = 60
        const val DEFAULT_DWELL_SEC = 20
    }
}
