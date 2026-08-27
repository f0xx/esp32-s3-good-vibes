package com.esp32s3.imusim

/** Mirrors backend `estimate_discharge_ma` for live UI (500 mAh default cell). */
object BatteryBenchEstimator {
    private const val V_FULL = 4.20f
    private const val V_EMPTY = 3.00f
    private const val DEFAULT_CELL_MAH = 500

    fun estimateMa(
        voltageV: Float,
        prevVoltageV: Float?,
        dtMs: Long,
        cellMah: Int = DEFAULT_CELL_MAH,
    ): Float? {
        val prev = prevVoltageV ?: return null
        if (dtMs <= 0 || cellMah <= 0) return null
        val dtH = dtMs / 3_600_000f
        if (dtH <= 0f) return null
        val vSpan = V_FULL - V_EMPTY
        if (vSpan <= 0f) return null
        val dSoc = (voltageV - prev) / vSpan
        return kotlin.math.abs(cellMah * dSoc / dtH)
    }
}
