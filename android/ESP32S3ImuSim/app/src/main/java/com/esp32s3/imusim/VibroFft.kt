package com.esp32s3.imusim

import kotlin.math.cos
import kotlin.math.sin
import kotlin.math.sqrt

/** Phone-side magnitude FFT (Case C) — power-of-2 Cooley–Tukey, real input. */
object VibroFft {
    data class Result(
        val magnitudes: FloatArray,
        val sampleHz: Float,
        val binHz: Float,
        val peakHz: Float,
        val peakMag: Float,
    )

    fun magnitudeSpectrum(samples: FloatArray, sampleHz: Float): Result? {
        if (samples.size < 16 || sampleHz <= 0f) return null
        val n = nextPow2(samples.size.coerceAtMost(512))
        val re = FloatArray(n)
        val im = FloatArray(n)
        for (i in samples.indices) {
            if (i >= n) break
            re[i] = samples[i]
        }
        fftInPlace(re, im)
        val half = n / 2
        val mags = FloatArray(half)
        var peakMag = 0f
        var peakIdx = 1
        for (i in 1 until half) {
            val mag = sqrt(re[i] * re[i] + im[i] * im[i]) / n
            mags[i] = mag
            if (mag > peakMag) {
                peakMag = mag
                peakIdx = i
            }
        }
        val binHz = sampleHz / n
        return Result(
            magnitudes = mags,
            sampleHz = sampleHz,
            binHz = binHz,
            peakHz = peakIdx * binHz,
            peakMag = peakMag,
        )
    }

    private fun nextPow2(v: Int): Int {
        var n = 1
        while (n < v) n = n shl 1
        return n.coerceIn(16, 512)
    }

    private fun fftInPlace(re: FloatArray, im: FloatArray) {
        val n = re.size
        var j = 0
        for (i in 1 until n) {
            var bit = n shr 1
            while (j and bit != 0) {
                j = j xor bit
                bit = bit shr 1
            }
            j = j xor bit
            if (i < j) {
                re[i] = re[j].also { re[j] = re[i] }
                im[i] = im[j].also { im[j] = im[i] }
            }
        }
        var len = 2
        while (len <= n) {
            val ang = (-2.0 * Math.PI / len).toFloat()
            val wlenRe = cos(ang)
            val wlenIm = sin(ang)
            var i = 0
            while (i < n) {
                var wRe = 1f
                var wIm = 0f
                for (k in 0 until len / 2) {
                    val uRe = re[i + k]
                    val uIm = im[i + k]
                    val vRe = re[i + k + len / 2] * wRe - im[i + k + len / 2] * wIm
                    val vIm = re[i + k + len / 2] * wIm + im[i + k + len / 2] * wRe
                    re[i + k] = uRe + vRe
                    im[i + k] = uIm + vIm
                    re[i + k + len / 2] = uRe - vRe
                    im[i + k + len / 2] = uIm - vIm
                    val nwRe = wRe * wlenRe - wIm * wlenIm
                    wIm = wRe * wlenIm + wIm * wlenRe
                    wRe = nwRe
                }
                i += len
            }
            len = len shl 1
        }
    }
}
