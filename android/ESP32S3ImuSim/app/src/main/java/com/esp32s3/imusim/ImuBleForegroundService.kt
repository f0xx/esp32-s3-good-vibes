package com.esp32s3.imusim

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.RemoteCallbackList
import android.os.SystemClock
import android.util.Log
import androidx.core.app.NotificationCompat
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Long-lived BLE session — survives activity rotation and app backgrounding.
 * UI binds via AIDL and registers callbacks for live batches.
 */
class ImuBleForegroundService : Service(), BleImuClient.Listener {

    companion object {
        private const val TAG = "ImuBleService"
        const val ACTION_BRIDGE_SYNC = "com.esp32s3.imusim.BRIDGE_SYNC"
        const val ACTION_AUTOPILOT = "com.esp32s3.imusim.AUTOPILOT"
        const val ACTION_CONNECT_RELAY = "com.esp32s3.imusim.CONNECT_RELAY"
        const val ACTION_BLE_RELAY = "com.esp32s3.imusim.BLE_RELAY"
        const val ACTION_STOP_AUTOPILOT = "com.esp32s3.imusim.STOP_AUTOPILOT"
        const val ACTION_STOP_CONNECT_RELAY = "com.esp32s3.imusim.STOP_CONNECT_RELAY"
        const val CHANNEL_ID = "imu_ble"
        const val NOTIFICATION_ID = 1
        private val CRASH_RELAY_DELAYS_MS = longArrayOf(3000L, 8000L, 18000L)
        private const val CONNECT_RETRY_PAUSE_MS = 900_000L
        private const val CONNECT_RETRY_PAUSE_MAX_MS = 1_800_000L
        private const val BT_WARMUP_MS = 8_000L
        private const val RELAY_PAUSE_MS = 15_000L
        private const val UI_RELAY_PAUSE_MS = 4_000L
        private const val MANUAL_CONNECT_RETRY_MS = 8_000L
        private const val CONNECT_FAILURE_COOLDOWN_THRESHOLD = 2
        private const val CONNECT_FAILURE_COOLDOWN_MS = 3_600_000L
        private const val CRASH_RELAY_RETRY_MS = 3_000L
        private const val CRASH_RELAY_MAX_ROUNDS = 12
        private const val TELEMETRY_UI_MS = 500L
        private const val NOTIFICATION_MIN_MS = 5000L
        private const val FFT_MIN_SAMPLES = 32
        private const val FFT_COLLECT_TIMEOUT_MS = 30_000L
        private const val FFT_COLLECT_TICK_MS = 50L
    }

    private val callbacks = RemoteCallbackList<IImuBleCallback>()
    private val callbacksBroadcastActive = AtomicBoolean(false)
    private val mainHandler = Handler(Looper.getMainLooper())
    private val ioExecutor = Executors.newSingleThreadExecutor()

    private lateinit var bleClient: BleImuClient
    private lateinit var session: ImuSessionStore
    private lateinit var verdictStore: VerdictStore
    private lateinit var offloadExporter: OffloadExporter

    private var connected = false
    private var caps = 0
    private var lastPower: ImuProtocol.PowerStatus? = null
    private var lastBatchJson: String? = null
    private var lastNotificationUpdateMs = 0L
    private var lastTelemetryUiMs = 0L
    private var lastStoredVerdictSeq = -1L
    private var lastDeviceStatus: ImuProtocol.Status? = null
    private var lastEspScreenOn: Boolean? = null
    private var pendingTelemetry: String? = null

    /** True only while extra (verdict/config) sync work runs during an established relay session. */
    private var bridgeSyncActive = false
    /** Requested by scheduler/manual trigger; serviced on the *next* CONNECTED state of the
     *  single always-on relay FSM — bridge sync never opens its own BLE connection. */
    private var pendingBridgeWork = false
    private var connectRelayActive = false
    private var bleRelayActive = false
    private var connectAttemptSeq = 0
    private var connectFailureStreak = 0
    private var userConnectedSession = false
    /** True while a foreground Activity is bound and visible. Drives whether the always-on
     *  relay FSM connects in minimal (no-notify) mode or full mode, and whether an existing
     *  minimal background session gets upgraded so the UI (e.g. the cube+axis scene) has data. */
    private var uiVisible = false
    /** True if userConnectedSession was flipped on by onUiVisibleChanged()'s auto-upgrade rather
     *  than an explicit manual Connect tap — reverted (without forcing a disconnect) once the UI
     *  goes back to the background so future reconnects resume power-saving minimal-relay mode. */
    private var autoPromotedFullSession = false
    private var autopilotActive = false
    private var relayFsmState = RelayFsmState.STARTING
    private var relayFsmCaption = "Starting…"
    private var relayFsmStarted = false
    private var btWarmupDone = false
    private var reconnectDueAtMs = 0L
    private var bleConnectGeneration = 0
    private var captionEpoch = 0
    private var lastTimeSyncRetryMs = 0L
    private var autoRefInProgress = false
    private var savedPollMsForBridge = 0
    private var clockCheckedThisSession = false
    private val crashRelayRunnable = Runnable { relayPendingCrash() }
    private val bridgeFinishRunnable = Runnable { completeBridgeSyncCycle() }
    private val connectRetryRunnable = Runnable { attemptAutoConnect() }
    private val fsmWarmupRunnable = Runnable { onFsmWarmupComplete() }
    private val fsmPauseRunnable = Runnable { onFsmPauseComplete() }
    private val reconnectWatchdogRunnable = Runnable { onReconnectWatchdog() }
    private val internalBridgeRunnable = Runnable {
        if (!autopilotActive || bridgeSyncActive) {
            return@Runnable
        }
        val settings = BridgeSyncSettings(this@ImuBleForegroundService)
        if (!settings.scheduled) {
            return@Runnable
        }
        startBridgeSyncCycle()
    }

    private val flushTelemetryRunnable = Runnable { flushPendingTelemetry() }

