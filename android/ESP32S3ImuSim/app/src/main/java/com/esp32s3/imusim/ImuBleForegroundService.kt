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
        private const val BT_WARMUP_MS = 30_000L
        private const val RELAY_PAUSE_MS = 30_000L
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

    private var bridgeSyncActive = false
    private var bridgeInitiatedConnect = false
    private var bridgeAwaitingCrashRelay = false
    private var connectRelayActive = false
    private var bleRelayActive = false
    private var connectAttemptSeq = 0
    private var connectFailureStreak = 0
    private var userConnectedSession = false
    private var autopilotActive = false
    private var relayFsmState = RelayFsmState.STARTING
    private var relayFsmCaption = "Starting…"
    private var relayFsmStarted = false
    private var autoRefInProgress = false
    private var savedPollMsForBridge = 0
    private val crashRelayRunnable = Runnable { relayPendingCrash() }
    private val bridgeFinishRunnable = Runnable { completeBridgeSyncCycle() }
    private val connectRetryRunnable = Runnable { attemptAutoConnect() }
    private val fsmWarmupRunnable = Runnable { onFsmWarmupComplete() }
    private val fsmPauseRunnable = Runnable { onFsmPauseComplete() }
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
                mainHandler.post { pushSessionRestoreToCallback(callback) }
            }
        }

        override fun unregisterCallback(callback: IImuBleCallback?) {
            if (callback != null) {
                callbacks.unregister(callback)
            }
        }

        override fun getSnapshot(): Bundle = buildSnapshot()

        override fun requestState() {
            mainHandler.post {
                broadcastRelayState()
                pushSessionRestoreToAll()
            }
        }

        override fun connect() {
            userConnectedSession = true
            cancelFsmTimers()
            bleClient.setMinimalRelayConnect(false)
            enterRelayState(RelayFsmState.SCAN_CONNECT, "Manual connect — scanning…")
            mainHandler.post { bleClient.connect() }
        }

        override fun disconnect() {
            userConnectedSession = false
            bridgeSyncActive = false
            bridgeAwaitingCrashRelay = false
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

        override fun vibroRefStart() {
            mainHandler.post {
                rawSampling.onRefRecording(true)
                bleClient.vibroRefStart { ok ->
                    broadcastBanner(
                        if (ok) StatusBannerLevel.OK else StatusBannerLevel.ERROR,
                        if (ok) {
                            "Ref recording — shake the device ~10 s"
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

        override fun injectCrash(kind: String?) {
            mainHandler.post {
                bleClient.injectCrash(kind ?: "panic") { ok ->
                    broadcastBanner(
                        if (ok) StatusBannerLevel.WARN else StatusBannerLevel.ERROR,
                        if (ok) {
                            "Crash inject: ${kind ?: "panic"} — device will reboot"
                        } else {
                            "Crash inject failed — crash BLE service missing or write rejected"
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
        broadcastStatus(text, important = false)
        updateNotification(force = true)
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

    override fun onDeviceStatus(status: ImuProtocol.Status) {
        lastDeviceStatus = status
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
                mainHandler.post { flushOffloadAcks() }
            }
        } else if (
            (status.offloadPending ?: 0) > 0 &&
            status.vibroVerdictLevel != null &&
            offloadExporter.lineCount() == 0
        ) {
            ioExecutor.execute {
                offloadExporter.exportVerdict(status)
                CloudUploadScheduler.enqueueNow(applicationContext)
                mainHandler.post { flushOffloadAcks() }
            }
        } else if ((status.offloadPending ?: 0) > 0) {
            status.pendingSessionSeq?.takeIf { it > 0L }?.let { ps ->
                ioExecutor.execute {
                    if (status.vibroRmsG != null) {
                        verdictStore.recordOfflineSession(status, ps)
                        offloadExporter.exportVerdict(status.copy(seq = ps))
                    }
                    mainHandler.post { flushOffloadAcks() }
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
        broadcastVibroCaption(formatVibroCaption(status))
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
            cancelFsmTimers()
            if (userConnectedSession) {
                enterRelayState(RelayFsmState.CONNECTED, "Connected — live IMU")
            } else if (bridgeSyncActive) {
                enterRelayState(RelayFsmState.CONNECTED, "Bridge sync — connected")
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
            val relayOnly = bleRelayActive && !userConnectedSession && !bridgeSyncActive
            if (!relayOnly) {
                WakeRelay.onConnect(bleClient, priorStatus)
                if (priorStatus == null || !WakeRelay.isDeepSleepProfile(priorStatus)) {
                    bleClient.setPollIntervalMs(session.pollMs)
                }
                bleClient.setMode(session.renderMode)
            }
            mainHandler.postDelayed({ flushOffloadAcks() }, 500)
            if (bridgeSyncActive) {
                savedPollMsForBridge = session.pollMs
                bleClient.setPollIntervalMs(2000)
                bridgeAwaitingCrashRelay = true
                relayCrashesUntilConfirmed {
                    bridgeAwaitingCrashRelay = false
                    if (this@ImuBleForegroundService.connected && bridgeSyncActive) {
                        syncConfigThenBridgeSetup(skipCrashSchedule = true)
                    }
                }
            } else if (bleRelayActive && !userConnectedSession && !bridgeSyncActive) {
                Log.i(TAG, "BLE relay — priority: 1) TIME sync 2) crash drain 3) cloud")
                relayCrashesUntilConfirmed {
                    finishBleRelaySession("crash relay done")
                }
            } else {
                relayPendingCrash()
                scheduleCrashRelayRetries()
            }
            if (priorStatus != null && WakeRelay.isDeepSleepProfile(priorStatus)) {
                mainHandler.postDelayed({ flushOffloadAcks() }, 1500)
            }
        } else {
            stopForegroundIfIdle()
            mainHandler.removeCallbacks(flushTelemetryRunnable)
            mainHandler.removeCallbacks(crashRelayRunnable)
            mainHandler.removeCallbacks(bridgeFinishRunnable)
            pendingTelemetry = null
            if (wasConnected) {
                bridgeAwaitingCrashRelay = false
                if (userConnectedSession) {
                    enterRelayState(RelayFsmState.PAUSE, "Disconnected")
                } else if (relayFsmActive()) {
                    scheduleFsmPauseThenConnect("Link lost — retry in ${RELAY_PAUSE_MS / 1000}s")
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
            enterRelayState(RelayFsmState.PAUSE, "Connect failed: $reason")
            broadcastBanner(StatusBannerLevel.WARN, reason)
            broadcastRelayState()
            return
        }
        if (relayFsmActive()) {
            scheduleFsmPauseThenConnect("Connect failed — retry in ${RELAY_PAUSE_MS / 1000}s ($reason)")
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

    private fun flushOffloadAcks() {
        if (!connected) return
        ioExecutor.execute {
            for (seq in verdictStore.pendingAckSeqs()) {
                bleClient.ackOffloadSeq(seq)
                verdictStore.markAcked(seq)
            }
            lastDeviceStatus?.pendingSessionSeq?.takeIf { it > 0L }?.let { ps ->
                if ((lastDeviceStatus?.offloadPending ?: 0) > 0) {
                    bleClient.ackOffloadSeq(ps)
                    verdictStore.markAcked(ps)
                }
            }
        }
    }

    /** Step 3b: read pending crashes from NVS ring, upload, clear per slot. Retries at 3/8/18 s. */
    private fun scheduleCrashRelayRetries() {
        mainHandler.removeCallbacks(crashRelayRunnable)
        for (delayMs in CRASH_RELAY_DELAYS_MS) {
            mainHandler.postDelayed(crashRelayRunnable, delayMs)
        }
    }

    private fun relayFsmActive(): Boolean = bleRelayActive || connectRelayActive

    private fun shouldAutoConnectRetry(): Boolean {
        if (userConnectedSession || connected || bridgeSyncActive) {
            return false
        }
        return relayFsmActive()
    }

    private fun connectRetryPauseMs(): Long = RELAY_PAUSE_MS

    private fun finishBleRelaySession(reason: String) {
        if (connected && bleRelayActive && !userConnectedSession && !bridgeSyncActive) {
            bleClient.disconnect()
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
        if (corr > 0L) {
            val msg = "ESP clock corrected ${corr}ms (src=$src drift=${drift}ms)"
            broadcastStatus(msg, important = true)
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
        if (!shouldAutoConnectRetry() || connected) {
            return
        }
        connectAttemptSeq++
        val msg = "Auto connect #$connectAttemptSeq (scan 25s)…"
        Log.i(TAG, msg)
        enterRelayState(RelayFsmState.SCAN_CONNECT, msg)
        bleClient.setMinimalRelayConnect(true)
        bleClient.connect()
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
                    if (upload.ok) {
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
                                "Crash already in cloud — ESP slot cleared (${first.reason})",
                                important = true,
                            )
                        }
                        mainHandler.postDelayed({ relayCrashesUntilConfirmed(round + 1, onDone) }, 500)
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
        for (info in crashes) {
            if (info.slot >= 0) {
                bleClient.clearDeviceCrashSlot(info.slot)
            }
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
                    if (upload.ok) {
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
                                "Crash already in cloud — ESP slot cleared (${first.reason})",
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
        mainHandler.postDelayed({
            enterRelayState(
                RelayFsmState.BT_WARMUP,
                "Bluetooth warmup ${BT_WARMUP_MS / 1000}s…",
            )
            mainHandler.postDelayed(fsmWarmupRunnable, BT_WARMUP_MS)
        }, 300L)
    }

    private fun onFsmWarmupComplete() {
        if (!relayFsmActive() || userConnectedSession || connected) {
            return
        }
        beginScanConnect("warmup done")
    }

    private fun onFsmPauseComplete() {
        if (!relayFsmActive() || userConnectedSession || connected || bridgeSyncActive) {
            return
        }
        beginScanConnect("pause ended")
    }

    private fun beginScanConnect(reason: String) {
        if (!relayFsmActive() || userConnectedSession || connected || bridgeSyncActive) {
            return
        }
        enterRelayState(RelayFsmState.SCAN_CONNECT, "Scan + connect… ($reason)")
        attemptAutoConnect()
    }

    private fun scheduleFsmPauseThenConnect(caption: String) {
        if (!relayFsmActive() || userConnectedSession) {
            return
        }
        enterRelayState(RelayFsmState.PAUSE, caption)
        cancelFsmTimers()
        mainHandler.postDelayed(fsmPauseRunnable, RELAY_PAUSE_MS)
        broadcastRelayState()
        updateNotification(force = true)
    }

    private fun cancelFsmTimers() {
        mainHandler.removeCallbacks(connectRetryRunnable)
        mainHandler.removeCallbacks(fsmWarmupRunnable)
        mainHandler.removeCallbacks(fsmPauseRunnable)
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
        session.lastStatus = caption
        Log.i(TAG, "FSM ${state.name}: $caption")
        broadcastRelayState()
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
        mainHandler.removeCallbacks(internalBridgeRunnable)
        mainHandler.removeCallbacks(connectRetryRunnable)
        if (bridgeSyncActive && !userConnectedSession) {
            bridgeSyncActive = false
            bridgeAwaitingCrashRelay = false
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

    private fun startBridgeSyncCycle() {
        if (bridgeSyncActive) {
            return
        }
        bridgeSyncActive = true
        bridgeInitiatedConnect = !connected
        startForeground(NOTIFICATION_ID, buildNotification())
        broadcastBanner(StatusBannerLevel.WARN, "Bridge sync: connecting to ESP…")
        if (connected) {
            savedPollMsForBridge = session.pollMs
            bleClient.setPollIntervalMs(2000)
            bridgeAwaitingCrashRelay = true
            relayCrashesUntilConfirmed {
                bridgeAwaitingCrashRelay = false
                if (this.connected && bridgeSyncActive) {
                    syncConfigThenBridgeSetup(skipCrashSchedule = true)
                }
            }
        } else {
            bleClient.connect()
        }
    }

    private fun syncConfigThenBridgeSetup(skipCrashSchedule: Boolean = false) {
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
                    mainHandler.postDelayed({ flushOffloadAcks() }, 500)
                    if (!skipCrashSchedule) {
                        relayPendingCrash()
                        scheduleCrashRelayRetries()
                    }
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

    private fun completeBridgeSyncCycle() {
        if (!bridgeSyncActive) {
            return
        }
        if (bridgeAwaitingCrashRelay) {
            mainHandler.postDelayed(bridgeFinishRunnable, 2000L)
            return
        }
        mainHandler.post { flushOffloadAcks() }
        ioExecutor.execute {
            val upload = cloudUploader.uploadAll()
            mainHandler.post {
                when {
                    upload.totalAccepted > 0 ->
                        broadcastBanner(StatusBannerLevel.OK, "Bridge: uploaded ${upload.summary}")
                    !CloudSettings(applicationContext).enabled ->
                        broadcastBanner(StatusBannerLevel.WARN, "Bridge done — cloud off")
                    else ->
                        broadcastBanner(StatusBannerLevel.OK, "Bridge sync done")
                }
                finishBridgeSyncCycle()
            }
        }
    }

    private fun finishBridgeSyncCycle() {
        if (!bridgeSyncActive) {
            return
        }
        bridgeSyncActive = false
        bridgeAwaitingCrashRelay = false
        mainHandler.removeCallbacks(bridgeFinishRunnable)
        mainHandler.removeCallbacks(connectRetryRunnable)
        if (savedPollMsForBridge > 0 && !userConnectedSession) {
            bleClient.setPollIntervalMs(savedPollMsForBridge)
            savedPollMsForBridge = 0
        }
        if (bridgeInitiatedConnect && connected && !userConnectedSession) {
            bleClient.disconnect()
            broadcastStatus("Bridge sync — disconnected to save ESP battery", important = true)
        }
        if (autopilotActive) {
            scheduleInternalBridgeNext()
            updateNotification(force = true)
        } else if (relayFsmActive() && !userConnectedSession) {
            scheduleFsmPauseThenConnect("Bridge done — pause ${RELAY_PAUSE_MS / 1000}s")
        } else {
            stopForegroundIfIdle()
        }
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
        val text = pendingTelemetry ?: return
        pendingTelemetry = null
        lastTelemetryUiMs = SystemClock.uptimeMillis()
        broadcastStatus(text, important = false)
    }

    private fun broadcastConnection(connected: Boolean) {
        foreachCallback { it.onConnectionChanged(connected) }
    }

    private fun broadcastStatus(text: String, important: Boolean) {
        if (important) {
            session.lastStatus = text
        }
        foreachCallback { it.onStatus(text) }
    }

    private fun broadcastBanner(level: StatusBannerLevel, message: String) {
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

    private inline fun foreachCallback(block: (IImuBleCallback) -> Unit) {
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
                "BLE relay #$connectAttemptSeq (10s/30s · $cloud)"
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
