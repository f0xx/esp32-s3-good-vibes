package com.esp32s3.imusim

import android.annotation.SuppressLint
import android.bluetooth.*
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import java.nio.charset.StandardCharsets
import java.util.UUID

class BleImuClient(
    private val context: Context,
    private val listener: Listener,
) {
    interface Listener {
        fun onStatus(text: String)
        fun onPollStats(seq: Long, recordCount: Int, pollMs: Int) {}
        fun onDeviceStatus(status: ImuProtocol.Status) {}
        fun onBatch(batch: ImuProtocol.Batch) {}
        fun onBatchJson(json: String) {}
        fun onPowerStatus(power: ImuProtocol.PowerStatus)
        fun onConnected(connected: Boolean)
        fun onConnectFailed(reason: String) {}
        fun onCaps(caps: Int) {}
        fun onEspRssi(rssiDbm: Int) {}
        fun onNetScan(json: String) {}
        fun onNetProfiles(json: String) {}
        fun onNetStatus(json: String) {}
        fun onBanner(level: StatusBannerLevel, text: String) {}
    }

    private val mainHandler = Handler(Looper.getMainLooper())
    private val bluetoothManager = context.getSystemService(BluetoothManager::class.java)
    private val adapter: BluetoothAdapter? = bluetoothManager?.adapter
    private var gatt: BluetoothGatt? = null
    private var pollMs = ImuProtocol.DEFAULT_POLL_MS
    private var watchdogRunnable: Runnable? = null
    private var statusPollRunnable: Runnable? = null
    private val statusPollIntervalMs = 5000L
    private val notifyFallbackMs = 2500L
    private var lastNotifyBatchAtMs = 0L
    private var pollGeneration = 0
    private var targetMode = ImuProtocol.MODE_COMPUTED
    private var lastSeq = -1L
    private var statusParseRetries = 0
    private var scanning = false
    private var bleSessionUp = false
    private var connectTimeoutRunnable: Runnable? = null
    private var gattLinkTimeoutRunnable: Runnable? = null
    private var gattPostConnectTimeoutRunnable: Runnable? = null
    private var connectFailureReported = false
    private var deviceCaps = 0
    private var netServiceAvailable = false
    var crashServiceAvailable = false
        private set
    var configServiceAvailable = false
        private set
    private var otaUploader: OtaUploader? = null
    private val cccdQueue = ArrayDeque<BluetoothGattDescriptor>()
    private var cccdGatt: BluetoothGatt? = null
    private var pendingNetScanRead: ((String?) -> Unit)? = null
    private var pendingNetProfilesRead: ((String?) -> Unit)? = null
    private var pendingCrashJsonRead: ((String?) -> Unit)? = null
    private var pendingVibroRefListRead: ((String?) -> Unit)? = null
    private var pendingFloorCalRead: ((String?) -> Unit)? = null
    private var pendingBenchRead: ((Boolean, Long, Long) -> Unit)? = null
    private var minimalRelayConnect = false
    private var fullSessionActive = false
    private var wearablePollRunnable: Runnable? = null
    private var rssiPollRunnable: Runnable? = null
    private var lastEspRssiDbm = ImuProtocol.RSSI_UNAVAIL
    private var pendingNotifyJson: String? = null
    private var notifyJsonPosted = false
    private var directConnectFallbackRunnable: Runnable? = null
    private var connectBusy = false
    private var connectSeq = 0
    private var sessionSetupPending = false
    private var timeSyncAttempts = 0
    private var timeSyncOkBannerShown = false
    private val sessionSetupRunnable = Runnable {
        val g = gatt ?: return@Runnable
        if (!bleSessionUp || !sessionSetupPending) return@Runnable
        beginGattSessionSetup(g)
    }
    private val timeSyncRetryRunnable = Runnable { retryTimeSyncIfNeeded() }

    fun isSessionUp(): Boolean = bleSessionUp

    fun isFullSessionUp(): Boolean = bleSessionUp && fullSessionActive

    fun isConnectBusy(): Boolean = connectBusy

    fun connectedDeviceAddress(): String? = gatt?.device?.address

    private sealed class GattRequest {
        abstract val tag: String

        data class ReadChar(
            val char: BluetoothGattCharacteristic,
            override val tag: String = "read",
        ) : GattRequest()

        data class WriteChar(
            val char: BluetoothGattCharacteristic,
            val payload: ByteArray,
            val onComplete: ((Boolean) -> Unit)? = null,
            override val tag: String = "write",
        ) : GattRequest()
    }

    private val gattQueue = ArrayDeque<GattRequest>()
    private var gattBusy = false
    private var gattRetryRunnable: Runnable? = null
    private var gattOpTimeoutRunnable: Runnable? = null

    private companion object {
        val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        const val CONNECT_SCAN_TIMEOUT_MS = 20_000L
        const val GATT_LINK_TIMEOUT_MS = 15_000L
        const val GATT_POST_CONNECT_TIMEOUT_MS = 24_000L
        const val GATT_FULL_SETUP_TIMEOUT_MS = 40_000L
        const val GATT_CONNECT_TIMEOUT_MS = 12_000L
        /** Force-clear a stuck GATT op (no read/write callback) — avoids hanging the queue forever. */
        const val GATT_OP_TIMEOUT_MS = 8_000L
        /** Wait for ESP connect grace before ATT service discovery (minimal relay). */
        /** Must exceed firmware BLE_CONNECT_GRACE_MS (12000) + link settle. */
        const val MINIMAL_DISCOVER_DELAY_MS = 14_000L
        const val PRE_SCAN_SETTLE_MS = 200L
        const val DIRECT_CONNECT_FALLBACK_MS = 8_000L
        const val RSSI_POLL_MS = 10_000L
    }

    fun deviceCaps(): Int = deviceCaps

    fun netAvailable(): Boolean = netServiceAvailable

    fun otaAvailable(): Boolean = (deviceCaps and ImuProtocol.CAP_OTA) != 0

    /** Background relay: skip WiFi net CCC + IMU poll burst that trips ESP32-S3 rwble asserts. */
    fun setMinimalRelayConnect(enabled: Boolean) {
        minimalRelayConnect = enabled
        if (!enabled) {
            stopWearableDataPoll()
        }
    }

    /** Slow DATA reads on a minimal (no-notify) relay link so MT200 piggyback fields reach cloud. */
    fun startWearableDataPoll(periodMs: Long = 2_000L) {
        stopWearableDataPoll()
        val tick = object : Runnable {
            override fun run() {
                if (!bleSessionUp) return
                pollDataOnly()
                mainHandler.postDelayed(this, periodMs)
            }
        }
        wearablePollRunnable = tick
        mainHandler.post(tick)
    }

    fun stopWearableDataPoll() {
        wearablePollRunnable?.let { mainHandler.removeCallbacks(it) }
        wearablePollRunnable = null
    }

    @SuppressLint("MissingPermission")
    fun connect(lastKnownAddress: String? = null) {
        mainHandler.post { startConnect(lastKnownAddress) }
    }

    @SuppressLint("MissingPermission")
    private fun startConnect(lastKnownAddress: String?) {
        connectSeq++
        connectBusy = true
        connectFailureReported = false
        if (adapter == null || !adapter.isEnabled) {
            connectBusy = false
            postBanner(StatusBannerLevel.ERROR, "Bluetooth off")
            listener.onConnectFailed("Bluetooth off")
            return
        }
        stopPoll()
        disconnectInternal(notifyListener = false)
        val addr = lastKnownAddress?.takeIf { it.isNotBlank() }
        if (addr != null) {
            tryDirectConnect(addr)
        } else {
            postStatus("Scanning for ${ImuProtocol.DEVICE_NAME}...")
            mainHandler.postDelayed({ startBleScan() }, PRE_SCAN_SETTLE_MS)
        }
    }

    /** Upgrade an existing minimal relay GATT session to full IMU notify/poll without reconnect. */
    @SuppressLint("MissingPermission")
    fun upgradeToFullSession(): Boolean {
        val g = gatt ?: return false
        if (!bleSessionUp || !minimalRelayConnect) {
            return false
        }
        minimalRelayConnect = false
        g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)
        if (g.getService(ImuProtocol.SERVICE_UUID) != null) {
            finishFullSessionSetup(g)
            return true
        }
        g.requestMtu(517)
        return true
    }

    @SuppressLint("MissingPermission")
    private fun tryDirectConnect(address: String) {
        postStatus("Direct connect $address…")
        try {
            val device = adapter?.getRemoteDevice(address) ?: run {
                postStatus("Scanning for ${ImuProtocol.DEVICE_NAME}...")
                mainHandler.postDelayed({ startBleScan() }, PRE_SCAN_SETTLE_MS)
                return
            }
            gatt = device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
            scheduleGattLinkTimeout()
            directConnectFallbackRunnable?.let { mainHandler.removeCallbacks(it) }
            directConnectFallbackRunnable = Runnable {
                if (!bleSessionUp && gatt != null && !scanning) {
                    postStatus("Direct connect slow — scanning…")
                    disconnect()
                    postStatus("Scanning for ${ImuProtocol.DEVICE_NAME}...")
                    mainHandler.postDelayed({ startBleScan() }, PRE_SCAN_SETTLE_MS)
                }
            }
            mainHandler.postDelayed(directConnectFallbackRunnable!!, DIRECT_CONNECT_FALLBACK_MS)
        } catch (_: IllegalArgumentException) {
            postStatus("Scanning for ${ImuProtocol.DEVICE_NAME}...")
            mainHandler.postDelayed({ startBleScan() }, PRE_SCAN_SETTLE_MS)
        }
    }

    @SuppressLint("MissingPermission")
    private fun startBleScan() {
        if (adapter == null || !adapter.isEnabled) {
            listener.onConnectFailed("Bluetooth off")
            return
        }
        // Unfiltered scan — ESP puts the IMU UUID in the adv packet and the name in scan
        // response only; hardware filters on some phones miss it for many seconds.
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        scanning = true
        adapter?.bluetoothLeScanner?.startScan(null, settings, scanCallback)
        connectTimeoutRunnable?.let { mainHandler.removeCallbacks(it) }
        connectTimeoutRunnable = Runnable {
            if (scanning) {
                adapter?.bluetoothLeScanner?.stopScan(scanCallback)
                scanning = false
                postBanner(StatusBannerLevel.WARN, "Device not found")
                reportConnectFailed("device not found (${CONNECT_SCAN_TIMEOUT_MS / 1000}s scan)")
            }
        }
        mainHandler.postDelayed(connectTimeoutRunnable!!, CONNECT_SCAN_TIMEOUT_MS)
    }

    @SuppressLint("MissingPermission")
    fun disconnect() {
        mainHandler.post { disconnectInternal(notifyListener = true) }
    }

    @SuppressLint("MissingPermission")
    private fun disconnectInternal(notifyListener: Boolean) {
        connectBusy = false
        sessionSetupPending = false
        stopTimeSyncRetries()
        mainHandler.removeCallbacks(sessionSetupRunnable)
        stopWearableDataPoll()
        pendingNotifyJson = null
        notifyJsonPosted = false
        stopRssiPoll()
        stopPoll()
        clearGattQueue()
        connectTimeoutRunnable?.let { mainHandler.removeCallbacks(it) }
        connectTimeoutRunnable = null
        clearGattConnectTimeouts()
        directConnectFallbackRunnable?.let { mainHandler.removeCallbacks(it) }
        directConnectFallbackRunnable = null
        if (scanning) {
            adapter?.bluetoothLeScanner?.stopScan(scanCallback)
            scanning = false
        }
        val wasUp = bleSessionUp
        gatt?.close()
        gatt = null
        bleSessionUp = false
        fullSessionActive = false
        netServiceAvailable = false
        cccdQueue.clear()
        cccdGatt = null
        pendingNetScanRead = null
        pendingNetProfilesRead = null
        pendingCrashJsonRead = null
        if (notifyListener && wasUp) {
            listener.onConnected(false)
        }
    }

    @SuppressLint("MissingPermission")
    fun setMode(mode: Int) {
        targetMode = mode
        lastSeq = -1L
        val ch = gatt?.getService(ImuProtocol.SERVICE_UUID)?.getCharacteristic(ImuProtocol.CHAR_MODE_UUID)
        if (ch != null) {
            // Queued (see setEspScreenOn's doc comment) — a direct write here can race the
            // periodic DATA/STATUS polls that share the same BluetoothGatt.
            enqueueGatt(GattRequest.WriteChar(char = ch, payload = byteArrayOf(mode.toByte())), highPriority = true)
        }
        mainHandler.postDelayed({ pollDataOnly() }, 80)
    }

    fun pollIntervalMs(): Int = pollMs

    /** Force an immediate DATA read (used during FFT sample collection). */
    fun requestDataPoll() {
        mainHandler.post { pollDataOnly() }
    }

    /** Routed through gattQueue (not a direct gatt.writeCharacteristic call) — a direct write here
     *  used to race the periodic STATUS/DATA polls (pollStatusOnly/pollDataOnly run every ~poll
     *  interval while connected), which are always in flight via the same queue. Two concurrent
     *  GATT ops on one BluetoothGatt silently drop or strand one of them (see syncTimeFromPhone's
     *  doc comment for the same class of bug) — this is what made "turn ESP display on/off" look
     *  flaky/no-op from the phone side even though the BLE write occasionally landed fine. */
    @SuppressLint("MissingPermission")
    fun setEspScreenOn(on: Boolean, onDone: ((Boolean) -> Unit)? = null) {
        val ch = gatt?.getService(ImuProtocol.SERVICE_UUID)
            ?.getCharacteristic(ImuProtocol.CHAR_SCREEN_UUID) ?: run {
            onDone?.invoke(false)
            return
        }
        enqueueGatt(
            GattRequest.WriteChar(
                char = ch,
                payload = byteArrayOf(if (on) 1 else 0),
                onComplete = { ok ->
                    if (ok) {
                        mainHandler.postDelayed({ pollStatusOnly() }, 120)
                    } else {
                        postBanner(StatusBannerLevel.ERROR, "Screen ${if (on) "on" else "off"} write failed")
                    }
                    onDone?.invoke(ok)
                },
            ),
            highPriority = true,
        )
    }

    /** mhz = 0 restores auto (mode-derived). Queued — see setEspScreenOn's doc comment. */
    @SuppressLint("MissingPermission")
    fun setCpuMhzOverride(mhz: Int, onDone: ((Boolean) -> Unit)? = null) {
        val ch = gatt?.getService(ImuProtocol.SERVICE_UUID)
            ?.getCharacteristic(ImuProtocol.CHAR_CPU_MHZ_UUID) ?: run {
            onDone?.invoke(false)
            return
        }
        enqueueGatt(
            GattRequest.WriteChar(
                char = ch,
                payload = byteArrayOf(mhz.coerceIn(0, 255).toByte()),
                onComplete = { ok ->
                    if (ok) {
                        mainHandler.postDelayed({ pollStatusOnly() }, 120)
                    } else {
                        postBanner(StatusBannerLevel.ERROR, "CPU speed override write failed")
                    }
                    onDone?.invoke(ok)
                },
            ),
            highPriority = true,
        )
    }

    /** Start (true) or stop (false) firmware battery-bench mode — config locked while active. */
    @SuppressLint("MissingPermission")
    fun setBatteryBench(start: Boolean, onDone: ((Boolean) -> Unit)? = null) {
        val ch = gatt?.getService(ImuProtocol.SERVICE_UUID)
            ?.getCharacteristic(ImuProtocol.CHAR_BENCH_UUID) ?: run {
            onDone?.invoke(false)
            return
        }
        val cmd = if (start) ImuProtocol.BENCH_CMD_START else ImuProtocol.BENCH_CMD_STOP
        enqueueGatt(
            GattRequest.WriteChar(
                char = ch,
                payload = byteArrayOf(cmd.toByte()),
                onComplete = { ok ->
                    if (ok) {
                        mainHandler.postDelayed({ pollStatusOnly() }, 200)
                    } else {
                        postBanner(StatusBannerLevel.ERROR, "Battery bench command failed")
                    }
                    onDone?.invoke(ok)
                },
            ),
            highPriority = true,
        )
    }

    @SuppressLint("MissingPermission")
    fun readBatteryBenchState(onDone: (Boolean, Long, Long) -> Unit) {
        val ch = gatt?.getService(ImuProtocol.SERVICE_UUID)
            ?.getCharacteristic(ImuProtocol.CHAR_BENCH_UUID) ?: run {
            onDone(false, 0L, 0L)
            return
        }
        pendingBenchRead = onDone
        queueRead(ch, highPriority = true)
    }

    private fun deliverBenchRead(characteristic: BluetoothGattCharacteristic) {
        val data = characteristic.value
        mainHandler.post {
            val cb = pendingBenchRead
            pendingBenchRead = null
            if (data == null || data.size < 9) {
                cb?.invoke(false, 0L, 0L)
                return@post
            }
            val active = data[0].toInt() != 0
            val sid = u32Le(data, 1)
            val seq = u32Le(data, 5)
            cb?.invoke(active, sid, seq)
        }
    }

    private fun u32Le(data: ByteArray, offset: Int): Long {
        return (data[offset].toLong() and 0xff) or
            ((data[offset + 1].toLong() and 0xff) shl 8) or
            ((data[offset + 2].toLong() and 0xff) shl 16) or
            ((data[offset + 3].toLong() and 0xff) shl 24)
    }

    /** hz = 0 restores auto (mode-derived); nonzero clamped firmware-side to [1,120].
     *  Queued — see setEspScreenOn's doc comment. */
    @SuppressLint("MissingPermission")
    fun setImuHzOverride(hz: Int, onDone: ((Boolean) -> Unit)? = null) {
        val ch = gatt?.getService(ImuProtocol.SERVICE_UUID)
            ?.getCharacteristic(ImuProtocol.CHAR_IMU_HZ_UUID) ?: run {
            onDone?.invoke(false)
            return
        }
        enqueueGatt(
            GattRequest.WriteChar(
                char = ch,
                payload = byteArrayOf(hz.coerceIn(0, 255).toByte()),
                onComplete = { ok ->
                    if (ok) {
                        mainHandler.postDelayed({ pollStatusOnly() }, 120)
                    } else {
                        postBanner(StatusBannerLevel.ERROR, "IMU sample rate override write failed")
                    }
                    onDone?.invoke(ok)
                },
            ),
            highPriority = true,
        )
    }

    @SuppressLint("MissingPermission")
    fun setPollIntervalMs(ms: Int) {
        val newMs = ms.coerceIn(33, 2000)
        val changed = newMs != pollMs
        pollMs = newMs
        val ch = gatt?.getService(ImuProtocol.SERVICE_UUID)?.getCharacteristic(ImuProtocol.CHAR_POLL_MS_UUID)
        if (ch != null && changed) {
            // Queued (see setEspScreenOn's doc comment) — a direct write here can race the
            // periodic DATA/STATUS polls that share the same BluetoothGatt.
            enqueueGatt(
                GattRequest.WriteChar(char = ch, payload = ImuProtocol.pollMsToBytes(pollMs)),
                highPriority = true,
            )
        }
        if (gatt != null && changed) {
            stopPoll()
            startPoll()
        }
    }

    private val scanCallback = object : ScanCallback() {
        @SuppressLint("MissingPermission")
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            if (!matchesTargetDevice(result)) {
                return
            }
            adapter?.bluetoothLeScanner?.stopScan(this)
            scanning = false
            connectTimeoutRunnable?.let { mainHandler.removeCallbacks(it) }
            connectTimeoutRunnable = null
            val name = result.device.name ?: result.scanRecord?.deviceName ?: "ESP32"
            postStatus("Connecting to $name...")
            gatt = result.device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
            scheduleGattLinkTimeout()
        }

        override fun onScanFailed(errorCode: Int) {
            if (!scanning) {
                return
            }
            adapter?.bluetoothLeScanner?.stopScan(this)
            scanning = false
            connectTimeoutRunnable?.let { mainHandler.removeCallbacks(it) }
            connectTimeoutRunnable = null
            postBanner(StatusBannerLevel.WARN, "BLE scan failed ($errorCode)")
            reportConnectFailed("scan failed (code=$errorCode)")
        }
    }

    private fun matchesTargetDevice(result: ScanResult): Boolean {
        val hasService = result.scanRecord?.serviceUuids?.contains(
            android.os.ParcelUuid(ImuProtocol.SERVICE_UUID),
        ) == true
        val name = result.device.name ?: result.scanRecord?.deviceName
        return name == ImuProtocol.DEVICE_NAME || hasService
    }

    private fun clearGattConnectTimeouts() {
        gattLinkTimeoutRunnable?.let { mainHandler.removeCallbacks(it) }
        gattLinkTimeoutRunnable = null
        gattPostConnectTimeoutRunnable?.let { mainHandler.removeCallbacks(it) }
        gattPostConnectTimeoutRunnable = null
    }

    private fun reportConnectFailed(reason: String) {
        if (connectFailureReported) {
            return
        }
        connectFailureReported = true
        connectBusy = false
        listener.onConnectFailed(reason)
    }

    private fun scheduleGattLinkTimeout() {
        gattLinkTimeoutRunnable?.let { mainHandler.removeCallbacks(it) }
        gattLinkTimeoutRunnable = Runnable {
            if (!bleSessionUp && gatt != null) {
                postBanner(StatusBannerLevel.WARN, "GATT link timeout")
                reportConnectFailed("GATT link timeout (${GATT_LINK_TIMEOUT_MS / 1000}s)")
                disconnect()
            }
        }
        mainHandler.postDelayed(gattLinkTimeoutRunnable!!, GATT_LINK_TIMEOUT_MS)
    }

    private fun scheduleGattPostConnectTimeout() {
        gattPostConnectTimeoutRunnable?.let { mainHandler.removeCallbacks(it) }
        val timeoutMs = if (minimalRelayConnect) {
            GATT_POST_CONNECT_TIMEOUT_MS
        } else {
            GATT_FULL_SETUP_TIMEOUT_MS
        }
        gattPostConnectTimeoutRunnable = Runnable {
            if (!bleSessionUp && gatt != null) {
                postBanner(StatusBannerLevel.WARN, "GATT setup timeout")
                reportConnectFailed("GATT setup timeout (${timeoutMs / 1000}s)")
                disconnect()
            }
        }
        mainHandler.postDelayed(gattPostConnectTimeoutRunnable!!, timeoutMs)
    }

    @SuppressLint("MissingPermission")
    private fun finishFullSessionSetup(gatt: BluetoothGatt) {
        if (fullSessionActive) {
            return
        }
        fullSessionActive = true
        sessionSetupPending = true
        enableNotify(gatt)
        enableNetNotify(gatt)
        /* TIME waits until CCCDs finish (beginGattSessionSetup). A queued TIME write
         * racing the direct writeDescriptor() in enableNotify() is silently dropped —
         * v151 connect showed NOTIFY + batches, never "TIME apply", so the phone
         * header stayed NTP FAIL even though WiFi/NTP is off by design. */
        scheduleTimeSyncRetries()
        readDeviceCaps(gatt)
        mainHandler.removeCallbacks(sessionSetupRunnable)
        mainHandler.postDelayed(sessionSetupRunnable, ImuProtocol.ESP_CONNECT_SETTLE_MS)
    }

    @SuppressLint("MissingPermission")
    private fun beginGattSessionSetup(gatt: BluetoothGatt) {
        if (!sessionSetupPending) {
            return
        }
        sessionSetupPending = false
        mainHandler.removeCallbacks(sessionSetupRunnable)
        syncTimeFromPhone(gatt)
        setMode(targetMode)
        setPollIntervalMs(pollMs)
        startPoll()
        postStatus("Polling every ${pollMs}ms")
    }

    fun requestTimeSyncRetry() {
        mainHandler.post {
            val g = gatt ?: return@post
            if (!bleSessionUp) return@post
            syncTimeFromPhone(g)
        }
    }

    private fun scheduleTimeSyncRetries() {
        timeSyncAttempts = 0
        mainHandler.removeCallbacks(timeSyncRetryRunnable)
        mainHandler.postDelayed(timeSyncRetryRunnable, 5000)
    }

    /** Public: called once the firmware confirms the clock is already synced (STATUS "clks":1),
     *  so the blind post-connect retry chain (up to 8 attempts over 40s, restarted on *every*
     *  reconnect regardless of whether a correction is even needed) doesn't keep firing pointless
     *  TIME writes for a session that's already time-synced. */
    fun stopTimeSyncRetries() {
        mainHandler.removeCallbacks(timeSyncRetryRunnable)
        timeSyncAttempts = 0
        timeSyncOkBannerShown = false
    }

    private fun retryTimeSyncIfNeeded() {
        val g = gatt ?: return
        if (!bleSessionUp || timeSyncAttempts >= 8) {
            return
        }
        timeSyncAttempts++
        syncTimeFromPhone(g)
        mainHandler.postDelayed(timeSyncRetryRunnable, 5000)
    }

    private val gattCallback = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                directConnectFallbackRunnable?.let { mainHandler.removeCallbacks(it) }
                directConnectFallbackRunnable = null
                connectTimeoutRunnable?.let { mainHandler.removeCallbacks(it) }
                connectTimeoutRunnable = null
                gattLinkTimeoutRunnable?.let { mainHandler.removeCallbacks(it) }
                gattLinkTimeoutRunnable = null
                scheduleGattPostConnectTimeout()
                postStatus("Connected — waiting for ESP grace")
                postBanner(StatusBannerLevel.OK, "Connected")
                /*
                 * Android's default (BALANCED) connection interval is ~30-50ms, which caps IMU
                 * notify throughput regardless of how fast the firmware prepares batches
                 * (ble_imu_gatt.c's ~33ms poll tick). CONNECTION_PRIORITY_HIGH asks the stack to
                 * renegotiate down to ~7.5-15ms, multiplying real notification throughput —
                 * skipped for the minimal background relay session (crash/status polling only,
                 * no streaming) to avoid needlessly draining phone battery for a link that isn't
                 * shipping high-rate data anyway.
                 */
                if (!minimalRelayConnect) {
                    gatt.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)
                }
                if (minimalRelayConnect) {
                    mainHandler.postDelayed({ gatt.discoverServices() }, MINIMAL_DISCOVER_DELAY_MS)
                } else {
                    gatt.requestMtu(517)
                }
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                connectBusy = false
                connectTimeoutRunnable?.let { mainHandler.removeCallbacks(it) }
                connectTimeoutRunnable = null
                clearGattConnectTimeouts()
                val wasUp = bleSessionUp
                stopRssiPoll()
                stopPoll()
                clearGattQueue()
                bleSessionUp = false
                fullSessionActive = false
                if (status != BluetoothGatt.GATT_SUCCESS && !wasUp && !connectFailureReported) {
                    reportConnectFailed("GATT disconnect (status=$status)")
                }
                if (wasUp) {
                    listener.onConnected(false)
                }
                postStatus("Disconnected")
            }
        }

        @SuppressLint("MissingPermission")
        override fun onReadRemoteRssi(gatt: BluetoothGatt, rssi: Int, status: Int) {
            val dbm = if (status == BluetoothGatt.GATT_SUCCESS) {
                ImuProtocol.normalizeRssiDbm(rssi)
            } else {
                ImuProtocol.RSSI_UNAVAIL
            }
            lastEspRssiDbm = dbm
            mainHandler.post { listener.onEspRssi(dbm) }
        }

        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                postBanner(StatusBannerLevel.ERROR, "Service discovery failed")
                reportConnectFailed("service discovery failed (status=$status)")
                disconnect()
                return
            }
            clearGattConnectTimeouts()
            configServiceAvailable = gatt.getService(ConfigProtocol.SERVICE_UUID) != null
            crashServiceAvailable = gatt.getService(CrashProtocol.SERVICE_UUID) != null
            if (!configServiceAvailable) {
                postBanner(StatusBannerLevel.WARN, "Config BLE service missing")
            }
            if (!crashServiceAvailable) {
                postBanner(StatusBannerLevel.WARN, "Crash BLE service missing — inject/BIST unavailable")
            }
            if (minimalRelayConnect) {
                bleSessionUp = true
                startRssiPoll()
                connectBusy = false
                mainHandler.postDelayed({
                    if (!bleSessionUp) return@postDelayed
                    syncTimeFromPhone(gatt)
                    scheduleTimeSyncRetries()
                }, ImuProtocol.ESP_CONNECT_SETTLE_MS)
                mainHandler.postDelayed({
                    if (bleSessionUp) {
                        listener.onConnected(true)
                        postStatus("Relay connect (minimal GATT)")
                    }
                }, ImuProtocol.ESP_CONNECT_SETTLE_MS)
            } else {
                finishFullSessionSetup(gatt)
                bleSessionUp = true
                startRssiPoll()
                connectBusy = false
                listener.onConnected(true)
            }
        }

        @SuppressLint("MissingPermission")
        override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
            if (gatt.getService(ImuProtocol.SERVICE_UUID) != null && !minimalRelayConnect) {
                if (!fullSessionActive) {
                    finishFullSessionSetup(gatt)
                    if (!bleSessionUp) {
                        bleSessionUp = true
                        startRssiPoll()
                        connectBusy = false
                        listener.onConnected(true)
                    }
                }
                return
            }
            gatt.discoverServices()
        }

        @SuppressLint("MissingPermission")
        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
        ) {
            when (characteristic.uuid) {
                ImuProtocol.CHAR_NOTIFY_UUID -> {
                    // Firmware v110+ ships the full batch JSON directly in the notification
                    // payload once MTU is large enough (see ble_imu_gatt.c's g_defer_notify_send
                    // handling) — a bare 4-byte seq means it fell back to the old "poke" (MTU too
                    // small, or an empty batch), so only then do we pay for a follow-up Read.
                    val value = characteristic.value
                    if (value != null && value.size >= ImuProtocol.COMPACT_HDR &&
                        value[0] == ImuProtocol.COMPACT_MAGIC
                    ) {
                        lastNotifyBatchAtMs = SystemClock.elapsedRealtime()
                        mainHandler.post { handleCompactBatch(value) }
                    } else if (value != null && value.size > 4 && value[0] == '{'.code.toByte()) {
                        val json = String(value, StandardCharsets.UTF_8)
                        if (ImuProtocol.looksLikeCompleteJson(json)) {
                            lastNotifyBatchAtMs = SystemClock.elapsedRealtime()
                            pendingNotifyJson = json
                            if (!notifyJsonPosted) {
                                notifyJsonPosted = true
                                mainHandler.post {
                                    notifyJsonPosted = false
                                    val latest = pendingNotifyJson
                                    pendingNotifyJson = null
                                    if (latest != null) {
                                        handleDataJson(latest)
                                    }
                                }
                            }
                        } else {
                            mainHandler.post { pollDataOnly() }
                        }
                    } else {
                        mainHandler.post { pollDataOnly() }
                    }
                }
                NetProtocol.CHAR_SCAN_UUID -> {
                    val json = characteristic.readUtf8()
                    mainHandler.post { listener.onNetScan(json) }
                }
                NetProtocol.CHAR_STATUS_UUID -> {
                    val json = characteristic.readUtf8()
                    mainHandler.post { listener.onNetStatus(json) }
                }
                NetProtocol.CHAR_PROFILES_UUID -> {
                    val json = characteristic.readUtf8()
                    mainHandler.post { listener.onNetProfiles(json) }
                }
            }
        }

        @SuppressLint("MissingPermission")
        override fun onCharacteristicWrite(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int,
        ) {
            mainHandler.post {
                val ok = status == BluetoothGatt.GATT_SUCCESS
                when (characteristic.uuid) {
                    ImuProtocol.CHAR_TIME_UUID -> {
                        if (ok && !timeSyncOkBannerShown) {
                            timeSyncOkBannerShown = true
                            postBanner(StatusBannerLevel.OK, "TIME sync OK")
                        } else if (!ok) {
                            postBanner(StatusBannerLevel.WARN, "TIME sync failed ($status)")
                        }
                    }
                    NetProtocol.CHAR_CMD_UUID -> {
                        if (!ok) {
                            postBanner(StatusBannerLevel.ERROR, "WiFi GATT error ($status)")
                        }
                    }
                    OtaProtocol.CHAR_CTRL_UUID, OtaProtocol.CHAR_DATA_UUID -> {
                        otaUploader?.onCharacteristicWrite(ok)
                    }
                }
                completeGattWrite(ok)
            }
        }

        @SuppressLint("MissingPermission")
        override fun onCharacteristicRead(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int,
        ) {
            mainHandler.post {
                completeGattRead()
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    if (characteristic.uuid == CrashProtocol.CHAR_INFO_UUID) {
                        pendingCrashJsonRead?.invoke(null)
                        pendingCrashJsonRead = null
                    }
                    if (characteristic.uuid == ConfigProtocol.CHAR_REFLIST_UUID) {
                        pendingVibroRefListRead?.invoke(null)
                        pendingVibroRefListRead = null
                    }
                    if (characteristic.uuid == ConfigProtocol.CHAR_FLOORCAL_UUID) {
                        pendingFloorCalRead?.invoke(null)
                        pendingFloorCalRead = null
                    }
                    postBanner(StatusBannerLevel.ERROR, "GATT read failed ($status)")
                    return@post
                }
                when (characteristic.uuid) {
                    ImuProtocol.CHAR_CAPS_UUID -> {
                        deviceCaps = ImuProtocol.parseCaps(characteristic.value ?: ByteArray(0))
                        if (deviceCaps != 0) {
                            postStatus(ImuProtocol.capsCaption(deviceCaps))
                            listener.onCaps(deviceCaps)
                        }
                        if (crashServiceAvailable && ImuProtocol.crashDebugFromCaps(deviceCaps)) {
                            postBanner(StatusBannerLevel.OK, "Crash debug BLE ready (caps DBG)")
                        }
                    }
                    ImuProtocol.CHAR_STATUS_UUID -> handleStatusRead(gatt, characteristic)
                    ImuProtocol.CHAR_DATA_UUID -> handleDataRead(characteristic)
                    NetProtocol.CHAR_SCAN_UUID -> deliverNetRead(characteristic) { listener.onNetScan(it) }
                    NetProtocol.CHAR_PROFILES_UUID -> deliverNetRead(characteristic) { listener.onNetProfiles(it) }
                    NetProtocol.CHAR_STATUS_UUID -> deliverNetRead(characteristic) { listener.onNetStatus(it) }
                    CrashProtocol.CHAR_INFO_UUID -> deliverCrashRead(characteristic)
                    ConfigProtocol.CHAR_REFLIST_UUID -> {
                        val json = characteristic.readUtf8()
                        pendingVibroRefListRead?.invoke(json)
                        pendingVibroRefListRead = null
                    }
                    ConfigProtocol.CHAR_FLOORCAL_UUID -> {
                        val json = characteristic.readUtf8()
                        pendingFloorCalRead?.invoke(json)
                        pendingFloorCalRead = null
                    }
                    ImuProtocol.CHAR_BENCH_UUID -> deliverBenchRead(characteristic)
                }
            }
        }

        @SuppressLint("MissingPermission")
        override fun onDescriptorWrite(
            gatt: BluetoothGatt,
            descriptor: BluetoothGattDescriptor,
            status: Int,
        ) {
            if (descriptor.uuid == CCCD_UUID && status == BluetoothGatt.GATT_SUCCESS) {
                writeNextCccd()
                if (cccdQueue.isEmpty() && sessionSetupPending) {
                    beginGattSessionSetup(gatt)
                }
            }
        }
    }

    private fun deliverCrashRead(characteristic: BluetoothGattCharacteristic) {
        val json = characteristic.readUtf8()
        mainHandler.post {
            pendingCrashJsonRead?.invoke(json)
            pendingCrashJsonRead = null
        }
    }

    @SuppressLint("MissingPermission")
    fun fetchAllPendingCrashes(onDone: (List<CrashFetcher.CrashInfo>) -> Unit) {
        val g = gatt ?: run {
            onDone(emptyList())
            return
        }
        val infoChar = g.getService(CrashProtocol.SERVICE_UUID)
            ?.getCharacteristic(CrashProtocol.CHAR_INFO_UUID)
        if (infoChar == null) {
            onDone(emptyList())
            return
        }
        pendingCrashJsonRead = { json ->
            when {
                json == null -> onDone(emptyList())
                CrashFetcher.isListJson(json) -> {
                    // Firmware v109+ embeds full per-slot detail in the list JSON itself — no
                    // per-slot write+read round trip needed. Older firmware only sends
                    // slot/seq/reason/pc/uptime here (no "pc" field alone would still parse via
                    // parseInfoObject, but backtrace/detail would be missing) — that's an
                    // acceptable degradation, not a correctness issue, and self-heals on reflash.
                    val detailed = CrashFetcher.parseListDetailed(json)
                    val slots = CrashFetcher.parseListSlots(json)
                    if (detailed.size >= slots.size && detailed.isNotEmpty()) {
                        onDone(detailed)
                    } else {
                        fetchCrashSlotDetails(slots, 0, mutableListOf(), onDone)
                    }
                }
                else -> {
                    val info = CrashFetcher.parseInfo(json)?.takeIf { it.pending }
                    onDone(if (info != null) listOf(info) else emptyList())
                }
            }
        }
        queueRead(infoChar, highPriority = true)
    }

    private fun fetchCrashSlotDetails(
        slots: List<Int>,
        index: Int,
        acc: MutableList<CrashFetcher.CrashInfo>,
        onDone: (List<CrashFetcher.CrashInfo>) -> Unit,
    ) {
        if (index >= slots.size) {
            onDone(acc)
            return
        }
        readCrashSlot(slots[index]) { info ->
            if (info != null) {
                acc.add(info)
            }
            fetchCrashSlotDetails(slots, index + 1, acc, onDone)
        }
    }

    @SuppressLint("MissingPermission")
    private fun readCrashSlot(slot: Int, onDone: (CrashFetcher.CrashInfo?) -> Unit) {
        val g = gatt ?: run {
            onDone(null)
            return
        }
        val svc = g.getService(CrashProtocol.SERVICE_UUID) ?: run {
            onDone(null)
            return
        }
        val ctrl = svc.getCharacteristic(CrashProtocol.CHAR_CTRL_UUID)
        val infoChar = svc.getCharacteristic(CrashProtocol.CHAR_INFO_UUID)
        if (ctrl == null || infoChar == null) {
            onDone(null)
            return
        }
        pendingCrashJsonRead = { json ->
            onDone(CrashFetcher.parseInfo(json ?: "")?.takeIf { it.pending })
        }
        enqueueGatt(
            GattRequest.WriteChar(
                char = ctrl,
                payload = "{\"op\":\"info\",\"slot\":$slot}".toByteArray(StandardCharsets.UTF_8),
            ),
            highPriority = true,
        )
        queueRead(infoChar, highPriority = true)
    }

    @SuppressLint("MissingPermission")
    fun fetchCrashIfPending(onDone: (CrashFetcher.CrashInfo?) -> Unit) {
        fetchAllPendingCrashes { list -> onDone(list.firstOrNull()) }
    }

    @SuppressLint("MissingPermission")
    fun clearDeviceCrashSlot(slot: Int, onDone: (() -> Unit)? = null) {
        val g = gatt ?: run {
            onDone?.invoke()
            return
        }
        val ctrl = g.getService(CrashProtocol.SERVICE_UUID)
            ?.getCharacteristic(CrashProtocol.CHAR_CTRL_UUID)
        if (ctrl == null) {
            onDone?.invoke()
            return
        }
        enqueueGatt(
            GattRequest.WriteChar(
                char = ctrl,
                payload = "{\"op\":\"clear\",\"slot\":$slot}".toByteArray(StandardCharsets.UTF_8),
                onComplete = { ok ->
                    if (ok) {
                        onDone?.invoke()
                    } else {
                        postBanner(StatusBannerLevel.ERROR, "Crash clear failed (slot $slot)")
                    }
                },
            ),
            highPriority = true,
        )
    }

    /** Clears several slots in one BLE write instead of one write per slot — cuts the round-trip
     *  count (and thus failure/retry surface) when multiple crashes were pending at once. */
    @SuppressLint("MissingPermission")
    fun clearDeviceCrashSlots(slots: List<Int>, onDone: (() -> Unit)? = null) {
        if (slots.isEmpty()) {
            onDone?.invoke()
            return
        }
        val g = gatt ?: run {
            onDone?.invoke()
            return
        }
        val ctrl = g.getService(CrashProtocol.SERVICE_UUID)
            ?.getCharacteristic(CrashProtocol.CHAR_CTRL_UUID)
        if (ctrl == null) {
            onDone?.invoke()
            return
        }
        val slotsJson = slots.joinToString(",")
        enqueueGatt(
            GattRequest.WriteChar(
                char = ctrl,
                payload = "{\"op\":\"clear\",\"slots\":[$slotsJson]}".toByteArray(StandardCharsets.UTF_8),
                onComplete = { ok ->
                    android.util.Log.i("BleImuClient", "clearDeviceCrashSlots($slotsJson) write ok=$ok")
                    if (ok) {
                        onDone?.invoke()
                    } else {
                        postBanner(StatusBannerLevel.ERROR, "Crash clear failed (slots $slotsJson)")
                    }
                },
            ),
            highPriority = true,
        )
    }

    @SuppressLint("MissingPermission")
    fun writeCrashCtrl(json: String, onDone: ((Boolean) -> Unit)? = null) {
        val g = gatt ?: run {
            onDone?.invoke(false)
            return
        }
        val ctrl = g.getService(CrashProtocol.SERVICE_UUID)
            ?.getCharacteristic(CrashProtocol.CHAR_CTRL_UUID)
        if (ctrl == null) {
            postBanner(
                StatusBannerLevel.ERROR,
                if (crashServiceAvailable) "Crash CTRL char missing" else "Crash BLE service missing — reflash v49+",
            )
            onDone?.invoke(false)
            return
        }
        enqueueGatt(
            GattRequest.WriteChar(
                char = ctrl,
                payload = json.toByteArray(StandardCharsets.UTF_8),
                onComplete = { ok ->
                    if (!ok) {
                        postBanner(StatusBannerLevel.ERROR, "Crash ctrl failed")
                    }
                    onDone?.invoke(ok)
                },
            ),
            highPriority = true,
        )
    }

    fun injectCrash(kind: String, onDone: ((Boolean) -> Unit)? = null) {
        writeCrashCtrl("{\"op\":\"inject\",\"kind\":\"$kind\"}", onDone)
    }

    fun runDeviceBist(onDone: ((Boolean) -> Unit)? = null) {
        writeCrashCtrl("{\"op\":\"bist\"}", onDone)
    }

    @SuppressLint("MissingPermission")
    fun eraseDeviceNvs(onDone: ((Boolean) -> Unit)? = null) {
        val g = gatt ?: run {
            onDone?.invoke(false)
            return
        }
        val cmd = g.getService(ConfigProtocol.SERVICE_UUID)
            ?.getCharacteristic(ConfigProtocol.CHAR_CMD_UUID)
        if (cmd == null) {
            postBanner(
                StatusBannerLevel.ERROR,
                if (configServiceAvailable) "Config CMD missing" else "Config BLE service missing",
            )
            onDone?.invoke(false)
            return
        }
        enqueueGatt(
            GattRequest.WriteChar(
                char = cmd,
                payload = byteArrayOf(ConfigProtocol.CMD_ERASE_NVS.toByte()),
                onComplete = { ok ->
                    if (ok) {
                        postBanner(StatusBannerLevel.WARN, "NVS erase — ESP rebooting in ~0.5s")
                    } else {
                        postBanner(StatusBannerLevel.ERROR, "NVS erase command failed")
                    }
                    onDone?.invoke(ok)
                },
            ),
            highPriority = true,
        )
    }

    @SuppressLint("MissingPermission")
    fun clearDeviceCrash(onDone: (() -> Unit)? = null) {
        val g = gatt ?: run {
            onDone?.invoke()
            return
        }
        val ctrl = g.getService(CrashProtocol.SERVICE_UUID)
            ?.getCharacteristic(CrashProtocol.CHAR_CTRL_UUID)
        if (ctrl == null) {
            onDone?.invoke()
            return
        }
        enqueueGatt(
            GattRequest.WriteChar(
                char = ctrl,
                payload = "{\"op\":\"clear\"}".toByteArray(StandardCharsets.UTF_8),
                onComplete = { ok ->
                    if (ok) {
                        onDone?.invoke()
                    } else {
                        postBanner(StatusBannerLevel.ERROR, "Crash clear failed")
                    }
                },
            ),
            highPriority = true,
        )
    }

    private fun deliverNetRead(
        characteristic: BluetoothGattCharacteristic,
        emit: (String) -> Unit,
    ) {
        val json = characteristic.readUtf8()
        mainHandler.post {
            when (characteristic.uuid) {
                NetProtocol.CHAR_SCAN_UUID -> {
                    pendingNetScanRead?.invoke(json)
                    pendingNetScanRead = null
                }
                NetProtocol.CHAR_PROFILES_UUID -> {
                    pendingNetProfilesRead?.invoke(json)
                    pendingNetProfilesRead = null
                }
            }
            if (json.isNotBlank()) {
                emit(json)
            }
        }
    }

    @SuppressLint("MissingPermission")
    private fun handleStatusRead(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
        val json = characteristic.readUtf8()
        if (!ImuProtocol.looksLikeCompleteJson(json)) {
            if (statusParseRetries < 2 && !gattBusy) {
                statusParseRetries++
                mainHandler.postDelayed({ pollStatusOnly() }, 80)
            }
            return
        }
        statusParseRetries = 0
        val st = ImuProtocol.parseStatusLenient(json)
            ?: run {
                android.util.Log.w("BleImuClient", "Status parse skip (${json.length} B)")
                return
            }
        listener.onDeviceStatus(st)
        listener.onPowerStatus(ImuProtocol.powerFromStatus(st))
    }

    private fun handleDataRead(characteristic: BluetoothGattCharacteristic) {
        handleDataJson(characteristic.readUtf8())
    }

    private fun handleCompactBatch(bytes: ByteArray) {
        val batch = ImuProtocol.parseCompact(bytes) ?: return
        if (batch.seq == lastSeq) return
        lastSeq = batch.seq
        val n = when (batch.mode) {
            ImuProtocol.MODE_RAW -> batch.raw.size
            else -> batch.computed.size
        }
        listener.onPollStats(batch.seq, n, pollMs)
        listener.onBatch(batch)
        listener.onPowerStatus(
            ImuProtocol.PowerStatus(
                source = batch.powerSource,
                voltageV = batch.voltageV,
                percent = batch.percent,
                valid = true,
                trendV = batch.trendV,
            ),
        )
    }

    /** Shared by the GATT-Read path (handleDataRead) and the notify-embedded-payload path
     *  (onCharacteristicChanged) now that firmware v110+ can ship the batch JSON directly in
     *  the notification instead of requiring a follow-up Read for every update. */
    private fun handleDataJson(json: String) {
        if (json.length < 4) {
            return
        }
        if (!ImuProtocol.looksLikeCompleteJson(json)) {
            pollDataOnly()
            return
        }
        try {
            val header = ImuProtocol.peekBatchHeader(json) ?: return
            lastNotifyBatchAtMs = SystemClock.elapsedRealtime()
            if (header.seq != lastSeq) {
                lastSeq = header.seq
                listener.onPollStats(header.seq, header.recordCount, pollMs)
                listener.onBatchJson(json)
            }
        } catch (e: Exception) {
            postBanner(StatusBannerLevel.WARN, "Batch parse error (${json.length} B)")
        }
    }

    private var pendingWriteComplete: ((Boolean) -> Unit)? = null

    private fun clearGattQueue() {
        gattQueue.clear()
        gattBusy = false
        pendingWriteComplete = null
        gattRetryRunnable?.let { mainHandler.removeCallbacks(it) }
        gattRetryRunnable = null
        cancelGattOpTimeout()
    }

    private fun enqueueGatt(req: GattRequest, highPriority: Boolean = false) {
        if (highPriority) {
            gattQueue.addFirst(req)
        } else {
            gattQueue.addLast(req)
        }
        pumpGattQueue()
    }

    private fun pumpGattQueue() {
        if (gattBusy) return
        val g = gatt ?: return
        val req = gattQueue.removeFirstOrNull() ?: return
        gattBusy = true
        when (req) {
            is GattRequest.ReadChar -> {
                if (g.readCharacteristic(req.char)) {
                    scheduleGattOpTimeout()
                    return
                }
                gattBusy = false
                scheduleGattRetry()
            }
            is GattRequest.WriteChar -> {
                pendingWriteComplete = req.onComplete
                req.char.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                req.char.value = req.payload
                if (g.writeCharacteristic(req.char)) {
                    scheduleGattOpTimeout()
                    return
                }
                pendingWriteComplete = null
                gattBusy = false
                req.onComplete?.invoke(false)
                gattQueue.addFirst(req)
                scheduleGattRetry()
            }
        }
    }

    private fun scheduleGattRetry() {
        gattRetryRunnable?.let { mainHandler.removeCallbacks(it) }
        gattRetryRunnable = Runnable {
            gattRetryRunnable = null
            pumpGattQueue()
        }
        mainHandler.postDelayed(gattRetryRunnable!!, 35)
    }

    /**
     * Defense-in-depth: if the Android BLE stack never delivers a read/write callback (e.g. a
     * concurrent non-queued GATT op raced it), gattBusy would otherwise stay stuck forever and
     * silently strand every future queued op (crash drain, TIME sync, polling). Force-clear and
     * move on so a single wedged op degrades gracefully instead of hanging the whole session.
     */
    private fun scheduleGattOpTimeout() {
        gattOpTimeoutRunnable?.let { mainHandler.removeCallbacks(it) }
        gattOpTimeoutRunnable = Runnable {
            gattOpTimeoutRunnable = null
            if (!gattBusy) return@Runnable
            android.util.Log.w("BleImuClient", "GATT op timed out (${GATT_OP_TIMEOUT_MS}ms) — clearing queue")
            gattBusy = false
            pendingWriteComplete?.invoke(false)
            pendingWriteComplete = null
            pendingCrashJsonRead?.invoke(null)
            pendingCrashJsonRead = null
            pendingNetScanRead?.invoke(null)
            pendingNetScanRead = null
            pendingNetProfilesRead?.invoke(null)
            pendingNetProfilesRead = null
            pendingBenchRead?.invoke(false, 0L, 0L)
            pendingBenchRead = null
            pumpGattQueue()
        }
        mainHandler.postDelayed(gattOpTimeoutRunnable!!, GATT_OP_TIMEOUT_MS)
    }

    private fun cancelGattOpTimeout() {
        gattOpTimeoutRunnable?.let { mainHandler.removeCallbacks(it) }
        gattOpTimeoutRunnable = null
    }

    private fun completeGattRead() {
        cancelGattOpTimeout()
        gattBusy = false
        pumpGattQueue()
    }

    private fun completeGattWrite(ok: Boolean) {
        cancelGattOpTimeout()
        pendingWriteComplete?.invoke(ok)
        pendingWriteComplete = null
        gattBusy = false
        pumpGattQueue()
    }

    private fun queueRead(char: BluetoothGattCharacteristic, highPriority: Boolean = false) {
        enqueueGatt(GattRequest.ReadChar(char), highPriority)
    }

    @SuppressLint("MissingPermission")
    private fun readDataCharacteristic(gatt: BluetoothGatt) {
        val dataChar = gatt.getService(ImuProtocol.SERVICE_UUID)
            ?.getCharacteristic(ImuProtocol.CHAR_DATA_UUID)
        if (dataChar == null) {
            return
        }
        queueRead(dataChar)
    }

    @SuppressLint("MissingPermission")
    private fun readDeviceCaps(gatt: BluetoothGatt) {
        val capsChar = gatt.getService(ImuProtocol.SERVICE_UUID)
            ?.getCharacteristic(ImuProtocol.CHAR_CAPS_UUID) ?: return
        queueRead(capsChar, highPriority = true)
    }

    @SuppressLint("MissingPermission")
    private fun syncTimeFromPhone(gatt: BluetoothGatt) {
        val ch = gatt.getService(ImuProtocol.SERVICE_UUID)?.getCharacteristic(ImuProtocol.CHAR_TIME_UUID)
            ?: return
        val tzMin = java.util.TimeZone.getDefault().getOffset(System.currentTimeMillis()) / 60_000
        val payload = ImuProtocol.timeSyncPayload(System.currentTimeMillis(), tzMin)
        // Routed through gattQueue (not a direct gatt.writeCharacteristic call) — a direct call here
        // used to race against the crash-relay's queued read fired ~1.5s later by the minimal-relay
        // connect path. Two concurrent GATT ops on one BluetoothGatt can silently strand the queue
        // (no read/write callback ever arrives), hanging crash drain until the link times out.
        enqueueGatt(
            GattRequest.WriteChar(char = ch, payload = payload),
            highPriority = true,
        )
    }

    @SuppressLint("MissingPermission")
    private fun enableNetNotify(gatt: BluetoothGatt) {
        val svc = gatt.getService(NetProtocol.SERVICE_UUID)
        netServiceAvailable = svc != null
        if (svc == null) {
            postBanner(StatusBannerLevel.WARN, "WiFi wizard unavailable on ESP")
            return
        }
        cccdGatt = gatt
        cccdQueue.clear()
        for (uuid in listOf(
                NetProtocol.CHAR_SCAN_UUID,
                NetProtocol.CHAR_STATUS_UUID,
                NetProtocol.CHAR_PROFILES_UUID,
            )) {
            val ch = svc.getCharacteristic(uuid) ?: continue
            gatt.setCharacteristicNotification(ch, true)
            val cccd = ch.getDescriptor(CCCD_UUID) ?: continue
            cccd.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
            cccdQueue.addLast(cccd)
        }
        writeNextCccd()
    }

    @SuppressLint("MissingPermission")
    private fun writeNextCccd() {
        val g = cccdGatt ?: return
        val desc = cccdQueue.removeFirstOrNull() ?: return
        if (g.writeDescriptor(desc) != true) {
            mainHandler.postDelayed({ writeNextCccd() }, 30)
        }
    }

    @SuppressLint("MissingPermission")
    fun sendNetCommand(json: String): Boolean {
        val g = gatt ?: run {
            postStatus("Not connected")
            return false
        }
        if (!netServiceAvailable) {
            postBanner(StatusBannerLevel.WARN, "WiFi BLE service missing on ESP")
            return false
        }
        val ch = g.getService(NetProtocol.SERVICE_UUID)?.getCharacteristic(NetProtocol.CHAR_CMD_UUID)
            ?: run {
                postBanner(StatusBannerLevel.ERROR, "WiFi CMD characteristic missing")
                return false
            }
        enqueueGatt(
            GattRequest.WriteChar(
                char = ch,
                payload = json.toByteArray(StandardCharsets.UTF_8),
                onComplete = { ok ->
                    if (ok) {
                        scheduleNetStatusReadFallback()
                    } else {
                        postBanner(StatusBannerLevel.ERROR, "WiFi command write failed")
                    }
                },
            ),
            highPriority = true,
        )
        return true
    }

    @SuppressLint("MissingPermission")
    fun readNetStatus(onDone: (String?) -> Unit) {
        val g = gatt ?: run {
            onDone(null)
            return
        }
        val ch = g.getService(NetProtocol.SERVICE_UUID)?.getCharacteristic(NetProtocol.CHAR_STATUS_UUID)
        if (ch == null) {
            onDone(null)
            return
        }
        enqueueGatt(
            GattRequest.ReadChar(ch),
            highPriority = true,
        )
        mainHandler.postDelayed({
            onDone(ch.readUtf8().takeIf { it.isNotBlank() })
        }, 180)
    }

    private fun scheduleNetStatusReadFallback() {
        for (delay in listOf(400L, 1500L, 4000L, 10000L, 16000L, 22000L)) {
            mainHandler.postDelayed({
                readNetStatus { json ->
                    if (!json.isNullOrBlank()) listener.onNetStatus(json)
                }
            }, delay)
        }
    }

    @SuppressLint("MissingPermission")
    fun requestNetScan() {
        val savedPoll = pollMs
        setPollIntervalMs(500)
        if (!sendNetCommand("""{"op":"scan"}""")) {
            setPollIntervalMs(savedPoll)
            return
        }
        scheduleNetScanReadFallback()
        mainHandler.postDelayed({ setPollIntervalMs(savedPoll) }, 20_000)
    }

    @SuppressLint("MissingPermission")
    fun requestNetProfiles() {
        if (!sendNetCommand("""{"op":"profiles"}""")) return
        scheduleNetProfilesReadFallback()
    }

    private fun scheduleNetScanReadFallback() {
        val delays = listOf(800L, 3000L, 8000L, 15000L)
        for (delay in delays) {
            mainHandler.postDelayed({
                readNetScan { json ->
                    if (!json.isNullOrBlank()) listener.onNetScan(json)
                }
            }, delay)
        }
    }

    private fun scheduleNetProfilesReadFallback() {
        mainHandler.postDelayed({ readNetProfiles { json ->
            if (!json.isNullOrBlank()) listener.onNetProfiles(json)
        } }, 400)
        mainHandler.postDelayed({ readNetProfiles { json ->
            if (!json.isNullOrBlank()) listener.onNetProfiles(json)
        } }, 1500)
    }
    @SuppressLint("MissingPermission")
    fun readNetScan(onDone: (String?) -> Unit) {
        val g = gatt ?: run {
            onDone(null)
            return
        }
        val ch = g.getService(NetProtocol.SERVICE_UUID)?.getCharacteristic(NetProtocol.CHAR_SCAN_UUID)
        if (ch == null) {
            onDone(null)
            return
        }
        pendingNetScanRead = onDone
        queueRead(ch, highPriority = true)
    }

    @SuppressLint("MissingPermission")
    fun readNetProfiles(onDone: (String?) -> Unit) {
        val g = gatt ?: run {
            onDone(null)
            return
        }
        val ch = g.getService(NetProtocol.SERVICE_UUID)?.getCharacteristic(NetProtocol.CHAR_PROFILES_UUID)
        if (ch == null) {
            onDone(null)
            return
        }
        pendingNetProfilesRead = onDone
        queueRead(ch, highPriority = true)
    }

    @SuppressLint("MissingPermission")
    fun readVibroRefList(onDone: (String?) -> Unit) {
        val g = gatt ?: run {
            onDone(null)
            return
        }
        val ch = g.getService(ConfigProtocol.SERVICE_UUID)?.getCharacteristic(ConfigProtocol.CHAR_REFLIST_UUID)
        if (ch == null) {
            onDone(null)
            return
        }
        pendingVibroRefListRead = onDone
        queueRead(ch, highPriority = true)
    }

    /** Flat-floor mounting calibration status JSON — see floor_calib.h. */
    @SuppressLint("MissingPermission")
    fun readFloorCalStatus(onDone: (String?) -> Unit) {
        val g = gatt ?: run {
            onDone(null)
            return
        }
        val ch = g.getService(ConfigProtocol.SERVICE_UUID)?.getCharacteristic(ConfigProtocol.CHAR_FLOORCAL_UUID)
        if (ch == null) {
            onDone(null)
            return
        }
        pendingFloorCalRead = onDone
        queueRead(ch, highPriority = true)
    }

    @SuppressLint("MissingPermission")
    private fun enableNotify(gatt: BluetoothGatt) {
        val notify = gatt.getService(ImuProtocol.SERVICE_UUID)?.getCharacteristic(ImuProtocol.CHAR_NOTIFY_UUID)
        if (notify != null) {
            gatt.setCharacteristicNotification(notify, true)
            val cccd = notify.getDescriptor(UUID.fromString("00002902-0000-1000-8000-00805f9b34fb"))
            cccd?.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
            gatt.writeDescriptor(cccd)
        }
    }

    /** Notify-driven: read DATA only (power is in batch header). STATUS polled separately. */
    @SuppressLint("MissingPermission")
    private fun pollDataOnly() {
        val g = gatt ?: return
        readDataCharacteristic(g)
    }

    @SuppressLint("MissingPermission")
    private fun pollStatusOnly() {
        if (gattBusy) return
        val g = gatt ?: return
        val statusChar = g.getService(ImuProtocol.SERVICE_UUID)
            ?.getCharacteristic(ImuProtocol.CHAR_STATUS_UUID) ?: return
        queueRead(statusChar)
    }

    private fun notifyBatchRecent(): Boolean {
        return lastNotifyBatchAtMs > 0L &&
            SystemClock.elapsedRealtime() - lastNotifyBatchAtMs < notifyFallbackMs
    }

    /** Notify + watchdog backup; STATUS on its own timer for vibro/temp. */
    private fun startPoll() {
        stopPoll()
        pollDataOnly()
        scheduleWatchdog()
        scheduleStatusPoll()
    }

    private fun stopPoll() {
        pollGeneration++
        lastNotifyBatchAtMs = 0L
        watchdogRunnable?.let { mainHandler.removeCallbacks(it) }
        watchdogRunnable = null
        statusPollRunnable?.let { mainHandler.removeCallbacks(it) }
        statusPollRunnable = null
        clearGattQueue()
    }

    private fun scheduleWatchdog() {
        val gen = pollGeneration
        watchdogRunnable?.let { mainHandler.removeCallbacks(it) }
        watchdogRunnable = Runnable {
            if (gen != pollGeneration) return@Runnable
            if (!gattBusy && !notifyBatchRecent()) {
                pollDataOnly()
            }
            scheduleWatchdog()
        }
        val watchdogMs = if (notifyBatchRecent()) 1000L else pollMs.toLong().coerceAtLeast(33L)
        mainHandler.postDelayed(watchdogRunnable!!, watchdogMs)
    }

    private fun scheduleStatusPoll() {
        val gen = pollGeneration
        statusPollRunnable?.let { mainHandler.removeCallbacks(it) }
        statusPollRunnable = Runnable {
            if (gen != pollGeneration) return@Runnable
            if (!gattBusy) {
                pollStatusOnly()
            }
            scheduleStatusPoll()
        }
        mainHandler.postDelayed(statusPollRunnable!!, statusPollIntervalMs)
    }

    @SuppressLint("MissingPermission")
    private fun requestRemoteRssi() {
        val g = gatt ?: return
        if (!bleSessionUp) return
        g.readRemoteRssi()
    }

    private fun startRssiPoll() {
        stopRssiPoll()
        requestRemoteRssi()
        rssiPollRunnable = object : Runnable {
            override fun run() {
                if (!bleSessionUp) return
                requestRemoteRssi()
                mainHandler.postDelayed(this, RSSI_POLL_MS)
            }
        }
        mainHandler.postDelayed(rssiPollRunnable!!, RSSI_POLL_MS)
    }

    private fun stopRssiPoll() {
        rssiPollRunnable?.let { mainHandler.removeCallbacks(it) }
        rssiPollRunnable = null
        lastEspRssiDbm = ImuProtocol.RSSI_UNAVAIL
    }

    private fun postStatus(text: String) {
        mainHandler.post { listener.onStatus(text) }
    }

    private fun postBanner(level: StatusBannerLevel, text: String) {
        mainHandler.post { listener.onBanner(level, text) }
    }

    @SuppressLint("MissingPermission")
    fun syncConfigFromDevice(onDone: (ByteArray?) -> Unit) {
        val g = gatt ?: run {
            onDone(null)
            return
        }
        val ch = g.getService(ConfigProtocol.SERVICE_UUID)?.getCharacteristic(ConfigProtocol.CHAR_DATA_UUID)
        if (ch == null) {
            onDone(null)
            return
        }
        g.readCharacteristic(ch)
        mainHandler.postDelayed({
            val blob = ch.value
            if (blob != null && blob.size >= ConfigProtocol.BLOB_SIZE) {
                onDone(blob.copyOf(ConfigProtocol.BLOB_SIZE))
            } else {
                onDone(null)
            }
        }, 250)
    }

    @SuppressLint("MissingPermission")
    fun pushConfigToDevice(blob: ByteArray, commit: Boolean, onDone: (Boolean) -> Unit) {
        val g = gatt ?: run {
            onDone(false)
            return
        }
        val data = g.getService(ConfigProtocol.SERVICE_UUID)?.getCharacteristic(ConfigProtocol.CHAR_DATA_UUID)
        val cmd = g.getService(ConfigProtocol.SERVICE_UUID)?.getCharacteristic(ConfigProtocol.CHAR_CMD_UUID)
        if (data == null || cmd == null) {
            onDone(false)
            return
        }
        data.value = blob.copyOf(ConfigProtocol.BLOB_SIZE)
        g.writeCharacteristic(data)
        if (commit) {
            mainHandler.postDelayed({
                cmd.value = byteArrayOf(ConfigProtocol.CMD_COMMIT.toByte())
                g.writeCharacteristic(cmd)
                onDone(true)
            }, 120)
        } else {
            onDone(true)
        }
    }

    @SuppressLint("MissingPermission")
    fun uploadFirmware(bytes: ByteArray, onProgress: (Int) -> Unit, onDone: (Boolean, String) -> Unit) {
        val g = gatt ?: run {
            onDone(false, "not connected")
            return
        }
        if (otaUploader?.let { true } == true) {
            onDone(false, "OTA already in progress")
            return
        }
        otaUploader = OtaUploader(g, mainHandler, onProgress) { ok, msg ->
            otaUploader = null
            onDone(ok, msg)
        }
        otaUploader?.start(bytes)
    }

    @SuppressLint("MissingPermission")
    fun ackOffloadSeq(seq: Long, onDone: ((Boolean) -> Unit)? = null) {
        val g = gatt ?: run {
            onDone?.invoke(false)
            return
        }
        val cmd = g.getService(ConfigProtocol.SERVICE_UUID)?.getCharacteristic(ConfigProtocol.CHAR_CMD_UUID)
            ?: run {
                onDone?.invoke(false)
                return
            }
        val payload = ByteArray(5)
        payload[0] = ConfigProtocol.CMD_OFFLOAD_ACK.toByte()
        payload[1] = (seq and 0xFF).toByte()
        payload[2] = ((seq shr 8) and 0xFF).toByte()
        payload[3] = ((seq shr 16) and 0xFF).toByte()
        payload[4] = ((seq shr 24) and 0xFF).toByte()
        enqueueGatt(
            GattRequest.WriteChar(
                char = cmd,
                payload = payload,
                onComplete = onDone,
            ),
            highPriority = false,
        )
    }

    @SuppressLint("MissingPermission")
    fun sendConfigCmd(payload: ByteArray, onDone: ((Boolean) -> Unit)? = null) {
        val g = gatt ?: run {
            onDone?.invoke(false)
            return
        }
        val cmd = g.getService(ConfigProtocol.SERVICE_UUID)?.getCharacteristic(ConfigProtocol.CHAR_CMD_UUID)
            ?: run {
                onDone?.invoke(false)
                return
            }
        enqueueGatt(
            GattRequest.WriteChar(
                char = cmd,
                payload = payload,
                onComplete = onDone,
            ),
            highPriority = true,
        )
    }

    fun sendConfigCmd(cmdByte: Byte, onDone: ((Boolean) -> Unit)? = null) =
        sendConfigCmd(byteArrayOf(cmdByte), onDone)

    /** `slot` 0..VIBRO_REF_SLOT_COUNT-1; `name` optional (truncated to VIBRO_REF_NAME_MAX_LEN). */
    fun vibroRefStart(slot: Int = 0, name: String = "", onDone: ((Boolean) -> Unit)? = null) {
        val nameBytes = name.toByteArray(StandardCharsets.UTF_8)
            .let { if (it.size > ConfigProtocol.VIBRO_REF_NAME_MAX_LEN) it.copyOf(ConfigProtocol.VIBRO_REF_NAME_MAX_LEN) else it }
        val payload = byteArrayOf(ConfigProtocol.CMD_VIBRO_REF_START.toByte(), slot.toByte()) + nameBytes
        sendConfigCmd(payload, onDone)
    }

    fun vibroRefStop(onDone: ((Boolean) -> Unit)? = null) =
        sendConfigCmd(ConfigProtocol.CMD_VIBRO_REF_STOP.toByte(), onDone)

    fun vibroRefSelect(slot: Int, onDone: ((Boolean) -> Unit)? = null) =
        sendConfigCmd(byteArrayOf(ConfigProtocol.CMD_VIBRO_REF_SELECT.toByte(), slot.toByte()), onDone)

    fun vibroRefDelete(slot: Int, onDone: ((Boolean) -> Unit)? = null) =
        sendConfigCmd(byteArrayOf(ConfigProtocol.CMD_VIBRO_REF_DELETE.toByte(), slot.toByte()), onDone)

    fun vibroRefClearAll(onDone: ((Boolean) -> Unit)? = null) =
        sendConfigCmd(ConfigProtocol.CMD_VIBRO_REF_CLEAR_ALL.toByte(), onDone)

    fun vibroArm(onDone: ((Boolean) -> Unit)? = null) =
        sendConfigCmd(ConfigProtocol.CMD_VIBRO_ARM.toByte(), onDone)

    /** Start flat-floor mounting calibration; device must sit still on a true-level reference
     * for `durationMs` (default 3000). See floor_calib.h. */
    fun floorCalibStart(durationMs: Int = 0, onDone: ((Boolean) -> Unit)? = null) {
        val payload = if (durationMs > 0) {
            byteArrayOf(
                ConfigProtocol.CMD_FLOOR_CALIB_START.toByte(),
                (durationMs and 0xFF).toByte(),
                ((durationMs shr 8) and 0xFF).toByte(),
            )
        } else {
            byteArrayOf(ConfigProtocol.CMD_FLOOR_CALIB_START.toByte())
        }
        sendConfigCmd(payload, onDone)
    }

    fun floorCalibClear(onDone: ((Boolean) -> Unit)? = null) =
        sendConfigCmd(ConfigProtocol.CMD_FLOOR_CALIB_CLEAR.toByte(), onDone)

    private fun BluetoothGattCharacteristic.readUtf8(): String {
        val bytes = value ?: return ""
        return String(bytes, StandardCharsets.UTF_8)
    }
}
