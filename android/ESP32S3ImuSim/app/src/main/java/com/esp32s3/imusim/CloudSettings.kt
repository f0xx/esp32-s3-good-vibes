package com.esp32s3.imusim

import android.content.Context
import android.provider.Settings

/** Phone-side cloud upload (Case C). ESP does not need to reach this URL. */
class CloudSettings(context: Context) {
    private val prefs = context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    var baseUrl: String
        get() = prefs.getString(KEY_BASE_URL, DEFAULT_BASE_URL)?.trim()?.let { normalizeBaseUrl(it) }
            ?.let { fixUrlScheme(it) }
            ?: DEFAULT_BASE_URL
        set(value) = prefs.edit().putString(KEY_BASE_URL, normalizeBaseUrl(value)).apply()

    var apiKey: String
        get() = prefs.getString(KEY_API_KEY, "") ?: ""
        set(value) = prefs.edit().putString(KEY_API_KEY, CloudSetupLink.sanitizeApiKey(value)).apply()

    var deviceId: String
        get() = prefs.getString(KEY_DEVICE_ID, DEFAULT_DEVICE_ID)?.trim().orEmpty()
            .ifEmpty { DEFAULT_DEVICE_ID }
        set(value) = prefs.edit().putString(KEY_DEVICE_ID, value.trim()).apply()

    var groupId: String
        get() = prefs.getString(KEY_GROUP_ID, "default") ?: "default"
        set(value) = prefs.edit().putString(KEY_GROUP_ID, value.trim()).apply()

    val enabled: Boolean
        get() = baseUrl.startsWith("http") && apiKey.isNotBlank()

    fun phoneId(context: Context): String {
        val cached = prefs.getString(KEY_PHONE_ID, null)
        if (!cached.isNullOrBlank()) return cached
        val id = Settings.Secure.getString(context.contentResolver, Settings.Secure.ANDROID_ID)
            ?: "phone-unknown"
        prefs.edit().putString(KEY_PHONE_ID, id).apply()
        return id
    }

    companion object {
        private const val PREFS = "cloud_settings"
        private const val KEY_BASE_URL = "base_url"
        private const val KEY_API_KEY = "api_key"
        private const val KEY_DEVICE_ID = "device_id"
        private const val KEY_GROUP_ID = "group_id"
        private const val KEY_PHONE_ID = "phone_id"
        const val DEFAULT_DEVICE_ID = ImuProtocol.DEVICE_NAME
        /** Public forwarder: Pi :8090 → artc0.intra.raptor.org:8080 */
        const val DEFAULT_BASE_URL = "https://apps.f0xx.org/app/good_vibes"
        /** Legacy HTTP forwarder (v1 API only). */
        const val LEGACY_FORWARDER_URL = "http://artc0.f0xx.org:8090"
        /** Direct intra/VPN backend (bypasses Pi forwarder). */
        const val EXAMPLE_LAN_URL = "http://artc0.intra.raptor.org:8080"

        fun normalizeBaseUrl(raw: String): String {
            val t = raw.trim().trimEnd('/')
            if (t.isEmpty()) return ""
            if (t.startsWith("http://", ignoreCase = true) || t.startsWith("https://", ignoreCase = true)) {
                return fixUrlScheme(t)
            }
            val withScheme = when {
                t.contains("apps.f0xx.org", ignoreCase = true) -> "https://$t"
                else -> "http://$t"
            }
            return fixUrlScheme(withScheme)
        }

        /**
         * Port 8090/8080 forwarders speak plain HTTP — HTTPS yields "unable to parse TLS packet header".
         * Public apps.f0xx.org must use HTTPS.
         */
        fun fixUrlScheme(url: String): String {
            val lower = url.lowercase()
            if (lower.startsWith("https://") &&
                (":8090" in lower || (":8080" in lower && "apps.f0xx.org" !in lower))
            ) {
                return "http://${url.substring(8)}"
            }
            if (lower.startsWith("http://") &&
                "apps.f0xx.org" in lower &&
                ":8090" !in lower
            ) {
                return "https://${url.substring(7)}"
            }
            return url
        }
    }
}
