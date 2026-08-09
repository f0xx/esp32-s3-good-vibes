package com.esp32s3.imusim

/**
 * M-relay-B: when ESP uses deep-sleep profile (6.1), phone runs aggressive sync
 * during the short wake window — fast poll and offload ACK flush.
 */
object WakeRelay {
    const val AGGRESSIVE_POLL_MS = ImuProtocol.MIN_POLL_MS

    fun isDeepSleepProfile(status: ImuProtocol.Status): Boolean =
        status.powerProfile == ImuProtocol.PROFILE_DEEP_SLEEP

    fun onConnect(client: BleImuClient, lastStatus: ImuProtocol.Status?) {
        if (lastStatus != null && isDeepSleepProfile(lastStatus)) {
            client.setPollIntervalMs(AGGRESSIVE_POLL_MS)
        }
    }

    fun onStatus(client: BleImuClient, status: ImuProtocol.Status) {
        if (!isDeepSleepProfile(status)) {
            return
        }
        if (client.pollIntervalMs() > AGGRESSIVE_POLL_MS) {
            client.setPollIntervalMs(AGGRESSIVE_POLL_MS)
        }
    }

    fun profileCaption(profile: Int?, awakeSec: Int?): String? {
        if (profile == null) return null
        val name = when (profile) {
            ImuProtocol.PROFILE_DEEP_SLEEP -> "pp:deep"
            ImuProtocol.PROFILE_BALANCED -> "pp:bal"
            ImuProtocol.PROFILE_PERFORMANCE -> "pp:perf"
            ImuProtocol.PROFILE_DC_SAVE -> "pp:dc-save"
            ImuProtocol.PROFILE_DC_FULL -> "pp:dc-full"
            else -> "pp:$profile"
        }
        return if (awakeSec != null && awakeSec > 0) "$name ${awakeSec}s" else name
    }
}
