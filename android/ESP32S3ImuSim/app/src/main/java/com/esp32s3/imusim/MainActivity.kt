package com.esp32s3.imusim

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Choreographer
import android.view.WindowManager
import android.widget.Button
import android.widget.EditText
import android.widget.RadioGroup
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicReference

class MainActivity : AppCompatActivity() {

    private lateinit var sceneView: ImuSceneView
    private lateinit var statusText: TextView
    private lateinit var modeGroup: RadioGroup
    private lateinit var pollEdit: EditText
    private lateinit var connectButton: Button
    private lateinit var espScreenButton: Button
    private lateinit var vibroMenuButton: Button
    private lateinit var deviceMenuButton: Button
    private lateinit var vibroText: TextView
    private lateinit var statusBanner: StatusBannerController

    private lateinit var sessionStore: ImuSessionStore
    private lateinit var cloudSettings: CloudSettings
    private lateinit var verdictStore: VerdictStore
    private lateinit var serviceController: ImuServiceController
    private var imuService: IImuBleService? = null
    private var connected = false
    private var espScreenOn = true
    private var renderMode = ImuProtocol.MODE_COMPUTED
    private var restoringUi = false
    private var crashDebugFirmware = false
    private var sessionCaps = 0

    private val fpsMeter = FpsMeter()
    private var fpsHudRunnable: Runnable? = null

    private val attitude = AttitudeEstimator()
    private val renderExecutor = Executors.newSingleThreadExecutor()
    private val cloudExecutor = Executors.newSingleThreadExecutor()
    private val mainHandler = Handler(Looper.getMainLooper())
    private val pendingBatchJson = AtomicReference<String?>(null)
    private var frameCallbackPosted = false
    private val frameCallback = Choreographer.FrameCallback {
        frameCallbackPosted = false
        val json = pendingBatchJson.getAndSet(null) ?: return@FrameCallback
        renderExecutor.execute { prepareAndApplyBatch(json) }
    }

    private sealed class PreparedUi {
        data class Frame(val power: ImuProtocol.PowerStatus?, val frame: SceneFrame) : PreparedUi()
        data class Waiting(val message: String) : PreparedUi()
        data class ParseError(val length: Int) : PreparedUi()
    }

    private val permissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { granted ->
            if (granted.values.all { it }) {
                connectBle()
            } else {
                statusText.text = "BLE permissions denied"
            }
        }

    private val otaPicker = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        val svc = imuService
        if (uri == null || svc == null || !connected) return@registerForActivityResult
        Thread {
            val bytes = OtaFirmwareFormats.parseFirmware(
                uri,
                contentResolver,
                { u -> contentResolver.openInputStream(u) },
            ) { err ->
                runOnUiThread { statusText.text = err }
            }
            runOnUiThread {
                if (bytes != null) {
                    statusText.text = "OTA uploading ${bytes.size} B..."
                    svc.uploadFirmware(bytes)
                }
            }
        }.start()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        statusBanner = StatusBannerController.attach(findViewById(android.R.id.content))
        bindViews()
        sceneView.fpsMeter = fpsMeter
        sessionStore = ImuSessionStore(this)
        cloudSettings = CloudSettings(this)
        verdictStore = VerdictStore(this)
        serviceController = ImuServiceController(applicationContext, serviceEvents)
        wireControls()
        serviceController.startAndBind()
        handleCloudSetupIntent(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleCloudSetupIntent(intent)
    }

    override fun onStart() {
        super.onStart()
        AppEventHub.onBanner = { level, message -> statusBanner.show(level, message) }
        if (::serviceController.isInitialized) {
            serviceController.requestState()
        }
    }

    override fun onStop() {
        AppEventHub.onBanner = null
        super.onStop()
    }

    override fun onDestroy() {
        stopFpsHud()
        setKeepScreenOn(false)
        cloudExecutor.shutdownNow()
        renderExecutor.shutdownNow()
        serviceController.unbind()
        super.onDestroy()
    }

    private fun bindViews() {
        sceneView = findViewById(R.id.sceneView)
        statusText = findViewById(R.id.statusText)
        modeGroup = findViewById(R.id.modeGroup)
        pollEdit = findViewById(R.id.pollEdit)
        connectButton = findViewById(R.id.connectButton)
        espScreenButton = findViewById(R.id.espScreenButton)
        vibroMenuButton = findViewById(R.id.vibroMenuButton)
        deviceMenuButton = findViewById(R.id.deviceMenuButton)
        vibroText = findViewById(R.id.vibroText)
        updateEspScreenButton()
    }

