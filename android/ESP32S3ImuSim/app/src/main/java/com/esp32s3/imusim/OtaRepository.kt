package com.esp32s3.imusim

import android.content.Context
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

class OtaRepository(private val context: Context) {
    private val settings = CloudSettings(context)
    private val cacheDir = File(context.cacheDir, "ota").apply { mkdirs() }

    fun fetchManifest(): OtaManifest? {
        if (!settings.enabled) return null
        val url = "${settings.baseUrl.trim().trimEnd('/')}/v1/ota/manifest"
        Log.i(TAG, "GET $url")
        val text = getText(url) ?: return null
        return OtaManifest.parse(text)
    }

    fun download(kind: OtaOffer.Kind, relativeUrl: String, size: Long, sha256: String): File? {
        val name = if (kind == OtaOffer.Kind.APK) "app-debug.apk" else "firmware.bin"
        val dest = File(cacheDir, name)
        val url = artifactUrl(relativeUrl)
        if (!getFile(url, dest, size)) {
            dest.delete()
            return null
        }
        val got = sha256Of(dest)
        if (got != sha256.lowercase()) {
            Log.w(TAG, "checksum mismatch want=$sha256 got=$got")
            dest.delete()
            return null
        }
        return dest
    }

    private fun artifactUrl(relative: String): String {
        val base = settings.baseUrl.trim().trimEnd('/')
        val rel = relative.trim().trimStart('/')
        return if (rel.startsWith("http")) rel else "$base/v1/ota/$rel"
    }

    private fun getText(urlStr: String): String? {
        val conn = open(urlStr)
        return try {
            val code = conn.responseCode
            val stream = if (code in 200..299) conn.inputStream else conn.errorStream
            val text = stream?.bufferedReader(Charsets.UTF_8)?.use { it.readText() }.orEmpty()
            if (code in 200..299) text else {
                Log.w(TAG, "GET $urlStr HTTP $code ${text.take(120)}")
                null
            }
        } catch (e: Exception) {
            Log.w(TAG, "GET $urlStr failed: ${e.message}")
            null
        } finally {
            conn.disconnect()
        }
    }

    private fun getFile(urlStr: String, dest: File, expectedSize: Long): Boolean {
        val conn = open(urlStr)
        return try {
            val code = conn.responseCode
            if (code !in 200..299) {
                Log.w(TAG, "GET file $urlStr HTTP $code")
                return false
            }
            FileOutputStream(dest).use { out ->
                conn.inputStream.use { it.copyTo(out) }
            }
            if (expectedSize > 0 && dest.length() != expectedSize) {
                Log.w(TAG, "size mismatch want=$expectedSize got=${dest.length()}")
                return false
            }
            true
        } catch (e: Exception) {
            Log.w(TAG, "download $urlStr failed: ${e.message}")
            false
        } finally {
            conn.disconnect()
        }
    }

    private fun open(urlStr: String): HttpURLConnection {
        return (URL(urlStr).openConnection() as HttpURLConnection).apply {
            requestMethod = "GET"
            connectTimeout = 12_000
            readTimeout = 60_000
            setRequestProperty("X-API-Key", settings.apiKey)
        }
    }

    companion object {
        private const val TAG = "OtaRepository"

        fun sha256Of(file: File): String {
            val md = MessageDigest.getInstance("SHA-256")
            file.inputStream().use { input ->
                val buf = ByteArray(16 * 1024)
                while (true) {
                    val n = input.read(buf)
                    if (n <= 0) break
                    md.update(buf, 0, n)
                }
            }
            return md.digest().joinToString("") { b -> "%02x".format(b) }
        }
    }
}
