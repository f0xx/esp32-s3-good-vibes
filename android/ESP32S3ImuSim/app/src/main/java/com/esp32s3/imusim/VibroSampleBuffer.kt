package com.esp32s3.imusim

import org.json.JSONObject
import kotlin.math.sqrt

/** Rolling magnitude buffer from RAW IMU batches for phone-side FFT. */
class VibroSampleBuffer(private val capacity: Int = 256) {
    private val ring = FloatArray(capacity)
    private var head = 0
    private var count = 0
    private var lastSampleMs = 0L

    val isFull: Boolean
        get() = count >= capacity

    val sampleHz: Float
        get() {
            if (count < 2) return 100f
            return 1000f / 10f // default IMU pipeline ~10 Hz on Zephyr; override when known
        }

    fun setSampleHz(hz: Float) {
        if (hz > 0f) sampleHzOverride = hz
    }

    private var sampleHzOverride = 0f

    fun effectiveSampleHz(): Float =
        if (sampleHzOverride > 0f) sampleHzOverride else sampleHz

    fun pushMagnitude(ax: Float, ay: Float, az: Float) {
        val mag = sqrt(ax * ax + ay * ay + az * az)
        ring[head] = mag
        head = (head + 1) % capacity
        if (count < capacity) count++
    }

    fun ingestRawBatch(batch: ImuProtocol.Batch) {
        var prevMs = 0L
        for (rec in batch.raw) {
            if (prevMs > 0L && rec.tMs > prevMs) {
                val dt = rec.tMs - prevMs
                if (dt in 5..500) {
                    setSampleHz(1000f / dt.toFloat())
                }
            }
            prevMs = rec.tMs
            pushMagnitude(rec.ax, rec.ay, rec.az)
            lastSampleMs = rec.tMs
        }
    }

    fun snapshot(): FloatArray {
        val out = FloatArray(count)
        if (count < capacity) {
            System.arraycopy(ring, 0, out, 0, count)
        } else {
            for (i in 0 until count) {
                val idx = (head + i) % capacity
                out[i] = ring[idx]
            }
        }
        return out
    }

    fun sampleCount(): Int = count

    fun clear() {
        head = 0
        count = 0
    }
}
