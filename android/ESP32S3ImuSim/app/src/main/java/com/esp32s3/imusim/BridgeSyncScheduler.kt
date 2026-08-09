package com.esp32s3.imusim

import android.content.Context
import android.content.Intent
import androidx.work.Constraints
import androidx.work.ExistingWorkPolicy
import androidx.work.NetworkType
import androidx.work.OneTimeWorkRequestBuilder
import androidx.work.WorkManager
import androidx.work.Worker
import androidx.work.WorkerParameters
import java.util.concurrent.TimeUnit

/** Periodic BLE bridge: connect → drain verdicts/crashes → cloud upload → disconnect. */
class BridgeSyncWorker(appContext: Context, params: WorkerParameters) :
    Worker(appContext, params) {

    override fun doWork(): Result {
        val settings = BridgeSyncSettings(applicationContext)
        if (!settings.scheduled) {
            return Result.success()
        }
        AutopilotRelay.startAutopilotService(applicationContext)
        val intent = Intent(applicationContext, ImuBleForegroundService::class.java).apply {
            action = ImuBleForegroundService.ACTION_BRIDGE_SYNC
        }
        applicationContext.startForegroundService(intent)
        BridgeSyncScheduler.scheduleNext(applicationContext)
        return Result.success()
    }
}

object BridgeSyncScheduler {
    private const val UNIQUE = "bridge_sync_chain"
    const val FIRST_SYNC_DELAY_MS = 30_000L

    fun scheduleNext(context: Context, firstSyncDelayMs: Long? = null) {
        val app = context.applicationContext
        val settings = BridgeSyncSettings(app)
        if (!settings.scheduled) {
            cancel(app)
            return
        }
        val delayMs = firstSyncDelayMs ?: settings.intervalMs(app).coerceAtLeast(TimeUnit.MINUTES.toMillis(1))
        val req = OneTimeWorkRequestBuilder<BridgeSyncWorker>()
            .setInitialDelay(delayMs, TimeUnit.MILLISECONDS)
            .setConstraints(
                Constraints.Builder()
                    .setRequiredNetworkType(NetworkType.NOT_REQUIRED)
                    .build(),
            )
            .addTag(UNIQUE)
            .build()
        WorkManager.getInstance(app).enqueueUniqueWork(
            UNIQUE,
            ExistingWorkPolicy.REPLACE,
            req,
        )
    }

    fun cancel(context: Context) {
        WorkManager.getInstance(context.applicationContext).cancelUniqueWork(UNIQUE)
    }

    fun reschedule(context: Context, firstSyncDelayMs: Long? = null) {
        cancel(context)
        scheduleNext(context, firstSyncDelayMs)
    }

    fun triggerNow(context: Context) {
        AutopilotRelay.startAutopilotService(context)
        val intent = Intent(context, ImuBleForegroundService::class.java).apply {
            action = ImuBleForegroundService.ACTION_BRIDGE_SYNC
        }
        context.startForegroundService(intent)
    }
}
