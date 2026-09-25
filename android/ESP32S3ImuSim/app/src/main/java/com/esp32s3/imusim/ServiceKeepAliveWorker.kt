package com.esp32s3.imusim

import android.content.Context
import android.util.Log
import androidx.work.CoroutineWorker
import androidx.work.ExistingPeriodicWorkPolicy
import androidx.work.PeriodicWorkRequestBuilder
import androidx.work.WorkManager
import androidx.work.WorkerParameters
import java.util.concurrent.TimeUnit

/**
 * Ensures [ImuBleForegroundService] stays alive in the background.
 * Triggered periodically by WorkManager.
 */
class ServiceKeepAliveWorker(
    context: Context,
    params: WorkerParameters
) : CoroutineWorker(context, params) {

    override suspend fun doWork(): Result {
        val app = applicationContext
        Log.i(TAG, "ServiceKeepAlive: re-arm always-on relay")
        AutopilotRelay.bootstrap(app)

        return Result.success()
    }

    companion object {
        private const val TAG = "ServiceKeepAlive"
        private const val WORK_NAME = "service_keep_alive"

        fun schedule(context: Context) {
            val request = PeriodicWorkRequestBuilder<ServiceKeepAliveWorker>(
                15, TimeUnit.MINUTES, // Minimum interval allowed by WorkManager
                5, TimeUnit.MINUTES   // Flex interval
            ).build()

            WorkManager.getInstance(context).enqueueUniquePeriodicWork(
                WORK_NAME,
                ExistingPeriodicWorkPolicy.KEEP,
                request
            )
            Log.i(TAG, "Scheduled periodic keep-alive worker")
        }
    }
}
