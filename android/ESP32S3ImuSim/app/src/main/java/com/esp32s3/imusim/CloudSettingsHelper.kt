package com.esp32s3.imusim

import android.content.Context
import java.util.concurrent.ExecutorService

/** Shared cloud save / upload logic for MainActivity and CloudSettingsActivity. */
object CloudSettingsHelper {

    fun helpMessage(context: Context, queuePending: Int, historyCount: Int): String {
        val bridge = BridgeSyncSettings(context)
        return "Recommended URL: ${CloudSettings.DEFAULT_BASE_URL}\n" +
            "LAN forwarder (HTTP only): ${CloudSettings.LEGACY_FORWARDER_URL}\n" +
            "Do not use https:// on port 8090 — causes TLS errors.\n\n" +
            "Easy setup: open the web dashboard on this phone → " +
            "\"Copy phone setup link\" → Import from clipboard here.\n\n" +
            "Upload queue: $queuePending · Local history: $historyCount\n" +
            "(Upload now sends queue first, then backfills from history if queue is empty)\n\n" +
            "Autopilot: enabling Cloud auto-starts ESP-aligned rendezvous unless you pick Manual.\n" +
            "Sync config once (Profile wizard) so phone knows capture/wake windows.\n" +
            "Backend gets vibro verdicts (not raw IMU samples) — ref is auto-captured on bridge.\n\n" +
            bridge.summary(context)
    }

    fun saveFields(
        cloud: CloudSettings,
        bridge: BridgeSyncSettings,
        url: String,
        key: String,
        deviceId: String,
        groupId: String,
        bridgeMode: BridgeSyncSettings.Mode,
        intervalMin: Int,
        dwellSec: Int,
    ) {
        cloud.baseUrl = url
        cloud.apiKey = key
        cloud.deviceId = deviceId
        cloud.groupId = groupId
        cloud.baseUrl = CloudSettings.fixUrlScheme(cloud.baseUrl)
        bridge.mode = bridgeMode
        bridge.intervalMinutes = intervalMin
        bridge.dwellSeconds = dwellSec
    }

    fun applyAsync(
        context: Context,
        uploadNow: Boolean,
        executor: ExecutorService,
        onComplete: (CloudUploader.BatchResult?) -> Unit,
    ) {
        val app = context.applicationContext
        val cloud = CloudSettings(app)
        val bridge = BridgeSyncSettings(app)
        executor.execute {
            cloud.baseUrl = CloudSettings.fixUrlScheme(cloud.baseUrl)
            if (cloud.enabled) {
                AutopilotRelay.onCloudEnabled(app, bridge)
            } else {
                AutopilotRelay.onCloudDisabled(app)
            }
            if (!cloud.enabled) {
                onComplete(null)
                return@execute
            }
            val batch = if (uploadNow) {
                CloudUploader(app).uploadAll()
            } else {
                CloudUploadScheduler.enqueueNow(app)
                null
            }
            onComplete(batch)
        }
    }
}
