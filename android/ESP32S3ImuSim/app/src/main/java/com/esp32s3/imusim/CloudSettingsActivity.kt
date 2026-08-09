package com.esp32s3.imusim

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.text.Editable
import android.text.TextWatcher
import android.widget.ArrayAdapter
import android.widget.Spinner
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.button.MaterialButton
import com.google.android.material.textfield.TextInputEditText
import java.util.concurrent.Executors

/** Full-screen scrollable cloud + bridge settings (replaces oversized AlertDialog). */
class CloudSettingsActivity : AppCompatActivity() {

    private lateinit var cloudSettings: CloudSettings
    private lateinit var bridgeSettings: BridgeSyncSettings
    private lateinit var statusBanner: StatusBannerController
    private lateinit var helpText: TextView
    private lateinit var urlEdit: TextInputEditText
    private lateinit var keyEdit: TextInputEditText
    private lateinit var keyStatus: TextView
    private lateinit var deviceEdit: TextInputEditText
    private lateinit var groupEdit: TextInputEditText
    private lateinit var bridgeModeSpinner: Spinner
    private lateinit var bridgeIntervalEdit: TextInputEditText
    private lateinit var bridgeDwellEdit: TextInputEditText

    private val executor = Executors.newSingleThreadExecutor()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_cloud_settings)
        cloudSettings = CloudSettings(this)
        bridgeSettings = BridgeSyncSettings(this)
        statusBanner = StatusBannerController.attach(findViewById(android.R.id.content))

        helpText = findViewById(R.id.cloudHelpText)
        urlEdit = findViewById(R.id.cloudUrlEdit)
        keyEdit = findViewById(R.id.cloudKeyEdit)
        keyStatus = findViewById(R.id.cloudKeyStatus)
        deviceEdit = findViewById(R.id.cloudDeviceEdit)
        groupEdit = findViewById(R.id.cloudGroupEdit)
        bridgeModeSpinner = findViewById(R.id.bridgeModeSpinner)
        bridgeIntervalEdit = findViewById(R.id.bridgeIntervalEdit)
        bridgeDwellEdit = findViewById(R.id.bridgeDwellEdit)

        findViewById<MaterialToolbar>(R.id.cloudToolbar).setNavigationOnClickListener { finish() }

        bridgeModeSpinner.adapter = ArrayAdapter(
            this,
            android.R.layout.simple_spinner_dropdown_item,
            BridgeSyncSettings.Mode.entries.map { it.label },
        )

        loadFieldsIntoUi()
        refreshKeyStatus()
        keyEdit.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {
                refreshKeyStatus()
            }
            override fun afterTextChanged(s: Editable?) {}
        })

        helpText.text = CloudSettingsHelper.helpMessage(this, 0, 0) + "\n(counting…)"
        executor.execute {
            val queue = OffloadExporter(applicationContext).lineCount()
            val history = VerdictStore(applicationContext).count()
            runOnUiThread {
                helpText.text = CloudSettingsHelper.helpMessage(this, queue, history)
            }
        }

        handleSetupIntent(intent)

        findViewById<MaterialButton>(R.id.cloudDefaultUrlButton).setOnClickListener {
            urlEdit.setText(CloudSettings.DEFAULT_BASE_URL)
        }
        findViewById<MaterialButton>(R.id.cloudPasteKeyButton).setOnClickListener {
            readClipboard()?.let { clip ->
                CloudSetupLink.parse(clip)?.let { applySetup(it) }
                    ?: keyEdit.setText(CloudSetupLink.sanitizeApiKey(clip))
                refreshKeyStatus()
            }
        }
        findViewById<MaterialButton>(R.id.cloudImportButton).setOnClickListener {
            val clip = readClipboard()
            if (clip == null) {
                statusBanner.show(StatusBannerLevel.ERROR, "Clipboard empty")
                return@setOnClickListener
            }
            val setup = CloudSetupLink.parse(clip)
            if (setup == null) {
                statusBanner.show(StatusBannerLevel.ERROR, "Clipboard is not a setup link or key")
                return@setOnClickListener
            }
            applySetup(setup)
            refreshKeyStatus()
            statusBanner.show(StatusBannerLevel.OK, getString(R.string.cloud_setup_imported))
        }
        findViewById<MaterialButton>(R.id.cloudTestButton).setOnClickListener { testConnection() }
        findViewById<MaterialButton>(R.id.bridgeSyncNowButton).setOnClickListener {
            saveFieldsFromUi()
            BridgeSyncScheduler.triggerNow(applicationContext)
            statusBanner.show(StatusBannerLevel.WARN, "Bridge sync started…")
        }
        findViewById<MaterialButton>(R.id.cloudSaveButton).setOnClickListener {
            saveAndApply(uploadNow = false, finishAfter = true)
        }
        findViewById<MaterialButton>(R.id.cloudUploadButton).setOnClickListener {
            saveAndApply(uploadNow = true, finishAfter = true)
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleSetupIntent(intent)
    }

    override fun onDestroy() {
        executor.shutdownNow()
        super.onDestroy()
    }

    private fun loadFieldsIntoUi() {
        urlEdit.setText(cloudSettings.baseUrl.ifEmpty { CloudSettings.DEFAULT_BASE_URL })
        keyEdit.setText(cloudSettings.apiKey)
        deviceEdit.setText(cloudSettings.deviceId)
        groupEdit.setText(cloudSettings.groupId)
        bridgeModeSpinner.setSelection(
            BridgeSyncSettings.Mode.entries.indexOf(bridgeSettings.mode).coerceAtLeast(0),
        )
        bridgeIntervalEdit.setText(bridgeSettings.intervalMinutes.toString())
        bridgeDwellEdit.setText(bridgeSettings.dwellSeconds.toString())
    }

    private fun handleSetupIntent(intent: Intent?) {
        val uri = intent?.data ?: return
        val setup = CloudSetupLink.parse(uri.toString()) ?: return
        applySetup(setup)
        statusBanner.show(StatusBannerLevel.OK, getString(R.string.cloud_setup_imported))
        saveAndApply(uploadNow = true, finishAfter = false)
        setIntent(Intent(this, CloudSettingsActivity::class.java))
    }

    private fun applySetup(setup: CloudSetupLink.Setup) {
        urlEdit.setText(setup.baseUrl)
        keyEdit.setText(setup.apiKey)
        setup.deviceId?.let { deviceEdit.setText(it) }
        setup.groupId?.let { groupEdit.setText(it) }
    }

    private fun refreshKeyStatus() {
        val k = CloudSetupLink.sanitizeApiKey(keyEdit.text?.toString().orEmpty())
        keyStatus.text = when {
            k.isEmpty() -> "API key missing — use Paste or Import from web dashboard"
            CloudSetupLink.keyLooksValid(k) -> "API key OK (${k.length} chars)"
            else -> "Key length ${k.length} — expected 48 hex chars"
        }
    }

    private fun saveFieldsFromUi() {
        CloudSettingsHelper.saveFields(
            cloud = cloudSettings,
            bridge = bridgeSettings,
            url = urlEdit.text?.toString().orEmpty(),
            key = keyEdit.text?.toString().orEmpty(),
            deviceId = deviceEdit.text?.toString().orEmpty(),
            groupId = groupEdit.text?.toString().orEmpty(),
            bridgeMode = BridgeSyncSettings.Mode.entries[bridgeModeSpinner.selectedItemPosition],
            intervalMin = bridgeIntervalEdit.text?.toString()?.toIntOrNull()
                ?: BridgeSyncSettings.DEFAULT_INTERVAL_MIN,
            dwellSec = bridgeDwellEdit.text?.toString()?.toIntOrNull()
                ?: BridgeSyncSettings.DEFAULT_DWELL_SEC,
        )
        urlEdit.setText(cloudSettings.baseUrl)
    }

    private fun saveAndApply(uploadNow: Boolean, finishAfter: Boolean) {
        saveFieldsFromUi()
        if (cloudSettings.enabled) {
            statusBanner.show(StatusBannerLevel.WARN, "Cloud enabled — saving…")
        } else {
            statusBanner.show(StatusBannerLevel.WARN, "Cloud disabled")
        }
        CloudSettingsHelper.applyAsync(this, uploadNow, executor) { batch ->
            runOnUiThread {
                reportUpload(batch)
                if (finishAfter) {
                    setResult(RESULT_OK)
                    finish()
                }
            }
        }
    }

    private fun testConnection() {
        saveFieldsFromUi()
        statusBanner.show(StatusBannerLevel.WARN, "Testing cloud connection…")
        executor.execute {
            val ping = CloudUploader(applicationContext).testConnection()
            runOnUiThread {
                statusBanner.show(
                    if (ping.ok) StatusBannerLevel.OK else StatusBannerLevel.ERROR,
                    if (ping.ok) "Cloud reachable — ${ping.message}" else ping.message,
                )
            }
        }
    }

    private fun reportUpload(batch: CloudUploader.BatchResult?) {
        if (batch == null) {
            if (!cloudSettings.enabled) {
                statusBanner.show(StatusBannerLevel.WARN, "Cloud off — set API key first")
            }
            return
        }
        val history = CloudUploader(this).localHistoryCount()
        val failed = listOf(batch.verdicts, batch.spectra, batch.crashes)
            .firstOrNull { !it.ok && it.message != "cloud disabled" }
        when {
            batch.totalAccepted > 0 ->
                statusBanner.show(StatusBannerLevel.OK, "OK! Uploaded ${batch.summary}")
            !cloudSettings.enabled ->
                statusBanner.show(StatusBannerLevel.WARN, "Cloud off — set API key first")
            failed != null && failed.message.contains("HTTP", ignoreCase = true) ->
                statusBanner.show(StatusBannerLevel.ERROR, "Upload failed: ${failed.message}")
            failed != null ->
                statusBanner.show(StatusBannerLevel.WARN, "Upload pending: ${failed.message}")
            history > 0 && batch.verdicts.message.contains("nothing", ignoreCase = true) ->
                statusBanner.show(
                    StatusBannerLevel.WARN,
                    "Nothing queued — $history verdicts in local history only",
                )
            else ->
                statusBanner.show(StatusBannerLevel.WARN, "Nothing to upload — wait for BLE verdicts")
        }
    }

    private fun readClipboard(): String? {
        val cm = getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager ?: return null
        val clip: ClipData = cm.primaryClip ?: return null
        if (clip.itemCount == 0) return null
        return clip.getItemAt(0).coerceToText(this)?.toString()?.trim()?.takeIf { it.isNotEmpty() }
    }

    companion object {
        fun open(context: Context) {
            context.startActivity(Intent(context, CloudSettingsActivity::class.java))
        }
    }
}