    private val aidl = object : IImuBleService.Stub() {
        override fun registerCallback(callback: IImuBleCallback?) {
            if (callback != null) {
                callbacks.register(callback)
                mainHandler.post {
                    if (!bleRelayActive && !connected) {
                        startBleRelayMode()
                    }
                    pushSessionRestoreToCallback(callback)
                }
            }
        }

        override fun unregisterCallback(callback: IImuBleCallback?) {
            if (callback != null) {
                callbacks.unregister(callback)
            }
        }

        override fun requestState() {
            mainHandler.post {
                broadcastRelayState()
                pushSessionRestoreToAll()
            }
        }

        override fun setUiVisible(active: Boolean) {
            mainHandler.post { onUiVisibleChanged(active) }
        }

        override fun connect() {
            userConnectedSession = true
            autoPromotedFullSession = false
            requestBleConnect(
                fullSession = true,
                reason = "Manual connect — scanning…",
            )
        }

        override fun disconnect() {
            userConnectedSession = false
            autoPromotedFullSession = false
            bridgeSyncActive = false
            cancelFsmTimers()
            mainHandler.post {
                bleClient.disconnect()
                enterRelayState(RelayFsmState.PAUSE, "Disconnected — pause ${RELAY_PAUSE_MS / 1000}s")
                scheduleFsmPauseThenConnect("Disconnected — pause ${RELAY_PAUSE_MS / 1000}s")
                stopForegroundIfIdle()
            }
        }

        override fun setMode(mode: Int) {
            session.renderMode = mode
            rawSampling.onMode(mode)
            mainHandler.post { bleClient.setMode(mode) }
        }

        override fun setPollIntervalMs(ms: Int) {
            session.pollMs = ms
            mainHandler.post { bleClient.setPollIntervalMs(ms) }
        }

        override fun requestConfigSync() {
            ioExecutor.execute {
                bleClient.syncConfigFromDevice { blob ->
                    if (blob != null) {
                        session.saveLocalConfig(blob)
                        broadcastConfig(blob)
                        val doc = DeviceConfigJson.fromBlob(blob, "esp")
                        ConfigCloudSync.upload(applicationContext, doc)
                    } else {
                        broadcastBanner(StatusBannerLevel.ERROR, "Config read failed")
                    }
                }
            }
        }

        override fun pushConfig(blob: ByteArray?, commit: Boolean) {
            if (blob == null) return
            ioExecutor.execute {
                bleClient.pushConfigToDevice(blob, commit) { ok ->
                    broadcastBanner(
                        if (ok) StatusBannerLevel.OK else StatusBannerLevel.ERROR,
                        if (ok) "Config pushed" else "Config push failed",
                    )
                }
            }
        }

        override fun uploadFirmware(firmware: ByteArray?) {
            if (firmware == null || firmware.isEmpty()) {
                broadcastOtaDone(false, "empty firmware")
                return
            }
            mainHandler.post {
                bleClient.uploadFirmware(
                    firmware,
                    onProgress = { pct -> broadcastOtaProgress(pct) },
                    onDone = { ok, msg -> broadcastOtaDone(ok, msg) },
                )
            }
        }

        override fun requestNetScan() {
            mainHandler.post { bleClient.requestNetScan() }
        }

        override fun requestNetProfiles() {
            mainHandler.post { bleClient.requestNetProfiles() }
        }

        override fun sendNetCommand(json: String?) {
            if (json == null) return
            mainHandler.post { bleClient.sendNetCommand(json) }
        }

        override fun vibroRefStart(slot: Int, name: String?) {
            mainHandler.post {
                rawSampling.onRefRecording(true)
                bleClient.vibroRefStart(slot, name ?: "") { ok ->
                    broadcastBanner(
                        if (ok) StatusBannerLevel.OK else StatusBannerLevel.ERROR,
                        if (ok) {
                            "Recording slot $slot — shake the device (up to 30s)"
                        } else {
                            "Ref start failed (BLE busy?)"
                        },
                    )
                }
            }
        }

        override fun vibroRefStop() {
            mainHandler.post {
                rawSampling.onRefRecording(false)
                bleClient.vibroRefStop { ok ->
                    broadcastBanner(
                        if (ok) StatusBannerLevel.OK else StatusBannerLevel.ERROR,
                        if (ok) "Reference saved" else "Ref stop failed",
                    )
                    if (ok) {
                        bleClient.readVibroRefList { json ->
                            if (json != null) {
                                foreachCallback { it.onVibroRefList(json) }
                            }
                        }
                    }
                }
            }
        }

        override fun vibroRefSelect(slot: Int) {
            mainHandler.post {
                bleClient.vibroRefSelect(slot) { ok ->
                    broadcastBanner(
                        if (ok) StatusBannerLevel.OK else StatusBannerLevel.ERROR,
                        if (ok) "Reference slot $slot active" else "Select failed",
                    )
                    if (ok) {
                        bleClient.readVibroRefList { json ->
                            if (json != null) {
                                foreachCallback { it.onVibroRefList(json) }
                            }
                        }
                    }
                }
            }
        }

        override fun vibroRefDelete(slot: Int) {
            mainHandler.post {
                bleClient.vibroRefDelete(slot) { ok ->
                    broadcastBanner(
                        if (ok) StatusBannerLevel.OK else StatusBannerLevel.ERROR,
                        if (ok) "Reference slot $slot deleted" else "Delete failed",
                    )
                    if (ok) {
                        bleClient.readVibroRefList { json ->
                            if (json != null) {
                                foreachCallback { it.onVibroRefList(json) }
                            }
                        }
                    }
                }
            }
        }

        override fun requestVibroRefList() {
            mainHandler.post {
                bleClient.readVibroRefList { json ->
                    if (json != null) {
                        foreachCallback { it.onVibroRefList(json) }
                    } else {
                        broadcastBanner(StatusBannerLevel.ERROR, "Ref list read failed")
                    }
                }
            }
        }

        override fun analyzeSpectrum() {
            mainHandler.post { startSpectrumAnalysis() }
        }

        override fun setEspScreenOn(on: Boolean) {
            mainHandler.post { bleClient.setEspScreenOn(on) }
        }

        override fun toggleEspScreen() {
            mainHandler.post {
                val current = lastDeviceStatus?.screenOn ?: true
                bleClient.setEspScreenOn(!current)
            }
        }

        override fun setCpuMhzOverride(mhz: Int) {
            mainHandler.post { bleClient.setCpuMhzOverride(mhz) }
        }

        override fun setImuHzOverride(hz: Int) {
            mainHandler.post { bleClient.setImuHzOverride(hz) }
        }

        override fun injectCrash(kind: String?) {
            mainHandler.post {
                val k = kind ?: "panic"
                bleClient.injectCrash(k) { ok ->
                    broadcastBanner(
                        if (ok) StatusBannerLevel.WARN else StatusBannerLevel.ERROR,
                        if (ok) {
                            "Crash inject ($k) queued — ESP rebooting; relay uploads on next connect"
                        } else {
                            "Crash inject failed — connect BLE and use debug firmware (CRASH_DEBUG=1)"
                        },
                    )
                }
            }
        }

        override fun eraseDeviceNvs() {
            mainHandler.post {
                bleClient.eraseDeviceNvs { ok ->
                    if (!ok) {
                        broadcastBanner(StatusBannerLevel.ERROR, "NVS erase failed — connect + v49 firmware")
                    }
                }
            }
        }

        override fun runDeviceBist() {
            mainHandler.post {
                bleClient.runDeviceBist { ok ->
                    broadcastBanner(
                        if (ok) StatusBannerLevel.OK else StatusBannerLevel.WARN,
                        if (ok) "BIST command sent — check STATUS bist field" else "BIST failed (debug firmware?)",
                    )
                }
            }
        }
    }

    private val vibroBuffer = VibroSampleBuffer()
    private val rawSampling = RawSamplingSession()
    private lateinit var cloudUploader: CloudUploader
    private var spectrumSeq = 1L
    private var fftCollectAttempt = 0
    private var fftSavedPollMs = 0
    private var fftCollectStartedMs = 0L
    private val fftCollectRunnable = Runnable { continueFftCollection() }

    private fun startSpectrumAnalysis() {
        mainHandler.removeCallbacks(fftCollectRunnable)
        fftCollectAttempt = 0
        fftCollectStartedMs = SystemClock.uptimeMillis()
        fftSavedPollMs = if (bleClient.pollIntervalMs() > ImuProtocol.MIN_POLL_MS) {
            bleClient.pollIntervalMs()
        } else {
            0
        }
        bleClient.setPollIntervalMs(ImuProtocol.MIN_POLL_MS)
        session.renderMode = ImuProtocol.MODE_RAW
        rawSampling.onMode(ImuProtocol.MODE_RAW)
        bleClient.setMode(ImuProtocol.MODE_RAW)
        broadcastStatus("FFT: RAW mode @ ${ImuProtocol.MIN_POLL_MS}ms poll — shake ESP…", important = true)
        mainHandler.postDelayed({ continueFftCollection() }, 250)
    }

    private fun finishFftCapture(restorePoll: Boolean) {
        if (restorePoll && fftSavedPollMs > 0) {
            bleClient.setPollIntervalMs(fftSavedPollMs)
            fftSavedPollMs = 0
        }
    }

    private fun continueFftCollection() {
        val samples = vibroBuffer.snapshot()
        val need = FFT_MIN_SAMPLES
        if (samples.size >= need) {
            ioExecutor.execute { runSpectrumAnalysis() }
            return
        }
        if (fftCollectAttempt == 0 || fftCollectAttempt % 20 == 0) {
            broadcastStatus("FFT: collecting RAW samples (${samples.size}/$need)…", important = false)
        }
        val elapsedMs = SystemClock.uptimeMillis() - fftCollectStartedMs
        if (elapsedMs >= FFT_COLLECT_TIMEOUT_MS) {
            finishFftCapture(restorePoll = true)
            val reason = fftCollectFailureReason(samples.size, need)
            broadcastBanner(StatusBannerLevel.WARN, reason)
            return
        }
        fftCollectAttempt++
        bleClient.requestDataPoll()
        mainHandler.postDelayed(fftCollectRunnable, FFT_COLLECT_TICK_MS)
    }

    private fun fftCollectFailureReason(have: Int, need: Int): String {
        val batchMode = runCatching {
            lastBatchJson?.let { ImuProtocol.parseBatch(it).mode }
        }.getOrNull()
        return when {
            batchMode != null && batchMode != ImuProtocol.MODE_RAW ->
                "FFT needs Raw IMU mode (ESP still in mode $batchMode) — retry FFT"
            lastDeviceStatus?.captureActive == false ->
                "Capture window closed on ESP — wait for next vibro window ($have/$need)"
            have == 0 ->
                "No IMU samples — check ESP IMU is live (connected in Raw mode?) ($have/$need)"
            else ->
                "Need more samples — shake ESP steadily ($have/$need)"
        }
    }

