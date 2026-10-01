package com.esp32s3.imusim

import android.app.AlarmManager
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.drawable.BitmapDrawable
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.Bundle
import android.os.IBinder
import android.os.RemoteCallbackList
import android.os.SystemClock
import android.util.Log
import androidx.core.app.NotificationCompat
import java.io.File
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Long-lived BLE session — survives activity rotation and app backgrounding.
 * UI binds via AIDL and registers callbacks for live batches.
 */
class ImuBleForegroundService : Service(), BleImuClient.Listener {

    companion object {
        private const val TAG = "ImuBleService"
        var isRunning = false
            private set
        const val ACTION_BRIDGE_SYNC = "com.esp32s3.imusim.BRIDGE_SYNC"
        const val ACTION_AUTOPILOT = "com.esp32s3.imusim.AUTOPILOT"
        const val ACTION_CONNECT_RELAY = "com.esp32s3.imusim.CONNECT_RELAY"
        const val ACTION_BLE_RELAY = "com.esp32s3.imusim.BLE_RELAY"
        const val ACTION_STOP_AUTOPILOT = "com.esp32s3.imusim.STOP_AUTOPILOT"
        const val ACTION_STOP_CONNECT_RELAY = "com.esp32s3.imusim.STOP_CONNECT_RELAY"
        const val ACTION_CHECK_OTA = "com.esp32s3.imusim.CHECK_OTA"
        const val ACTION_SYNC_OTA_CHANNEL = "com.esp32s3.imusim.SYNC_OTA_CHANNEL"
        const val CHANNEL_ID = "imu_ble_lock"
        private const val WATCHDOG_MS = 60_000L
        const val NOTIFICATION_ID = 1
        private val CRASH_RELAY_DELAYS_MS = longArrayOf(3000L, 8000L, 18000L)
        private const val CONNECT_RETRY_PAUSE_MS = 900_000L
        private const val CONNECT_RETRY_PAUSE_MAX_MS = 1_800_000L
        private const val BT_WARMUP_MS = 8_000L
        private const val RELAY_PAUSE_MS = 15_000L
        private const val UI_RELAY_PAUSE_MS = 4_000L
        private const val MANUAL_CONNECT_RETRY_MS = 8_000L
        private const val WIFI_SCAN_RECONNECT_MS = 6_000L
        private const val FW_OTA_RECONNECT_MS = 5_000L
        /** Wait for slot erase + re-advertise (~4s settle + ~10–14s erase). */
        private const val FW_OTA_ERASE_RECONNECT_MS = 16_000L
        /** After ESP is connectable again — brief GATT settle before resume begin. */
        private const val FW_OTA_ERASE_READY_MS = 1_200L
        private const val CONNECT_FAILURE_COOLDOWN_THRESHOLD = 2
        private const val CONNECT_FAILURE_COOLDOWN_MS = 3_600_000L
        private const val CRASH_RELAY_RETRY_MS = 3_000L
        private const val CRASH_RELAY_MAX_ROUNDS = 12
        private const val TELEMETRY_UI_MS = 500L
        private const val NOTIFICATION_MIN_MS = 5000L
        private const val FFT_MIN_SAMPLES = 32
        private const val FFT_COLLECT_TIMEOUT_MS = 30_000L
        private const val FFT_COLLECT_TICK_MS = 50L
        private const val OTA_POLL_MS = 300_000L
        private const val OTA_FIRST_DELAY_MS = 20_000L
        private const val OPERATOR_POLL_MS = 45_000L
    }

    private val callbacks = RemoteCallbackList<IImuBleCallback>()
    private val callbacksBroadcastActive = AtomicBoolean(false)
    private val work = ServiceWorkQueue()

    private fun high(key: String, delayMs: Long = 0L, block: () -> Unit) =
        work.post(ServiceWorkQueue.Priority.HIGH, key, delayMs, block)

    private fun normal(key: String, delayMs: Long = 0L, block: () -> Unit) =
        work.post(ServiceWorkQueue.Priority.NORMAL, key, delayMs, block)

    private fun ui(key: String? = null, delayMs: Long = 0L, block: () -> Unit) =
        work.post(ServiceWorkQueue.Priority.UI, key, delayMs, block)

    private fun low(key: String, delayMs: Long = 0L, block: () -> Unit) =
        work.post(ServiceWorkQueue.Priority.LOW, key, delayMs, block)

    private lateinit var bleClient: BleImuClient
    private lateinit var session: ImuSessionStore
    private lateinit var verdictStore: VerdictStore
    private lateinit var offloadExporter: OffloadExporter
    private lateinit var batteryBenchStore: BatteryBenchStore

    private var benchSessionId: Long = 0L
    private var benchStartedMs: Long = 0L
    private var benchLabel: String? = null
    private var benchLastSeq: Long = -1L
    private var benchLastVoltage: Float? = null
    private var benchLastTs: Long = 0L
    private var benchUserActive: Boolean = false
    private var benchFwActive: Boolean = false
    private var benchConfirmGen: Int = 0

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
    private var lastPushedOtaChannel: String? = null
    private var lastOtaChannelPushMs = 0L

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
    /** After FW OTA reboot: ignore leftover STATUS on the dying link; unlock on the next boot. */
    private var fwOtaAwaitingFreshStatus = false
    private var otaEraseRetryBytes: ByteArray? = null
    private var otaEraseResumePending = false
    private var lastTelemetryExportMs = 0L
    private var autoRefInProgress = false
    private var savedPollMsForBridge = 0
    private var clockCheckedThisSession = false
    private fun runInternalBridgeTick() {
        if (!autopilotActive || bridgeSyncActive) return
        val settings = BridgeSyncSettings(this)
        if (!settings.scheduled) return
        startBridgeSyncCycle()
    }

