package com.esp32s3.imusim

/** Public CDN announce + backbone hosts. Distinct prefix from Android Cast `/v0/ota`. */
object OtaCdn {
    const val PRODUCT = "good_vibes"
    const val DIRECTOR = "https://cdn.f0xx.org"
    val BACKBONES = arrayOf(
        "https://cdn0.f0xx.org",
        "https://cdn1.f0xx.org",
        "https://cdn2.f0xx.org",
    )

    fun channelPath(channelFile: String = "stable.json"): String {
        return "/$PRODUCT/v0/ota/channel/$channelFile"
    }

    fun channelUrls(channelFile: String = "stable.json"): List<String> {
        val path = channelPath(channelFile)
        val out = ArrayList<String>(1 + BACKBONES.size)
        out.add(DIRECTOR + path)
        for (h in BACKBONES) {
            out.add(h + path)
        }
        return out
    }

    fun channelFileForBuilder(channel: String): String {
        return when (channel.lowercase()) {
            "", "imu", "prod", "stable" -> "stable.json"
            "staging" -> "staging.json"
            "dev" -> "dev.json"
            else -> if (channel.endsWith(".json")) channel else "$channel.json"
        }
    }
}
