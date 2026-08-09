package com.esp32s3.imusim

import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.sin
import kotlin.math.sqrt

data class AttitudeState(
    var roll: Float = 0f,
    var pitch: Float = 0f,
    var yaw: Float = 0f,
)

class AttitudeEstimator(private val alpha: Float = 0.12f) {
    val state = AttitudeState()
    private var lastMs: Long = 0

    fun reset() {
        state.roll = 0f
        state.pitch = 0f
        state.yaw = 0f
        lastMs = 0
    }

    fun update(sample: ImuProtocol.RawRecord) {
        val dt = if (lastMs == 0L) {
            0.01f
        } else {
            ((sample.tMs - lastMs).coerceAtLeast(1L)).toFloat() / 1000f
        }
        lastMs = sample.tMs

        val rollAcc = atan2(sample.ay, sample.az)
        val pitchAcc = atan2(-sample.ax, sqrt(sample.ay * sample.ay + sample.az * sample.az))

        val rollGyro = state.roll + sample.gx * DEG2RAD * dt
        val pitchGyro = state.pitch + sample.gy * DEG2RAD * dt
        val yawGyro = state.yaw + sample.gz * DEG2RAD * dt

        state.roll = (1f - alpha) * rollGyro + alpha * rollAcc
        state.pitch = (1f - alpha) * pitchGyro + alpha * pitchAcc
        state.yaw = yawGyro
    }

    fun rotationMatrix(): FloatArray {
        val cr = cos(state.roll)
        val sr = sin(state.roll)
        val cp = cos(state.pitch)
        val sp = sin(state.pitch)
        val cy = cos(state.yaw)
        val sy = sin(state.yaw)

        val rx = floatArrayOf(1f, 0f, 0f, 0f, cr, -sr, 0f, sr, cr)
        val ry = floatArrayOf(cp, 0f, sp, 0f, 1f, 0f, -sp, 0f, cp)
        val rz = floatArrayOf(cy, -sy, 0f, sy, cy, 0f, 0f, 0f, 1f)
        return mul3(mul3(rz, ry), rx)
    }

    private fun mul3(a: FloatArray, b: FloatArray): FloatArray {
        val out = FloatArray(9)
        for (r in 0 until 3) {
            for (c in 0 until 3) {
                out[r * 3 + c] =
                    a[r * 3 + 0] * b[0 * 3 + c] +
                        a[r * 3 + 1] * b[1 * 3 + c] +
                        a[r * 3 + 2] * b[2 * 3 + c]
            }
        }
        return out
    }

    companion object {
        private const val DEG2RAD = (Math.PI / 180.0).toFloat()
    }
}
