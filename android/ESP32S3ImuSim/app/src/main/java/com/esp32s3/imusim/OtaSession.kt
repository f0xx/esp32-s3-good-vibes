package com.esp32s3.imusim

import android.os.Handler
import android.os.Looper
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Single-flight OTA. App update is always first; Zephyr firmware is second.
 * After the user accepts, [locked] stays true until [fail] or [complete]
 * (firmware reboot unlocks on the next STATUS, or the restart watchdog).
 */
object OtaSession {
    enum class Kind { APP, FIRMWARE }

    enum class Phase {
        IDLE,
        CHECKING,
        OFFER,
        DOWNLOADING,
        INSTALLING,
        UPLOADING,
        RESTARTING,
        FAILED,
    }

    data class State(
        val phase: Phase = Phase.IDLE,
        val kind: Kind? = null,
        val caption: String = "",
        val level: StatusBannerLevel = StatusBannerLevel.WARN,
        val percent: Int? = null,
        val locked: Boolean = false,
        val currentApp: String = "",
        val offerApp: String = "",
        val currentFw: String = "",
        val offerFw: String = "",
    )

    private val main = Handler(Looper.getMainLooper())
    val io = Executors.newSingleThreadExecutor { r ->
        Thread(r, "imu-ota").apply { isDaemon = true }
    }

    @Volatile
    var state: State = State()
        private set

    @Volatile
    var listener: ((State) -> Unit)? = null

    private val occupied = AtomicBoolean(false)
    @Volatile
    private var lastProgressMs = 0L
    @Volatile
    private var lastProgressPct = -1
    private val checkWatchdog = Runnable {
        if (state.phase == Phase.CHECKING) {
            fail("OTA check timed out")
        }
    }
    private val restartWatchdog = Runnable {
        if (state.phase == Phase.RESTARTING) {
            complete(
                "Firmware OTA · reconnect timed out — tap Connect (crash drain needs BLE)",
                ok = false,
            )
        }
    }

    fun blocksNewRequest(): Boolean {
        val p = state.phase
        return p != Phase.IDLE && p != Phase.FAILED
    }

    fun locked(): Boolean = state.locked

    fun pauseScene(): Boolean = state.locked

    /** Skip mid-upload pipeline reconnect. After reboot we must reconnect.
     * Slot pre-erase intentionally drops BLE once — allow reconnect then. */
    fun skipPipelineReconnect(): Boolean =
        state.locked && state.kind == Kind.FIRMWARE && state.phase == Phase.UPLOADING &&
            !awaitingSlotEraseReconnect

    @Volatile
    private var awaitingSlotEraseReconnect = false

    fun noteSlotEraseDisconnect() {
        awaitingSlotEraseReconnect = true
        publish(
            state.copy(
                phase = Phase.UPLOADING,
                kind = Kind.FIRMWARE,
                locked = true,
                caption = "Firmware OTA · ESP erasing slot (BLE drops, then resumes)…",
                level = StatusBannerLevel.WARN,
            ),
        )
    }

    fun clearSlotEraseReconnect() {
        awaitingSlotEraseReconnect = false
    }

    fun awaitingSlotEraseReconnect(): Boolean = awaitingSlotEraseReconnect

    fun firmwareRestarting(): Boolean =
        state.kind == Kind.FIRMWARE && state.phase == Phase.RESTARTING

    fun offeredFwVersionCode(): Int = parseFwCode(state.offerFw)

    fun previousFwVersionCode(): Int = parseFwCode(state.currentFw)

    fun beginCheck(currentApp: String, currentFw: String): Boolean {
        if (state.phase != Phase.IDLE && state.phase != Phase.FAILED && state.phase != Phase.CHECKING) {
            return false
        }
        occupied.set(true)
        armCheckWatchdog()
        publish(
            State(
                phase = Phase.CHECKING,
                caption = "Checking OTA… app $currentApp · fw $currentFw",
                level = StatusBannerLevel.WARN,
                locked = false,
                currentApp = currentApp,
                currentFw = currentFw,
            ),
        )
        return true
    }

