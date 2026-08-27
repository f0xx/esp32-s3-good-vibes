package com.esp32s3.imusim

import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.zip.GZIPInputStream
import java.util.zip.GZIPOutputStream
import kotlin.math.abs
import kotlin.math.round

/** Shared float16 band compaction for BLE `b16` and cloud payloads. */
object MetricsCompact {
    fun f32ToF16(v: Float): Int {
        val bits = java.lang.Float.floatToIntBits(v)
        val sign = (bits ushr 16) and 0x8000
        var exp = (bits ushr 23) and 0xff
        val mant = bits and 0x7fffff
        if (exp == 255) {
            return sign or 0x7c00 or (if (mant != 0) 0x200 else 0)
        }
        if (exp == 0 && mant == 0) {
            return sign
        }
        var newExp = exp - 127 + 15
        if (newExp >= 31) {
            return sign or 0x7c00
        }
        if (newExp <= 0) {
            if (newExp < -10) return sign
            var m = mant or 0x800000
            m = m ushr (1 - newExp)
            return sign or (m ushr 13)
        }
        return sign or (newExp shl 10) or (mant ushr 13)
    }

    fun f16ToF32(bits: Int): Float {
        val sign = (bits and 0x8000) shl 16
        val exp = (bits ushr 10) and 0x1f
        val mant = bits and 0x3ff
        val out = when {
            exp == 0 -> if (mant == 0) sign else {
                var m = mant
                var e = -1
                while (m and 0x400 == 0) {
                    m = m shl 1
                    e--
                }
                m = m and 0x3ff
                sign or ((127 - 15 + 1 + e) shl 23) or (m shl 13)
            }
            exp == 31 -> sign or 0x7f800000 or (mant shl 13)
            else -> sign or ((exp + 127 - 15) shl 23) or (mant shl 13)
        }
        return java.lang.Float.intBitsToFloat(out)
    }

    fun bandsToF16Array(bands: FloatArray): IntArray =
        IntArray(bands.size) { f32ToF16(bands[it]) }

    /** Gzip-compress UTF-8 JSONL for pending upload storage. */
    fun gzipText(text: String): ByteArray {
        val bos = ByteArrayOutputStream()
        GZIPOutputStream(bos).use { it.write(text.toByteArray(Charsets.UTF_8)) }
        return bos.toByteArray()
    }

    fun gunzipText(data: ByteArray): String =
        GZIPInputStream(data.inputStream()).use {
            it.readBytes().toString(Charsets.UTF_8)
        }

    /** Optional: pack spectrum bins to float16 LE bytes (~50% size). */
    fun binsToF16Bytes(bins: List<Float>): ByteArray {
        val bb = ByteBuffer.allocate(bins.size * 2).order(ByteOrder.LITTLE_ENDIAN)
        bins.forEach { bb.putShort(f32ToF16(it).toShort()) }
        return bb.array()
    }
}