    private fun wireControls() {
        modeGroup.setOnCheckedChangeListener { _, checkedId ->
            if (restoringUi) return@setOnCheckedChangeListener
            renderMode = when (checkedId) {
                R.id.modeRaw -> ImuProtocol.MODE_RAW
                R.id.modeScene -> ImuProtocol.MODE_SCENE
                else -> ImuProtocol.MODE_COMPUTED
            }
            attitude.reset()
            imuService?.setMode(renderMode)
            if (connected) {
                statusText.text = modeStatusLabel(renderMode)
            }
        }

        connectButton.setOnClickListener {
            if (connected) {
                imuService?.disconnect()
            } else {
                ensurePermissionsAndConnect()
            }
        }

        espScreenButton.setOnClickListener {
            if (!requireBleConnected { espScreenButton.performClick() }) return@setOnClickListener
            espScreenOn = !espScreenOn
            updateEspScreenButton()
            imuService?.setEspScreenOn(espScreenOn)
            statusText.text = if (espScreenOn) "Turning ESP display on…" else "Turning ESP display off…"
        }

        vibroMenuButton.setOnClickListener { showVibroMenu() }
        deviceMenuButton.setOnClickListener { showDeviceMenu() }
    }

    private enum class VibroMenuItem { REF_START, REF_STOP, HISTORY, FFT, MODE }

    private enum class DeviceMenuItem {
        PROFILE, CONFIG_EDITOR, MIX, ERASE_NVS, CRASH_DEBUG, WIFI, SYNC, OTA, CLOUD, SCREEN,
    }

    private fun showVibroMenu() {
        val items = VibroMenuItem.values()
        AlertDialog.Builder(this)
            .setTitle(R.string.menu_vibro)
            .setItems(items.map { vibroMenuLabel(it) }.toTypedArray()) { _, which ->
                when (items[which]) {
                    VibroMenuItem.REF_START -> {
                        if (!requireBleConnected { showVibroMenu() }) return@setItems
                        statusText.text = "Ref start…"
                        imuService?.vibroRefStart()
                    }
                    VibroMenuItem.REF_STOP -> {
                        if (!requireBleConnected { showVibroMenu() }) return@setItems
                        statusText.text = "Ref stop…"
                        imuService?.vibroRefStop()
                    }
                    VibroMenuItem.HISTORY -> showVerdictHistory()
                    VibroMenuItem.FFT -> {
                        if (!requireBleConnected { showVibroMenu() }) return@setItems
                        modeGroup.check(R.id.modeRaw)
                        renderMode = ImuProtocol.MODE_RAW
                        imuService?.setMode(renderMode)
                        statusText.text = "FFT: RAW mode — collecting samples…"
                        imuService?.analyzeSpectrum()
                    }
                    VibroMenuItem.MODE -> showVibroModeDialog()
                }
            }
            .show()
    }

    private fun showDeviceMenu() {
        val items = DeviceMenuItem.entries
        AlertDialog.Builder(this)
            .setTitle(R.string.menu_device)
            .setItems(items.map { deviceMenuLabel(it) }.toTypedArray()) { _, which ->
                when (items[which]) {
                    DeviceMenuItem.PROFILE -> showProfileWizard()
                    DeviceMenuItem.CONFIG_EDITOR -> {
                        if (!requireBleConnected { showDeviceMenu() }) return@setItems
                        startActivity(Intent(this, ConfigEditorActivity::class.java))
                    }
                    DeviceMenuItem.MIX -> showMixSettingsDialog()
                    DeviceMenuItem.ERASE_NVS -> {
                        if (!requireBleConnected { showDeviceMenu() }) return@setItems
                        confirmEraseNvs()
                    }
                    DeviceMenuItem.CRASH_DEBUG -> showCrashDebugDialog()
                    DeviceMenuItem.WIFI -> {
                        if (!requireBleConnected { showDeviceMenu() }) return@setItems
                        startActivity(Intent(this, WifiWizardActivity::class.java))
                    }
                    DeviceMenuItem.SYNC -> {
                        if (!connected) {
                            statusText.text = "Connect first"
                            return@setItems
                        }
                        imuService?.requestConfigSync()
                    }
                    DeviceMenuItem.OTA -> {
                        if (!connected) {
                            statusText.text = "Connect first"
                            return@setItems
                        }
                        statusText.text = "Pick firmware (${OtaFirmwareFormats.pickerSummary()})"
                        otaPicker.launch(OtaFirmwareFormats.openDocumentMimeTypes())
                    }
                    DeviceMenuItem.CLOUD -> CloudSettingsActivity.open(this)
                    DeviceMenuItem.SCREEN -> espScreenButton.performClick()
                }
            }
            .show()
    }