    private val aidl = object : IImuBleService.Stub() {
        override fun registerCallback(callback: IImuBleCallback?) {
            if (callback != null) {
                callbacks.register(callback)
                ui("ui.restore") {
                    if (!bleRelayActive && !connected) {
                        startBleRelayMode()
                    }
                    pushSessionRestoreToCallback(callback)
                    try {
                        callback.onCaps(caps)
                    } catch (_: Exception) {
                    }
                }
            }
        }

        override fun unregisterCallback(callback: IImuBleCallback?) {
            if (callback != null) {
                callbacks.unregister(callback)
            }
        }

        override fun requestState() {
            ui("ui.requestState") {
                broadcastRelayState()
                pushSessionRestoreToAll()
            }
        }

        override fun setUiVisible(active: Boolean) {
            ui("ui.visible") { onUiVisibleChanged(active) }
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
            high("ble.disconnect") {
                bleClient.disconnect()
                enterRelayState(RelayFsmState.PAUSE, "Disconnected — pause ${RELAY_PAUSE_MS / 1000}s")
                scheduleFsmPauseThenConnect("Disconnected — pause ${RELAY_PAUSE_MS / 1000}s")
                stopForegroundIfIdle()
            }
        }

        override fun setMode(mode: Int) {
            session.renderMode = mode
            rawSampling.onMode(mode)
            high("ble.setMode") { bleClient.setMode(mode) }
        }

        override fun setPollIntervalMs(ms: Int) {
            session.pollMs = ms
            high("ble.setPoll") { bleClient.setPollIntervalMs(ms) }
        }

        override fun requestConfigSync() {
            high("ble.config.sync") {
                bleClient.syncConfigFromDevice { blob ->
                    if (blob != null) {
                        session.saveLocalConfig(blob)
                        broadcastConfig(blob)
                        val doc = DeviceConfigJson.fromBlob(blob, "esp")
                        reconcileConfigWithCloud(doc, blob)
                    } else {
                        broadcastBanner(StatusBannerLevel.ERROR, "Config read failed")
                    }
                }
            }
        }

        override fun pushConfig(blob: ByteArray?, commit: Boolean) {
            if (blob == null) return
            high("ble.config.push") {
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
                OtaSession.fail("empty firmware")
                broadcastOtaDone(false, "empty firmware")
                return
            }
            val tmp = java.io.File(cacheDir, "ota-inline.bin")
            tmp.writeBytes(firmware)
            startFirmwareFile(tmp)
        }

        override fun uploadFirmwarePath(path: String?) {
            val file = path?.let { java.io.File(it) }
            if (file == null || !file.isFile || file.length() <= 0L) {
                OtaSession.fail("empty firmware")
                broadcastOtaDone(false, "empty firmware")
                return
            }
            startFirmwareFile(file)
        }

        override fun requestNetScan() {
            high("ble.net.scan") { bleClient.requestNetScan() }
        }

        override fun requestNetProfiles() {
            high("ble.net.profiles") { bleClient.requestNetProfiles() }
        }

        override fun sendNetCommand(json: String?) {
            if (json == null) return
            high("ble.net.cmd") { bleClient.sendNetCommand(json) }
        }

        override fun vibroRefStart(slot: Int, name: String?) {
            high("ble.vibro.ref.start") {
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
            high("ble.vibro.ref.stop") {
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
            high("ble.vibro.ref.select") {
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
            high("ble.vibro.ref.delete") {
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

        override fun vibroRefClearAll() {
            high("ble.vibro.ref.clear") {
                bleClient.vibroRefClearAll { ok ->
                    broadcastBanner(
                        if (ok) StatusBannerLevel.OK else StatusBannerLevel.ERROR,
                        if (ok) "All reference slots erased" else "Clear all failed",
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

        override fun vibroArm() {
            high("ble.vibro.arm") {
                bleClient.vibroArm { ok ->
                    broadcastBanner(
                        if (ok) StatusBannerLevel.OK else StatusBannerLevel.ERROR,
                        if (ok) "Monitoring armed" else "Arm failed",
                    )
                }
            }
        }

        override fun vibroSetSensingPaused(paused: Boolean) {
            high("ble.vibro.pause") {
                bleClient.vibroSetSensingPaused(paused) { ok ->
                    broadcastBanner(
                        if (ok) StatusBannerLevel.OK else StatusBannerLevel.ERROR,
                        if (!ok) {
                            "Repair pause failed"
                        } else if (paused) {
                            "Sensing paused (repair)"
                        } else {
                            "Sensing resumed"
                        },
                    )
                }
            }
        }

        override fun requestVibroRefList() {
            high("ble.vibro.list") {
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
            normal("fft.start") { startSpectrumAnalysis() }
        }

        override fun setEspScreenOn(on: Boolean) {
            high("ble.screen") { bleClient.setEspScreenOn(on) }
        }

        override fun toggleEspScreen() {
            high("ble.screen.toggle") {
                val current = lastDeviceStatus?.screenOn ?: true
                bleClient.setEspScreenOn(!current)
            }
        }

        override fun setCpuMhzOverride(mhz: Int) {
            high("ble.cpuMhz") { bleClient.setCpuMhzOverride(mhz) }
        }

        override fun setImuHzOverride(hz: Int) {
            high("ble.imuHz") { bleClient.setImuHzOverride(hz) }
        }

        override fun startBatteryBench(label: String?) {
            normal("bench.start") { startBatteryBenchInternal(label?.trim().orEmpty()) }
        }

        override fun stopBatteryBench() {
            normal("bench.stop") { stopBatteryBenchInternal() }
        }

        override fun injectCrash(kind: String?) {
            high("ble.crash.inject") {
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
            high("ble.nvs.erase") {
                bleClient.eraseDeviceNvs { ok ->
                    if (!ok) {
                        broadcastBanner(StatusBannerLevel.ERROR, "NVS erase failed — connect + v49 firmware")
                    }
                }
            }
        }

        override fun runDeviceBist() {
            high("ble.bist") {
                bleClient.runDeviceBist { ok ->
                    broadcastBanner(
                        if (ok) StatusBannerLevel.OK else StatusBannerLevel.WARN,
                        if (ok) "BIST command sent — check STATUS bist field" else "BIST failed (debug firmware?)",
                    )
                }
            }
        }

        override fun setDebugLed(mask: Int) {
            high("ble.led") {
                bleClient.setDebugLed(mask) { ok ->
                    if (!ok) {
                        broadcastBanner(StatusBannerLevel.ERROR, "LED debug write failed — connect first")
                    }
                }
            }
        }

        override fun clearDebugLed() {
            high("ble.led.clear") { bleClient.clearDebugLed() }
        }

        override fun floorCalibStart(durationMs: Int) {
            high("ble.floor.start") {
                bleClient.floorCalibStart(durationMs) { ok ->
                    if (ok) {
                        broadcastBanner(StatusBannerLevel.OK, "Floor calibration started — hold still")
                        pollFloorCalUntilDone()
                    } else {
                        broadcastBanner(StatusBannerLevel.ERROR, "Floor calibration start failed (BLE busy?)")
                    }
                }
            }
        }

        override fun floorCalibClear() {
            high("ble.floor.clear") {
                bleClient.floorCalibClear { ok ->
                    broadcastBanner(
                        if (ok) StatusBannerLevel.OK else StatusBannerLevel.ERROR,
                        if (ok) "Floor calibration cleared" else "Floor calibration clear failed",
                    )
                    if (ok) {
                        bleClient.readFloorCalStatus { json ->
                            if (json != null) foreachCallback { it.onFloorCalStatus(json) }
                        }
                    }
                }
            }
        }

        override fun requestFloorCalStatus() {
            high("ble.floor.status") {
                bleClient.readFloorCalStatus { json ->
                    if (json != null) {
                        foreachCallback { it.onFloorCalStatus(json) }
                    } else {
                        broadcastBanner(StatusBannerLevel.ERROR, "Floor calibration status read failed")
                    }
                }
            }
        }

        override fun startGeoTracking() {
            ui("geo.start") {
                if (!::geoTracker.isInitialized) return@ui
                geoTracker.start(force = true)
            }
        }

        override fun stopGeoTracking() {
            ui("geo.stop") {
                if (!::geoTracker.isInitialized) return@ui
                geoTracker.stop()
            }
        }

        override fun seedGeoAnchor(lat: Double, lon: Double) {
            ui("geo.seed") {
                if (!::geoTracker.isInitialized) return@ui
                geoTracker.seedAnchor(lat, lon)
            }
        }
    }

    /** Sampling window runs on-device over a few seconds; poll status until it reports done. */
    private fun pollFloorCalUntilDone(attempt: Int = 0) {
        high("ble.floor.poll", 400) {
            bleClient.readFloorCalStatus { json ->
                if (json != null) foreachCallback { it.onFloorCalStatus(json) }
                val stillSampling = json?.contains("\"sampling\":1") == true
                if (stillSampling && attempt < 40) {
                    pollFloorCalUntilDone(attempt + 1)
                }
            }
        }
    }

    private val vibroBuffer = VibroSampleBuffer()
    private val rawSampling = RawSamplingSession()
    private lateinit var cloudUploader: CloudUploader
    private lateinit var geoTracker: GeoTracker
    private var spectrumSeq = 1L
    private var fftCollectAttempt = 0
    private var fftSavedPollMs = 0
    private var fftCollectStartedMs = 0L
    private fun startSpectrumAnalysis() {
        work.cancel("fft.tick")
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
        normal("fft.tick", 250) { continueFftCollection() }
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
            low("fft.analyze") { runSpectrumAnalysis() }
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
        normal("fft.tick", FFT_COLLECT_TICK_MS) { continueFftCollection() }
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

    private var lastOperatorAlert: Boolean? = null

    private fun scheduleOtaPoll(delayMs: Long) {
        low("ota.poll", delayMs) {
            runOtaPoll(force = false)
            scheduleOtaPoll(OTA_POLL_MS)
        }
    }

    private fun scheduleOperatorPoll(delayMs: Long) {
        low("operator.poll", delayMs) {
            pollOperatorStatus()
            scheduleOperatorPoll(OPERATOR_POLL_MS)
        }
    }

    override fun onCreate() {
        super.onCreate()
        isRunning = true
        createNotificationChannel()
        // Enter the foreground before any asynchronous work. This is required on modern
        // Android and prevents a cold-started service from being killed during initialization.
        startForegroundNow()
        session = ImuSessionStore(this)
        verdictStore = VerdictStore(this)
        offloadExporter = OffloadExporter(this)
        batteryBenchStore = BatteryBenchStore(this)
        cloudUploader = CloudUploader(this)
        geoTracker = GeoTracker(cloudUploader, work.executor(ServiceWorkQueue.Priority.LOW, "geo.io"))
        bleClient = BleImuClient(applicationContext, this)
        armProcessWatchdog()
        scheduleOtaPoll(OTA_FIRST_DELAY_MS)
        scheduleOperatorPoll(8_000L)
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
            ACTION_CHECK_OTA -> runOtaPoll(force = true)
            ACTION_SYNC_OTA_CHANNEL -> syncOtaChannelToDevice(force = true)
            else -> startBleRelayMode()
        }
        ensureRelayAlive()
        return START_STICKY
    }

    override fun onTaskRemoved(rootIntent: Intent?) {
        scheduleRestart("task removed")
        super.onTaskRemoved(rootIntent)
    }

    override fun onDestroy() {
        isRunning = false
        scheduleRestart("service destroyed")
        work.cancel("ota.poll")
        work.cancel("operator.poll")
        bleClient.disconnect()
        geoTracker.stop()
        work.shutdown()
        callbacks.kill()
        super.onDestroy()
    }

    override fun onTrimMemory(level: Int) {
        super.onTrimMemory(level)
        Log.w(TAG, "onTrimMemory level=$level")
    }

    private fun scheduleRestart(reason: String) {
        longArrayOf(1_000L, 8_000L, 30_000L, 90_000L).forEachIndexed { i, delay ->
            armRestartAlarm(1001 + i, delay)
        }
        Log.i(TAG, "service restart scheduled: $reason")
    }

    private fun armRestartAlarm(requestCode: Int, delayMs: Long) {
        val alarm = getSystemService(AlarmManager::class.java) ?: return
        val restart = PendingIntent.getBroadcast(
            this,
            requestCode,
            Intent(this, ServiceRestartReceiver::class.java).setAction(
                ServiceRestartReceiver.ACTION_RESTART,
            ),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        val at = SystemClock.elapsedRealtime() + delayMs
        val canExact = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            alarm.canScheduleExactAlarms()
        } else {
            true
        }
        try {
            if (canExact) {
                alarm.setExactAndAllowWhileIdle(AlarmManager.ELAPSED_REALTIME_WAKEUP, at, restart)
            } else {
                alarm.setAndAllowWhileIdle(AlarmManager.ELAPSED_REALTIME_WAKEUP, at, restart)
            }
        } catch (e: SecurityException) {
            alarm.setAndAllowWhileIdle(AlarmManager.ELAPSED_REALTIME_WAKEUP, at, restart)
            Log.w(TAG, "exact alarm denied, using idle alarm: ${e.message}")
        }
    }

    private fun runOtaPoll(force: Boolean) {
        if (OtaSession.blocksNewRequest()) {
            if (!force || OtaSession.state.phase != OtaSession.Phase.CHECKING) {
                Log.i(TAG, "ota poll skipped — ${OtaSession.state.phase}")
                return
            }
        }
        OtaSession.io.execute {
            try {
                val appLabel = OtaSession.installedAppLabel(this)
                val fwName = lastDeviceStatus?.fwVersion
                    ?: OtaSettings(this).lastFwVersion.ifBlank { "?" }
                val fwCode = lastDeviceStatus?.fwVersionCode ?: OtaSettings(this).lastFwVersionCode
                val fwLabel = if (fwCode > 0) "$fwName ($fwCode)" else fwName
                if (!OtaSession.beginCheck(appLabel, fwLabel)) return@execute
                val result = OtaCoordinator(
                    this,
                    bleConnected = { connected },
                    otaCapable = { caps == 0 || (caps and ImuProtocol.CAP_OTA) != 0 },
                    liveFwCode = { lastDeviceStatus?.fwVersionCode ?: 0 },
                    liveFwName = { lastDeviceStatus?.fwVersion.orEmpty() },
                ).inspect(force)
                when (result) {
                    is OtaCoordinator.Result.App -> {
                        val apk = result.apk
                        OtaSession.offerApp(appLabel, "${apk.versionName} (${apk.versionCode})", fwLabel)
                        OtaOfferHub.publish(
                            this,
                            OtaOffer(
                                kind = OtaOffer.Kind.APK,
                                title = getString(R.string.ota_prompt_apk_title),
                                body = getString(
                                    R.string.ota_prompt_apk_body,
                                    result.currentName,
                                    result.currentCode,
                                    apk.versionName,
                                    apk.versionCode,
                                ),
                                versionLabel = apk.versionName,
                                urls = apk.downloadUrls(),
                                size = apk.size,
                                sha256 = apk.sha256,
                                currentLabel = "${result.currentName} (${result.currentCode})",
                                apkVersionCode = apk.versionCode,
                            ),
                        )
                        OtaSession.decline()
                    }
                    is OtaCoordinator.Result.Firmware -> {
                        val fw = result.fw
                        val offer = "${fw.version} (${fw.versionCode})"
                        OtaSession.offerFw(appLabel, "${result.currentName} (${result.currentCode})", offer)
                        OtaOfferHub.publish(
                            this,
                            OtaOffer(
                                kind = OtaOffer.Kind.FW,
                                title = getString(R.string.ota_prompt_fw_title),
                                body = getString(
                                    R.string.ota_prompt_fw_body,
                                    result.currentName,
                                    result.currentCode,
                                    fw.version,
                                    fw.versionCode,
                                ),
                                versionLabel = fw.version,
                                urls = fw.downloadUrls(),
                                size = fw.size,
                                sha256 = fw.sha256,
                                currentLabel = "${result.currentName} (${result.currentCode})",
                                fwVersionCode = fw.versionCode,
                            ),
                        )
                        OtaSession.decline()
                    }
                    is OtaCoordinator.Result.None -> OtaSession.noneAvailable(result.reason)
                }
                Log.i(TAG, "ota inspect ${result::class.simpleName}: ${
                    when (result) {
                        is OtaCoordinator.Result.None -> result.reason
                        is OtaCoordinator.Result.App -> result.apk.versionName
                        is OtaCoordinator.Result.Firmware -> result.fw.version
                    }
                }")
            } catch (e: Exception) {
                Log.w(TAG, "ota poll: ${e.message}")
                OtaSession.fail(e.message ?: "check failed")
            }
        }
    }

    private fun pollOperatorStatus() {
        low("operator.fetch") {
            try {
                val api = CloudOperatorApi(this)
                if (!api.configured() || api.deviceId().isBlank()) return@low
                val op = api.operatorStatus(api.deviceId()) ?: return@low
                val alert = op.operatorAlert
                if (alert == lastOperatorAlert) return@low
                lastOperatorAlert = alert
                ui("ui.operator.banner") {
                    if (alert) {
                        broadcastBanner(StatusBannerLevel.ERROR, "Operator ALARM — page the mechanic")
                    } else if (op.operatorLevel >= 1) {
                        broadcastBanner(StatusBannerLevel.WARN, "Operator WARN — trend, not a single window")
                    }
                }
            } catch (e: Exception) {
                Log.w(TAG, "operator poll: ${e.message}")
            }
        }
    }

    override fun onStatus(text: String) {
        session.lastStatus = text
        val lower = text.lowercase()
        if (lower.startsWith("connecting") || lower.startsWith("scanning")) {
            relayFsmCaption = text
            updateNotification(force = true)
        }
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

    override fun onPipelineNeedsReconnect(reason: String) {
        if (OtaSession.skipPipelineReconnect()) {
            Log.w(TAG, "BLE pipeline reconnect skipped during firmware OTA upload ($reason)")
            return
        }
        Log.w(TAG, "BLE pipeline reconnect: $reason")
        if (!(uiVisible || userConnectedSession)) {
            return
        }
        if (bleClient.isConnectBusy()) {
            return
        }
        val msg = "ESP stalled — reconnecting…"
        session.lastStatus = msg
        broadcastBanner(StatusBannerLevel.WARN, msg)
        bleClient.disconnect()
        high("ble.pipeline.reconnect", 600L) {
            requestBleConnect(fullSession = true, reason = msg)
        }
    }

    private fun syncOtaChannelToDevice(force: Boolean) {
        if (!connected) return
        val phone = CloudSettings(applicationContext).otaChannel
        val esp = lastDeviceStatus?.otaChannel
        if (!force && esp != null && esp.equals(phone, ignoreCase = true)) {
            lastPushedOtaChannel = phone
            return
        }
        if (!force && lastPushedOtaChannel == phone &&
            SystemClock.uptimeMillis() - lastOtaChannelPushMs < 30_000L
        ) {
            return
        }
        lastPushedOtaChannel = phone
        lastOtaChannelPushMs = SystemClock.uptimeMillis()
        high("ble.ota.channel") {
            bleClient.setOtaChannel(phone) { ok ->
                Log.i(TAG, "sync ota channel=$phone espWas=$esp ok=$ok")
            }
        }
    }

    override fun onPollStats(seq: Long, recordCount: Int, pollMs: Int) {
        throttleTelemetry("seq=$seq n=$recordCount poll=${pollMs}ms")
    }

    private var lastClockSyncedUi: Boolean? = null
    private var lastReportedClockCorrMs = -1L
    private var lastVibroCaption: String? = null

    override fun onDeviceStatus(status: ImuProtocol.Status) {
        lastDeviceStatus = status
        mergeDeviceCaps(status.feat)
        OtaSettings(applicationContext).noteFw(status.fwVersion.orEmpty(), status.fwVersionCode ?: 0)
        maybeCompleteFirmwareOta(status)
        syncOtaChannelToDevice(force = false)
        val synced = status.clockSynced == true
        if (lastClockSyncedUi != synced) {
            lastClockSyncedUi = synced
            val tz = status.clockTzMin ?: 0
            foreachCallback { it.onClockState(synced, tz) }
            updateNotification(force = true)
        }
        if (connected && status.clockSynced != true) {
            val now = SystemClock.uptimeMillis()
            if (now - lastTimeSyncRetryMs > 12_000L) {
                lastTimeSyncRetryMs = now
                high("ble.time.retry") { bleClient.requestTimeSyncRetry() }
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
        status.wrssiDbm?.let { lastWrssi = it }
        maybeRelayLinkRssi(status.seq)
        val now = SystemClock.uptimeMillis()
        if (now - lastTelemetryExportMs >= 30_000L &&
            (status.chipTempC != null || status.cpuMhzApplied != null || status.spoolCapB != null)
        ) {
            lastTelemetryExportMs = now
            low("cloud.telemetry") {
                offloadExporter.exportTelemetry(status)
                CloudUploadScheduler.enqueueNow(applicationContext)
            }
        }
        if (!status.repair &&
            status.vibroVerdictLevel != null &&
            (status.pendingSessionSeq ?: 0L) != lastStoredVerdictSeq &&
            (status.pendingSessionSeq ?: 0L) > 0L
        ) {
            lastStoredVerdictSeq = status.pendingSessionSeq ?: status.seq
            low("cloud.verdict") {
                verdictStore.record(status)
                offloadExporter.exportVerdict(status)
                CloudUploadScheduler.enqueueNow(applicationContext)
                high("ble.offload.schedule") { scheduleFlushOffloadAcks(0) }
            }
        } else if (
            !status.repair &&
            (status.offloadPending ?: 0) > 0 &&
            status.vibroVerdictLevel != null &&
            offloadExporter.lineCount() == 0
        ) {
            low("cloud.verdict.catchup") {
                offloadExporter.exportVerdict(status)
                CloudUploadScheduler.enqueueNow(applicationContext)
                high("ble.offload.schedule") { scheduleFlushOffloadAcks(0) }
            }
        } else if ((status.offloadPending ?: 0) > 0) {
            status.pendingSessionSeq?.takeIf { it > 0L }?.let { ps ->
                low("cloud.verdict.offline") {
                    if (status.vibroRmsG != null) {
                        verdictStore.recordOfflineSession(status, ps)
                        offloadExporter.exportVerdict(status.copy(seq = ps))
                    }
                    high("ble.offload.schedule") { scheduleFlushOffloadAcks(0) }
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
        if (OffloadAckStore.highWater(applicationContext) >
            maxOf(status.offloadAckSeq ?: 0L, lastLocallyAckedSeq)
        ) {
            scheduleFlushOffloadAcks(80)
        }
        if (extras.isNotEmpty()) {
            throttleTelemetry(extras.joinToString(" "))
        }
        broadcastVibroCaptionIfChanged(formatVibroCaption(status))
        handleBatteryBenchStatus(status)
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
        if (status.repair) parts.add("repair")
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
            if (OtaSession.firmwareRestarting()) {
                fwOtaAwaitingFreshStatus = true
                userConnectedSession = true
            }
            mt200ScanSentThisSession = false
            connectFailureStreak = 0
            captionEpoch++
            work.cancel("ble.reconnect.watchdog")
            cancelFsmTimers()
            cancelPendingStatusUpdates()
            clockCheckedThisSession = false
            lastReportedClockCorrMs = -1L
            lastVibroCaption = null
            bleClient.connectedDeviceAddress()?.let { addr ->
                session.lastBleAddress = addr
                DeviceIdHelper.maybeSyncCloudDeviceId(CloudSettings(applicationContext), addr)
            }
            if (userConnectedSession) {
                enterRelayState(RelayFsmState.CONNECTED, "Connected — live IMU")
            } else if (bleRelayActive) {
                // Caption set once here; the branch below (TIME sync -> crash drain) reuses it
                // instead of overwriting it a second time with the same "connected" transition.
                enterRelayState(RelayFsmState.CONNECTED, "Connected — TIME sent, fetching crashes…")
            } else {
                enterRelayState(RelayFsmState.CONNECTED, "Connected — fetching ESP data…")
            }
            startForegroundNow()
            if (otaEraseResumePending) {
                scheduleOtaResumeAfterErase()
            }
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
                high("ble.wake.onConnect", ImuProtocol.ESP_CONNECT_SETTLE_MS) {
                    if (!this@ImuBleForegroundService.connected) return@high
                    WakeRelay.onConnect(bleClient, priorStatus)
                }
            }
            scheduleFlushOffloadAcks(ImuProtocol.ESP_CONNECT_SETTLE_MS)
            high("ble.bench.read", ImuProtocol.ESP_CONNECT_SETTLE_MS + 400L) {
                if (!this@ImuBleForegroundService.connected) return@high
                bleClient.readBatteryBenchState { active, sid, seq ->
                    if (active && sid > 0L) {
                        benchUserActive = true
                        benchFwActive = true
                        bleClient.setBenchPoll(true)
                        applyBenchChar(true, sid, seq)
                    }
                }
            }
            high("ble.mt200.start", ImuProtocol.ESP_CONNECT_SETTLE_MS + 1500L) {
                if (!this@ImuBleForegroundService.connected) return@high
                maybeStartMt200Bridge()
            }
            if (bleRelayActive && !userConnectedSession) {
                // Single connect authority: after ESP grace — crash drain then bridge/cloud.
                Log.i(TAG, "Relay connected — crash drain after ${ImuProtocol.ESP_CONNECT_SETTLE_MS}ms settle")
                normal("crash.relay.confirmed", ImuProtocol.ESP_CONNECT_SETTLE_MS) {
                    if (!this@ImuBleForegroundService.connected) return@normal
                    relayCrashesUntilConfirmed {
                        if (pendingBridgeWork && this@ImuBleForegroundService.connected) {
                            beginBridgeWorkThenFinish()
                        } else {
                            finishBleRelaySession("crash relay done")
                        }
                    }
                }
            } else {
                /* Full / user session: drain every pending ring slot until empty — same
                 * until-confirmed loop as relay (not one-shot timed retries). */
                Log.i(TAG, "User session — crash drain until empty after settle")
                normal("crash.relay.confirmed", ImuProtocol.ESP_CONNECT_SETTLE_MS) {
                    if (!this@ImuBleForegroundService.connected) return@normal
                    relayCrashesUntilConfirmed { }
                }
            }
        } else {
            stopForegroundIfIdle()
            work.cancel("ui.telemetry.flush")
            work.cancelPrefix("crash.relay.")
            work.cancel("bridge.finish")
            work.cancel("ble.offload.flush")
            offloadAckInFlight = false
            lastLocallyAckedSeq = 0L
            pendingTelemetry = null
            lastEspRssi = ImuProtocol.RSSI_UNAVAIL
            lastWrssi = ImuProtocol.RSSI_UNAVAIL
            caps = 0
            bleClient.setBenchPoll(false)
            broadcastCaps()
            maybeRelayLinkRssi(0L)
            if (wasConnected) {
                // An unexpected mid-session drop during bridge work (e.g. supervision timeout)
                // never reaches finishBridgeSyncCycle() — clear it here too. pendingBridgeWork is
                // intentionally kept so the still-pending sync is retried on the next connect.
                if (bridgeSyncActive) {
                    Log.w(TAG, "Bridge work dropped mid-session — will retry on next connect")
                    bridgeSyncActive = false
                    work.cancel("bridge.finish")
                }
                if (OtaSession.firmwareRestarting()) {
                    fwOtaAwaitingFreshStatus = true
                    userConnectedSession = true
                    scheduleReconnectPause("Firmware OTA · reconnecting…")
                } else if (OtaSession.awaitingSlotEraseReconnect()) {
                    userConnectedSession = true
                    scheduleReconnectPause("Firmware OTA · erasing slot…")
                } else if (userConnectedSession) {
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
        if (benchUserActive || benchFwActive) {
            pushBenchLive(power.voltageV, power.percent)
        }
    }

    override fun onBatteryBenchChar(active: Boolean, sessionId: Long, sampleSeq: Long) {
        if (active || benchFwActive) {
            applyBenchChar(active, sessionId, sampleSeq)
        }
    }

    override fun onBatch(batch: ImuProtocol.Batch) {
        if (batch.mode == ImuProtocol.MODE_RAW && batch.raw.isNotEmpty()) {
            vibroBuffer.ingestRawBatch(batch)
            rawSampling.onBatch(batch, vibroBuffer)?.let { hint ->
                broadcastBanner(hint.level, hint.message)
            }
        }
        broadcastBatch(ImuProtocol.batchToUiJson(batch))
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
        maybeRelayAhrs(json)
        maybeRelayImuDeadReckon(json)
        maybeRelayWearable(json)
        maybeRelayBench(json)
        broadcastBatch(json)
        maybeBroadcastBleStats()
    }

    private var lastBleStatsMs = 0L

    private fun maybeBroadcastBleStats() {
        val now = SystemClock.uptimeMillis()
        if (now - lastBleStatsMs < 400L) return
        lastBleStatsMs = now
        val rx = bleClient.rxBytes
        val tx = bleClient.txBytes
        val rssi = lastEspRssi
        foreachCallback { it.onBleStats(rx, tx, rssi) }
    }

    private var lastAhrsRelayMs = 0L

    /** Throttled relay of the "rot4" (int16 x10000 rotation matrix) DATA JSON field to the
     *  backend's live AHRS endpoint for the web debug page — see CloudUploader.uploadAhrsSample.
     *  onBatchJson fires at BLE-tick rate (~30-90 Hz); ~200ms is plenty for a debug viewer and
     *  keeps this from hammering the network/battery. Best-effort: parse/network failures are
     *  swallowed, never surfaced to the user or retried. */
    private fun maybeRelayAhrs(json: String) {
        if (!cloudUploader.isEnabledForAhrs()) return
        val now = System.currentTimeMillis()
        if (now - lastAhrsRelayMs < 200L) return
        lastAhrsRelayMs = now
        runCatching {
            val root = org.json.JSONObject(json)
            val rot4 = root.optJSONArray("rot4") ?: return
            if (rot4.length() != 9) return
            val rot = DoubleArray(9) { i -> rot4.optInt(i, 0) / 10000.0 }
            val seq = root.optLong("s", 0L)
            low("cloud.ahrs") {
                runCatching { cloudUploader.uploadAhrsSample(seq, now, rot) }
            }
        }
    }

    private var lastImuDeadReckonMs = 0L

    /** Throttled feed of "wdcm"/"yawd100" (firmware v143+) into GeoTracker's dead-reckoning —
     *  see GeoTracker.onImuSample. ~1s cadence is plenty for a walking-pace demo trace. */
    private fun maybeRelayImuDeadReckon(json: String) {
        val now = System.currentTimeMillis()
        if (now - lastImuDeadReckonMs < 400L) return
        runCatching {
            val root = org.json.JSONObject(json)
            if (!root.has("wdcm") || !root.has("yawd100")) return
            lastImuDeadReckonMs = now
            val walkCm = root.optInt("wdcm", 0)
            val yawDeg = root.optInt("yawd100", 0) / 100.0
            geoTracker.onImuSample(walkCm, yawDeg, now)
        }
    }

    private var lastWearableRelayMs = 0L
    private var lastWhr = -1
    private var lastWsp = -1
    private var lastWst = -1
    private var lastWbat = -1
    private var lastWkcal = -1
    private var lastWdst = -1
    private var lastWdcm = -1
    private var lastEspRssi = ImuProtocol.RSSI_UNAVAIL
    private var lastWrssi = ImuProtocol.RSSI_UNAVAIL
    private var lastUploadedEspRssi = Int.MIN_VALUE
    private var lastUploadedWrssi = Int.MIN_VALUE
    private var lastWearableSeq = 0L
    /** Phone-side upload counter. BLE DATA `s` can freeze on RSSI-only flushes. */
    private var wearableUploadSeq = 0L
    private var mt200ScanSentThisSession = false

    override fun onEspRssi(rssiDbm: Int) {
        lastEspRssi = ImuProtocol.normalizeRssiDbm(rssiDbm)
        maybeRelayLinkRssi(lastWearableSeq)
    }

    /** Throttled relay of firmware DATA piggyback fields (whr/wsp/wst/wbat/wkcal/wdst/wok + wdcm) to
     *  POST /v1/ingest/wearable. wok=0 means the ESP32 has never locked a watch sample;
     *  IMU walk_cm still uploads so Grafana can compare against MT200 steps. RSSI rides
     *  the same POST: phone-measured ESP hop, ESP-measured MT200 hop. */
    private fun maybeRelayWearable(json: String) {
        if (!cloudUploader.isEnabledForAhrs()) return
        runCatching {
            val root = org.json.JSONObject(json)
            val wok = root.optInt("wok", 0)
            val walkCm = if (root.has("wdcm")) root.optInt("wdcm", 0) else null
            if (root.has("wrssi")) {
                lastWrssi = ImuProtocol.normalizeRssiDbm(root.optInt("wrssi"))
            }
            lastWearableSeq = root.optLong("s", lastWearableSeq)
            val hr = if (wok != 0) root.optInt("whr", 0).takeIf { it in 30..220 } else null
            val spo2 = if (wok != 0) root.optInt("wsp", 0).takeIf { it in 70..100 } else null
            val steps = if (wok != 0 && root.has("wst")) root.optInt("wst", 0) else null
            val bat = if (wok != 0) root.optInt("wbat", 0).takeIf { it in 1..100 } else null
            val kcalX10 = if (wok != 0 && root.has("wkcal")) root.optInt("wkcal", 0).takeIf { it >= 0 } else null
            val distMm = if (wok != 0 && root.has("wdst")) root.optInt("wdst", 0).takeIf { it >= 0 } else null
            flushWearable(lastWearableSeq, hr, spo2, steps, bat, walkCm, kcalX10, distMm)
        }
    }

    private fun maybeRelayLinkRssi(seq: Long) {
        flushWearable(seq, null, null, null, null, null, null, null)
    }

    private fun flushWearable(
        seq: Long,
        hr: Int?,
        spo2: Int?,
        steps: Int?,
        bat: Int?,
        walkCm: Int?,
        kcalX10: Int?,
        distMm: Int?,
    ) {
        if (!cloudUploader.isEnabledForAhrs()) return
        val now = System.currentTimeMillis()
        val hrV = hr ?: -1
        val spo2V = spo2 ?: -1
        val stepsV = steps ?: -1
        val batV = bat ?: -1
        val walkV = walkCm ?: -1
        val kcalV = kcalX10 ?: -1
        val distV = distMm ?: -1
        val rssiChanged = lastEspRssi != lastUploadedEspRssi || lastWrssi != lastUploadedWrssi
        val metricsChanged =
            hrV != lastWhr || spo2V != lastWsp || stepsV != lastWst ||
                batV != lastWbat || walkV != lastWdcm || kcalV != lastWkcal || distV != lastWdst
        val hasMetrics = hr != null || spo2 != null || steps != null || bat != null ||
            walkCm != null || kcalX10 != null || distMm != null
        if (!hasMetrics && !rssiChanged && now - lastWearableRelayMs < 15_000L) return
        if (hasMetrics && !metricsChanged && !rssiChanged && now - lastWearableRelayMs < 15_000L) {
            return
        }
        if (now - lastWearableRelayMs < 2_000L) return
        lastWearableRelayMs = now
        if (hasMetrics) {
            lastWhr = hrV
            lastWsp = spo2V
            lastWst = stepsV
            lastWbat = batV
            lastWdcm = walkV
            lastWkcal = kcalV
            lastWdst = distV
        }
        lastUploadedEspRssi = lastEspRssi
        lastUploadedWrssi = lastWrssi
        /* -127 is "not measured", not a weak link. Leave it out of the series. */
        val rssiEsp = lastEspRssi.takeIf { it > -120 && it < 20 }
        val rssiMt200 = lastWrssi.takeIf { it > -120 && it < 20 }
        if (wearableUploadSeq == 0L && seq > 0L) {
            wearableUploadSeq = seq
        }
        wearableUploadSeq += 1L
        val uploadSeq = wearableUploadSeq
        Log.i(
            TAG,
            "Wearable relay seq=$uploadSeq ble_s=$seq hr=$hr spo2=$spo2 steps=$steps bat=$bat " +
                "kcalX10=$kcalX10 distMm=$distMm walkCm=$walkCm " +
                "rssiEsp=$rssiEsp rssiMt200=$rssiMt200",
        )
        val sendHr = if (hasMetrics) hr else null
        val sendSpo2 = if (hasMetrics) spo2 else null
        val sendSteps = if (hasMetrics) steps else null
        val sendBat = if (hasMetrics) bat else null
        val sendWalk = if (hasMetrics) walkCm else null
        val sendKcal = if (hasMetrics) kcalX10 else null
        val sendDist = if (hasMetrics) distMm else null
        low("cloud.wearable") {
            runCatching {
                cloudUploader.uploadWearableSamples(
                    uploadSeq,
                    now,
                    sendHr,
                    sendSpo2,
                    sendSteps,
                    sendBat,
                    sendWalk,
                    rssiEsp = rssiEsp,
                    rssiMt200 = rssiMt200,
                    kcalX10 = sendKcal,
                    distanceMm = sendDist,
                )
            }
        }
    }

    private fun maybeStartMt200Bridge() {
        if (mt200ScanSentThisSession) return
        if (!bleClient.crashServiceAvailable) return
        mt200ScanSentThisSession = true
        bleClient.writeCrashCtrl("{\"op\":\"mt200_scan\"}") { ok ->
            Log.i(TAG, "MT200 scan trigger ${if (ok) "ok" else "failed"}")
        }
    }

    override fun onCaps(caps: Int) {
        mergeDeviceCaps(caps)
    }

    /** OR STATUS `feat` with GATT CHAR_CAPS so handshake still announces features
     *  when the CAPS characteristic is missed. */
    private fun mergeDeviceCaps(incoming: Int) {
        if (incoming == 0) return
        val merged = caps or incoming
        val changed = merged != caps
        caps = merged
        if (changed) {
            Log.i(TAG, "caps=${ImuProtocol.capsCaption(caps)} (0x${caps.toString(16)})")
            pushSessionRestoreToAll()
        }
        broadcastCaps()
    }

    private fun broadcastCaps() {
        foreachCallback {
            try {
                it.onCaps(caps)
            } catch (_: Exception) {
            }
        }
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

    /** Locally-known "high water mark" of the seq we've successfully ACKed. STATUS is only
     *  polled every 5s, so `lastDeviceStatus.offloadAckSeq` lags well behind an ACK we just sent —
     *  without this, the self-reschedule below kept re-computing the *same* seqToAck against the
     *  stale status and firing duplicate ACKs every ~800ms until the next STATUS poll finally
     *  caught up (the "offload ACK seq=N — ring rotated" spam seen on serial, one seq repeated
     *  4-10x). Reset on disconnect since a firmware reboot restarts seq numbering from scratch. */
    private var lastLocallyAckedSeq = 0L

    /**
     * Single-flight scheduler for [flushOffloadAcks]. Keyed [ServiceWorkQueue] post replaces any
     * pending flush so overlapping triggers cannot pile up independent timers.
     */
    private fun scheduleFlushOffloadAcks(delayMs: Long) {
        high("ble.offload.flush", delayMs) { flushOffloadAcks() }
    }

    /** One GATT write: high-water seq the backend (or local queue if cloud off) has taken. */
    private fun flushOffloadAcks() {
        if (!connected || offloadAckInFlight) return
        val status = lastDeviceStatus ?: return
        val lastAck = maxOf(status.offloadAckSeq ?: 0L, lastLocallyAckedSeq)
        val cloudHw = OffloadAckStore.highWater(applicationContext)
        val seqToAck = when {
            cloudHw > lastAck -> cloudHw
            !CloudSettings(applicationContext).enabled ->
                status.pendingSessionSeq?.takeIf { it > lastAck }
            else -> null
        } ?: return

        offloadAckInFlight = true
        bleClient.ackOffloadSeq(seqToAck) { ok ->
            high("ble.offload.ack.done") {
                offloadAckInFlight = false
                if (ok) {
                    lastLocallyAckedSeq = seqToAck
                    low("cloud.verdict.ack") { verdictStore.markAcked(seqToAck) }
                }
            }
        }
    }

    /** Step 3b: drain every pending crash ring slot until empty (or max rounds). */
    private fun scheduleCrashRelayAfterSettle() {
        work.cancelPrefix("crash.relay.")
        normal("crash.relay.confirmed", ImuProtocol.ESP_CONNECT_SETTLE_MS) {
            if (!connected) return@normal
            relayCrashesUntilConfirmed { }
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
        high("ble.connect") {
            if (generation != bleConnectGeneration) {
                Log.i(TAG, "Connect superseded: $reason")
                return@high
            }
            if (connected && fullSession && bleClient.isFullSessionUp()) {
                return@high
            }
            if (bleClient.isConnectBusy()) {
                Log.i(TAG, "Connect deferred — GATT busy ($reason)")
                high("ble.connect.busy", 2000) {
                    if (generation != bleConnectGeneration || connected) {
                        return@high
                    }
                    if (!bleClient.isConnectBusy()) {
                        bleClient.connect(session.lastBleAddress)
                    } else {
                        requestBleConnect(fullSession, reason)
                    }
                }
                return@high
            }
            bleClient.connect(session.lastBleAddress)
        }
    }

    private fun connectRetryPauseMs(): Long = when {
        OtaSession.firmwareRestarting() -> FW_OTA_RECONNECT_MS
        bleClient.expectingWifiRadioDrop() -> WIFI_SCAN_RECONNECT_MS
        userConnectedSession -> MANUAL_CONNECT_RETRY_MS
        uiVisible -> UI_RELAY_PAUSE_MS
        else -> RELAY_PAUSE_MS
    }

    private fun finishBleRelaySession(reason: String) {
        val keepWearable = connected && CloudSettings(applicationContext).enabled
        if (connected && bleRelayActive && !userConnectedSession && !uiVisible && !keepWearable) {
            bleClient.disconnect()
        }
        if (keepWearable && !userConnectedSession && !uiVisible) {
            bleClient.startWearableDataPoll()
            enterRelayState(RelayFsmState.CONNECTED, "Connected — wearable relay")
            Log.i(TAG, "Keeping BLE link for wearable relay ($reason)")
            return
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
            low("cloud.uploadAll") {
                val upload = cloudUploader.uploadAll()
                high("fsm.afterCloud") {
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
                low("cloud.clock") {
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
        high("ble.connect.retry", pause) { attemptAutoConnect() }
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
            high("ble.connect.retry", 2000L) { attemptAutoConnect() }
            return
        }
        if (!shouldAutoConnectRetry()) {
            return
        }
        connectAttemptSeq++
        val msg = getString(R.string.notification_scanning)
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
            if (connected) {
                bleClient.restoreHighRateLink()
            }
            if (connected && !userConnectedSession) {
                Log.i(TAG, "UI foregrounded during minimal relay session — upgrading to full BLE session")
                userConnectedSession = true
                autoPromotedFullSession = true
                cancelFsmTimers()
                cancelPendingStatusUpdates()
                bleClient.setMinimalRelayConnect(false)
                bleClient.restoreHighRateLink()
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
            Log.i(TAG, "Crash drain: fetched ${crashes.size} pending crash(es)")
            if (crashes.isEmpty()) {
                onDone()
                return@fetchAllPendingCrashes
            }
            low("crash.drain.upload") {
                /* Also keep a local copy for CloudUploadScheduler retries. */
                for (info in crashes) {
                    offloadExporter.exportCrashJson(CrashFetcher.toOffloadJson(info))
                }
                /* Direct POST of the fetched list — do not rely solely on draining the
                 * offload file (scheduler can empty it between export and upload). */
                val upload = cloudUploader.uploadCrashInfos(crashes)
                Log.i(
                    TAG,
                    "Crash drain: upload ok=${upload.ok} accepted=${upload.accepted} " +
                        "duplicates=${upload.duplicates} msg=${upload.message}",
                )
                high("crash.drain.followup") {
                    if (!connected) {
                        onDone()
                        return@high
                    }
                    if (upload.ok && upload.accepted > 0) {
                        clearDeviceCrashSlots(crashes)
                        val first = crashes.first()
                        broadcastBanner(StatusBannerLevel.OK, "OK!")
                        broadcastStatus(
                            "Crash relayed: ${crashes.size}x (${first.reason})",
                            important = true,
                        )
                        normal("crash.drain.next", 500) { relayCrashesUntilConfirmed(round + 1, onDone) }
                    } else if (upload.ok && upload.duplicates > 0 && upload.accepted == 0) {
                        /* Only clear on pure-duplicate when every fetched crash was accounted
                         * for — never clear on empty/"no crashes pending" races. */
                        clearDeviceCrashSlots(crashes)
                        val first = crashes.first()
                        broadcastStatus(
                            "Crash already in cloud (seq ${first.seq}) — ESP slot cleared",
                            important = true,
                        )
                        normal("crash.drain.next", 500) { relayCrashesUntilConfirmed(round + 1, onDone) }
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
                        normal("crash.drain.retry", CRASH_RELAY_RETRY_MS) {
                            relayCrashesUntilConfirmed(round + 1, onDone)
                        }
                    } else {
                        CloudUploadScheduler.enqueueNow(applicationContext)
                        normal("crash.drain.retry", CRASH_RELAY_RETRY_MS) {
                            relayCrashesUntilConfirmed(round + 1, onDone)
                        }
                    }
                }
            }
        }
    }

    private fun clearDeviceCrashSlots(crashes: List<CrashFetcher.CrashInfo>) {
        val slots = crashes.mapNotNull { it.slot.takeIf { s -> s >= 0 } }
        Log.i(TAG, "Crash drain: clearing slots=$slots (from ${crashes.size} crash(es))")
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
            low("crash.pending.upload") {
                for (info in crashes) {
                    offloadExporter.exportCrashJson(CrashFetcher.toOffloadJson(info))
                }
                val upload = cloudUploader.uploadPendingCrashes(crashes.size.coerceAtLeast(1))
                high("crash.pending.ui") {
                    if (!connected) return@high
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
        startForegroundNow()
        work.cancel("bridge.tick")
        if (BridgeSyncSettings(this).scheduled) {
            scheduleInternalBridgeNext(BridgeSyncScheduler.FIRST_SYNC_DELAY_MS)
            // Autopilot requires an active BLE relay cycle to drain buffers.
            if (!relayFsmActive()) {
                startBleRelayMode()
            }
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
        startForegroundNow()
        Log.i(
            TAG,
            "BLE relay FSM started (warmup ${BT_WARMUP_MS / 1000}s, pause ${RELAY_PAUSE_MS / 1000}s)",
        )
        startRelayFsm()
        updateNotification(force = true)
    }

    /** Keep the scan → connect → pause → scan loop armed for the whole service life. */
    private fun ensureRelayAlive() {
        if (!bleRelayActive || !relayFsmStarted) {
            startBleRelayMode()
        } else if (!connected && relayFsmState == RelayFsmState.PAUSE) {
            val now = SystemClock.uptimeMillis()
            if (reconnectDueAtMs == 0L || now > reconnectDueAtMs + 5_000L) {
                onFsmPauseComplete()
            }
        }
        armProcessWatchdog()
        updateNotification(force = true)
    }

    private fun armProcessWatchdog() {
        armRestartAlarm(1002, WATCHDOG_MS)
    }

    private fun startRelayFsm() {
        relayFsmStarted = true
        cancelFsmTimers()
        enterRelayState(RelayFsmState.STARTING, "Background service started")
        if (btWarmupDone) {
            high("fsm.scan") { beginScanConnect("FSM resumed") }
            return
        }
        high("fsm.warmup.enter", 300L) {
            enterRelayState(
                RelayFsmState.BT_WARMUP,
                "Bluetooth warmup ${BT_WARMUP_MS / 1000}s…",
            )
            high("fsm.warmup", BT_WARMUP_MS) { onFsmWarmupComplete() }
        }
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
        work.cancel("ble.reconnect.watchdog")
        if (connected || (!relayFsmActive() && !userConnectedSession)) {
            return
        }
        val delay = fixedDelayMs ?: run {
            val untilDue = reconnectDueAtMs - SystemClock.uptimeMillis() + 3_000L
            untilDue.coerceIn(5_000L, 45_000L)
        }
        high("ble.reconnect.watchdog", delay) { onReconnectWatchdog() }
    }

    private fun beginScanConnect(reason: String) {
        if (!relayFsmActive() || userConnectedSession || connected) {
            return
        }
        enterRelayState(RelayFsmState.SCAN_CONNECT, getString(R.string.notification_scanning) + " ($reason)")
        attemptAutoConnect()
    }

    private fun scheduleReconnectPause(caption: String) {
        if (!relayFsmActive() && !userConnectedSession) {
            return
        }
        enterRelayState(RelayFsmState.PAUSE, caption)
        val pauseMs = when {
            OtaSession.awaitingSlotEraseReconnect() -> FW_OTA_ERASE_RECONNECT_MS
            OtaSession.firmwareRestarting() -> FW_OTA_RECONNECT_MS
            bleClient.expectingWifiRadioDrop() -> WIFI_SCAN_RECONNECT_MS
            userConnectedSession -> MANUAL_CONNECT_RETRY_MS
            uiVisible -> UI_RELAY_PAUSE_MS
            else -> RELAY_PAUSE_MS
        }
        reconnectDueAtMs = SystemClock.uptimeMillis() + pauseMs
        cancelFsmTimers()
        high("fsm.pause", pauseMs) { onFsmPauseComplete() }
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
        work.cancel("ui.telemetry.flush")
    }

    private fun cancelFsmTimers() {
        work.cancel("ble.connect.retry")
        work.cancel("fsm.warmup")
        work.cancel("fsm.warmup.enter")
        work.cancel("fsm.pause")
        // reconnect watchdog intentionally kept — safety net if pause callback is lost
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
        updateNotification(force = true)
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
        work.cancel("bridge.tick")
        work.cancel("ble.connect.retry")
        if (bridgeSyncActive && !userConnectedSession) {
            bridgeSyncActive = false
            work.cancel("bridge.finish")
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
            high("bridge.autoRef.stop", 12_000L) {
                bleClient.vibroRefStop {
                    autoRefInProgress = false
                }
            }
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
                high("fsm.pause") { onFsmPauseComplete() }
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
        startForegroundNow()
        savedPollMsForBridge = session.pollMs
        bleClient.setPollIntervalMs(2000)
        syncConfigThenBridgeSetup()
    }

    /** Handshake config reconciliation — see ConfigCloudSync.reconcile() for the priority rule
     *  (device wins unless the cloud is strictly newer). Called from both the manual
     *  requestConfigSync() AIDL entrypoint and the periodic background bridge sync, i.e. every
     *  point where the phone freshly reads the ESP's live config. Runs on LOW (network). */
    private fun reconcileConfigWithCloud(doc: DeviceConfigJson.Doc, blob: ByteArray) {
        low("cloud.config.reconcile") {
            when (val result = runCatching { ConfigCloudSync.reconcile(applicationContext, doc, blob) }.getOrNull()) {
                is ConfigCloudSync.ReconcileResult.PushToDevice -> {
                    bleClient.pushConfigToDevice(result.blob, true) { ok ->
                        broadcastBanner(
                            if (ok) StatusBannerLevel.OK else StatusBannerLevel.ERROR,
                            if (ok) "Cloud config (rev ${result.cloudRevision}) applied to device"
                            else "Cloud config push to device failed",
                        )
                    }
                }
                is ConfigCloudSync.ReconcileResult.UploadedToCloud, null -> Unit
            }
        }
    }

    private fun syncConfigThenBridgeSetup() {
        high("bridge.config.sync") {
            bleClient.syncConfigFromDevice { blob ->
                if (blob != null) {
                    session.saveLocalConfig(blob)
                    broadcastConfig(blob)
                    val doc = DeviceConfigJson.fromBlob(blob, "esp")
                    reconcileConfigWithCloud(doc, blob)
                }
                high("bridge.config.after") {
                    if (!connected || !bridgeSyncActive) {
                        return@high
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
        work.cancel("bridge.tick")
        val delay = overrideDelayMs ?: settings.intervalMs(this).coerceAtLeast(30_000L)
        normal("bridge.tick", delay) { runInternalBridgeTick() }
        BridgeSyncScheduler.scheduleNext(applicationContext)
    }

    private fun scheduleBridgeFinish() {
        work.cancel("bridge.finish")
        val dwellMs = EspRendezvous.suggestedDwellSec(this, lastDeviceStatus) * 1000L
        normal("bridge.finish", dwellMs) { completeBridgeSyncCycle() }
    }

    /** Bridge work done — hand off to the single relay pause/reconnect/cloud-sync path. */
    private fun completeBridgeSyncCycle() {
        if (!bridgeSyncActive) {
            return
        }
        bridgeSyncActive = false
        work.cancel("bridge.finish")
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
        if (!work.isQueued("ui.telemetry.flush")) {
            val delay = TELEMETRY_UI_MS - (now - lastTelemetryUiMs)
            ui("ui.telemetry.flush", delay.coerceAtLeast(1L)) { flushPendingTelemetry() }
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
        ui("ui.banner") {
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
        val payload = runCatching {
            if (!::geoTracker.isInitialized) return@runCatching json
            val root = org.json.JSONObject(json)
            geoTracker.appendSnapshot(root)
            root.toString()
        }.getOrDefault(json)
        foreachCallback { it.onBatchJson(payload) }
    }

    private fun broadcastConfig(blob: ByteArray) {
        foreachCallback { it.onConfigBlob(blob) }
    }

    private fun scheduleFirmwareOtaReconnect() {
        fwOtaAwaitingFreshStatus = true
        userConnectedSession = true
        broadcastBanner(StatusBannerLevel.WARN, "DFU done — reconnecting…")
        high("ble.ota.reconnect", FW_OTA_RECONNECT_MS) {
            if (!OtaSession.firmwareRestarting()) {
                return@high
            }
            if (connected) {
                bleClient.disconnect()
            }
            requestBleConnect(fullSession = true, reason = "DFU done — reconnecting…")
        }
    }

    private fun maybeCompleteFirmwareOta(status: ImuProtocol.Status) {
        if (!OtaSession.firmwareRestarting() || !fwOtaAwaitingFreshStatus || !connected) {
            return
        }
        val liveCode = status.fwVersionCode ?: 0
        val liveName = status.fwVersion.orEmpty()
        if (liveCode <= 0 && liveName.isBlank()) {
            return
        }
        val offerCode = OtaSession.offeredFwVersionCode()
        val prevCode = OtaSession.previousFwVersionCode()
        val liveLabel = if (liveCode > 0) {
            "${liveName.ifBlank { "fw" }} ($liveCode)"
        } else {
            liveName
        }
        fwOtaAwaitingFreshStatus = false
        val message: String
        val ok: Boolean
        when {
            offerCode > 0 && liveCode == offerCode -> {
                message = "Firmware OTA · $liveLabel confirmed"
                ok = true
            }
            offerCode > 0 && liveCode > 0 && liveCode != offerCode -> {
                val hint = if (prevCode > 0 && liveCode == prevCode) {
                    "swap reverted"
                } else {
                    "unexpected image"
                }
                message = "Firmware OTA · still $liveLabel — $hint"
                ok = false
            }
            else -> {
                message = "Firmware OTA · board back · $liveLabel"
                ok = true
            }
        }
        OtaSession.complete(message, ok = ok)
        broadcastBanner(if (ok) StatusBannerLevel.OK else StatusBannerLevel.WARN, message)
        broadcastOtaDone(ok, message)
    }

    private fun startFirmwareFile(file: File) {
        if (OtaSession.blocksNewRequest() && !OtaSession.locked()) {
            broadcastOtaDone(false, "OTA already running")
            return
        }
        val live = lastDeviceStatus
        val liveLabel = live?.fwVersionCode?.takeIf { it > 0 }?.let { code ->
            "${live.fwVersion.orEmpty()} ($code)"
        } ?: live?.fwVersion.orEmpty()
        if (!OtaSession.occupyFirmwareUpload(
                currentFw = OtaSession.state.currentFw.ifBlank { liveLabel },
                offerFw = OtaSession.state.offerFw,
            )
        ) {
            if (!OtaSession.locked() || OtaSession.state.kind != OtaSession.Kind.FIRMWARE) {
                broadcastOtaDone(false, "OTA already running")
                return
            }
        }
        low("ota.fw.read") {
            val bytes = runCatching { file.readBytes() }.getOrNull()
            if (bytes == null || bytes.isEmpty()) {
                high("ota.fw.fail") {
                    OtaSession.fail("could not read firmware")
                    broadcastOtaDone(false, "could not read firmware")
                }
                return@low
            }
            Log.i(TAG, "OTA firmware ${bytes.size} B from ${file.name}")
            high("ota.fw.upload") {
                startFirmwareUpload(bytes, resumeAfterErase = false)
            }
        }
    }

    private fun startFirmwareUpload(bytes: ByteArray, resumeAfterErase: Boolean) {
        bleClient.uploadFirmware(
            bytes,
            onProgress = { pct ->
                OtaSession.uploading(pct)
                broadcastOtaProgress(pct)
            },
            onDone = { ok, msg ->
                otaEraseRetryBytes = null
                otaEraseResumePending = false
                OtaSession.clearSlotEraseReconnect()
                if (ok) {
                    OtaSession.restarting(OtaSession.Kind.FIRMWARE)
                    scheduleFirmwareOtaReconnect()
                    broadcastOtaDone(true, "DFU done — reconnecting…")
                } else {
                    OtaSession.fail(msg)
                    broadcastOtaDone(false, msg)
                }
            },
            onEraseReconnect = {
                otaEraseRetryBytes = bytes
                otaEraseResumePending = true
                OtaSession.noteSlotEraseDisconnect()
                userConnectedSession = true
                broadcastBanner(
                    StatusBannerLevel.WARN,
                    "Firmware OTA · ESP erasing (~16s) — reconnecting when ready…",
                )
                Log.i(TAG, "OTA slot erase — scheduling BLE reconnect")
            },
            resumeAfterErase = resumeAfterErase,
        )
    }

    private fun scheduleOtaResumeAfterErase() {
        val bytes = otaEraseRetryBytes
        if (bytes == null || !otaEraseResumePending) {
            return
        }
        OtaSession.uploading(0)
        broadcastBanner(
            StatusBannerLevel.WARN,
            "Firmware OTA · resume upload after slot erase…",
        )
        work.cancel("ota.fw.erase.wait")
        high("ota.fw.erase.wait", FW_OTA_ERASE_READY_MS) {
            if (!connected) {
                Log.w(TAG, "OTA erase resume aborted — not connected")
                return@high
            }
            if (!otaEraseResumePending) {
                return@high
            }
            otaEraseResumePending = false
            OtaSession.clearSlotEraseReconnect()
            Log.i(TAG, "OTA resume after erase ${bytes.size} B")
            startFirmwareUpload(bytes, resumeAfterErase = true)
        }
    }

    private fun broadcastOtaProgress(pct: Int) {
        foreachCallback { it.onOtaProgress(pct) }
        updateNotification(force = true)
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

    private fun startBatteryBenchInternal(label: String) {
        if (!connected) {
            broadcastBanner(StatusBannerLevel.WARN, "Connect BLE first")
            return
        }
        if (benchUserActive || lastDeviceStatus?.benchActive == true || benchFwActive) {
            broadcastBanner(StatusBannerLevel.WARN, "Battery bench already running")
            return
        }
        benchLabel = label.ifBlank { null }
        benchUserActive = true
        benchStartedMs = System.currentTimeMillis()
        benchLastSeq = -1L
        benchLastVoltage = null
        benchLastTs = 0L
        val confirmGen = ++benchConfirmGen
        bleClient.setBatteryBench(true) { ok ->
            if (!ok) {
                abortBenchStart("Battery bench GATT write failed (is CHAR_BENCH on this firmware?)")
                return@setBatteryBench
            }
            confirmBenchStarted(confirmGen, 0)
        }
    }

    private fun abortBenchStart(message: String) {
        benchConfirmGen++
        benchUserActive = false
        benchFwActive = false
        benchStartedMs = 0L
        bleClient.setBenchPoll(false)
        broadcastBanner(StatusBannerLevel.ERROR, message)
        val power = lastPower
        broadcastBatteryBench(
            false,
            0L,
            0L,
            power?.voltageV ?: 0f,
            power?.percent ?: 0,
            0L,
            0f,
        )
    }

    private fun confirmBenchStarted(gen: Int, attempt: Int) {
        if (gen != benchConfirmGen) return
        bleClient.readBatteryBenchState { active, sid, seq ->
            if (gen != benchConfirmGen) return@readBatteryBenchState
            if (active && sid > 0L) {
                applyBenchChar(true, sid, seq)
                broadcastBanner(
                    StatusBannerLevel.OK,
                    "Battery bench started — keep USB unplugged for discharge",
                )
                return@readBatteryBenchState
            }
            if (attempt < 8) {
                normal("bench.confirm", 400L) { confirmBenchStarted(gen, attempt + 1) }
                return@readBatteryBenchState
            }
            abortBenchStart(
                "ESP refused bench start. Unplug USB-C — firmware rejects DC/USB power.",
            )
            bleClient.setBatteryBench(false, null)
        }
    }

    private fun stopBatteryBenchInternal() {
        if (!connected) return
        benchConfirmGen++
        bleClient.setBenchPoll(false)
        val prior = lastDeviceStatus
        bleClient.setBatteryBench(false) { ok ->
            benchUserActive = false
            benchFwActive = false
            if (ok && prior != null && prior.benchSessionId != null) {
                val stopSeq = (benchLastSeq + 1).coerceAtLeast(0L)
                recordBenchSample(prior, sessionStopped = true, forceSeq = stopSeq)
            }
            low("cloud.bench.stop") {
                val upload = cloudUploader.uploadPendingBatteryBench(500)
                ui("ui.bench.stop") {
                    if (upload.accepted > 0) {
                        broadcastBanner(StatusBannerLevel.OK, "Bench: uploaded ${upload.accepted} samples")
                    }
                }
            }
        }
    }

    private var benchWasActive = false
    private var lastBenchDcWarnMs = 0L

    private fun maybeRelayBench(json: String) {
        if (!benchUserActive && !benchFwActive) return
        runCatching {
            val root = org.json.JSONObject(json)
            if (!root.has("bb") && !root.has("bsid")) return@runCatching
            val active = root.optInt("bb", 0) != 0
            if (!active && !benchFwActive) return@runCatching
            val sid = root.optLong("bsid", 0L)
            val seq = root.optLong("bseq", -1L)
            applyBenchChar(active, sid, seq)
        }
    }

    private fun applyBenchChar(active: Boolean, sid: Long, seq: Long) {
        benchFwActive = active
        val power = lastPower
        val prev = lastDeviceStatus
        val merged = (prev ?: ImuProtocol.Status(
            seq = 0L,
            mode = 0,
            count = 0,
            bytes = 0,
            voltageV = power?.voltageV ?: 0f,
            percent = power?.percent ?: 0,
            powerSource = power?.source ?: ImuProtocol.POWER_UNKNOWN,
        )).copy(
            benchActive = active,
            benchSessionId = sid.takeIf { it > 0L } ?: prev?.benchSessionId,
            benchSampleSeq = if (seq >= 0L) seq else prev?.benchSampleSeq,
            voltageV = power?.voltageV ?: prev?.voltageV ?: 0f,
            percent = power?.percent ?: prev?.percent ?: 0,
            powerSource = power?.source ?: prev?.powerSource ?: ImuProtocol.POWER_UNKNOWN,
        )
        if (prev != null) {
            lastDeviceStatus = merged
        }
        handleBatteryBenchStatus(merged)
    }

    private fun pushBenchLive(voltageV: Float, pct: Int) {
        val elapsed = if (benchStartedMs > 0L) System.currentTimeMillis() - benchStartedMs else 0L
        val dtMs = if (benchLastTs > 0L) System.currentTimeMillis() - benchLastTs else 0L
        val estMa = BatteryBenchEstimator.estimateMa(voltageV, benchLastVoltage, dtMs) ?: 0f
        broadcastBatteryBench(
            benchFwActive || benchUserActive,
            benchSessionId,
            benchLastSeq.coerceAtLeast(0L),
            voltageV,
            pct,
            elapsed,
            estMa,
        )
    }

    private fun handleBatteryBenchStatus(status: ImuProtocol.Status) {
        val active = status.benchActive
        val sid = status.benchSessionId ?: 0L
        val seq = status.benchSampleSeq ?: -1L

        if (active && sid > 0L && seq >= 0L && seq != benchLastSeq) {
            recordBenchSample(status, sessionStopped = false)
            benchLastSeq = seq
            benchSessionId = sid
            val now = System.currentTimeMillis()
            benchLastVoltage = status.voltageV
            benchLastTs = now
            if (benchStartedMs == 0L) benchStartedMs = now
        }

        if (active && (status.powerSource == ImuProtocol.POWER_DC_USB ||
                lastPower?.source == ImuProtocol.POWER_DC_USB)
        ) {
            val now = SystemClock.uptimeMillis()
            if (now - lastBenchDcWarnMs > 30_000L) {
                lastBenchDcWarnMs = now
                broadcastBanner(StatusBannerLevel.WARN, "Bench on USB/DC — unplug for discharge measurement")
            }
        }

        val elapsed = if (benchStartedMs > 0L) System.currentTimeMillis() - benchStartedMs else 0L
        val dtMs = if (benchLastTs > 0L) System.currentTimeMillis() - benchLastTs else 0L
        val estMa = BatteryBenchEstimator.estimateMa(status.voltageV, benchLastVoltage, dtMs) ?: 0f

        if (active || benchUserActive || benchWasActive || benchFwActive) {
            broadcastBatteryBench(
                active,
                sid,
                seq.coerceAtLeast(0L),
                status.voltageV,
                status.percent,
                elapsed,
                estMa,
            )
        }

        if (benchWasActive && !active) {
            benchUserActive = false
            benchFwActive = false
            bleClient.setBenchPoll(false)
            benchStartedMs = 0L
            benchLastSeq = -1L
            low("cloud.bench.upload") {
                val upload = cloudUploader.uploadPendingBatteryBench(500)
                ui("ui.bench.uploaded") {
                    if (upload.accepted > 0) {
                        broadcastBanner(StatusBannerLevel.OK, "Bench ended — uploaded ${upload.accepted} samples")
                    }
                }
            }
        }
        benchWasActive = active
    }

    private fun recordBenchSample(
        status: ImuProtocol.Status,
        sessionStopped: Boolean,
        forceSeq: Long? = null,
    ) {
        val sid = status.benchSessionId ?: benchSessionId
        if (sid <= 0L) return
        val seq = forceSeq ?: status.benchSampleSeq ?: benchLastSeq.takeIf { it >= 0L } ?: return
        val now = System.currentTimeMillis()
        val sample = BatteryBenchStore.Sample(
            sessionId = sid,
            seq = seq,
            tsMs = now,
            voltageV = status.voltageV,
            pct = status.percent,
            trendV = status.trendV,
            src = status.powerSource,
            cpuMhz = status.cpuMhzApplied,
            imuHz = status.imuHzTarget,
            renderHz = status.renderHzTarget,
            chipTempC = status.chipTempC,
            uptimeMs = status.benchUptimeMs,
            sessionStartedMs = benchStartedMs.takeIf { it > 0L } ?: now,
            sessionStopped = sessionStopped,
            label = benchLabel,
            profileSnapshot = benchProfileSnapshot(status),
        )
        low("bench.sample") {
            batteryBenchStore.append(sample)
            CloudUploadScheduler.enqueueNow(applicationContext)
        }
    }

    private fun benchProfileSnapshot(status: ImuProtocol.Status): org.json.JSONObject? {
        val o = org.json.JSONObject()
        var any = false
        status.powerProfile?.let { o.put("power_profile", it); any = true }
        status.cpuMhzApplied?.let { o.put("cpu_mhz", it); any = true }
        status.imuHzTarget?.let { o.put("imu_hz", it); any = true }
        status.renderHzTarget?.let { o.put("render_hz", it); any = true }
        status.screenOn?.let { o.put("screen_on", it); any = true }
        return if (any) o else null
    }

    private fun broadcastBatteryBench(
        active: Boolean,
        sessionId: Long,
        sampleSeq: Long,
        voltageV: Float,
        pct: Int,
        elapsedMs: Long,
        estMa: Float,
    ) {
        foreachCallback {
            it.onBatteryBench(active, sessionId, sampleSeq, voltageV, pct, elapsedMs, estMa)
        }
    }

    /**
     * RemoteCallbackList.beginBroadcast()/finishBroadcast() do not support reentrancy: a nested
     * call on the same thread (e.g. a local in-process callback synchronously triggering another
     * broadcast before the outer one finishes) or a racing call from another thread both throw
     * "beginBroadcast() called while already in a broadcast" and crash the process. Guard with an
     * atomic flag and defer any nested/racing call back onto the UI queue — it will retry
     * once the in-flight broadcast has called finishBroadcast().
     */
    private fun foreachCallback(block: (IImuBleCallback) -> Unit) {
        if (!callbacksBroadcastActive.compareAndSet(false, true)) {
            ui { foreachCallback(block) }
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
        updateNotification(force = true)
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                getString(R.string.notification_channel_ble),
                NotificationManager.IMPORTANCE_HIGH,
            )
            channel.lockscreenVisibility = Notification.VISIBILITY_PUBLIC
            channel.setSound(null, null)
            channel.enableVibration(false)
            val nm = getSystemService(NotificationManager::class.java)
            nm.createNotificationChannel(channel)
        }
    }

    private fun startForegroundNow() {
        val notification = buildNotification()
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(
                NOTIFICATION_ID,
                notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE,
            )
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
    }

    private fun buildNotification(): Notification {
        val open = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(notificationBody())
            .setStyle(NotificationCompat.BigTextStyle().bigText(notificationBody()))
            .setSmallIcon(R.drawable.ic_stat_imu)
            .setLargeIcon(appIconBitmap())
            .setContentIntent(open)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setVisibility(NotificationCompat.VISIBILITY_PUBLIC)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
            .build()
    }

    private fun appIconBitmap(): Bitmap? {
        val drawable = packageManager.getApplicationIcon(applicationInfo)
        if (drawable is BitmapDrawable) {
            return drawable.bitmap
        }
        val w = drawable.intrinsicWidth.coerceAtLeast(1)
        val h = drawable.intrinsicHeight.coerceAtLeast(1)
        val bitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bitmap)
        drawable.setBounds(0, 0, canvas.width, canvas.height)
        drawable.draw(canvas)
        return bitmap
    }

    private fun notificationBody(): String {
        val ota = OtaSession.state
        val pct = ota.percent
        if (pct != null && (
                ota.phase == OtaSession.Phase.UPLOADING ||
                    ota.phase == OtaSession.Phase.DOWNLOADING ||
                    ota.phase == OtaSession.Phase.INSTALLING ||
                    ota.phase == OtaSession.Phase.RESTARTING
                )
        ) {
            return getString(R.string.notification_dfu, pct)
        }
        if (ota.phase == OtaSession.Phase.UPLOADING || ota.phase == OtaSession.Phase.RESTARTING) {
            return ota.caption.ifBlank { getString(R.string.notification_dfu, 0) }
        }
        if (!connected) {
            return relayFsmCaption.ifBlank { getString(R.string.notification_idle) }
        }
        val ntp = when (lastClockSyncedUi) {
            true -> "NTP synced"
            false -> "NTP syncing"
            null -> "link up"
        }
        val prefix = if (bridgeSyncActive) "Connected · bridge sync" else "Connected"
        return "$prefix · $ntp"
    }

    private fun updateNotification(force: Boolean = false) {
        val now = SystemClock.uptimeMillis()
        if (!force && now - lastNotificationUpdateMs < NOTIFICATION_MIN_MS) {
            return
        }
        lastNotificationUpdateMs = now
        val nm = getSystemService(NotificationManager::class.java)
        nm.notify(NOTIFICATION_ID, buildNotification())
    }
}
