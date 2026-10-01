package com.esp32s3.imusim

import android.os.Handler
import android.os.Looper
import android.util.Log
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executor
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.ScheduledThreadPoolExecutor
import java.util.concurrent.ThreadFactory
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Four lanes for [ImuBleForegroundService]:
 *  - [Priority.HIGH] BLE / reconnect / GATT-adjacent (main looper — Android BLE is main-affine)
 *  - [Priority.NORMAL] FSM helpers, crash drain, bench, bridge (background)
 *  - [Priority.UI] AIDL callbacks / banners that must touch [RemoteCallbackList] on main
 *  - [Priority.LOW] HTTP, disk, FFT, periodic polls that can wait (background)
 *
 * A non-null [key] is unique across all lanes. Re-posting the same key drops the queued
 * (not yet running) job and schedules the fresh lambda. Running work is left alone.
 */
class ServiceWorkQueue {
    enum class Priority { HIGH, NORMAL, UI, LOW }

    private val lock = Any()
    private val mainHandler = Handler(Looper.getMainLooper())
    private val normal = newLane("imu-svc-normal")
    private val low = newLane("imu-svc-low")
    private val pending = ConcurrentHashMap<String, Slot>()
    private val dead = AtomicBoolean(false)

    private class Slot(
        val priority: Priority,
        val mainRunnable: Runnable?,
        val future: ScheduledFuture<*>?,
    )

    fun post(
        priority: Priority,
        key: String? = null,
        delayMs: Long = 0L,
        work: () -> Unit,
    ) {
        if (dead.get()) return
        val delay = delayMs.coerceAtLeast(0L)
        val wrapped = Runnable {
            try {
                work()
            } catch (t: Throwable) {
                Log.e(TAG, "task ${key ?: "anon"} failed", t)
            }
        }
        if (key.isNullOrBlank()) {
            enqueue(priority, delay, wrapped, token = null)
            return
        }
        synchronized(lock) {
            dropLocked(key)
            enqueue(priority, delay, wrapped, token = key)
        }
    }

    fun cancel(key: String) {
        synchronized(lock) { dropLocked(key) }
    }

    fun cancelPrefix(prefix: String) {
        synchronized(lock) {
            pending.keys.filter { it.startsWith(prefix) }.forEach { dropLocked(it) }
        }
    }

    fun isQueued(key: String): Boolean = pending.containsKey(key)

    fun executor(priority: Priority, key: String): Executor =
        Executor { r -> post(priority, key) { r.run() } }

    fun shutdown() {
        dead.set(true)
        synchronized(lock) {
            pending.keys.toList().forEach { dropLocked(it) }
        }
        normal.shutdownNow()
        low.shutdownNow()
        mainHandler.removeCallbacksAndMessages(null)
    }

    private fun enqueue(priority: Priority, delayMs: Long, wrapped: Runnable, token: String?) {
        when (priority) {
            Priority.HIGH, Priority.UI -> {
                val r = Runnable {
                    if (token != null) pending.remove(token)
                    wrapped.run()
                }
                if (token != null) {
                    pending[token] = Slot(priority, r, null)
                }
                if (delayMs <= 0L) mainHandler.post(r) else mainHandler.postDelayed(r, delayMs)
            }
            Priority.NORMAL, Priority.LOW -> {
                val pool = if (priority == Priority.NORMAL) normal else low
                val r = Runnable {
                    if (token != null) pending.remove(token)
                    wrapped.run()
                }
                val future = pool.schedule(r, delayMs, TimeUnit.MILLISECONDS)
                if (token != null) {
                    pending[token] = Slot(priority, null, future)
                }
            }
        }
    }

    private fun dropLocked(key: String) {
        val slot = pending.remove(key) ?: return
        slot.mainRunnable?.let { mainHandler.removeCallbacks(it) }
        slot.future?.cancel(false)
    }

    private fun newLane(name: String): ScheduledThreadPoolExecutor {
        val factory = ThreadFactory { r ->
            Thread(r, name).apply { isDaemon = true }
        }
        return ScheduledThreadPoolExecutor(1, factory).apply {
            removeOnCancelPolicy = true
        }
    }

    companion object {
        private const val TAG = "ImuSvcQueue"
    }
}