    fun offerApp(currentApp: String, offerApp: String, currentFw: String): Boolean {
        if (state.phase != Phase.CHECKING && state.phase != Phase.IDLE) return false
        occupied.set(true)
        clearCheckWatchdog()
        publish(
            state.copy(
                phase = Phase.OFFER,
                kind = Kind.APP,
                caption = "App OTA ready · now $currentApp → $offerApp",
                level = StatusBannerLevel.WARN,
                currentApp = currentApp,
                offerApp = offerApp,
                currentFw = currentFw,
            ),
        )
        return true
    }

    fun offerFw(currentApp: String, currentFw: String, offerFw: String): Boolean {
        if (state.phase != Phase.CHECKING && state.phase != Phase.IDLE) return false
        occupied.set(true)
        clearCheckWatchdog()
        publish(
            state.copy(
                phase = Phase.OFFER,
                kind = Kind.FIRMWARE,
                caption = "Firmware OTA ready · now $currentFw → $offerFw",
                level = StatusBannerLevel.WARN,
                currentApp = currentApp,
                currentFw = currentFw,
                offerFw = offerFw,
            ),
        )
        return true
    }

    fun noneAvailable(reason: String) {
        occupied.set(false)
        clearCheckWatchdog()
        clearRestartWatchdog()
        publish(
            state.copy(
                phase = Phase.IDLE,
                kind = null,
                caption = reason,
                level = StatusBannerLevel.OK,
                locked = false,
                percent = null,
            ),
        )
    }

    fun decline() {
        occupied.set(false)
        clearCheckWatchdog()
        clearRestartWatchdog()
        publish(
            state.copy(
                phase = Phase.IDLE,
                kind = null,
                caption = "",
                locked = false,
                percent = null,
            ),
        )
    }

    /** User accepted. Locks the UI until [fail] (success restarts the process). */
    fun accept(): Boolean {
        if (state.phase != Phase.OFFER && state.phase != Phase.IDLE) return false
        val kind = state.kind ?: return false
        occupied.set(true)
        clearCheckWatchdog()
        val label = if (kind == Kind.APP) "App OTA" else "Firmware OTA"
        publish(
            state.copy(
                phase = Phase.DOWNLOADING,
                locked = true,
                caption = "$label · starting…",
                level = StatusBannerLevel.WARN,
                percent = 0,
            ),
        )
        return true
    }

    fun occupyFirmwareUpload(currentFw: String, offerFw: String): Boolean {
        if (blocksNewRequest() && state.phase != Phase.DOWNLOADING) return false
        occupied.set(true)
        clearCheckWatchdog()
        publish(
            state.copy(
                phase = Phase.UPLOADING,
                kind = Kind.FIRMWARE,
                locked = true,
                currentFw = currentFw.ifBlank { state.currentFw },
                offerFw = offerFw.ifBlank { state.offerFw },
                caption = "Firmware OTA · uploading…",
                level = StatusBannerLevel.WARN,
                percent = 0,
            ),
        )
        return true
    }

    fun downloading(kind: Kind, received: Long, total: Long) {
        val pct = if (total > 0) ((received * 100L) / total).toInt().coerceIn(0, 99) else 0
        val now = android.os.SystemClock.uptimeMillis()
        if (pct != 100 && pct == lastProgressPct && now - lastProgressMs < 200L) return
        lastProgressMs = now
        lastProgressPct = pct
        val label = if (kind == Kind.APP) "App OTA" else "Firmware OTA"
        val fromTo = versionArrow(kind)
        publish(
            state.copy(
                phase = Phase.DOWNLOADING,
                kind = kind,
                locked = true,
                percent = pct,
                caption = "$label · downloading $pct% (${fmtMb(received)}/${fmtMb(total)} MB)$fromTo",
                level = StatusBannerLevel.WARN,
            ),
        )
    }

    fun installing() {
        publish(
            state.copy(
                phase = Phase.INSTALLING,
                kind = Kind.APP,
                locked = true,
                percent = 100,
                caption = "App OTA · installing… confirm if Android asks${versionArrow(Kind.APP)}",
                level = StatusBannerLevel.WARN,
            ),
        )
    }

