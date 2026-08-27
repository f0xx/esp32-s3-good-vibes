package com.esp32s3.imusim

import android.content.Context

/** High-water seq ACKed by the backend — flushed to the device in one GATT write. */
object OffloadAckStore {
    private const val PREFS = "offload_ack"
    private const val KEY_HW = "cloud_hw_seq"

    fun noteCloudAcked(context: Context, seq: Long) {
        if (seq <= 0L) return
        val p = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val cur = p.getLong(KEY_HW, 0L)
        if (seq > cur) {
            p.edit().putLong(KEY_HW, seq).apply()
        }
    }

    fun highWater(context: Context): Long =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getLong(KEY_HW, 0L)
}
