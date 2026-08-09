package com.esp32s3.imusim

import android.net.Uri
import org.json.JSONObject

/** Parse one-tap / paste cloud credentials for the phone app. */
object CloudSetupLink {
    data class Setup(
        val baseUrl: String,
        val apiKey: String,
        val deviceId: String? = null,
        val groupId: String? = null,
    )

    fun build(baseUrl: String, apiKey: String, deviceId: String?, groupId: String?): String {
        val b = Uri.Builder()
            .scheme("goodvibes")
            .authority("cloud")
            .appendQueryParameter("u", CloudSettings.normalizeBaseUrl(baseUrl))
            .appendQueryParameter("k", sanitizeApiKey(apiKey))
        deviceId?.trim()?.takeIf { it.isNotEmpty() }?.let { b.appendQueryParameter("d", it) }
        groupId?.trim()?.takeIf { it.isNotEmpty() }?.let { b.appendQueryParameter("g", it) }
        return b.build().toString()
    }

    fun parse(raw: String): Setup? {
        val text = raw.trim()
        if (text.isEmpty()) return null
        when {
            text.startsWith("goodvibes://", ignoreCase = true) -> return parseUri(Uri.parse(text))
            text.startsWith("http://", ignoreCase = true) ||
                text.startsWith("https://", ignoreCase = true) -> {
                if (text.contains("k=", ignoreCase = true) || text.contains("key=", ignoreCase = true)) {
                    return parseUri(Uri.parse(text))
                }
            }
            text.startsWith("{") -> return parseJson(text)
        }
        val keyOnly = sanitizeApiKey(text)
        if (keyOnly.length == 48 && keyOnly.all { it in '0'..'9' || it in 'a'..'f' }) {
            return Setup(CloudSettings.DEFAULT_BASE_URL, keyOnly)
        }
        return null
    }

    fun sanitizeApiKey(raw: String): String {
        var k = raw.trim()
        val prefix = Regex("""(?i)^x-api-key\s*:\s*""")
        k = k.replace(prefix, "")
        k = k.trim().trim('"', '\'', '`')
        return k
    }

    fun keyLooksValid(key: String): Boolean {
        val k = sanitizeApiKey(key)
        return k.length == 48 && k.all { it in '0'..'9' || it in 'a'..'f' || it in 'A'..'F' }
    }

    private fun parseUri(uri: Uri): Setup? {
        val key = uri.getQueryParameter("k")
            ?: uri.getQueryParameter("key")
            ?: return null
        val url = uri.getQueryParameter("u")
            ?: uri.getQueryParameter("url")
            ?: uri.getQueryParameter("base")
            ?: CloudSettings.DEFAULT_BASE_URL
        return Setup(
            baseUrl = CloudSettings.normalizeBaseUrl(url),
            apiKey = sanitizeApiKey(key),
            deviceId = uri.getQueryParameter("d") ?: uri.getQueryParameter("device"),
            groupId = uri.getQueryParameter("g") ?: uri.getQueryParameter("group"),
        )
    }

    private fun parseJson(text: String): Setup? {
        return try {
            val o = JSONObject(text)
            val key = o.optString("key", o.optString("api_key", ""))
            if (key.isBlank()) return null
            Setup(
                baseUrl = CloudSettings.normalizeBaseUrl(
                    o.optString("url", o.optString("base_url", CloudSettings.DEFAULT_BASE_URL)),
                ),
                apiKey = sanitizeApiKey(key),
                deviceId = o.optString("device_id").ifBlank { null },
                groupId = o.optString("group_id").ifBlank { null },
            )
        } catch (_: Exception) {
            null
        }
    }
}
