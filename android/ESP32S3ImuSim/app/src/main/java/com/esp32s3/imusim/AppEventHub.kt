package com.esp32s3.imusim

import android.os.Handler
import android.os.Looper

/** UI events from background workers (cloud upload, etc.). */
object AppEventHub {
    private val mainHandler = Handler(Looper.getMainLooper())

    @Volatile
    var onBanner: ((StatusBannerLevel, String) -> Unit)? = null

    fun showBanner(level: StatusBannerLevel, message: String) {
        mainHandler.post { onBanner?.invoke(level, message) }
    }
}
