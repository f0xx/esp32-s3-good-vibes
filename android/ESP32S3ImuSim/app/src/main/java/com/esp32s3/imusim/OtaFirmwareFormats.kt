package com.esp32s3.imusim

import android.net.Uri
import android.webkit.MimeTypeMap
import java.io.InputStream
import java.util.Locale

/** Supported mobile→ESP32 BLE OTA payload formats (ESP `Update` API expects raw flash bytes). */
object OtaFirmwareFormats {
    /** File extensions accepted by [parseFirmware]. */
    val SUPPORTED_EXTENSIONS: List<String> = listOf("bin")

    /** MIME types passed to the system document picker. */
    fun openDocumentMimeTypes(): Array<String> = arrayOf(
        "application/octet-stream",
        "application/x-binary",
        "application/macbinary",
        "binary/octet-stream",
    )

    fun extensionFromUri(name: String?): String? {
        if (name.isNullOrBlank()) return null
        val dot = name.lastIndexOf('.')
        if (dot < 0 || dot == name.length - 1) return null
        return name.substring(dot + 1).lowercase(Locale.US)
    }

    fun extensionFromUri(uri: Uri, resolver: android.content.ContentResolver): String? {
        val name = resolver.query(uri, arrayOf(android.provider.OpenableColumns.DISPLAY_NAME), null, null, null)
            ?.use { c ->
                if (c.moveToFirst()) c.getString(0) else null
            }
        return extensionFromUri(name)
    }

    fun isSupportedExtension(ext: String?): Boolean =
        ext != null && ext.lowercase(Locale.US) in SUPPORTED_EXTENSIONS

    fun unsupportedExtensionMessage(ext: String?): String {
        val got = ext?.let { ".$it" } ?: "(no extension)"
        val want = SUPPORTED_EXTENSIONS.joinToString(", ") { ".$it" }
        return "Unsupported firmware $got — need $want"
    }

    fun pickerSummary(): String =
        SUPPORTED_EXTENSIONS.joinToString(", ") { ".$it" }

    /**
     * Reads and validates firmware bytes from a content [Uri].
     * Returns null + human message via [errorOut] on failure.
     */
    fun parseFirmware(
        uri: Uri,
        resolver: android.content.ContentResolver,
        openStream: (Uri) -> InputStream?,
        errorOut: (String) -> Unit,
    ): ByteArray? {
        val ext = extensionFromUri(uri, resolver)
        if (!isSupportedExtension(ext)) {
            errorOut(unsupportedExtensionMessage(ext))
            return null
        }
        val bytes = try {
            openStream(uri)?.use { it.readBytes() }
        } catch (e: Exception) {
            errorOut("Read failed: ${e.message}")
            return null
        }
        if (bytes == null || bytes.isEmpty()) {
            errorOut("Firmware file is empty")
            return null
        }
        if (bytes.size < 1024) {
            errorOut("Firmware too small (${bytes.size} B)")
            return null
        }
        return bytes
    }

    fun guessMime(ext: String): String =
        MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext) ?: "application/octet-stream"
}
