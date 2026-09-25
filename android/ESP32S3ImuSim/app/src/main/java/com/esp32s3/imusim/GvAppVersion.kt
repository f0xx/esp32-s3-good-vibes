package com.esp32s3.imusim

/**
 * Good Vibes four-part version (same idea as Android Cast, own counters).
 * Display: `00.0001.0000.00005`.
 * Code is the dotted fields with matching place values (no floor):
 * `major*1_000_000_000 + minor*100_000 + patch` → `1000000005`.
 * Line is display-only. Android versionCode is signed 32-bit, so major is 0–2.
 */
object GvAppVersion {
    const val MAJOR_PLACE = 1_000_000_000
    const val MINOR_PLACE = 100_000
    private val FOUR_PART = Regex("""^(\d+)\.(\d+)\.(\d+)\.(\d+)$""")

    data class Parsed(
        val line: Int,
        val major: Int,
        val minor: Int,
        val patch: Int,
        val versionName: String,
        val versionCode: Int,
    )

    fun format(line: Int, major: Int, minor: Int, patch: Int): String {
        return "%02d.%04d.%04d.%05d".format(
            line.coerceIn(0, 99),
            major.coerceIn(0, 2),
            minor.coerceAtLeast(0),
            patch.coerceAtLeast(0),
        )
    }

    fun pack(major: Int, minor: Int, patch: Int): Int {
        val m = major.coerceIn(0, 2)
        val n = minor.coerceIn(0, 9999)
        val p = patch.coerceIn(0, 99999)
        return m * MAJOR_PLACE + n * MINOR_PLACE + p
    }

    fun parse(raw: String?): Parsed? {
        val s = raw?.trim().orEmpty()
        if (s.isEmpty()) return null
        val m = FOUR_PART.matchEntire(s) ?: return null
        val line = m.groupValues[1].toInt()
        val major = m.groupValues[2].toInt()
        val minor = m.groupValues[3].toInt()
        val patch = m.groupValues[4].toInt()
        return Parsed(line, major, minor, patch, format(line, major, minor, patch), pack(major, minor, patch))
    }

    fun parseCode(raw: String?): Int {
        parse(raw)?.let { return it.versionCode }
        val m = Regex("""v(\d+)""", RegexOption.IGNORE_CASE).find(raw?.trim().orEmpty())
        return m?.groupValues?.get(1)?.toIntOrNull() ?: 0
    }
}
