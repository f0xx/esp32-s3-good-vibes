package com.esp32s3.imusim

import android.annotation.SuppressLint
import android.bluetooth.*
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Handler
import android.os.Looper
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
        fun onBatch(batch: ImuProtocol.Batch)
        fun onBatchJson(json: String) {}
        fun onPowerStatus(power: ImuProtocol.PowerStatus)
        fun onConnected(connected: Boolean)
        fun onConnectFailed(reason: String) {}
        fun onCaps(caps: Int) {}
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
    private val statusPollIntervalMs = 2000L
    private var pollGeneration = 0
    private var targetMode = ImuProtocol.MODE_COMPUTED
    private var lastSeq = -1L
    private var scanning = false
    private var bleSessionUp = false
    private var connectTimeoutRunnable: Runnable? = null
    private var gattConnectTimeoutRunnable: Runnable? = null
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
    private var minimalRelayConnect = false

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
        const val CONNECT_SCAN_TIMEOUT_MS = 25_000L
        const val GATT_CONNECT_TIMEOUT_MS = 12_000L
        /** Force-clear a stuck GATT op (no read/write callback) — avoids hanging the queue forever. */
        const val GATT_OP_TIMEOUT_MS = 8_000L
        /** Wait for ESP connect grace before ATT service discovery (minimal relay). */
        /** Must exceed firmware BLE_CONNECT_GRACE_MS (12000) + link settle. */
        const val MINIMAL_DISCOVER_DELAY_MS = 14_000L
    }

    fun deviceCaps(): Int = deviceCaps

    fun netAvailable(): Boolean = netServiceAvailable

    fun otaAvailable(): Boolean = (deviceCaps and ImuProtocol.CAP_OTA) != 0

    /** Background relay: skip WiFi net CCC + IMU poll burst that trips ESP32-S3 rwble asserts. */
    fun setMinimalRelayConnect(enabled: Boolean) {
        minimalRelayConnect = enabled
    }

    @SuppressLint("MissingPermission")
    fun connect() {
        if (adapter == null || !adapter.isEnabled) {
            postBanner(StatusBannerLevel.ERROR, "Bluetooth off")
            return
        }
        stopPoll()
        disconnect()
        postStatus("Scanning for ${ImuProtocol.DEVICE_NAME}...")
        val filters = listOf(
            ScanFilter.Builder().setServiceUuid(android.os.ParcelUuid(ImuProtocol.SERVICE_UUID)).build(),
            ScanFilter.Builder().setDeviceName(ImuProtocol.DEVICE_NAME).build(),
        )
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_BALANCED)
            .build()
        scanning = true
        adapter.bluetoothLeScanner.startScan(filters, settings, scanCallback)
        connectTimeoutRunnable?.let { mainHandler.removeCallbacks(it) }
        connectTimeoutRunnable = Runnable {
            if (scanning) {
                adapter.bluetoothLeScanner.stopScan(scanCallback)
                scanning = false
                postBanner(StatusBannerLevel.WARN, "Device not found")
                listener.onConnectFailed("device not found (${CONNECT_SCAN_TIMEOUT_MS / 1000}s scan)")
            }
        }
        mainHandler.postDelayed(connectTimeoutRunnable!!, CONNECT_SCAN_TIMEOUT_MS)
    }

    @SuppressLint("MissingPermission")
    fun disconnect() {
        stopPoll()
        clearGattQueue()
        connectTimeoutRunnable?.let { mainHandler.removeCallbacks(it) }
        connectTimeoutRunnable = null
        gattConnectTimeoutRunnable?.let { mainHandler.removeCallbacks(it) }
        gattConnectTimeoutRunnable = null
        if (scanning) {
            adapter?.bluetoothLeScanner?.stopScan(scanCallback)
            scanning = false
        }
        val wasUp = bleSessionUp
        gatt?.close()
        gatt = null
        bleSessionUp = false
        netServiceAvailable = false
        cccdQueue.clear()
        cccdGatt = null
        pendingNetScanRead = null
        pendingNetProfilesRead = null
        pendingCrashJsonRead = null
        if (wasUp) {
            listener.onConnected(false)
        }
    }

    @SuppressLint("MissingPermission")
    fun setMode(mode: Int) {
        targetMode = mode
        lastSeq = -1L
        val ch = gatt?.getService(ImuProtocol.SERVICE_UUID)?.getCharacteristic(ImuProtocol.CHAR_MODE_UUID)
        if (ch != null) {
            ch.value = byteArrayOf(mode.toByte())
            gatt?.writeCharacteristic(ch)
        }
        mainHandler.postDelayed({ pollDataOnly() }, 80)
    }

    fun pollIntervalMs(): Int = pollMs

    /** Force an immediate DATA read (used during FFT sample collection). */
    fun requestDataPoll() {
        mainHandler.post { pollDataOnly() }
    }

    @SuppressLint("MissingPermission")
    fun setEspScreenOn(on: Boolean) {
        val ch = gatt?.getService(ImuProtocol.SERVICE_UUID)
            ?.getCharacteristic(ImuProtocol.CHAR_SCREEN_UUID) ?: return
        ch.value = byteArrayOf(if (on) 1 else 0)
        gatt?.writeCharacteristic(ch)
        mainHandler.postDelayed({ pollStatusOnly() }, 120)
    }

    @SuppressLint("MissingPermission")
    fun setPollIntervalMs(ms: Int) {
        pollMs = ms.coerceIn(33, 2000)
        val ch = gatt?.getService(ImuProtocol.SERVICE_UUID)?.getCharacteristic(ImuProtocol.CHAR_POLL_MS_UUID)
        if (ch != null) {
            ch.value = ImuProtocol.pollMsToBytes(pollMs)
            gatt?.writeCharacteristic(ch)
        }
        if (gatt != null) {
            stopPoll()
            startPoll()
        }
    }

    private val scanCallback = object : ScanCallback() {
        @SuppressLint("MissingPermission")
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val hasService = result.scanRecord?.serviceUuids?.contains(
                android.os.ParcelUuid(ImuProtocol.SERVICE_UUID),
            ) == true
            val name = result.device.name
            if (name != ImuProtocol.DEVICE_NAME && !hasService) {
                return
            }
            adapter?.bluetoothLeScanner?.stopScan(this)
            scanning = false
            connectTimeoutRunnable?.let { mainHandler.removeCallbacks(it) }
            connectTimeoutRunnable = null
            postStatus("Connecting to ${name ?: "ESP32"}...")
            gatt = result.device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
            scheduleGattConnectTimeout()
        }
    }

    private fun scheduleGattConnectTimeout() {
        gattConnectTimeoutRunnable?.let { mainHandler.removeCallbacks(it) }
        val timeoutMs = if (minimalRelayConnect) {
            MINIMAL_DISCOVER_DELAY_MS + 20_000L
        } else {
            GATT_CONNECT_TIMEOUT_MS
        }
        gattConnectTimeoutRunnable = Runnable {
            if (!bleSessionUp && gatt != null) {
                postBanner(StatusBannerLevel.WARN, "GATT connect timeout")
                listener.onConnectFailed("GATT connect timeout (${timeoutMs / 1000}s)")
                disconnect()
            }
        }
        mainHandler.postDelayed(gattConnectTimeoutRunnable!!, timeoutMs)
    }

    private val gattCallback = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                connectTimeoutRunnable?.let { mainHandler.removeCallbacks(it) }
                connectTimeoutRunnable = null
                gattConnectTimeoutRunnable?.let { mainHandler.removeCallbacks(it) }
                gattConnectTimeoutRunnable = null
                postStatus("Connected — waiting for ESP grace")
                postBanner(StatusBannerLevel.OK, "Connected")
                if (minimalRelayConnect) {
                    mainHandler.postDelayed({ gatt.discoverServices() }, MINIMAL_DISCOVER_DELAY_MS)
                } else {
                    gatt.requestMtu(517)
                }
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                connectTimeoutRunnable?.let { mainHandler.removeCallbacks(it) }
                connectTimeoutRunnable = null
                gattConnectTimeoutRunnable?.let { mainHandler.removeCallbacks(it) }
                gattConnectTimeoutRunnable = null
                val wasUp = bleSessionUp
                stopPoll()
                clearGattQueue()
                bleSessionUp = false
                if (status != BluetoothGatt.GATT_SUCCESS && !wasUp) {
                    listener.onConnectFailed("GATT disconnect (status=$status)")
                }
                if (wasUp) {
                    listener.onConnected(false)
                }
                postStatus("Disconnected")
            }
        }

        @SuppressLint("MissingPermission")
        override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
            gatt.discoverServices()
        }

        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                postBanner(StatusBannerLevel.ERROR, "Service discovery failed")
                listener.onConnectFailed("service discovery failed (status=$status)")
                return
            }
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
                syncTimeFromPhone(gatt)
                mainHandler.postDelayed({
                    if (bleSessionUp) {
                        listener.onConnected(true)
                        postStatus("Relay connect (minimal GATT)")
                    }
                }, 1500)
            } else {
                enableNotify(gatt)
                enableNetNotify(gatt)
                setMode(targetMode)
                setPollIntervalMs(pollMs)
                syncTimeFromPhone(gatt)
                readDeviceCaps(gatt)
                bleSessionUp = true
                listener.onConnected(true)
                postStatus("Polling every ${pollMs}ms")
                startPoll()
            }
        }

        @SuppressLint("MissingPermission")
        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
        ) {
            when (characteristic.uuid) {
                ImuProtocol.CHAR_NOTIFY_UUID -> mainHandler.post { pollDataOnly() }
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
                        if (ok) {
                            postBanner(StatusBannerLevel.OK, "TIME sync OK")
                        } else {
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
                    postBanner(StatusBannerLevel.ERROR, "GATT read failed ($status)")
                    return@post
                }
                when (characteristic.uuid) {
                    ImuProtocol.CHAR_STATUS_UUID -> handleStatusRead(gatt, characteristic)
                    ImuProtocol.CHAR_DATA_UUID -> handleDataRead(characteristic)
                    NetProtocol.CHAR_SCAN_UUID -> deliverNetRead(characteristic) { listener.onNetScan(it) }
                    NetProtocol.CHAR_PROFILES_UUID -> deliverNetRead(characteristic) { listener.onNetProfiles(it) }
                    NetProtocol.CHAR_STATUS_UUID -> deliverNetRead(characteristic) { listener.onNetStatus(it) }
                    CrashProtocol.CHAR_INFO_UUID -> deliverCrashRead(characteristic)
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
                    fetchCrashSlotDetails(CrashFetcher.parseListSlots(json), 0, mutableListOf(), onDone)
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
        try {
            val st = ImuProtocol.parseStatus(json)
            listener.onDeviceStatus(st)
            listener.onPowerStatus(ImuProtocol.powerFromStatus(st))
        } catch (e: Exception) {
            postBanner(StatusBannerLevel.ERROR, "Status parse error: ${e.message}")
        }
    }

    private fun handleDataRead(characteristic: BluetoothGattCharacteristic) {
        val json = characteristic.readUtf8()
        if (json.length >= 4) {
            try {
                val batch = ImuProtocol.parseBatch(json)
                if (batch.seq != lastSeq) {
                    lastSeq = batch.seq
                    val count = when (batch.mode) {
                        ImuProtocol.MODE_RAW -> batch.raw.size
                        ImuProtocol.MODE_SCENE -> batch.scene.size
                        else -> batch.computed.size
                    }
                    listener.onPollStats(batch.seq, count, pollMs)
                    listener.onBatchJson(json)
                    listener.onBatch(batch)
                }
            } catch (e: Exception) {
                postBanner(StatusBannerLevel.WARN, "Batch parse error (${json.length} B)")
            }
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
        gatt.readCharacteristic(capsChar)
        mainHandler.postDelayed({
            deviceCaps = ImuProtocol.parseCaps(capsChar.value ?: ByteArray(0))
            if (deviceCaps != 0) {
                postStatus(ImuProtocol.capsCaption(deviceCaps))
                listener.onCaps(deviceCaps)
            }
            if (crashServiceAvailable && ImuProtocol.crashDebugFromCaps(deviceCaps)) {
                postBanner(StatusBannerLevel.OK, "Crash debug BLE ready (caps DBG)")
            }
        }, 200)
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
        listener.onStatus("TIME handshake ${System.currentTimeMillis()} tz=${tzMin}min")
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
        val g = gatt ?: return
        val statusChar = g.getService(ImuProtocol.SERVICE_UUID)
            ?.getCharacteristic(ImuProtocol.CHAR_STATUS_UUID) ?: return
        queueRead(statusChar)
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
            if (!gattBusy) pollDataOnly()
            scheduleWatchdog()
        }
        mainHandler.postDelayed(watchdogRunnable!!, pollMs.toLong().coerceAtLeast(33L))
    }

    private fun scheduleStatusPoll() {
        val gen = pollGeneration
        statusPollRunnable?.let { mainHandler.removeCallbacks(it) }
        statusPollRunnable = Runnable {
            if (gen != pollGeneration) return@Runnable
            pollStatusOnly()
            scheduleStatusPoll()
        }
        mainHandler.postDelayed(statusPollRunnable!!, statusPollIntervalMs)
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
    fun ackOffloadSeq(seq: Long) {
        val g = gatt ?: return
        val cmd = g.getService(ConfigProtocol.SERVICE_UUID)?.getCharacteristic(ConfigProtocol.CHAR_CMD_UUID)
            ?: return
        val payload = ByteArray(5)
        payload[0] = ConfigProtocol.CMD_OFFLOAD_ACK.toByte()
        payload[1] = (seq and 0xFF).toByte()
        payload[2] = ((seq shr 8) and 0xFF).toByte()
        payload[3] = ((seq shr 16) and 0xFF).toByte()
        payload[4] = ((seq shr 24) and 0xFF).toByte()
        cmd.value = payload
        g.writeCharacteristic(cmd)
    }

    @SuppressLint("MissingPermission")
    fun sendConfigCmd(cmdByte: Byte, onDone: ((Boolean) -> Unit)? = null) {
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
                payload = byteArrayOf(cmdByte),
                onComplete = onDone,
            ),
            highPriority = true,
        )
    }

    fun vibroRefStart(onDone: ((Boolean) -> Unit)? = null) =
        sendConfigCmd(ConfigProtocol.CMD_VIBRO_REF_START.toByte(), onDone)

    fun vibroRefStop(onDone: ((Boolean) -> Unit)? = null) =
        sendConfigCmd(ConfigProtocol.CMD_VIBRO_REF_STOP.toByte(), onDone)

    private fun BluetoothGattCharacteristic.readUtf8(): String {
        val bytes = value ?: return ""
        return String(bytes, StandardCharsets.UTF_8)
    }
}
