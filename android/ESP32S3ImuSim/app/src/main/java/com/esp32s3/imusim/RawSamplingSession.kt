package com.esp32s3.imusim

import android.os.SystemClock
import kotlin.math.sqrt

/** RAW magnitude sampling state — drives top banner hints for FFT / diagnosis. */
class RawSamplingSession(
    private val minFftSamples: Int = 32,
    private val stableWindow: Int = 16,
) {
    enum class Phase {
        IDLE,
        SCHEDULE_IDLE,
        COLLECTING,
        UNSTABLE,
        NEED_MORE,
        READY,
    }

    data class Hint(val level: StatusBannerLevel, val message: String)

    private var phase = Phase.IDLE
    private var lastHint: Hint? = null
    private var refRecording = false
    private var captureActive = true
    private var vibroTier = 0

    private var lastEmitMs = 0L

    fun onMode(mode: Int) {
        if (mode != ImuProtocol.MODE_RAW) {
            phase = Phase.IDLE
            lastHint = null
        }
    }

    fun onRefRecording(active: Boolean) {
        refRecording = active
    }

    fun onStatus(status: ImuProtocol.Status) {
        captureActive = status.captureActive ?: true
        vibroTier = status.vibroTier ?: vibroTier
        if (status.vibroRefReady && refRecording) {
            refRecording = false
        }
    }

    fun onBatch(batch: ImuProtocol.Batch, buffer: VibroSampleBuffer): Hint? {
        if (refRecording) {
            return emit(
                Hint(StatusBannerLevel.WARN, "Shake the device — recording reference"),
                force = true,
            )
        }
        if (batch.mode != ImuProtocol.MODE_RAW) {
            phase = Phase.IDLE
            return null
        }
        if (!captureActive) {
            phase = Phase.SCHEDULE_IDLE
            return emit(
                Hint(
                    StatusBannerLevel.WARN,
                    "Sampling paused — capture window closed (tier $vibroTier)",
                ),
            )
        }
        val n = buffer.sampleCount()
        if (n < minFftSamples) {
            phase = Phase.NEED_MORE
            val msg = if (buffer.isUnstable(stableWindow)) {
                "Unstable signal — shake steadily (${n}/$minFftSamples)"
            } else {
                "Collecting samples — shake the device (${n}/$minFftSamples)"
            }
            return emit(Hint(StatusBannerLevel.WARN, msg), throttleMs = 2000)
        }
        if (buffer.isUnstable(stableWindow)) {
            phase = Phase.UNSTABLE
            return emit(
                Hint(
                    StatusBannerLevel.WARN,
                    "Unstable profile — keep shaking or hold mount steady",
                ),
            )
        }
        phase = Phase.READY
        return emit(Hint(StatusBannerLevel.OK, "Samples ready — tap FFT or wait"))
    }

    fun phase(): Phase = phase

    private fun emit(hint: Hint, force: Boolean = false, throttleMs: Long = 800): Hint? {
        val now = SystemClock.uptimeMillis()
        if (!force && lastHint?.message == hint.message) {
            return null
        }
        if (!force && now - lastEmitMs < throttleMs) {
            return null
        }
        lastEmitMs = now
        lastHint = hint
        return hint
    }
}

fun VibroSampleBuffer.isUnstable(window: Int): Boolean {
    val samples = snapshot()
    if (samples.size < window) {
        return false
    }
    val slice = samples.takeLast(window)
    val mean = slice.average().toFloat()
    if (mean < 0.05f) {
        return true
    }
    var varSum = 0.0
    for (v in slice) {
        val d = v - mean
        varSum += d * d
    }
    val std = sqrt(varSum / slice.size).toFloat()
    val cv = std / mean
    return cv > 0.35f
}
