package com.esp32s3.imusim

import android.os.SystemClock
import java.util.Calendar
import java.util.Locale
import java.util.TimeZone

/** ESP panel + phone scene header: 15s stats → 5s date → 10s time (mirrored on both sides). */
object HeaderRotator {
    const val STATS_MS = 15_000L
    const val DATE_MS = 5_000L
    const val TIME_MS = 10_000L
    const val CYCLE_MS = STATS_MS + DATE_MS + TIME_MS

    enum class Phase { STATS, DATE, TIME }

    fun phase(nowUptimeMs: Long = SystemClock.uptimeMillis()): Phase {
        val t = nowUptimeMs % CYCLE_MS
        return when {
            t < STATS_MS -> Phase.STATS
            t < STATS_MS + DATE_MS -> Phase.DATE
            else -> Phase.TIME
        }
    }

    fun formatStats(distanceM: Float, power: ImuProtocol.PowerStatus?): String {
        val walk = String.format(Locale.US, "%.1f m", distanceM)
        val ps = power?.caption() ?: "p/s:--"
        return "$walk $ps"
    }

    fun formatDate(nowMs: Long = System.currentTimeMillis(), tzMin: Int? = null): String {
        val cal = calendarFor(nowMs, tzMin)
        val dow = when (cal.get(Calendar.DAY_OF_WEEK)) {
            Calendar.MONDAY -> "Mon"
            Calendar.TUESDAY -> "Tue"
            Calendar.WEDNESDAY -> "Wed"
            Calendar.THURSDAY -> "Thu"
            Calendar.FRIDAY -> "Fri"
            Calendar.SATURDAY -> "Sat"
            else -> "Sun"
        }
        return String.format(
            Locale.US,
            "%02d:%02d:%04d, %s",
            cal.get(Calendar.DAY_OF_MONTH),
            cal.get(Calendar.MONTH) + 1,
            cal.get(Calendar.YEAR),
            dow,
        )
    }

    fun formatTime(nowMs: Long = System.currentTimeMillis(), tzMin: Int? = null, clockSynced: Boolean = true): String {
        val ntp = if (clockSynced) "OK" else "FAIL"
        if (!clockSynced) {
            return "--:--:-- NTP $ntp"
        }
        val cal = calendarFor(nowMs, tzMin)
        return String.format(
            Locale.US,
            "%02d:%02d:%02d NTP %s",
            cal.get(Calendar.HOUR_OF_DAY),
            cal.get(Calendar.MINUTE),
            cal.get(Calendar.SECOND),
            ntp,
        )
    }

    fun label(
        distanceM: Float,
        power: ImuProtocol.PowerStatus?,
        nowUptimeMs: Long = SystemClock.uptimeMillis(),
        nowMs: Long = System.currentTimeMillis(),
        tzMin: Int? = null,
        clockSynced: Boolean = true,
    ): String = when (phase(nowUptimeMs)) {
        Phase.STATS -> formatStats(distanceM, power)
        Phase.DATE -> formatDate(nowMs, tzMin)
        Phase.TIME -> formatTime(nowMs, tzMin, clockSynced)
    }

    private fun calendarFor(nowMs: Long, tzMin: Int?): Calendar {
        val offsetMs = if (tzMin != null) {
            tzMin.toLong() * 60_000L
        } else {
            TimeZone.getDefault().getOffset(nowMs).toLong()
        }
        return Calendar.getInstance(TimeZone.getTimeZone("UTC")).apply {
            timeInMillis = nowMs + offsetMs
        }
    }
}
