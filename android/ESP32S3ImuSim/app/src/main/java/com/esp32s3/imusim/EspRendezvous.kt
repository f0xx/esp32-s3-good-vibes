package com.esp32s3.imusim

import android.content.Context
import java.util.concurrent.TimeUnit
import kotlin.math.max
import kotlin.math.min

/**
 * Align phone bridge connects with ESP vibro capture / wake windows.
 * Algorithm matches zephyr/app/common/vibro_schedule.c.
 */
object EspRendezvous {
    const val PRECONNECT_SEC = 10
    private const val MIN_DELAY_MS = 30_000L
    private const val FALLBACK_DELAY_MS = 300_000L

    data class WindowInfo(
        val active: Boolean,
        val secLeft: Int,
        val secUntilOpen: Int,
    )

    fun fromSession(context: Context): DeviceConfigFields? =
        DeviceConfigFields.fromBlob(ImuSessionStore(context).loadLocalConfig())

    /** Delay before the next BLE bridge connect attempt. */
    fun nextConnectDelayMs(context: Context, nowSec: Long = System.currentTimeMillis() / 1000L): Long {
        val cfg = fromSession(context) ?: return FALLBACK_DELAY_MS
        val info = combinedWindow(cfg, nowSec)
        if (info.active) {
            return 0L
        }
        val leadSec = max(0, info.secUntilOpen - PRECONNECT_SEC)
        return max(MIN_DELAY_MS, TimeUnit.SECONDS.toMillis(leadSec.toLong()))
    }

    /** Suggested dwell while connected: cover rest of capture/wake window. */
    fun suggestedDwellSec(context: Context, status: ImuProtocol.Status?, nowSec: Long = System.currentTimeMillis() / 1000L): Int {
        val settings = BridgeSyncSettings(context)
        val base = settings.dwellSeconds
        status?.captureWindowSecRemaining?.let { rem ->
            if (rem > 0) {
                return max(base, min(rem + 8, 120))
            }
        }
        if (status?.captureActive == true) {
            return max(base, 30)
        }
        val cfg = fromSession(context) ?: return base
        val info = combinedWindow(cfg, nowSec)
        return if (info.active) {
            max(base, min(info.secLeft + 8, 120))
        } else {
            base
        }
    }

    fun summary(context: Context, nowSec: Long = System.currentTimeMillis() / 1000L): String {
        val cfg = fromSession(context) ?: return "rendezvous: no ESP config (sync config once)"
        val info = combinedWindow(cfg, nowSec)
        val mode = when (cfg.vibroScheduleMode) {
            1 -> "interval"
            2 -> "random"
            else -> "always"
        }
        val wake = if (cfg.deepSleepEnable != 0) {
            " wake=${cfg.activeWindowSec}s/${cfg.wakeIntervalSec}s"
        } else {
            ""
        }
        return if (info.active) {
            "rendezvous: IN window (${info.secLeft}s left) cap $mode${cfg.vibroWindowSec}s/${cfg.vibroIntervalSec}s$wake"
        } else {
            "rendezvous: next in ${info.secUntilOpen}s (preconnect ${PRECONNECT_SEC}s) $mode$wake"
        }
    }

    fun combinedWindow(cfg: DeviceConfigFields, nowSec: Long): WindowInfo {
        val vibro = windowForSchedule(
            mode = cfg.vibroScheduleMode,
            intervalSec = cfg.vibroIntervalSec,
            windowSec = cfg.vibroWindowSec,
            jitterSec = cfg.vibroJitterSec,
            nowSec = nowSec,
            mixEvery = cfg.vibroMixEvery,
            mixRatio = cfg.vibroMixRatio,
            dynShortRatio = cfg.vibroDynShortRatio,
            dynNestedRatio = cfg.vibroDynNestedRatio,
        )
        if (cfg.deepSleepEnable == 0 || cfg.wakeIntervalSec <= 0) {
            return vibro
        }
        val wake = windowForSchedule(
            mode = 1,
            intervalSec = cfg.wakeIntervalSec,
            windowSec = cfg.activeWindowSec.coerceAtLeast(1),
            jitterSec = 0,
            nowSec = nowSec,
        )
        return mergeWindows(vibro, wake)
    }

    fun windowForSchedule(
        mode: Int,
        intervalSec: Int,
        windowSec: Int,
        jitterSec: Int,
        nowSec: Long,
        mixEvery: Int = 0,
        mixRatio: Int = 0,
        dynShortRatio: Int = 0,
        dynNestedRatio: Int = 0,
    ): WindowInfo {
        if (mode == 0) {
            return WindowInfo(active = true, secLeft = 3600, secUntilOpen = 0)
        }
        var interval = intervalSec.coerceAtLeast(1)
        val maxWindow = windowSec.coerceAtLeast(1).coerceAtMost(interval)
        val bucket = (nowSec / interval).toInt()
        var window = effectiveWindowSec(maxWindow, bucket, mixEvery, mixRatio, dynShortRatio, dynNestedRatio)
        if (window > interval) {
            window = interval
        }
        val phase = (nowSec % interval).toInt()
        val (start, end) = when (mode) {
            1 -> 0 to window
            else -> {
                val span = interval - maxWindow + 1
                val slot = hash32(bucket xor jitterSec) % span
                slot to (slot + window)
            }
        }
        return if (phase >= start && phase < end) {
            WindowInfo(active = true, secLeft = end - phase, secUntilOpen = 0)
        } else if (phase < start) {
            WindowInfo(active = false, secLeft = 0, secUntilOpen = start - phase)
        } else {
            val nextBucket = bucket + 1
            val nextStart = when (mode) {
                1 -> 0
                else -> {
                    val span = interval - maxWindow + 1
                    hash32(nextBucket xor jitterSec) % span
                }
            }
            WindowInfo(active = false, secLeft = 0, secUntilOpen = interval - phase + nextStart)
        }
    }

    private fun effectiveWindowSec(
        maxWindow: Int,
        bucket: Int,
        mixEvery: Int,
        mixRatio: Int,
        dynShortRatio: Int,
        dynNestedRatio: Int,
    ): Int {
        if (mixEvery < 2 || mixRatio < 2) {
            return maxWindow
        }
        if (bucket % mixEvery == mixEvery - 1) {
            return maxWindow
        }
        var window = (maxWindow / mixRatio).coerceAtLeast(2)
        if (dynShortRatio >= 2 && bucket % 2 == 0) {
            window = (window / dynShortRatio).coerceAtLeast(2)
        }
        if (dynNestedRatio >= 2 && bucket % 4 == 0) {
            window = (window / dynNestedRatio).coerceAtLeast(2)
        }
        return window
    }

    private fun mergeWindows(a: WindowInfo, b: WindowInfo): WindowInfo {
        if (a.active) {
            return if (b.active) {
                WindowInfo(
                    active = true,
                    secLeft = max(a.secLeft, b.secLeft),
                    secUntilOpen = 0,
                )
            } else {
                a
            }
        }
        if (b.active) {
            return b
        }
        return WindowInfo(
            active = false,
            secLeft = 0,
            secUntilOpen = min(a.secUntilOpen, b.secUntilOpen),
        )
    }

    /** Port of vibro_schedule.c hash32. */
    private fun hash32(x: Int): Int {
        var v = x.toLong() and 0xFFFF_FFFFL
        v = v xor (v shr 16)
        v = (v * 0x7feb352dL) and 0xFFFF_FFFFL
        v = v xor (v shr 15)
        v = (v * 0x846ca68bL) and 0xFFFF_FFFFL
        v = v xor (v shr 16)
        return v.toInt()
    }
}