    fun uploading(percent: Int) {
        val pct = percent.coerceIn(0, 100)
        publish(
            state.copy(
                phase = Phase.UPLOADING,
                kind = Kind.FIRMWARE,
                locked = true,
                percent = pct,
                caption = "DFU $pct%",
                level = StatusBannerLevel.WARN,
            ),
        )
    }

    fun restarting(kind: Kind) {
        val label = if (kind == Kind.APP) "App OTA · restarting…" else "Firmware OTA · board restarting…"
        if (kind == Kind.FIRMWARE) {
            armRestartWatchdog()
        }
        publish(
            state.copy(
                phase = Phase.RESTARTING,
                kind = kind,
                locked = true,
                percent = 100,
                caption = label + versionArrow(kind),
                level = StatusBannerLevel.WARN,
            ),
        )
    }

    fun complete(message: String, ok: Boolean = true) {
        occupied.set(false)
        awaitingSlotEraseReconnect = false
        clearCheckWatchdog()
        clearRestartWatchdog()
        publish(
            state.copy(
                phase = Phase.IDLE,
                kind = null,
                locked = false,
                percent = null,
                caption = message,
                level = if (ok) StatusBannerLevel.OK else StatusBannerLevel.WARN,
            ),
        )
    }

    fun fail(message: String) {
        occupied.set(false)
        awaitingSlotEraseReconnect = false
        clearCheckWatchdog()
        clearRestartWatchdog()
        val label = when (state.kind) {
            Kind.APP -> "App OTA failed"
            Kind.FIRMWARE -> "Firmware OTA failed"
            null -> "OTA failed"
        }
        publish(
            state.copy(
                phase = Phase.FAILED,
                locked = false,
                caption = "$label: $message",
                level = StatusBannerLevel.ERROR,
            ),
        )
    }

    fun installedAppLabel(context: android.content.Context): String {
        return try {
            val info = context.packageManager.getPackageInfo(context.packageName, 0)
            val code = OtaCoordinator.installedApkVersionCode(context)
            "${info.versionName ?: "?"} ($code)"
        } catch (_: Exception) {
            "?"
        }
    }

    private fun versionArrow(kind: Kind): String {
        return when (kind) {
            Kind.APP -> {
                val a = state.currentApp
                val b = state.offerApp
                if (a.isNotBlank() && b.isNotBlank()) " · $a → $b" else ""
            }
            Kind.FIRMWARE -> {
                val a = state.currentFw
                val b = state.offerFw
                if (a.isNotBlank() && b.isNotBlank()) " · $a → $b" else ""
            }
        }
    }

    private fun armCheckWatchdog() {
        main.removeCallbacks(checkWatchdog)
        main.postDelayed(checkWatchdog, CHECK_TIMEOUT_MS)
    }

    private fun clearCheckWatchdog() {
        main.removeCallbacks(checkWatchdog)
    }

    private fun armRestartWatchdog() {
        main.removeCallbacks(restartWatchdog)
        main.postDelayed(restartWatchdog, RESTART_TIMEOUT_MS)
    }

    private fun clearRestartWatchdog() {
        main.removeCallbacks(restartWatchdog)
    }

    internal fun parseFwCode(label: String): Int {
        val trimmed = label.trim()
        Regex("""\((\d+)\)\s*$""").find(trimmed)?.groupValues?.get(1)?.toIntOrNull()?.let {
            return it
        }
        return trimmed.toIntOrNull() ?: 0
    }

    private fun fmtMb(bytes: Long): String {
        if (bytes <= 0L) return "0.0"
        return "%.1f".format(bytes / 1_000_000.0)
    }

    private const val CHECK_TIMEOUT_MS = 20_000L
    private const val RESTART_TIMEOUT_MS = 40_000L

    private fun publish(next: State) {
        val apply = Runnable {
            state = next
            listener?.invoke(next)
        }
        if (Looper.myLooper() == Looper.getMainLooper()) apply.run() else main.post(apply)
    }
}