    private fun runSpectrumAnalysis() {
        val samples = vibroBuffer.snapshot()
        if (samples.size < FFT_MIN_SAMPLES) {
            finishFftCapture(restorePoll = true)
            broadcastBanner(StatusBannerLevel.WARN, "Need more samples — shake the device")
            return
        }
        val fft = VibroFft.magnitudeSpectrum(samples, vibroBuffer.effectiveSampleHz()) ?: run {
            finishFftCapture(restorePoll = true)
            broadcastBanner(StatusBannerLevel.ERROR, "FFT failed")
            return
        }
        val seq = spectrumSeq++
        val bins = fft.magnitudes.drop(1).take(128).map { it }
        offloadExporter.exportSpectrum(seq, fft.sampleHz, fft.binHz, bins, fft.peakHz, fft.peakMag)
        val upload = cloudUploader.uploadPendingSpectra(5)
        val msg = String.format(
            java.util.Locale.US,
            "FFT peak %.1f Hz @ %.4fg",
            fft.peakHz,
            fft.peakMag,
        )
        broadcastVibroCaption(msg)
        broadcastStatus(msg, important = true)
        when {
            upload.ok && upload.accepted > 0 -> broadcastBanner(StatusBannerLevel.OK, "OK!")
            !CloudSettings(applicationContext).enabled ->
                broadcastBanner(StatusBannerLevel.WARN, "Cloud off — spectrum saved on phone")
            else -> broadcastBanner(StatusBannerLevel.ERROR, "Failed to upload batch: ${upload.message}")
        }
        finishFftCapture(restorePoll = true)
    }

    override fun onCreate() {
        super.onCreate()
        session = ImuSessionStore(this)
        verdictStore = VerdictStore(this)
        offloadExporter = OffloadExporter(this)
        cloudUploader = CloudUploader(this)
        bleClient = BleImuClient(applicationContext, this)
        createNotificationChannel()
    }

