package com.esp32s3.imusim

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.io.File

/** Sticky OTA prompt: MainActivity shows a dialog; otherwise a notification. */
object OtaOfferHub {
    private const val TAG = "OtaOfferHub"
    private val mainHandler = Handler(Looper.getMainLooper())

    @Volatile
    var pending: OtaOffer? = null
        private set

    @Volatile
    var onOffer: ((OtaOffer) -> Unit)? = null

    fun publish(context: Context, offer: OtaOffer) {
        pending = offer
        mainHandler.post {
            val cb = onOffer
            if (cb != null) {
                cb(offer)
            } else {
                OtaNotifier.show(context.applicationContext, offer)
            }
        }
    }

    fun clear() {
        pending = null
    }

    fun consume(): OtaOffer? {
        val o = pending
        pending = null
        return o
    }

    fun log(msg: String) {
        Log.i(TAG, msg)
    }
}

data class OtaOffer(
    val kind: Kind,
    val title: String,
    val body: String,
    val file: File,
    val versionLabel: String,
    val apkVersionCode: Int = 0,
    val fwVersionCode: Int = 0,
) {
    enum class Kind { APK, FW }
}
