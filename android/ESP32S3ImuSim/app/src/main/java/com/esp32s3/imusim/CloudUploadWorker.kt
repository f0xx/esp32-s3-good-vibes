package com.esp32s3.imusim

import android.content.Context
import androidx.work.Constraints
import androidx.work.ExistingPeriodicWorkPolicy
import androidx.work.NetworkType
import androidx.work.OneTimeWorkRequestBuilder
import androidx.work.PeriodicWorkRequestBuilder
import androidx.work.WorkManager
import androidx.work.Worker
import androidx.work.WorkerParameters
import java.util.concurrent.TimeUnit

class CloudUploadWorker(appContext: Context, params: WorkerParameters) :
    Worker(appContext, params) {

    override fun doWork(): Result {
        val uploader = CloudUploader(applicationContext)
        val batch = uploader.uploadAll()
        reportBatch(batch)
        return when {
            batch.totalAccepted > 0 -> Result.success()
            batch.verdicts.message == "cloud disabled" -> Result.success()
            batch.verdicts.ok && batch.spectra.ok && batch.crashes.ok -> Result.success()
            batch.verdicts.ok || batch.spectra.ok || batch.crashes.ok -> Result.success()
            else -> Result.retry()
        }
    }

    private fun reportBatch(batch: CloudUploader.BatchResult) {
        when {
            batch.totalAccepted > 0 ->
                AppEventHub.showBanner(StatusBannerLevel.OK, "Uploaded ${batch.summary}")
            batch.verdicts.ok && batch.spectra.ok && batch.crashes.ok &&
                batch.batteryBench.ok && batch.telemetry.ok -> {
                AppEventHub.showBanner(StatusBannerLevel.OK, "Cloud synced — ${batch.summary}")
            }
            batch.verdicts.message == "cloud disabled" -> return
            batch.verdicts.message.contains("nothing pending", ignoreCase = true) &&
                batch.spectra.message.contains("no spectra", ignoreCase = true) &&
                batch.crashes.message.contains("no crashes", ignoreCase = true) -> {
                val history = CloudUploader(applicationContext).localHistoryCount()
                if (history > 0) {
                    AppEventHub.showBanner(
                        StatusBannerLevel.WARN,
                        "Nothing queued — $history verdicts in local history",
                    )
                }
            }
            else -> {
                val failed = listOf(batch.verdicts, batch.spectra, batch.crashes)
                    .firstOrNull { !it.ok && it.message != "cloud disabled" }
                if (failed != null && failed.message.contains("HTTP", ignoreCase = true)) {
                    AppEventHub.showBanner(StatusBannerLevel.ERROR, "Upload failed: ${failed.message}")
                }
            }
        }
    }
}

object CloudUploadScheduler {
    private const val PERIODIC = "cloud_upload_periodic"
    private const val ONESHOT = "cloud_upload_now"

    fun enqueueNow(context: Context) {
        if (!CloudSettings(context).enabled) return
        val req = OneTimeWorkRequestBuilder<CloudUploadWorker>()
            .setConstraints(networkConstraints())
            .addTag(ONESHOT)
            .build()
        WorkManager.getInstance(context.applicationContext).enqueue(req)
    }

    fun schedulePeriodic(context: Context) {
        if (!CloudSettings(context).enabled) return
        val req = PeriodicWorkRequestBuilder<CloudUploadWorker>(15, TimeUnit.MINUTES)
            .setConstraints(networkConstraints())
            .addTag(PERIODIC)
            .build()
        WorkManager.getInstance(context.applicationContext).enqueueUniquePeriodicWork(
            PERIODIC,
            ExistingPeriodicWorkPolicy.KEEP,
            req,
        )
    }

    fun cancelAll(context: Context) {
        WorkManager.getInstance(context.applicationContext).cancelAllWorkByTag(PERIODIC)
        WorkManager.getInstance(context.applicationContext).cancelAllWorkByTag(ONESHOT)
    }

    private fun networkConstraints(): Constraints =
        Constraints.Builder()
            .setRequiredNetworkType(NetworkType.CONNECTED)
            .build()
}
