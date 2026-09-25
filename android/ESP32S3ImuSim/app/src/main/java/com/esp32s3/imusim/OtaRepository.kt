package com.esp32s3.imusim

import android.content.Context
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import java.util.zip.ZipFile

class OtaRepository(private val context: Context) {
    private val settings = CloudSettings(context)
    private val cacheDir = File(context.cacheDir, "ota").apply { mkdirs() }

    fun fetchManifest(): OtaManifest? {
        val fromCdn = fetchCdnManifest()
        if (fromCdn != null && fromCdn.available) return fromCdn
        if (!settings.enabled) return fromCdn
        val url = "${settings.baseUrl.trim().trimEnd('/')}/v1/ota/manifest"
        Log.i(TAG, "GET $url")
        val text = getText(url, withApiKey = true) ?: return fromCdn
        return OtaManifest.parse(text) ?: fromCdn
    }

    private fun fetchCdnManifest(): OtaManifest? {
        val channelFile = OtaCdn.channelFileForBuilder(settings.otaChannel)
        val channelUrls = linkedSetOf<String>()
        /* Selected channel first. The baked-in stable URL used to be prepended
         * and always won, so staging/dev never showed up. */
        channelUrls.addAll(OtaCdn.channelUrls(channelFile))
        val configured = BuildConfig.OTA_CHANNEL_URL_DEFAULT.trim()
        if (configured.isNotEmpty()) channelUrls.add(configured)
        var lastChannel: String? = null
        for (chUrl in channelUrls) {
            Log.i(TAG, "GET channel $chUrl (pref=${settings.otaChannel})")
            val body = getText(chUrl, withApiKey = false) ?: continue
            lastChannel = body
            val manifestUrls = OtaManifest.parseChannel(body)
            if (manifestUrls.isEmpty()) continue
            for (mUrl in manifestUrls) {
                Log.i(TAG, "GET manifest $mUrl")
                val text = getText(mUrl, withApiKey = false) ?: continue
                val parsed = OtaManifest.parse(text)
                if (parsed != null && parsed.available) return parsed
            }
        }
        lastChannel?.let { Log.w(TAG, "CDN channel had no usable manifest") }
        return null
    }

    fun download(
        kind: OtaOffer.Kind,
        urls: List<String>,
        size: Long,
        sha256: String,
        onProgress: ((Long, Long) -> Unit)? = null,
    ): File? {
        val name = if (kind == OtaOffer.Kind.APK) "app-debug.apk" else "firmware.bin"
        val dest = File(cacheDir, name)
        val candidates = urls.map { artifactUrl(it) }.distinct().filter { it.isNotBlank() }
        for (url in candidates) {
            dest.delete()
            if (!getFile(url, dest, size, onProgress)) {
                dest.delete()
                continue
            }
            if (kind == OtaOffer.Kind.APK && dest.name.endsWith(".apk").not() && looksLikeZip(dest)) {
                val extracted = extractApkFromBundle(dest) ?: continue
                dest.delete()
                extracted.copyTo(dest, overwrite = true)
                extracted.delete()
            }
            val got = sha256Of(dest)
            if (got != sha256.lowercase()) {
                Log.w(TAG, "checksum mismatch $url want=$sha256 got=$got")
                dest.delete()
                continue
            }
            return dest
        }
        return null
    }

    fun download(
        kind: OtaOffer.Kind,
        relativeUrl: String,
        size: Long,
        sha256: String,
        onProgress: ((Long, Long) -> Unit)? = null,
    ): File? {
        return download(kind, listOf(relativeUrl), size, sha256, onProgress)
    }

    private fun looksLikeZip(file: File): Boolean {
        if (file.length() < 4) return false
        file.inputStream().use { ins ->
            val b = ByteArray(2)
            if (ins.read(b) != 2) return false
            return b[0] == 0x50.toByte() && b[1] == 0x4B.toByte()
        }
    }

    private fun extractApkFromBundle(zip: File): File? {
        return try {
            ZipFile(zip).use { zf ->
                val entry = zf.getEntry("package.apk") ?: return null
                val out = File(cacheDir, "bundle-package.apk")
                zf.getInputStream(entry).use { ins ->
                    FileOutputStream(out).use { os -> ins.copyTo(os) }
                }
                out
            }
        } catch (e: Exception) {
            Log.w(TAG, "bundle unpack failed: ${e.message}")
            null
        }
    }