    private fun vibroMenuLabel(item: VibroMenuItem): String = when (item) {
        VibroMenuItem.REF_START -> getString(R.string.vibro_ref_start)
        VibroMenuItem.REF_STOP -> getString(R.string.vibro_ref_stop)
        VibroMenuItem.HISTORY -> getString(R.string.vibro_history)
        VibroMenuItem.FFT -> getString(R.string.vibro_fft)
        VibroMenuItem.MODE -> getString(R.string.vibro_mode)
    }

    private fun deviceMenuLabel(item: DeviceMenuItem): String = when (item) {
        DeviceMenuItem.PROFILE -> getString(R.string.profile_wizard)
        DeviceMenuItem.CONFIG_EDITOR -> getString(R.string.config_editor_title)
        DeviceMenuItem.MIX -> getString(R.string.vibro_mix_settings)
        DeviceMenuItem.ERASE_NVS -> getString(R.string.erase_nvs_menu)
        DeviceMenuItem.CRASH_DEBUG -> getString(R.string.crash_debug_menu)
        DeviceMenuItem.WIFI -> getString(R.string.wifi_wizard)
        DeviceMenuItem.SYNC -> "Sync cfg"
        DeviceMenuItem.OTA -> getString(R.string.ota_file)
        DeviceMenuItem.CLOUD -> getString(R.string.cloud_settings)
        DeviceMenuItem.SCREEN -> if (espScreenOn) {
            getString(R.string.esp_screen_off)
        } else {
            getString(R.string.esp_screen_on)
        }
    }

    private fun updateEspScreenButton() {
        espScreenButton.text = when {
            !connected -> getString(R.string.esp_screen_connect)
            espScreenOn -> getString(R.string.esp_screen_off)
            else -> getString(R.string.esp_screen_on)
        }
    }

    private fun showVibroModeDialog() {
        if (!requireBleConnected { showVibroModeDialog() }) return
        val modes = VibroDiagnosisMode.Id.values()
        AlertDialog.Builder(this)
            .setTitle(R.string.vibro_mode)
            .setItems(modes.map { it.label }.toTypedArray()) { _, which ->
                confirmVibroMode(modes[which])
            }
            .show()
    }

