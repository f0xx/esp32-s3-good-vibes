package com.esp32s3.imusim

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.Locale

/** mix / dynamic capture bytes — DeviceConfigV1.reserved[0..3]. */
object VibroMixConfig {
    private const val OFF_MIX_EVERY = 171
    private const val OFF_MIX_RATIO = 172
    private const val OFF_DYN_SHORT = 173
    private const val OFF_DYN_NESTED = 174

    data class Mix(
        val every: Int,
        val ratio: Int,
        val dynShort: Int,
        val dynNested: Int,
    ) {
        val enabled: Boolean get() = every >= 2 && ratio >= 2
    }

    fun read(blob: ByteArray?): Mix {
        if (blob == null || blob.size < ConfigProtocol.BLOB_SIZE) {
            return Mix(0, 0, 0, 0)
        }
        return Mix(
            every = blob[OFF_MIX_EVERY].toInt() and 0xFF,
            ratio = blob[OFF_MIX_RATIO].toInt() and 0xFF,
            dynShort = blob[OFF_DYN_SHORT].toInt() and 0xFF,
            dynNested = blob[OFF_DYN_NESTED].toInt() and 0xFF,
        )
    }

    fun write(blob: ByteArray, mix: Mix): ByteArray {
        val out = if (blob.size >= ConfigProtocol.BLOB_SIZE) {
            blob.copyOf(ConfigProtocol.BLOB_SIZE)
        } else {
            ProfilePresets.apply(null, ProfilePresets.Id.VIBRO_NORMAL).blob
        }
        val bb = ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN)
        bb.put(OFF_MIX_EVERY, mix.every.coerceIn(0, 255).toByte())
        bb.put(OFF_MIX_RATIO, mix.ratio.coerceIn(0, 255).toByte())
        bb.put(OFF_DYN_SHORT, mix.dynShort.coerceIn(0, 255).toByte())
        bb.put(OFF_DYN_NESTED, mix.dynNested.coerceIn(0, 255).toByte())
        return out
    }

    fun format(mix: Mix): String {
        if (!mix.enabled) {
            return "mix off (full window every capture)"
        }
        val base = String.format(Locale.US, "mix %d:%d", mix.every, mix.ratio)
        val dyn = buildList {
            if (mix.dynShort >= 2) add("dyn/${mix.dynShort}@even")
            if (mix.dynNested >= 2) add("nest/${mix.dynNested}@÷4")
        }
        return if (dyn.isEmpty()) base else "$base ${dyn.joinToString(" ")}"
    }

    fun parseField(name: String, text: String, current: Int): Int? {
        val trimmed = text.trim()
        if (trimmed.isEmpty()) {
            return current
        }
        val v = trimmed.toIntOrNull() ?: return null
        return v.coerceIn(0, 255)
    }
}