    private fun artifactUrl(relative: String): String {
        val rel = relative.trim()
        if (rel.startsWith("http://") || rel.startsWith("https://")) return rel
        val base = settings.baseUrl.trim().trimEnd('/')
        return "$base/v1/ota/${rel.trimStart('/')}"
    }

    private fun getText(urlStr: String, withApiKey: Boolean): String? {
        val started = android.os.SystemClock.elapsedRealtime()
        val conn = openFollowing(urlStr, withApiKey, JSON_TIMEOUT_MS)
        return try {
            val code = conn.responseCode
            val stream = if (code in 200..299) conn.inputStream else conn.errorStream
            val text = stream?.bufferedReader(Charsets.UTF_8)?.use { it.readText() }.orEmpty()
            val ms = android.os.SystemClock.elapsedRealtime() - started
            if (code in 200..299) {
                Log.i(TAG, "GET $urlStr HTTP $code ${text.length} B ${ms}ms")
                text
            } else {
                Log.w(TAG, "GET $urlStr HTTP $code ${text.take(120)} ${ms}ms")
                null
            }
        } catch (e: Exception) {
            Log.w(TAG, "GET $urlStr failed: ${e.message}")
            null
        } finally {
            conn.disconnect()
        }
    }

    private fun getFile(
        urlStr: String,
        dest: File,
        expectedSize: Long,
        onProgress: ((Long, Long) -> Unit)? = null,
    ): Boolean {
        val conn = openFollowing(urlStr, withApiKey = urlStr.contains("/v1/ota/"), timeoutMs = 60_000)
        return try {
            val code = conn.responseCode
            if (code in 300..399) {
                val loc = conn.getHeaderField("Location")
                if (!loc.isNullOrBlank()) {
                    conn.disconnect()
                    return getFile(loc, dest, expectedSize, onProgress)
                }
            }
            if (code !in 200..299) {
                Log.w(TAG, "GET file $urlStr HTTP $code")
                return false
            }
            val total = conn.contentLengthLong.takeIf { it > 0 } ?: expectedSize
            var received = 0L
            FileOutputStream(dest).use { out ->
                conn.inputStream.use { input ->
                    val buf = ByteArray(16 * 1024)
                    while (true) {
                        val n = input.read(buf)
                        if (n <= 0) break
                        out.write(buf, 0, n)
                        received += n
                        onProgress?.invoke(received, total)
                    }
                }
            }
            if (expectedSize > 0 && dest.length() != expectedSize) {
                // Bundle zip size differs from inner APK; allow if zip magic.
                if (!looksLikeZip(dest)) {
                    Log.w(TAG, "size mismatch want=$expectedSize got=${dest.length()}")
                    return false
                }
            }
            true
        } catch (e: Exception) {
            Log.w(TAG, "download $urlStr failed: ${e.message}")
            false
        } finally {
            conn.disconnect()
        }
    }

    private fun openFollowing(urlStr: String, withApiKey: Boolean, timeoutMs: Int): HttpURLConnection {
        var current = urlStr
        repeat(4) {
            val conn = open(current, withApiKey, timeoutMs)
            conn.instanceFollowRedirects = false
            val code = try {
                conn.responseCode
            } catch (e: Exception) {
                conn.disconnect()
                throw e
            }
            if (code in 300..399) {
                val loc = conn.getHeaderField("Location")
                conn.disconnect()
                if (loc.isNullOrBlank()) {
                    return conn
                }
                current = if (loc.startsWith("http")) loc else URL(URL(current), loc).toString()
                Log.i(TAG, "GET redirect $code -> $current")
                return@repeat
            }
            return conn
        }
        return open(current, withApiKey, timeoutMs)
    }

    private fun open(urlStr: String, withApiKey: Boolean, timeoutMs: Int = 60_000): HttpURLConnection {
        return (URL(urlStr).openConnection() as HttpURLConnection).apply {
            instanceFollowRedirects = false
            requestMethod = "GET"
            connectTimeout = timeoutMs.coerceAtMost(12_000)
            readTimeout = timeoutMs
            setRequestProperty("Connection", "close")
            if (withApiKey && settings.apiKey.isNotBlank()) {
                setRequestProperty("X-API-Key", settings.apiKey)
            }
        }
    }

    companion object {
        private const val TAG = "OtaRepository"
        private const val JSON_TIMEOUT_MS = 8_000

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
