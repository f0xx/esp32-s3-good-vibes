package com.esp32s3.imusim

/**
 * Rolling 1 s windows for BLE receive, UI apply, and Canvas draw rates.
 */
class FpsMeter(private val windowMs: Long = 1000L) {
    private var windowStartMs = 0L
    private var bleRx = 0
    private var uiApply = 0
    private var draw = 0
    private var bleRxFps = 0f
    private var uiApplyFps = 0f
    private var drawFps = 0f
    private var pollMs = ImuProtocol.DEFAULT_POLL_MS
    private var drawFpsCap = 0

    data class Snapshot(
        val bleRxFps: Float,
        val uiApplyFps: Float,
        val drawFps: Float,
        val pollMs: Int,
        val targetFps: Float,
        val drawFpsCap: Int,
    ) {
        fun caption(): String {
            val capLabel = if (drawFpsCap <= 0) "auto" else drawFpsCap.toString()
            return String.format(
                java.util.Locale.US,
                "fps BLE %.0f UI %.0f draw %.0f (poll %dms, tgt %.0f, cap %s)",
                bleRxFps,
                uiApplyFps,
                drawFps,
                pollMs,
                targetFps,
                capLabel,
            )
        }

        fun hudLine(): String {
            return String.format(
                java.util.Locale.US,
                "BLE %.0f UI %.0f draw %.0f",
                bleRxFps,
                uiApplyFps,
                drawFps,
            )
        }
    }

    @Synchronized
    fun setPollMs(ms: Int) {
        pollMs = ms
    }

    @Synchronized
    fun setDrawFpsCap(cap: Int) {
        drawFpsCap = cap.coerceIn(0, 25)
    }

    @Synchronized
    fun onBleBatch(nowMs: Long = System.currentTimeMillis()) {
        roll(nowMs)
        bleRx++
        publish(nowMs)
    }

    @Synchronized
    fun onUiApply(nowMs: Long = System.currentTimeMillis()) {
        roll(nowMs)
        uiApply++
        publish(nowMs)
    }

    @Synchronized
    fun onDraw(nowMs: Long = System.currentTimeMillis()) {
        roll(nowMs)
        draw++
        publish(nowMs)
    }

    @Synchronized
    fun snapshot(nowMs: Long = System.currentTimeMillis()): Snapshot {
        roll(nowMs)
        return Snapshot(
            bleRxFps = bleRxFps,
            uiApplyFps = uiApplyFps,
            drawFps = drawFps,
            pollMs = pollMs,
            targetFps = 1000f / pollMs.coerceAtLeast(1),
            drawFpsCap = drawFpsCap,
        )
    }

    private fun roll(nowMs: Long) {
        if (windowStartMs == 0L) {
            windowStartMs = nowMs
            return
        }
        val elapsed = nowMs - windowStartMs
        if (elapsed < windowMs) return
        bleRxFps = bleRx * 1000f / elapsed
        uiApplyFps = uiApply * 1000f / elapsed
        drawFps = draw * 1000f / elapsed
        windowStartMs = nowMs
        bleRx = 0
        uiApply = 0
        draw = 0
    }

    private fun publish(nowMs: Long) {
        roll(nowMs)
    }
}
