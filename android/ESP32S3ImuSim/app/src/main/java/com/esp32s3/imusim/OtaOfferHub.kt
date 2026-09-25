package com.esp32s3.imusim

import android.content.Context
import android.content.Intent
import android.os.Handler
import android.os.Looper
import android.util.Log
import org.json.JSONArray
import org.json.JSONObject

/** Sticky OTA prompt: persisted so the UI process can show it after a background check. */
object OtaOfferHub {
    private const val TAG = "OtaOfferHub"
    private const val PREFS = "ota_offer"
    private const val KEY_JSON = "pending"
    private val mainHandler = Handler(Looper.getMainLooper())

    @Volatile
    var pending: OtaOffer? = null
        private set

    @Volatile
    var onOffer: ((OtaOffer) -> Unit)? = null

    fun publish(context: Context, offer: OtaOffer) {
        val app = context.applicationContext
        val previous = load(app)
        val same = previous != null && sameOffer(previous, offer)
        pending = offer
        save(app, offer)
        mainHandler.post { onOffer?.invoke(offer) }
        if (same) {
            return
        }
        launchPrompt(app)
        OtaNotifier.show(app, offer)
    }

    fun load(context: Context): OtaOffer? {
        pending?.let { return it }
        val raw = context.applicationContext
            .getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getString(KEY_JSON, null) ?: return null
        return decode(raw)?.also { pending = it }
    }

    fun clear() {
        pending = null
    }

    fun clear(context: Context) {
        pending = null
        context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit().remove(KEY_JSON).commit()
    }

    fun consume(): OtaOffer? {
        val o = pending
        pending = null
        return o
    }

    fun log(msg: String) {
        Log.i(TAG, msg)
    }

    private fun launchPrompt(context: Context) {
        val intent = Intent(context, MainActivity::class.java).apply {
            addFlags(
                Intent.FLAG_ACTIVITY_NEW_TASK or
                    Intent.FLAG_ACTIVITY_SINGLE_TOP or
                    Intent.FLAG_ACTIVITY_CLEAR_TOP,
            )
            putExtra(OtaNotifier.EXTRA_OTA_PROMPT, true)
        }
        try {
            context.startActivity(intent)
            Log.i(TAG, "launched OTA prompt UI")
        } catch (e: Exception) {
            Log.w(TAG, "OTA prompt activity blocked: ${e.message}")
        }
    }

    private fun sameOffer(a: OtaOffer, b: OtaOffer): Boolean {
        return a.kind == b.kind &&
            a.apkVersionCode == b.apkVersionCode &&
            a.fwVersionCode == b.fwVersionCode &&
            a.versionLabel == b.versionLabel
    }

    private fun save(context: Context, offer: OtaOffer) {
        val urls = JSONArray()
        offer.urls.forEach { urls.put(it) }
        val json = JSONObject()
            .put("kind", offer.kind.name)
            .put("title", offer.title)
            .put("body", offer.body)
            .put("versionLabel", offer.versionLabel)
            .put("urls", urls)
            .put("size", offer.size)
            .put("sha256", offer.sha256)
            .put("currentLabel", offer.currentLabel)
            .put("apkVersionCode", offer.apkVersionCode)
            .put("fwVersionCode", offer.fwVersionCode)
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit().putString(KEY_JSON, json.toString()).commit()
    }

    private fun decode(raw: String): OtaOffer? {
        return try {
            val o = JSONObject(raw)
            val urlsJson = o.optJSONArray("urls")
            val urls = mutableListOf<String>()
            if (urlsJson != null) {
                for (i in 0 until urlsJson.length()) {
                    urls.add(urlsJson.optString(i))
                }
            }
            OtaOffer(
                kind = OtaOffer.Kind.valueOf(o.getString("kind")),
                title = o.optString("title"),
                body = o.optString("body"),
                versionLabel = o.optString("versionLabel"),
                urls = urls,
                size = o.optLong("size"),
                sha256 = o.optString("sha256"),
                currentLabel = o.optString("currentLabel"),
                apkVersionCode = o.optInt("apkVersionCode"),
                fwVersionCode = o.optInt("fwVersionCode"),
            )
        } catch (e: Exception) {
            Log.w(TAG, "decode offer: ${e.message}")
            null
        }
    }
}

data class OtaOffer(
    val kind: Kind,
    val title: String,
    val body: String,
    val versionLabel: String,
    val urls: List<String>,
    val size: Long,
    val sha256: String,
    val currentLabel: String,
    val apkVersionCode: Int = 0,
    val fwVersionCode: Int = 0,
) {
    enum class Kind { APK, FW }
}