    private fun confirmVibroMode(mode: VibroDiagnosisMode.Id) {
        val msg = VibroDiagnosisMode.describe(mode)
        AlertDialog.Builder(this)
            .setTitle(mode.label)
            .setMessage(msg)
            .setPositiveButton("Push to ESP") { _, _ -> applyVibroMode(mode) }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun applyVibroMode(mode: VibroDiagnosisMode.Id) {
        val svc = imuService ?: return
        statusText.text = "Vibro mode: ${mode.label}…"
        svc.requestConfigSync()
        Thread {
            Thread.sleep(400)
            val base = sessionStore.loadLocalConfig()
            val blob = VibroDiagnosisMode.applyToBlob(
                base ?: ByteArray(ConfigProtocol.BLOB_SIZE),
                mode,
            )
            runOnUiThread {
                svc.pushConfig(blob, true)
                val spec = VibroDiagnosisMode.spec(mode)
                imuService?.setPollIntervalMs(spec.pollMs)
                pollEdit.setText(spec.pollMs.toString())
                statusBanner.show(StatusBannerLevel.OK, "Vibro mode: ${mode.label}")
            }
        }.start()
    }

    private fun showVerdictHistory() {
        val rows = verdictStore.recent(40)
        if (rows.isEmpty()) {
            AlertDialog.Builder(this)
                .setTitle(R.string.vibro_history)
                .setMessage("No verdicts stored yet. Record a reference, then wait for pattern detection.")
                .setPositiveButton(android.R.string.ok, null)
                .show()
            return
        }
        val lines = rows.joinToString("\n") { r ->
            val tag = when (r.level) {
                ImuProtocol.VERDICT_ALERT -> "ALERT"
                ImuProtocol.VERDICT_WARN -> "WARN"
                else -> "OK"
            }
            String.format(
                java.util.Locale.US,
                "%tF %<tT seq=%d %s rms=%.3f c=%.2f",
                r.tsMs,
                r.seq,
                tag,
                r.rmsG,
                r.corr,
            )
        }
        AlertDialog.Builder(this)
            .setTitle(getString(R.string.vibro_history) + " (${rows.size})")
            .setMessage(lines)
            .setPositiveButton(android.R.string.ok, null)
            .show()
    }

    private val serviceEvents = object : ImuServiceController.Events {
        override fun onServiceReady(service: IImuBleService) {
            imuService = service
            runOnUiThread { serviceController.requestState() }
        }

        override fun onServiceLost() {
            imuService = null
            runOnUiThread {
                connected = false
                connectButton.text = getString(R.string.connect)
                setKeepScreenOn(false)
                statusText.text = getString(R.string.status_disconnected)
            }
        }

        override fun onRelayState(
            state: RelayFsmState,
            caption: String,
            bleConnected: Boolean,
            showDisconnect: Boolean,
        ) {
            runOnUiThread {
                updateConnectedUi(bleConnected, showDisconnect)
                if (caption.isNotBlank()) {
                    statusText.text = caption
                    val level = when (state) {
                        RelayFsmState.CONNECTED, RelayFsmState.CLOUD_SYNC -> StatusBannerLevel.OK
                        RelayFsmState.STARTING, RelayFsmState.BT_WARMUP,
                        RelayFsmState.SCAN_CONNECT, RelayFsmState.PAUSE,
                        -> StatusBannerLevel.WARN
                    }
                    statusBanner.show(level, caption)
                }
            }
        }

        override fun onSessionRestore(snapshot: Bundle) {
            runOnUiThread { applySessionRestore(snapshot) }
        }

        override fun onConnectionChanged(connected: Boolean) {
            runOnUiThread { updateConnectedUi(connected, connected) }
        }

        override fun onStatus(text: String) {
            runOnUiThread { statusText.text = text }
        }

        override fun onBanner(level: StatusBannerLevel, message: String) {
            runOnUiThread {
                statusBanner.show(level, message)
                statusText.text = message
            }
        }

        override fun onPowerStatus(power: ImuProtocol.PowerStatus) {
            runOnUiThread { sceneView.setPowerStatus(power) }
        }

        override fun onBatchJson(batchJson: String) {
            fpsMeter.onBleBatch()
            scheduleBatchRender(batchJson)
        }

        override fun onConfigBlob(blob: ByteArray) {
            runOnUiThread { statusText.text = ConfigSummary.format(blob) }
        }

        override fun onOtaProgress(percent: Int) {
            runOnUiThread { statusText.text = "OTA $percent%" }
        }

        override fun onOtaDone(ok: Boolean, message: String) {
            runOnUiThread {
                statusText.text = if (ok) message else "OTA failed: $message"
                statusBanner.show(
                    if (ok) StatusBannerLevel.OK else StatusBannerLevel.ERROR,
                    if (ok) "OK!" else "OTA failed: $message",
                )
            }
        }

        override fun onVibroCaption(caption: String) {
            runOnUiThread {
                vibroText.text = caption
                if (caption.contains("dbg")) {
                    crashDebugFirmware = true
                }
            }
        }

        override fun onEspScreenState(on: Boolean) {
            runOnUiThread {
                espScreenOn = on
                updateEspScreenButton()
            }
        }
    }

    private fun applySessionRestore(snap: Bundle) {
        restoringUi = true
        connected = snap.getBoolean(ImuSessionStore.KEY_CONNECTED, false)
        val showDisconnect = snap.getBoolean(ImuSessionStore.KEY_SHOW_DISCONNECT, connected)
        renderMode = snap.getInt(ImuSessionStore.KEY_MODE, ImuProtocol.MODE_COMPUTED)
        pollEdit.setText(snap.getInt(ImuSessionStore.KEY_POLL_MS, ImuProtocol.DEFAULT_POLL_MS).toString())
        when (renderMode) {
            ImuProtocol.MODE_RAW -> modeGroup.check(R.id.modeRaw)
            ImuProtocol.MODE_SCENE -> modeGroup.check(R.id.modeScene)
            else -> modeGroup.check(R.id.modeComputed)
        }
        snap.getString(ImuSessionStore.KEY_RELAY_CAPTION)
            ?.takeIf { it.isNotBlank() }
            ?.let { statusText.text = it }
            ?: snap.getString(ImuSessionStore.KEY_STATUS)?.let { statusText.text = it }
        crashDebugFirmware = snap.getBoolean(ImuSessionStore.KEY_CRASH_DEBUG, false)
        sessionCaps = snap.getInt(ImuSessionStore.KEY_CAPS, 0)
        if (ImuProtocol.crashDebugFromCaps(sessionCaps)) {
            crashDebugFirmware = true
        }
        updateConnectedUi(connected, showDisconnect)
        if (snap.getBoolean(ImuSessionStore.KEY_POWER_VALID)) {
            sceneView.setPowerStatus(
                ImuProtocol.PowerStatus(
                    snap.getInt(ImuSessionStore.KEY_POWER_SOURCE),
                    snap.getFloat(ImuSessionStore.KEY_POWER_V),
                    snap.getInt(ImuSessionStore.KEY_POWER_PCT),
                    true,
                ),
            )
        }
        snap.getString(ImuSessionStore.KEY_LAST_BATCH)?.let { scheduleBatchRender(it) }
        restoringUi = false
    }

    private fun scheduleBatchRender(batchJson: String) {
        pendingBatchJson.set(batchJson)
        if (frameCallbackPosted) return
        frameCallbackPosted = true
        runOnUiThread {
            Choreographer.getInstance().postFrameCallback(frameCallback)
        }
    }

    private fun prepareAndApplyBatch(json: String) {
        val prepared = prepareBatch(json)
        runOnUiThread { applyPreparedBatch(prepared) }
    }

    private fun prepareBatch(json: String): PreparedUi {
        val batch = ImuProtocol.parseBatchLenient(json)
            ?: return PreparedUi.ParseError(json.length)
        val power = ImuProtocol.powerFromFields(
            batch.powerSource,
            batch.voltageV,
            batch.percent,
            batch.trendV,
        )
        return when (batch.mode) {
            ImuProtocol.MODE_RAW -> {
                val sample = batch.raw.lastOrNull()
                    ?: return PreparedUi.Waiting("Raw: waiting for samples…")
                attitude.update(sample)
                PreparedUi.Frame(power, buildRawFrame(sample, batch.screenW, batch.screenH))
            }
            ImuProtocol.MODE_COMPUTED -> {
                val record = batch.computed.lastOrNull()
                    ?: return PreparedUi.Waiting("Computed: waiting for frame…")
                PreparedUi.Frame(power, buildComputedFrame(record, batch.screenW, batch.screenH))
            }
            ImuProtocol.MODE_SCENE -> {
                val record = batch.scene.lastOrNull()
                    ?: return PreparedUi.Waiting("Scene: waiting for frame…")
                PreparedUi.Frame(
                    power,
                    SceneFrame.SceneDirect(record, batch.screenW, batch.screenH),
                )
            }
            else -> PreparedUi.Waiting("Unknown mode ${batch.mode}")
        }
    }

    private fun applyPreparedBatch(prepared: PreparedUi) {
        when (prepared) {
            is PreparedUi.ParseError -> {
                statusText.text = "Parse error (${prepared.length} B) — retrying…"
            }
            is PreparedUi.Waiting -> {
                statusText.text = prepared.message
            }
            is PreparedUi.Frame -> {
                fpsMeter.onUiApply()
                sceneView.setPowerStatus(prepared.power)
                sceneView.setFrame(prepared.frame)
            }
        }
        if (pendingBatchJson.get() != null && !frameCallbackPosted) {
            frameCallbackPosted = true
            Choreographer.getInstance().postFrameCallback(frameCallback)
        }
    }

    private fun updateConnectedUi(connected: Boolean, showDisconnect: Boolean = connected) {
        this.connected = connected
        setKeepScreenOn(connected)
        connectButton.text = if (showDisconnect) {
            getString(R.string.disconnect)
        } else {
            getString(R.string.connect)
        }
        updateEspScreenButton()
        if (connected) {
            startFpsHud()
        } else {
            stopFpsHud()
            sceneView.setPowerStatus(null)
            sceneView.setFpsHud(null)
        }
    }

    private fun startFpsHud() {
        stopFpsHud()
        val tick = object : Runnable {
            override fun run() {
                if (!connected) return
                refreshStatusLine()
                statusText.postDelayed(this, 1000L)
            }
        }
        fpsHudRunnable = tick
        statusText.postDelayed(tick, 1000L)
    }

    private fun refreshStatusLine() {
        val snap = fpsMeter.snapshot()
        sceneView.setFpsHud(snap.hudLine())
        statusText.text = snap.caption()
    }

    private fun stopFpsHud() {
        fpsHudRunnable?.let { statusText.removeCallbacks(it) }
        fpsHudRunnable = null
    }

    private fun setKeepScreenOn(on: Boolean) {
        if (on) {
            window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        } else {
            window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }
    }

    private fun ensurePermissionsAndConnect() {
        val needed = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_SCAN) != PackageManager.PERMISSION_GRANTED) {
                needed.add(Manifest.permission.BLUETOOTH_SCAN)
            }
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
                needed.add(Manifest.permission.BLUETOOTH_CONNECT)
            }
        }
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_GRANTED) {
            needed.add(Manifest.permission.ACCESS_FINE_LOCATION)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
                needed.add(Manifest.permission.POST_NOTIFICATIONS)
            }
        }
        if (needed.isEmpty()) {
            connectBle()
        } else {
            permissionLauncher.launch(needed.toTypedArray())
        }
    }

    private fun connectBle() {
        val pollMs = pollEdit.text.toString().toIntOrNull()?.coerceIn(
            ImuProtocol.MIN_POLL_MS,
            2000,
        ) ?: ImuProtocol.LIVE_POLL_MS
        pollEdit.setText(pollMs.toString())
        fpsMeter.setPollMs(pollMs)
        imuService?.setPollIntervalMs(pollMs)
        imuService?.connect()
    }

    private fun buildRawFrame(sample: ImuProtocol.RawRecord, sw: Int, sh: Int): SceneFrame {
        val rot = attitude.rotationMatrix()
        val z = 0.75f
        val cam = Camera3D(aspect = sw.toFloat() / sh.toFloat())
        val body = Vec3(sample.ax, sample.ay, sample.az)
        var world = Projection.transform(rot, body)
        world = Vec3(world.x, world.y, world.z - 1f)
        var probe = Vec3(world.x * 0.45f, world.y * 0.45f, world.z * 0.45f)
        probe = Vec3(probe.x, probe.y, probe.z + 2f)
        val touch = Projection.project(probe, sw, sh, cam)
        val back = Projection.unproject(touch, probe.z, sw, sh, cam)
        return SceneFrame.RawDerived(
            distanceM = sample.distanceM,
            zoomX = z,
            zoomY = z,
            zoomZ = z,
            footerX = back.x,
            footerY = back.y,
            footerZ = back.z,
            screenW = sw,
            screenH = sh,
            rot = rot,
        )
    }

    private fun buildComputedFrame(record: ImuProtocol.ComputedRecord, sw: Int, sh: Int): SceneFrame {
        return SceneFrame.ComputedBoard(
            distanceM = record.distanceM,
            footerX = record.footerX,
            footerY = record.footerY,
            footerZ = record.footerZ,
            zoomX = record.zoomX,
            zoomY = record.zoomY,
            zoomZ = record.zoomZ,
            screenW = sw,
            screenH = sh,
            rot = record.rot,
            axes = record.axes,
        )
    }

    private fun modeStatusLabel(mode: Int): String = when (mode) {
        ImuProtocol.MODE_RAW -> "Mode → Raw IMU (connected)"
        ImuProtocol.MODE_SCENE -> "Mode → Angles / scene (connected)"
        else -> "Mode → Computed IMU (connected)"
    }

    private fun requireBleConnected(action: () -> Unit): Boolean {
        if (connected && imuService != null) {
            return true
        }
        AlertDialog.Builder(this)
            .setTitle(R.string.app_name)
            .setMessage(getString(R.string.connect_ble_first))
            .setPositiveButton(R.string.connect) { _, _ -> ensurePermissionsAndConnect() }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
        return false
    }

    private fun showCrashDebugDialog() {
        if (!requireBleConnected { showCrashDebugDialog() }) return
        val fwNote = if (crashDebugFirmware || ImuProtocol.crashDebugFromCaps(sessionCaps)) {
            "Debug firmware detected (STATUS dbg=1 or caps DBG)."
        } else {
            "No dbg=1 / caps DBG — inject/BIST need v49+ with CRASH_DEBUG=1. Try Erase NVS first."
        }
        val kinds = arrayOf(
            "Erase NVS (settings) → ESP reboot",
            "Run BIST (self-test)",
            "Inject: k_panic",
            "Inject: __ASSERT",
            "Inject: null deref",
            "Inject: divide by zero",
            "Inject: stack overflow",
            "Inject: WDT stall",
        )
        AlertDialog.Builder(this)
            .setTitle(R.string.crash_debug_menu)
            .setMessage(
                "$fwNote\n\nStart with NVS erase if inject/config misbehave. " +
                    "Injections reboot the ESP; phone relays crash to Good Vibes.",
            )
            .setItems(kinds) { _, which ->
                val svc = imuService ?: return@setItems
                when (which) {
                    0 -> confirmEraseNvs()
                    1 -> svc.runDeviceBist()
                    2 -> confirmCrashInject("panic")
                    3 -> confirmCrashInject("assert")
                    4 -> confirmCrashInject("null")
                    5 -> confirmCrashInject("div0")
                    6 -> confirmCrashInject("stack")
                    7 -> confirmCrashInject("wdt")
                }
            }
            .show()
    }

    private fun confirmEraseNvs() {
        AlertDialog.Builder(this)
            .setTitle("Erase ESP NVS?")
            .setMessage(
                "Wipes ESP settings flash (fixes corrupt profile/NVS). " +
                    "ESP reboots in ~0.5s.\n\nAfter reconnect, status should show cfg pp=1 " +
                    "(balanced default). pp=5 means old config may still be in NVS — watch tty " +
                    "for \"storage partition erased\".",
            )
            .setPositiveButton("Erase") { _, _ ->
                imuService?.eraseDeviceNvs()
                statusText.text = "NVS erase sent — wait for ESP reboot…"
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun confirmCrashInject(kind: String) {
        AlertDialog.Builder(this)
            .setTitle("Inject crash?")
            .setMessage("Trigger \"$kind\" fault on ESP. Device will reboot. Continue?")
            .setPositiveButton("Inject") { _, _ ->
                imuService?.injectCrash(kind)
                statusText.text = "Crash inject ($kind) — wait for reboot + BLE relay…"
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun showMixSettingsDialog() {
        if (!requireBleConnected { showMixSettingsDialog() }) return
        val base = sessionStore.loadLocalConfig()
        val current = VibroMixConfig.read(base)
        val layout = android.widget.LinearLayout(this).apply {
            orientation = android.widget.LinearLayout.VERTICAL
            setPadding(48, 24, 48, 8)
        }
        fun field(label: String, value: Int): android.widget.EditText {
            layout.addView(android.widget.TextView(this).apply {
                text = label
                setTextColor(android.graphics.Color.WHITE)
            })
            return android.widget.EditText(this).apply {
                inputType = android.text.InputType.TYPE_CLASS_NUMBER
                setText(value.toString())
                setTextColor(android.graphics.Color.WHITE)
                layout.addView(this)
            }
        }
        val everyEdit = field("Mix every N buckets (0=off, ≥2)", current.every)
        val ratioEdit = field("Sub-window divisor (≥2)", current.ratio)
        val dynEdit = field("Dyn short ÷ on even buckets (0=off)", current.dynShort)
        val nestEdit = field("Nested ÷ on bucket%4==0 (0=off)", current.dynNested)
        AlertDialog.Builder(this)
            .setTitle(R.string.vibro_mix_settings)
            .setMessage(
                "Capture mix. Every Nth interval bucket keeps full window; " +
                    "others use window÷ratio, optionally ÷dyn (even buckets) and ÷nest (÷4 buckets).\n\n" +
                    VibroMixConfig.format(current),
            )
            .setView(layout)
            .setPositiveButton("Push to ESP") { _, _ ->
                val every = VibroMixConfig.parseField("every", everyEdit.text.toString(), current.every)
                val ratio = VibroMixConfig.parseField("ratio", ratioEdit.text.toString(), current.ratio)
                val dynShort = VibroMixConfig.parseField("dyn", dynEdit.text.toString(), current.dynShort)
                val dynNested = VibroMixConfig.parseField("nest", nestEdit.text.toString(), current.dynNested)
                if (every == null || ratio == null || dynShort == null || dynNested == null) {
                    statusText.text = "Invalid mix values"
                    return@setPositiveButton
                }
                val mix = VibroMixConfig.Mix(every, ratio, dynShort, dynNested)
                if (mix.enabled && mix.ratio < 2) {
                    statusText.text = "Mix ratio must be ≥2 when mix is enabled"
                    return@setPositiveButton
                }
                val blob = VibroMixConfig.write(
                    base ?: ProfilePresets.apply(null, ProfilePresets.Id.VIBRO_NORMAL).blob,
                    mix,
                )
                sessionStore.saveLocalConfig(blob)
                imuService?.pushConfig(blob, true)
                statusText.text = "Mix: ${VibroMixConfig.format(mix)}"
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun showProfileWizard() {
        if (!requireBleConnected { showProfileWizard() }) return
        statusText.text = getString(R.string.profile_wizard_intro)
        val presets = ProfilePresets.Id.entries.toTypedArray()
        val labels = presets.map { it.label }.toTypedArray()
        AlertDialog.Builder(this)
            .setTitle(R.string.profile_wizard)
            .setItems(labels) { _, which -> confirmProfilePreset(presets[which]) }
            .show()
    }

    private fun confirmProfilePreset(preset: ProfilePresets.Id) {
        val base = sessionStore.loadLocalConfig()
        val preview = ProfilePresets.apply(base, preset)
        val summary = buildString {
            append(ProfilePresets.describe(preset))
            append("\n\n")
            append(ConfigSummary.format(preview.blob))
            if (ProfilePresets.isVibrationPreset(preset)) {
                append("\n\nVibration fields are written to ESP only after you confirm.")
            }
        }
        AlertDialog.Builder(this)
            .setTitle(preset.label)
            .setMessage(summary)
            .setPositiveButton("Push to ESP") { _, _ -> applyProfilePreset(preset) }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun applyProfilePreset(preset: ProfilePresets.Id) {
        val svc = imuService
        if (svc == null || !connected) {
            requireBleConnected { applyProfilePreset(preset) }
            return
        }
        val base = sessionStore.loadLocalConfig()
        val result = ProfilePresets.apply(base, preset)
        sessionStore.saveLocalConfig(result.blob)
        svc.pushConfig(result.blob, true)
        svc.setPollIntervalMs(result.pollMs)
        svc.setMode(result.bleMode)
        restoringUi = true
        pollEdit.setText(result.pollMs.toString())
        renderMode = result.bleMode
        when (result.bleMode) {
            ImuProtocol.MODE_RAW -> modeGroup.check(R.id.modeRaw)
            ImuProtocol.MODE_SCENE -> modeGroup.check(R.id.modeScene)
            else -> modeGroup.check(R.id.modeComputed)
        }
        restoringUi = false
        attitude.reset()
        statusText.text = "Profile: ${preset.label} (poll ${result.pollMs}ms)"
    }

    private fun handleCloudSetupIntent(intent: Intent?) {
        val data = intent?.data ?: return
        startActivity(Intent(this, CloudSettingsActivity::class.java).apply { this.data = data })
        setIntent(Intent(this, MainActivity::class.java))
    }

    private fun reportCloudUploadResult(batch: CloudUploader.BatchResult?) {
        if (batch == null) {
            if (!cloudSettings.enabled) {
                statusBanner.show(StatusBannerLevel.WARN, "Cloud off — set API key first")
                statusText.text = "Cloud disabled"
            }
            return
        }
        val history = CloudUploader(this).localHistoryCount()
        val failed = listOf(batch.verdicts, batch.spectra, batch.crashes)
            .firstOrNull { !it.ok && it.message != "cloud disabled" }
        when {
            batch.totalAccepted > 0 -> {
                statusBanner.show(StatusBannerLevel.OK, "OK! Uploaded ${batch.summary}")
                statusText.text = "Cloud upload: ${batch.summary}"
            }
            !cloudSettings.enabled -> {
                statusBanner.show(StatusBannerLevel.WARN, "Cloud off — set API key first")
                statusText.text = "Cloud disabled"
            }
            failed != null && failed.message.contains("HTTP", ignoreCase = true) -> {
                statusBanner.show(StatusBannerLevel.ERROR, "Upload failed: ${failed.message}")
                statusText.text = "Cloud failed: ${failed.message}"
            }
            failed != null -> {
                statusBanner.show(StatusBannerLevel.WARN, "Upload pending: ${failed.message}")
                statusText.text = "Cloud: ${failed.message}"
            }
            history > 0 && batch.verdicts.message.contains("nothing", ignoreCase = true) -> {
                statusBanner.show(
                    StatusBannerLevel.WARN,
                    "Nothing queued — $history verdicts in local history only",
                )
                statusText.text = "Cloud: queue empty ($history in History)"
            }
            else -> {
                statusBanner.show(StatusBannerLevel.WARN, "Nothing to upload — wait for BLE verdicts")
                statusText.text = "Cloud: nothing pending"
            }
        }
    }

    private fun runCloudUploadOnBackground() {
        cloudExecutor.execute {
            val batch = CloudUploader(applicationContext).uploadAll()
            runOnUiThread { reportCloudUploadResult(batch) }
        }
    }
}
