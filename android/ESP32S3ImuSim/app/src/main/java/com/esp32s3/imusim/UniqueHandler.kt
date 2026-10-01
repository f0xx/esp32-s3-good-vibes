package com.esp32s3.imusim

import android.os.Handler

/**
 * Post to a [Handler] with a unique token so a new schedule of the same kind replaces any
 * still-queued (not yet running) sibling — [Handler.removeCallbacksAndMessages] + tokenized
 * [Handler.postDelayed].
 */
object UniqueHandler {
    fun post(handler: Handler, token: Any, delayMs: Long = 0L, work: () -> Unit) {
        handler.removeCallbacksAndMessages(token)
        val r = Runnable { work() }
        if (delayMs <= 0L) {
            handler.postDelayed(r, token, 0L)
        } else {
            handler.postDelayed(r, token, delayMs)
        }
    }

    fun cancel(handler: Handler, token: Any) {
        handler.removeCallbacksAndMessages(token)
    }
}

/** Tokens for [BleImuClient] main-looper work (must be identity-stable objects). */
object BleUiToken {
    val CONNECT = Any()
    val SCAN = Any()
    val DISCONNECT = Any()
    val POLL_DATA = Any()
    val POLL_STATUS = Any()
    val SESSION_SETUP = Any()
    val TIME_SYNC = Any()
    val GATT_RETRY = Any()
    val CCCD = Any()
    val NET_PROFILES = Any()
    val DISCOVER = Any()
    val STATUS_NOTIFY = Any()
    val BANNER = Any()
    val RSSI_NOTIFY = Any()
    val BATCH = Any()
}