    override fun onBind(intent: Intent?): IBinder = aidl

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_BRIDGE_SYNC -> startBridgeSyncCycle()
            ACTION_AUTOPILOT -> startAutopilotMode()
            ACTION_CONNECT_RELAY -> startConnectRelayMode()
            ACTION_BLE_RELAY -> startBleRelayMode()
            ACTION_STOP_AUTOPILOT -> stopAutopilotMode()
            ACTION_STOP_CONNECT_RELAY -> stopConnectRelayMode()
            else -> if (!connected && !autopilotActive && !bleRelayActive) {
                startForeground(NOTIFICATION_ID, buildNotification())
                stopForegroundIfIdle()
            }
        }
        return START_STICKY
    }

    override fun onDestroy() {
        bleClient.disconnect()
        ioExecutor.shutdownNow()
        callbacks.kill()
        super.onDestroy()
    }

    // --- BleImuClient.Listener ---

    override fun onStatus(text: String) {
        session.lastStatus = text
        if (connected && userConnectedSession && isLiveSessionNoise(text)) {
            return
        }
        broadcastStatus(text, important = false)
    }

    /** Routine BLE telemetry that must not overwrite the status line during live IMU viewing. */
    private fun isLiveSessionNoise(text: String): Boolean {
        val lower = text.lowercase()
        return lower.startsWith("time handshake") ||
            lower.startsWith("polling every") ||
            lower.contains("esp clock corrected") ||
            lower.startsWith("clock sync ok") ||
            lower.contains("clock drift")
    }

    override fun onBanner(level: StatusBannerLevel, text: String) {
        session.lastStatus = text
        broadcastBanner(level, text)
        broadcastStatus(text, important = true)
        updateNotification(force = true)
    }

    override fun onPollStats(seq: Long, recordCount: Int, pollMs: Int) {
        throttleTelemetry("seq=$seq n=$recordCount poll=${pollMs}ms")
    }

    private var lastClockSyncedUi: Boolean? = null
    private var lastReportedClockCorrMs = -1L
    private var lastVibroCaption: String? = null

    override fun onDeviceStatus(status: ImuProtocol.Status) {
        lastDeviceStatus = status
        val synced = status.clockSynced == true
        if (lastClockSyncedUi != synced) {
            lastClockSyncedUi = synced
            val tz = status.clockTzMin ?: 0
            foreachCallback { it.onClockState(synced, tz) }
        }
        if (connected && status.clockSynced != true) {
            val now = SystemClock.uptimeMillis()
            if (now - lastTimeSyncRetryMs > 12_000L) {
                lastTimeSyncRetryMs = now
                mainHandler.post { bleClient.requestTimeSyncRetry() }
            }
        } else if (synced) {
            // Already synced (common case — the RTC survives BLE disconnects) — cancel the
            // blind post-connect retry chain instead of letting it burn through all 8 attempts
            // over 40s of pointless TIME writes on every single reconnect.
            bleClient.stopTimeSyncRetries()
        }
        status.screenOn?.let { on ->
            if (lastEspScreenOn != on) {
                lastEspScreenOn = on
                broadcastEspScreen(on)
            }
        }
        rawSampling.onStatus(status)
        WakeRelay.onStatus(bleClient, status)
        reportClockSyncStatus(status)
        if (status.vibroVerdictLevel != null && status.seq != lastStoredVerdictSeq) {
            lastStoredVerdictSeq = status.seq
            ioExecutor.execute {
                verdictStore.record(status)
                offloadExporter.exportVerdict(status)
                CloudUploadScheduler.enqueueNow(applicationContext)
                mainHandler.post { scheduleFlushOffloadAcks(0) }
            }
        } else if (
            (status.offloadPending ?: 0) > 0 &&
            status.vibroVerdictLevel != null &&
            offloadExporter.lineCount() == 0
        ) {
            ioExecutor.execute {
                offloadExporter.exportVerdict(status)
                CloudUploadScheduler.enqueueNow(applicationContext)
                mainHandler.post { scheduleFlushOffloadAcks(0) }
            }
        } else if ((status.offloadPending ?: 0) > 0) {
            status.pendingSessionSeq?.takeIf { it > 0L }?.let { ps ->
                ioExecutor.execute {
                    if (status.vibroRmsG != null) {
                        verdictStore.recordOfflineSession(status, ps)
                        offloadExporter.exportVerdict(status.copy(seq = ps))
                    }
                    mainHandler.post { scheduleFlushOffloadAcks(0) }
                }
            }
        }
        val extras = buildList {
            WakeRelay.profileCaption(status.powerProfile, status.awakeSecondsRemaining)?.let { add(it) }
            if (status.captureActive == false) add("cap=idle")
            status.chipTempC?.let { add(String.format(java.util.Locale.US, "tc=%.0fC", it)) }
            status.vibroRmsG?.let { add(String.format(java.util.Locale.US, "vrms=%.3fg", it)) }
            if (status.vibroRefReady) add("ref")
            status.vibroVerdictLevel?.let { level ->
                add(ImuProtocol.verdictCaption(level, status.vibroCorr))
            }
            if ((status.offloadPending ?: 0) > 0) {
                add("offload pending")
            }
        }
        if (extras.isNotEmpty()) {
            throttleTelemetry(extras.joinToString(" "))
        }
        broadcastVibroCaptionIfChanged(formatVibroCaption(status))
    }

    private fun broadcastVibroCaptionIfChanged(caption: String) {
        if (caption == lastVibroCaption) return
        lastVibroCaption = caption
        broadcastVibroCaption(caption)
    }

    private fun formatVibroCaption(status: ImuProtocol.Status): String {
        val parts = mutableListOf<String>()
        VibroDiagnosisMode.fromTier(status.vibroTier)?.let { parts.add(it.label) }
        status.captureActive?.let { active ->
            parts.add(if (active) "capture on" else "capture idle")
        }
        status.vibroRmsG?.let { parts.add(String.format(java.util.Locale.US, "vrms=%.3fg", it)) }
        status.edgeCrest?.let { parts.add(String.format(java.util.Locale.US, "cr=%.2f", it)) }
        status.edgeZcrHz?.let { parts.add(String.format(java.util.Locale.US, "zcr=%.1fHz", it)) }
        status.bandRms?.let { b ->
            if (b.isNotEmpty()) {
                val peak = b.maxOrNull() ?: 0f
                parts.add(String.format(java.util.Locale.US, "b=%.3f", peak))
            }
        }
        status.bandCorr?.let { parts.add(String.format(java.util.Locale.US, "bc=%.2f", it)) }
        status.captureMixWindowSec?.let { mix ->
            if (mix > 0) parts.add("mix=${mix}s")
        }
        status.pendingSessionSeq?.let { ps ->
            if (ps > 0L) parts.add("sess=$ps")
        }
        if (status.vibroRefReady) parts.add("ref")
        status.resetReason?.let { parts.add("rr=$it") }
        status.bistStatus?.let { parts.add("bist=$it") }
        if (status.crashDebugEnabled) parts.add("dbg")
        status.vibroVerdictLevel?.let { lv ->
            parts.add(ImuProtocol.verdictCaption(lv, status.vibroCorr))
        }
        return if (parts.isEmpty()) "Vibro: collecting…" else parts.joinToString(" · ")
    }

    override fun onConnected(connected: Boolean) {
        val wasConnected = this.connected
        this.connected = connected
        if (connected) {
            connectFailureStreak = 0
            captionEpoch++
            mainHandler.removeCallbacks(reconnectWatchdogRunnable)
            cancelFsmTimers()
            cancelPendingStatusUpdates()
            clockCheckedThisSession = false
            lastReportedClockCorrMs = -1L
            lastVibroCaption = null
            bleClient.connectedDeviceAddress()?.let { session.lastBleAddress = it }
            if (userConnectedSession) {
                enterRelayState(RelayFsmState.CONNECTED, "Connected — live IMU")
            } else if (bleRelayActive) {
                // Caption set once here; the branch below (TIME sync -> crash drain) reuses it
                // instead of overwriting it a second time with the same "connected" transition.
                enterRelayState(RelayFsmState.CONNECTED, "Connected — TIME sent, fetching crashes…")
            } else {
                enterRelayState(RelayFsmState.CONNECTED, "Connected — fetching ESP data…")
            }
            startForeground(NOTIFICATION_ID, buildNotification())
            val pendingVerdicts = offloadExporter.lineCount()
            if (pendingVerdicts > 0 && !CloudSettings(applicationContext).enabled) {
                broadcastBanner(
                    StatusBannerLevel.WARN,
                    "Cloud off — $pendingVerdicts verdicts queued (Cloud → API key)",
                )
            }
            val priorStatus = lastDeviceStatus ?: session.lastStatus?.let {
                runCatching { ImuProtocol.parseStatus(it) }.getOrNull()
            }
            val relayOnly = bleRelayActive && !userConnectedSession
            if (!relayOnly) {
                mainHandler.postDelayed({
                    if (!this@ImuBleForegroundService.connected) return@postDelayed
                    WakeRelay.onConnect(bleClient, priorStatus)
                }, ImuProtocol.ESP_CONNECT_SETTLE_MS)
            }
            scheduleFlushOffloadAcks(ImuProtocol.ESP_CONNECT_SETTLE_MS)
            if (bleRelayActive && !userConnectedSession) {
                // Single connect authority: after ESP grace — crash drain then bridge/cloud.
                Log.i(TAG, "Relay connected — crash drain after ${ImuProtocol.ESP_CONNECT_SETTLE_MS}ms settle")
                mainHandler.postDelayed({
                    if (!this@ImuBleForegroundService.connected) return@postDelayed
                    relayCrashesUntilConfirmed {
                        if (pendingBridgeWork && this@ImuBleForegroundService.connected) {
                            beginBridgeWorkThenFinish()
                        } else {
                            finishBleRelaySession("crash relay done")
                        }
                    }
                }, ImuProtocol.ESP_CONNECT_SETTLE_MS)
            } else {
                scheduleCrashRelayAfterSettle()
            }
        } else {
            stopForegroundIfIdle()
            mainHandler.removeCallbacks(flushTelemetryRunnable)
            mainHandler.removeCallbacks(crashRelayRunnable)
            mainHandler.removeCallbacks(bridgeFinishRunnable)
            flushOffloadAcksRunnable?.let { mainHandler.removeCallbacks(it) }
            flushOffloadAcksRunnable = null
            offloadAckInFlight = false
            lastLocallyAckedSeq = 0L
            pendingTelemetry = null
            if (wasConnected) {
                // An unexpected mid-session drop during bridge work (e.g. supervision timeout)
                // never reaches finishBridgeSyncCycle() — clear it here too. pendingBridgeWork is
                // intentionally kept so the still-pending sync is retried on the next connect.
                if (bridgeSyncActive) {
                    Log.w(TAG, "Bridge work dropped mid-session — will retry on next connect")
                    bridgeSyncActive = false
                    mainHandler.removeCallbacks(bridgeFinishRunnable)
                }
                if (userConnectedSession) {
                    scheduleReconnectPause("Link lost — retry in ${MANUAL_CONNECT_RETRY_MS / 1000}s")
                } else if (relayFsmActive()) {
                    scheduleReconnectPause("Link lost — retry in ${RELAY_PAUSE_MS / 1000}s")
                }
            }
        }
        broadcastConnection(connected)
        broadcastRelayState()
        updateNotification(force = true)
    }

    override fun onConnectFailed(reason: String) {
        connectFailureStreak++
        if (userConnectedSession) {
            scheduleReconnectPause("Connect failed — retry in ${MANUAL_CONNECT_RETRY_MS / 1000}s ($reason)")
            broadcastBanner(StatusBannerLevel.WARN, reason)
            return
        }
        if (relayFsmActive()) {
            scheduleReconnectPause("Connect failed — retry in ${RELAY_PAUSE_MS / 1000}s ($reason)")
        } else {
            broadcastBanner(StatusBannerLevel.WARN, reason)
        }
    }

    override fun onPowerStatus(power: ImuProtocol.PowerStatus) {
        lastPower = power
        broadcastPower(power)
    }

    override fun onBatch(batch: ImuProtocol.Batch) {
        // Batches forwarded via onBatchJson.
    }

    override fun onBatchJson(json: String) {
        lastBatchJson = json
        runCatching {
            val batch = ImuProtocol.parseBatch(json)
            if (batch.mode == ImuProtocol.MODE_RAW && batch.raw.isNotEmpty()) {
                vibroBuffer.ingestRawBatch(batch)
                rawSampling.onBatch(batch, vibroBuffer)?.let { hint ->
                    broadcastBanner(hint.level, hint.message)
                }
            }
        }
        broadcastBatch(json)
    }

    override fun onCaps(caps: Int) {
        this.caps = caps
    }

    override fun onNetScan(json: String) {
        session.lastNetScanJson = json
        broadcastNetScan(json)
    }

    override fun onNetProfiles(json: String) {
        session.lastNetProfilesJson = json
        broadcastNetProfiles(json)
    }

    override fun onNetStatus(json: String) {
        session.lastNetStatusJson = json
        broadcastNetStatus(json)
    }

    private var offloadAckInFlight = false
    private var flushOffloadAcksRunnable: Runnable? = null

    /** Locally-known "high water mark" of the seq we've successfully ACKed. STATUS is only
     *  polled every 5s, so `lastDeviceStatus.offloadAckSeq` lags well behind an ACK we just sent —
     *  without this, the self-reschedule below kept re-computing the *same* seqToAck against the
     *  stale status and firing duplicate ACKs every ~800ms until the next STATUS poll finally
     *  caught up (the "offload ACK seq=N — ring rotated" spam seen on serial, one seq repeated
     *  4-10x). Reset on disconnect since a firmware reboot restarts seq numbering from scratch. */
    private var lastLocallyAckedSeq = 0L

    /**
     * Single-flight scheduler for [flushOffloadAcks]. Every trigger site (new verdict, connect,
     * bridge sync, self-reschedule after a successful ACK, …) used to call
     * `mainHandler.postDelayed({ flushOffloadAcks() }, …)` with a fresh anonymous Runnable each
     * time. None of those closures could be cancelled, so overlapping triggers (e.g. a new verdict
     * arriving mid-chain) could pile up independent timers. Routing every call through one named
     * Runnable field guarantees at most one flush attempt is ever scheduled at a time.
     */
    private fun scheduleFlushOffloadAcks(delayMs: Long) {
        flushOffloadAcksRunnable?.let { mainHandler.removeCallbacks(it) }
        val r = Runnable {
            flushOffloadAcksRunnable = null
            flushOffloadAcks()
        }
        flushOffloadAcksRunnable = r
        mainHandler.postDelayed(r, delayMs)
    }

    /** One queued GATT write per pending offload — stale SQLite seqs must not flood CMD. */
    private fun flushOffloadAcks() {
        if (!connected || offloadAckInFlight) return
        val status = lastDeviceStatus ?: return
        if ((status.offloadPending ?: 0) <= 0) return

        val lastAck = maxOf(status.offloadAckSeq ?: 0L, lastLocallyAckedSeq)
        val seqToAck = status.pendingSessionSeq?.takeIf { it > lastAck }
            ?: lastStoredVerdictSeq.takeIf { it > lastAck }
            ?: return

        offloadAckInFlight = true
        bleClient.ackOffloadSeq(seqToAck) { ok ->
            mainHandler.post {
                offloadAckInFlight = false
                if (ok) {
                    lastLocallyAckedSeq = seqToAck
                    ioExecutor.execute { verdictStore.markAcked(seqToAck) }
                    // Only chase another round if we already know (locally) there's a seq beyond
                    // the one we just ACKed — otherwise wait for the next real trigger (new
                    // verdict / STATUS poll) instead of blindly re-polling every 800ms.
                    val moreKnownLocally = (lastDeviceStatus?.pendingSessionSeq ?: 0L) > seqToAck ||
                        lastStoredVerdictSeq > seqToAck
                    if (moreKnownLocally) {
                        scheduleFlushOffloadAcks(800)
                    }
                }
            }
        }
    }

    /** Step 3b: read pending crashes from NVS ring, upload, clear per slot. Retries after settle. */
    private fun scheduleCrashRelayAfterSettle() {
        mainHandler.removeCallbacks(crashRelayRunnable)
        mainHandler.postDelayed(crashRelayRunnable, ImuProtocol.ESP_CONNECT_SETTLE_MS)
        for (delayMs in CRASH_RELAY_DELAYS_MS) {
            mainHandler.postDelayed(
                crashRelayRunnable,
                ImuProtocol.ESP_CONNECT_SETTLE_MS + delayMs,
            )
        }
    }

    private fun scheduleCrashRelayRetries() {
        scheduleCrashRelayAfterSettle()
    }

    private fun relayFsmActive(): Boolean = bleRelayActive || connectRelayActive

    private fun shouldAutoConnectRetry(): Boolean {
        if (connected) {
            return false
        }
        return userConnectedSession || relayFsmActive()
    }

    /**
     * Single entry for every BLE connect attempt (manual, relay FSM, link-loss retry).
     * Supersedes any in-flight connect so background + manual taps cannot interleave.
     */
    private fun requestBleConnect(fullSession: Boolean, reason: String) {
        if (connected && fullSession && bleClient.isFullSessionUp()) {
            Log.i(TAG, "Connect skipped — full session already up ($reason)")
            enterRelayState(RelayFsmState.CONNECTED, "Connected — live IMU")
            return
        }
        if (connected && fullSession && bleClient.upgradeToFullSession()) {
            Log.i(TAG, "Connect upgraded minimal session in place ($reason)")
            enterRelayState(RelayFsmState.CONNECTED, "Connected — live IMU")
            return
        }
        bleConnectGeneration++
        val generation = bleConnectGeneration
        cancelFsmTimers()
        cancelPendingStatusUpdates()
        bleClient.setMinimalRelayConnect(!fullSession)
        enterRelayState(RelayFsmState.SCAN_CONNECT, reason)
        mainHandler.post {
            if (generation != bleConnectGeneration) {
                Log.i(TAG, "Connect superseded: $reason")
                return@post
            }
            if (connected && fullSession && bleClient.isFullSessionUp()) {
                return@post
            }
            if (bleClient.isConnectBusy()) {
                Log.i(TAG, "Connect deferred — GATT busy ($reason)")
                mainHandler.postDelayed({
                    if (generation != bleConnectGeneration || connected) {
                        return@postDelayed
                    }
                    if (!bleClient.isConnectBusy()) {
                        bleClient.connect(session.lastBleAddress)
                    } else {
                        requestBleConnect(fullSession, reason)
                    }
                }, 2000)
                return@post
            }
            bleClient.connect(session.lastBleAddress)
        }
    }

    private fun connectRetryPauseMs(): Long = when {
        userConnectedSession -> MANUAL_CONNECT_RETRY_MS
        uiVisible -> UI_RELAY_PAUSE_MS
        else -> RELAY_PAUSE_MS
    }

    private fun finishBleRelaySession(reason: String) {
        if (connected && bleRelayActive && !userConnectedSession && !uiVisible) {
            bleClient.disconnect()
        }
        if (userConnectedSession || uiVisible) {
            if (connected) {
                enterRelayState(RelayFsmState.CONNECTED, "Connected — live IMU")
            } else {
                scheduleReconnectPause(
                    if (uiVisible) {
                        "Reconnecting…"
                    } else {
                        "Link lost — retry in ${MANUAL_CONNECT_RETRY_MS / 1000}s"
                    },
                )
            }
            return
        }
        if (relayFsmActive() && !userConnectedSession) {
            enterRelayState(RelayFsmState.CLOUD_SYNC, "Cloud sync…")
            ioExecutor.execute {
                val upload = cloudUploader.uploadAll()
                mainHandler.post {
                    val caption = if (upload.totalAccepted > 0) {
                        "Cloud OK — pause ${RELAY_PAUSE_MS / 1000}s ($reason)"
                    } else {
                        "Relay done — pause ${RELAY_PAUSE_MS / 1000}s ($reason)"
                    }
                    scheduleFsmPauseThenConnect(caption)
                }
            }
        } else {
            scheduleConnectRetry(reason)
        }
    }

    private fun reportClockSyncStatus(status: ImuProtocol.Status) {
        val src = status.clockSource ?: return
        val drift = status.clockDriftMs ?: return
        val corr = status.clockCorrMs ?: 0L
        val synced = status.clockSynced == true
        Log.i(
            TAG,
            "clock status synced=$synced src=$src tz=${status.clockTzMin} drift=${drift}ms corr=${corr}ms",
        )
        if (corr > 0L && corr != lastReportedClockCorrMs) {
            lastReportedClockCorrMs = corr
            val msg = "ESP clock corrected ${corr}ms (src=$src drift=${drift}ms)"
            if (!userConnectedSession) {
                broadcastStatus(msg, important = true)
            }
            if (CloudSettings(applicationContext).enabled) {
                ioExecutor.execute {
                    cloudUploader.uploadClockEvent(
                        src = src,
                        driftMs = drift,
                        corrMs = corr,
                        tzMin = status.clockTzMin ?: 0,
                        unixSec = status.clockUnixSec ?: 0L,
                    )
                }
            }
        }
        // Explicit once-per-connect user-visible verdict: priority #1 per FSM spec.
        // "NOK" tolerance is +-5min; firmware auto-applies at 4min so a correction always
        // implies the pre-correction drift was inside the NOK zone.
        if (!clockCheckedThisSession) {
            clockCheckedThisSession = true
            val driftAbsMin = Math.abs(drift) / 60_000.0
            when {
                corr > 0L -> broadcastBanner(
                    StatusBannerLevel.WARN,
                    String.format(java.util.Locale.US, "Clock drift was %.1fmin — corrected (src=$src)", driftAbsMin),
                )
                !synced -> broadcastBanner(StatusBannerLevel.WARN, "Clock not synced yet")
                driftAbsMin > 5.0 -> broadcastBanner(
                    StatusBannerLevel.WARN,
                    String.format(java.util.Locale.US, "Clock drift %.1fmin (NOK, src=$src)", driftAbsMin),
                )
                else -> broadcastBanner(
                    StatusBannerLevel.OK,
                    String.format(java.util.Locale.US, "Clock sync OK (drift %.1fmin, src=$src)", driftAbsMin),
                )
            }
        }
    }

    private fun scheduleConnectRetry(reason: String) {
        if (!shouldAutoConnectRetry()) {
            return
        }
        val pause = connectRetryPauseMs()
        mainHandler.removeCallbacks(connectRetryRunnable)
        mainHandler.postDelayed(connectRetryRunnable, pause)
        val msg = "ESP retry in ${pause / 1000}s ($reason)"
        Log.i(TAG, msg)
        broadcastStatus(msg, important = true)
        updateNotification(force = true)
    }

    private fun attemptAutoConnect() {
        if (connected) {
            return
        }
        if (bleClient.isConnectBusy()) {
            Log.i(TAG, "Auto connect skipped — connect in flight; retry in 2s")
            mainHandler.postDelayed({ attemptAutoConnect() }, 2000L)
            return
        }
        if (!shouldAutoConnectRetry()) {
            return
        }
        connectAttemptSeq++
        val msg = "Auto connect #$connectAttemptSeq (scan 20s)…"
        Log.i(TAG, msg)
        requestBleConnect(
            fullSession = uiVisible || userConnectedSession,
            reason = msg,
        )
    }

    /**
     * The always-on relay FSM connects in minimal (no-notify) mode in the background to save
     * power/BLE traffic for crash & status sync only. If the Activity becomes visible while that
     * minimal session is already up, notifications were never enabled, so the scene view (and any
     * live batch data) stays blank. Upgrade in place by reconnecting with full setup — mirrors
     * what the manual Connect button already does.
     */
    private fun onUiVisibleChanged(active: Boolean) {
        uiVisible = active
        if (active) {
            if (connected && !userConnectedSession) {
                Log.i(TAG, "UI foregrounded during minimal relay session — upgrading to full BLE session")
                userConnectedSession = true
                autoPromotedFullSession = true
                cancelFsmTimers()
                cancelPendingStatusUpdates()
                bleClient.setMinimalRelayConnect(false)
                if (bleClient.upgradeToFullSession()) {
                    enterRelayState(RelayFsmState.CONNECTED, "Connected — live IMU")
                } else {
                    requestBleConnect(
                        fullSession = true,
                        reason = "Foreground — upgrading link…",
                    )
                }
            } else if (!connected) {
                ensureRelayConnectForUi()
            }
        } else if (autoPromotedFullSession) {
            autoPromotedFullSession = false
            userConnectedSession = false
        }
    }

    /** UI is visible but BLE is down — (re)start the relay FSM instead of sitting on "Disconnected". */
    private fun ensureRelayConnectForUi() {
        if (connected) {
            return
        }
        if (!relayFsmActive()) {
            startBleRelayMode()
            return
        }
        cancelFsmTimers()
        when (relayFsmState) {
            RelayFsmState.PAUSE, RelayFsmState.STARTING, RelayFsmState.BT_WARMUP -> {
                requestBleConnect(
                    fullSession = true,
                    reason = "UI foreground — connecting…",
                )
            }
            RelayFsmState.SCAN_CONNECT -> {
                if (!bleClient.isConnectBusy()) {
                    requestBleConnect(
                        fullSession = true,
                        reason = "UI foreground — retrying scan…",
                    )
                }
            }
            else -> wakeRelayFsmNow()
        }
    }

    /** On connect: fetch crashes immediately and retry upload until backend accepts (or none left). */
    private fun relayCrashesUntilConfirmed(round: Int = 0, onDone: () -> Unit) {
        if (!connected) {
            onDone()
            return
        }
        if (round >= CRASH_RELAY_MAX_ROUNDS) {
            broadcastBanner(StatusBannerLevel.WARN, "Crash relay — max retries, continuing")
            onDone()
            return
        }
        bleClient.fetchAllPendingCrashes { crashes ->
            if (crashes.isEmpty()) {
                onDone()
                return@fetchAllPendingCrashes
            }
            ioExecutor.execute {
                for (info in crashes) {
                    offloadExporter.exportCrashJson(CrashFetcher.toOffloadJson(info))
                }
                val upload = cloudUploader.uploadPendingCrashes(crashes.size.coerceAtLeast(1))
                mainHandler.post {
                    if (!connected) {
                        onDone()
                        return@post
                    }
                    if (upload.ok && (upload.accepted > 0 || upload.duplicates > 0)) {
                        // Dedup key is (device_id, seq, pc) — a "duplicate" here means this exact
                        // crash is already safely recorded in the cloud (e.g. a prior upload
                        // succeeded but the link dropped before the slot-clear write landed).
                        // Safe to clear either way; only the banner differs.
                        clearDeviceCrashSlots(crashes)
                        val first = crashes.first()
                        if (upload.accepted > 0) {
                            broadcastBanner(StatusBannerLevel.OK, "OK!")
                            broadcastStatus(
                                "Crash relayed: ${crashes.size}x (${first.reason})",
                                important = true,
                            )
                        } else {
                            broadcastStatus(
                                "Crash already in cloud (seq ${first.seq}) — ESP slot cleared",
                                important = true,
                            )
                        }
                        mainHandler.postDelayed({ relayCrashesUntilConfirmed(round + 1, onDone) }, 500)
                    } else if (upload.ok) {
                        broadcastBanner(
                            StatusBannerLevel.WARN,
                            "Crash upload returned 0 accepted — ESP slot kept",
                        )
                        onDone()
                    } else if (!CloudSettings(applicationContext).enabled) {
                        broadcastBanner(
                            StatusBannerLevel.WARN,
                            "Cloud off — ${crashes.size} crash(es) saved on phone",
                        )
                        onDone()
                    } else if (upload.message.contains("HTTP", ignoreCase = true)) {
                        CloudUploadScheduler.enqueueNow(applicationContext)
                        mainHandler.postDelayed(
                            { relayCrashesUntilConfirmed(round + 1, onDone) },
                            CRASH_RELAY_RETRY_MS,
                        )
                    } else {
                        CloudUploadScheduler.enqueueNow(applicationContext)
                        mainHandler.postDelayed(
                            { relayCrashesUntilConfirmed(round + 1, onDone) },
                            CRASH_RELAY_RETRY_MS,
                        )
                    }
                }
            }
        }
    }

    private fun clearDeviceCrashSlots(crashes: List<CrashFetcher.CrashInfo>) {
        val slots = crashes.mapNotNull { it.slot.takeIf { s -> s >= 0 } }
        if (slots.isNotEmpty()) {
            bleClient.clearDeviceCrashSlots(slots)
        }
        if (crashes.any { it.slot < 0 }) {
            bleClient.clearDeviceCrash()
        }
    }

    private fun relayPendingCrash() {
        if (!connected) return
        bleClient.fetchAllPendingCrashes { crashes ->
            if (crashes.isEmpty()) return@fetchAllPendingCrashes
            ioExecutor.execute {
                for (info in crashes) {
                    offloadExporter.exportCrashJson(CrashFetcher.toOffloadJson(info))
                }
                val upload = cloudUploader.uploadPendingCrashes(crashes.size.coerceAtLeast(1))
                mainHandler.post {
                    if (!connected) return@post
                    if (upload.ok && (upload.accepted > 0 || upload.duplicates > 0)) {
                        clearDeviceCrashSlots(crashes)
                        val first = crashes.first()
                        if (upload.accepted > 0) {
                            broadcastBanner(StatusBannerLevel.OK, "OK!")
                            broadcastStatus(
                                "Crash relayed: ${crashes.size}x (${first.reason})",
                                important = true,
                            )
                        } else {
                            broadcastStatus(
                                "Crash already in cloud (seq ${first.seq}) — ESP slot cleared",
                                important = true,
                            )
                        }
                    } else if (!CloudSettings(applicationContext).enabled) {
                        broadcastBanner(StatusBannerLevel.WARN, "Cloud off — crash saved on phone")
                    } else if (upload.message.contains("HTTP", ignoreCase = true)) {
                        CloudUploadScheduler.enqueueNow(applicationContext)
                        broadcastBanner(
                            StatusBannerLevel.ERROR,
                            "Failed to upload batch: ${upload.message}",
                        )
                    } else {
                        CloudUploadScheduler.enqueueNow(applicationContext)
                        broadcastBanner(
                            StatusBannerLevel.WARN,
                            "Phone offline — crash queued locally",
                        )
                    }
                }
            }
        }
    }

    private fun startAutopilotMode() {
        autopilotActive = true
        startForeground(NOTIFICATION_ID, buildNotification())
        mainHandler.removeCallbacks(internalBridgeRunnable)
        if (BridgeSyncSettings(this).scheduled) {
            scheduleInternalBridgeNext(BridgeSyncScheduler.FIRST_SYNC_DELAY_MS)
        }
        updateNotification(force = true)
    }

    private fun startConnectRelayMode() {
        connectRelayActive = true
        startBleRelayMode()
    }

    private fun startBleRelayMode() {
        if (bleRelayActive && relayFsmStarted) {
            return
        }
        bleRelayActive = true
        connectRelayActive = true
        connectAttemptSeq = 0
        startForeground(NOTIFICATION_ID, buildNotification())
        Log.i(
            TAG,
            "BLE relay FSM started (warmup ${BT_WARMUP_MS / 1000}s, pause ${RELAY_PAUSE_MS / 1000}s)",
        )
        startRelayFsm()
        updateNotification(force = true)
    }

    private fun startRelayFsm() {
        relayFsmStarted = true
        cancelFsmTimers()
        enterRelayState(RelayFsmState.STARTING, "Background service started")
        if (btWarmupDone) {
            mainHandler.post { beginScanConnect("FSM resumed") }
            return
        }
        mainHandler.postDelayed({
            enterRelayState(
                RelayFsmState.BT_WARMUP,
                "Bluetooth warmup ${BT_WARMUP_MS / 1000}s…",
            )
            mainHandler.postDelayed(fsmWarmupRunnable, BT_WARMUP_MS)
        }, 300L)
    }

    private fun onFsmWarmupComplete() {
        btWarmupDone = true
        if (!relayFsmActive() || userConnectedSession || connected) {
            return
        }
        beginScanConnect("warmup done")
    }

    private fun onFsmPauseComplete() {
        Log.i(
            TAG,
            "FSM pause ended (connected=$connected bridge=$bridgeSyncActive " +
                "user=$userConnectedSession relay=${relayFsmActive()})",
        )
        if (connected) {
            return
        }
        if (userConnectedSession) {
            requestBleConnect(
                fullSession = true,
                reason = "Retrying manual connect…",
            )
            return
        }
        if (!relayFsmActive()) {
            Log.w(TAG, "FSM pause ended but relay inactive — no reconnect")
            return
        }
        beginScanConnect("pause ended")
    }

    private fun onReconnectWatchdog() {
        if (connected) {
            return
        }
        if (!relayFsmActive() && !userConnectedSession) {
            return
        }
        val now = SystemClock.uptimeMillis()
        if (relayFsmState == RelayFsmState.SCAN_CONNECT && now < reconnectDueAtMs + 45_000L) {
            // Scan + GATT still in progress — check again later.
            armReconnectWatchdog(10_000L)
            return
        }
        if (now < reconnectDueAtMs) {
            armReconnectWatchdog()
            return
        }
        Log.w(TAG, "Reconnect watchdog: overdue in ${relayFsmState.name} — forcing retry")
        onFsmPauseComplete()
    }

    private fun armReconnectWatchdog(fixedDelayMs: Long? = null) {
        mainHandler.removeCallbacks(reconnectWatchdogRunnable)
        if (connected || (!relayFsmActive() && !userConnectedSession)) {
            return
        }
        val delay = fixedDelayMs ?: run {
            val untilDue = reconnectDueAtMs - SystemClock.uptimeMillis() + 3_000L
            untilDue.coerceIn(5_000L, 45_000L)
        }
        mainHandler.postDelayed(reconnectWatchdogRunnable, delay)
    }

    private fun beginScanConnect(reason: String) {
        if (!relayFsmActive() || userConnectedSession || connected) {
            return
        }
        enterRelayState(RelayFsmState.SCAN_CONNECT, "Scan + connect… ($reason)")
        attemptAutoConnect()
    }

    private fun scheduleReconnectPause(caption: String) {
        if (!relayFsmActive() && !userConnectedSession) {
            return
        }
        enterRelayState(RelayFsmState.PAUSE, caption)
        val pauseMs = when {
            userConnectedSession -> MANUAL_CONNECT_RETRY_MS
            uiVisible -> UI_RELAY_PAUSE_MS
            else -> RELAY_PAUSE_MS
        }
        reconnectDueAtMs = SystemClock.uptimeMillis() + pauseMs
        cancelFsmTimers()
        mainHandler.postDelayed(fsmPauseRunnable, pauseMs)
        armReconnectWatchdog()
        broadcastRelayState()
        updateNotification(force = true)
        Log.i(TAG, "Reconnect scheduled in ${pauseMs / 1000}s: $caption")
    }

    private fun scheduleFsmPauseThenConnect(caption: String) {
        scheduleReconnectPause(caption)
    }

    private fun cancelPendingStatusUpdates() {
        pendingTelemetry = null
        mainHandler.removeCallbacks(flushTelemetryRunnable)
    }

    private fun cancelFsmTimers() {
        mainHandler.removeCallbacks(connectRetryRunnable)
        mainHandler.removeCallbacks(fsmWarmupRunnable)
        mainHandler.removeCallbacks(fsmPauseRunnable)
        // reconnectWatchdogRunnable intentionally kept — safety net if pause callback is lost
    }

    private fun relayBannerLevel(state: RelayFsmState): StatusBannerLevel = when (state) {
        RelayFsmState.CONNECTED, RelayFsmState.CLOUD_SYNC -> StatusBannerLevel.OK
        RelayFsmState.STARTING, RelayFsmState.BT_WARMUP,
        RelayFsmState.SCAN_CONNECT, RelayFsmState.PAUSE,
        -> StatusBannerLevel.WARN
    }

    private fun enterRelayState(state: RelayFsmState, caption: String) {
        relayFsmState = state
        relayFsmCaption = caption
        Log.i(TAG, "FSM ${state.name}: $caption")
        broadcastRelayState()
        if (connected) {
            if (state == RelayFsmState.CONNECTED || state == RelayFsmState.CLOUD_SYNC) {
                session.lastStatus = caption
                broadcastStatus(caption, important = true)
                if (caption.isNotBlank() && state == RelayFsmState.CONNECTED) {
                    broadcastBanner(relayBannerLevel(state), caption)
                }
            }
            return
        }
        session.lastStatus = caption
        broadcastStatus(caption, important = true)
        if (caption.isNotBlank()) {
            broadcastBanner(relayBannerLevel(state), caption)
        }
    }

    private fun showDisconnectButton(): Boolean = connected

    private fun broadcastRelayState() {
        foreachCallback {
            it.onRelayState(
                relayFsmState.id,
                relayFsmCaption,
                connected,
                showDisconnectButton(),
            )
        }
    }

    private fun stopConnectRelayMode() {
        connectRelayActive = false
        bleRelayActive = false
        relayFsmStarted = false
        cancelFsmTimers()
        updateNotification(force = true)
        stopForegroundIfIdle()
    }

    private fun stopAutopilotMode() {
        autopilotActive = false
        pendingBridgeWork = false
        mainHandler.removeCallbacks(internalBridgeRunnable)
        mainHandler.removeCallbacks(connectRetryRunnable)
        if (bridgeSyncActive && !userConnectedSession) {
            bridgeSyncActive = false
            mainHandler.removeCallbacks(bridgeFinishRunnable)
        }
        updateNotification(force = true)
        stopForegroundIfIdle()
    }

    private fun maybeAutoRefForBridge() {
        if (!bridgeSyncActive || autoRefInProgress) {
            return
        }
        if (lastDeviceStatus?.vibroRefReady == true) {
            return
        }
        autoRefInProgress = true
        broadcastStatus("Bridge: auto reference capture (~12s)…", important = true)
        bleClient.vibroRefStart { ok ->
            if (!ok) {
                autoRefInProgress = false
                return@vibroRefStart
            }
            mainHandler.postDelayed({
                bleClient.vibroRefStop {
                    autoRefInProgress = false
                }
            }, 12_000L)
        }
    }

    /**
     * Bridge sync (verdict/config sync) never opens its own BLE connection — it only sets a
     * request flag serviced by the single always-on relay FSM once it reaches CONNECTED (after
     * crash drain). This removes the old two-connect-authorities race that could leave
     * bridgeSyncActive stuck and silently freeze reconnects.
     */
    private fun startBridgeSyncCycle() {
        if (bridgeSyncActive || pendingBridgeWork) {
            return
        }
        pendingBridgeWork = true
        Log.i(TAG, "Bridge sync requested — will run on next relay connect")
        if (connected && !userConnectedSession) {
            beginBridgeWorkThenFinish()
        } else if (!connected && !userConnectedSession) {
            wakeRelayFsmNow()
        }
        // else: user is manually connected — pendingBridgeWork stays set and is picked up once
        // they disconnect and the relay FSM resumes.
    }

    /** Nudge the relay FSM to (re)connect now instead of waiting out its pause timer. */
    private fun wakeRelayFsmNow() {
        if (connected) {
            return
        }
        if (userConnectedSession && !uiVisible) {
            return
        }
        if (!relayFsmActive()) {
            startBleRelayMode()
            return
        }
        if (relayFsmState == RelayFsmState.PAUSE) {
            cancelFsmTimers()
            if (uiVisible || userConnectedSession) {
                requestBleConnect(
                    fullSession = true,
                    reason = "Wake — connecting…",
                )
            } else {
                mainHandler.post(fsmPauseRunnable)
            }
            return
        }
        if (relayFsmState == RelayFsmState.STARTING || relayFsmState == RelayFsmState.BT_WARMUP) {
            if (uiVisible) {
                cancelFsmTimers()
                requestBleConnect(fullSession = true, reason = "Wake — connecting…")
            }
        }
    }

    private fun beginBridgeWorkThenFinish() {
        if (bridgeSyncActive || !connected) {
            return
        }
        bridgeSyncActive = true
        pendingBridgeWork = false
        startForeground(NOTIFICATION_ID, buildNotification())
        savedPollMsForBridge = session.pollMs
        bleClient.setPollIntervalMs(2000)
        syncConfigThenBridgeSetup()
    }

    private fun syncConfigThenBridgeSetup() {
        ioExecutor.execute {
            bleClient.syncConfigFromDevice { blob ->
                if (blob != null) {
                    session.saveLocalConfig(blob)
                    broadcastConfig(blob)
                }
                mainHandler.post {
                    if (!connected || !bridgeSyncActive) {
                        return@post
                    }
                    scheduleFlushOffloadAcks(500)
                    maybeAutoRefForBridge()
                    scheduleBridgeFinish()
                }
            }
        }
    }

    private fun scheduleInternalBridgeNext(overrideDelayMs: Long? = null) {
        if (!autopilotActive) {
            return
        }
        val settings = BridgeSyncSettings(this)
        if (!settings.scheduled) {
            return
        }
        mainHandler.removeCallbacks(internalBridgeRunnable)
        val delay = overrideDelayMs ?: settings.intervalMs(this).coerceAtLeast(30_000L)
        mainHandler.postDelayed(internalBridgeRunnable, delay)
        BridgeSyncScheduler.scheduleNext(applicationContext)
    }

    private fun scheduleBridgeFinish() {
        mainHandler.removeCallbacks(bridgeFinishRunnable)
        val dwellMs = EspRendezvous.suggestedDwellSec(this, lastDeviceStatus) * 1000L
        mainHandler.postDelayed(bridgeFinishRunnable, dwellMs)
    }

    /** Bridge work done — hand off to the single relay pause/reconnect/cloud-sync path. */
    private fun completeBridgeSyncCycle() {
        if (!bridgeSyncActive) {
            return
        }
        bridgeSyncActive = false
        mainHandler.removeCallbacks(bridgeFinishRunnable)
        if (savedPollMsForBridge > 0 && !userConnectedSession) {
            bleClient.setPollIntervalMs(savedPollMsForBridge)
            savedPollMsForBridge = 0
        }
        if (autopilotActive) {
            scheduleInternalBridgeNext()
        }
        finishBleRelaySession("bridge sync done")
    }

    private fun buildSnapshot(): Bundle =
        session.toSnapshotBundle(
            connected,
            lastPower,
            caps,
            lastBatchJson,
            lastDeviceStatus?.crashDebugEnabled == true ||
                ImuProtocol.crashDebugFromCaps(caps),
            lastDeviceStatus?.bistStatus,
            relayFsmState.id,
            relayFsmCaption,
            showDisconnectButton(),
        )

    private fun pushSessionRestoreToAll() {
        val snap = buildSnapshot()
        foreachCallback { pushSessionRestoreToCallback(it, snap) }
    }

    private fun pushSessionRestoreToCallback(callback: IImuBleCallback) {
        pushSessionRestoreToCallback(callback, buildSnapshot())
    }

    private fun pushSessionRestoreToCallback(callback: IImuBleCallback, snap: Bundle) {
        try {
            callback.onRelayState(
                snap.getInt(ImuSessionStore.KEY_RELAY_STATE, RelayFsmState.STARTING.id),
                snap.getString(ImuSessionStore.KEY_RELAY_CAPTION) ?: "",
                snap.getBoolean(ImuSessionStore.KEY_CONNECTED),
                snap.getBoolean(ImuSessionStore.KEY_SHOW_DISCONNECT),
            )
            callback.onSessionRestore(snap)
            callback.onClockState(
                lastDeviceStatus?.clockSynced == true,
                lastDeviceStatus?.clockTzMin ?: 0,
            )
        } catch (_: Exception) {
        }
    }

    private fun throttleTelemetry(text: String) {
        pendingTelemetry = text
        val now = SystemClock.uptimeMillis()
        if (now - lastTelemetryUiMs >= TELEMETRY_UI_MS) {
            flushPendingTelemetry()
            return
        }
        if (!mainHandler.hasCallbacks(flushTelemetryRunnable)) {
            val delay = TELEMETRY_UI_MS - (now - lastTelemetryUiMs)
            mainHandler.postDelayed(flushTelemetryRunnable, delay.coerceAtLeast(1L))
        }
    }

    private fun flushPendingTelemetry() {
        if (connected) {
            pendingTelemetry = null
            return
        }
        val text = pendingTelemetry ?: return
        pendingTelemetry = null
        lastTelemetryUiMs = SystemClock.uptimeMillis()
        broadcastStatus(text, important = false)
    }

    private fun broadcastConnection(connected: Boolean) {
        foreachCallback { it.onConnectionChanged(connected) }
        if (connected) {
            foreachCallback { it.onCaptionEpoch(captionEpoch) }
        }
    }

    private fun isFsmNoiseCaption(text: String): Boolean {
        val lower = text.lowercase()
        return lower.contains("link lost") ||
            lower.contains("retry") ||
            lower.contains("auto connect") ||
            lower.contains("pause") ||
            lower.contains("scanning") ||
            lower.contains("scan + connect") ||
            lower.contains("connect blocked") ||
            lower.contains("relay done") ||
            lower.contains("cloud ok") ||
            lower.contains("warmup") ||
            lower.contains("direct connect") ||
            lower.contains("waiting for esp grace") ||
            lower.contains("disconnected")
    }

    private fun broadcastStatus(text: String, important: Boolean) {
        if (connected && isFsmNoiseCaption(text)) {
            return
        }
        if (important && (!connected || !isFsmNoiseCaption(text))) {
            session.lastStatus = text
        }
        foreachCallback { it.onStatus(text) }
    }

    private fun broadcastBanner(level: StatusBannerLevel, message: String) {
        if (connected && isFsmNoiseCaption(message)) {
            return
        }
        mainHandler.post {
            val code = when (level) {
                StatusBannerLevel.OK -> 0
                StatusBannerLevel.WARN -> 1
                StatusBannerLevel.ERROR -> 2
            }
            foreachCallback { it.onBanner(code, message) }
        }
    }

    private fun broadcastPower(power: ImuProtocol.PowerStatus) {
        foreachCallback {
            it.onPowerStatus(power.source, power.voltageV, power.percent, power.valid)
        }
    }

    private fun broadcastBatch(json: String) {
        foreachCallback { it.onBatchJson(json) }
    }

    private fun broadcastConfig(blob: ByteArray) {
        foreachCallback { it.onConfigBlob(blob) }
    }

    private fun broadcastOtaProgress(pct: Int) {
        foreachCallback { it.onOtaProgress(pct) }
    }

    private fun broadcastOtaDone(ok: Boolean, message: String) {
        foreachCallback { it.onOtaDone(ok, message) }
    }

    private fun broadcastNetScan(json: String) {
        foreachCallback { it.onNetScan(json) }
    }

    private fun broadcastNetProfiles(json: String) {
        foreachCallback { it.onNetProfiles(json) }
    }

    private fun broadcastNetStatus(json: String) {
        foreachCallback { it.onNetStatus(json) }
    }

    private fun broadcastVibroCaption(caption: String) {
        foreachCallback { it.onVibroCaption(caption) }
    }

    private fun broadcastEspScreen(on: Boolean) {
        foreachCallback { it.onEspScreenState(on) }
    }

    /**
     * RemoteCallbackList.beginBroadcast()/finishBroadcast() do not support reentrancy: a nested
     * call on the same thread (e.g. a local in-process callback synchronously triggering another
     * broadcast before the outer one finishes) or a racing call from another thread both throw
     * "beginBroadcast() called while already in a broadcast" and crash the process. Guard with an
     * atomic flag and defer any nested/racing call back onto mainHandler's queue — it will retry
     * once the in-flight broadcast has called finishBroadcast().
     */
    private fun foreachCallback(block: (IImuBleCallback) -> Unit) {
        if (!callbacksBroadcastActive.compareAndSet(false, true)) {
            mainHandler.post { foreachCallback(block) }
            return
        }
        try {
            val n = callbacks.beginBroadcast()
            try {
                for (i in 0 until n) {
                    try {
                        block(callbacks.getBroadcastItem(i))
                    } catch (_: Exception) {
                    }
                }
            } finally {
                callbacks.finishBroadcast()
            }
        } finally {
            callbacksBroadcastActive.set(false)
        }
    }

    private fun stopForegroundIfIdle() {
        if (connected || autopilotActive || bleRelayActive) {
            return
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            stopForeground(STOP_FOREGROUND_DETACH)
        } else {
            @Suppress("DEPRECATION")
            stopForeground(true)
        }
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                getString(R.string.notification_channel_ble),
                NotificationManager.IMPORTANCE_LOW,
            )
            val nm = getSystemService(NotificationManager::class.java)
            nm.createNotificationChannel(channel)
        }
    }

    private fun buildNotification(): Notification {
        val open = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        val text = when {
            connected -> session.lastStatus.ifEmpty { getString(R.string.notification_connected) }
            bridgeSyncActive -> "Bridge sync…"
            bleRelayActive -> {
                val cloud = if (CloudSettings(this).enabled) "cloud on" else "cloud off"
                "BLE relay #$connectAttemptSeq (20s scan · ${RELAY_PAUSE_MS / 1000}s pause · $cloud)"
            }
            autopilotActive -> getString(R.string.notification_autopilot)
            else -> getString(R.string.notification_idle)
        }
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(text)
            .setSmallIcon(android.R.drawable.ic_menu_compass)
            .setContentIntent(open)
            .setOngoing(connected || autopilotActive || bleRelayActive)
            .build()
    }

    private fun updateNotification(force: Boolean = false) {
        if (!connected && !bleRelayActive && !autopilotActive) {
            return
        }
        val now = SystemClock.uptimeMillis()
        if (!force && now - lastNotificationUpdateMs < NOTIFICATION_MIN_MS) {
            return
        }
        lastNotificationUpdateMs = now
        val nm = getSystemService(NotificationManager::class.java)
        nm.notify(NOTIFICATION_ID, buildNotification())
    }
}
