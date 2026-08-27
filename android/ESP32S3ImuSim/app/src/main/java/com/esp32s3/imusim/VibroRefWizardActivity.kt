package com.esp32s3.imusim

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.os.SystemClock
import android.widget.TextView
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.button.MaterialButton
import com.google.android.material.textfield.TextInputEditText

/** Reference calibration wizard: reset all refs, record N of M (5–15 s), upload to cloud. */
class VibroRefWizardActivity : AppCompatActivity() {

    private lateinit var statusBanner: StatusBannerController
    private lateinit var statusText: TextView
    private lateinit var labelInput: TextInputEditText
    private lateinit var recordButton: MaterialButton
    private lateinit var finishButton: MaterialButton
    private lateinit var serviceController: ImuServiceController

    private var imuService: IImuBleService? = null
    private var connected = false
    private var totalCount = 3
    private var currentIndex = 0
    private var recording = false
    private var recordStartedMs = 0L
    private var lastRefListJson: String? = null
    private var resetOnFirstRecord = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_vibro_ref_wizard)
        statusBanner = StatusBannerController.attach(findViewById(android.R.id.content))
        statusText = findViewById(R.id.vibroRefWizardStatus)
        labelInput = findViewById(R.id.vibroRefWizardLabel)
        recordButton = findViewById(R.id.vibroRefWizardRecordButton)
        finishButton = findViewById(R.id.vibroRefWizardFinishButton)

        findViewById<MaterialToolbar>(R.id.vibroRefWizardToolbar).setNavigationOnClickListener { finish() }
        recordButton.setOnClickListener { onRecordClicked() }
        finishButton.setOnClickListener { finishWizard() }

        serviceController = ImuServiceController(applicationContext, serviceEvents)
        askSetupQuestions()
        refreshUi()
    }

    override fun onStart() {
        super.onStart()
        serviceController.startAndBind()
    }

    override fun onStop() {
        serviceController.unbind()
        super.onStop()
    }

    private fun askSetupQuestions() {
        AlertDialog.Builder(this)
            .setTitle(R.string.vibro_ref_wizard_reset_title)
            .setMessage(R.string.vibro_ref_wizard_reset_msg)
            .setPositiveButton(R.string.vibro_ref_wizard_reset_yes) { _, _ ->
                resetOnFirstRecord = true
                askTotalCount()
            }
            .setNegativeButton(R.string.vibro_ref_wizard_reset_no) { _, _ -> askTotalCount() }
            .show()
    }

    private fun askTotalCount() {
        val choices = arrayOf("1", "2", "3", "4", "5")
        AlertDialog.Builder(this)
            .setTitle(R.string.vibro_ref_wizard_count_title)
            .setItems(choices) { _, which ->
                totalCount = which + 1
                currentIndex = 0
                refreshUi()
            }
            .show()
    }

    private fun onRecordClicked() {
        val svc = imuService
        if (!connected || svc == null) {
            statusBanner.show(StatusBannerLevel.WARN, getString(R.string.connect_ble_first))
            return
        }
        if (recording) {
            svc.vibroRefStop()
            recording = false
            currentIndex++
            statusText.text = getString(R.string.vibro_ref_wizard_saved, currentIndex, totalCount)
            svc.requestVibroRefList()
            refreshUi()
            return
        }
        if (currentIndex >= totalCount) return

        AlertDialog.Builder(this)
            .setTitle(getString(R.string.vibro_ref_wizard_ready_title, currentIndex + 1, totalCount))
            .setMessage(R.string.vibro_ref_wizard_ready_msg)
            .setPositiveButton(android.R.string.ok) { _, _ ->
                if (resetOnFirstRecord) {
                    resetOnFirstRecord = false
                    svc.vibroRefClearAll()
                }
                val label = labelInput.text?.toString()?.trim().orEmpty()
                val name = if (label.isBlank()) "ref ${currentIndex + 1}" else label
                svc.vibroRefStart(currentIndex, name)
                recording = true
                recordStartedMs = SystemClock.uptimeMillis()
                statusText.text = getString(R.string.vibro_ref_wizard_recording)
                refreshUi()
                window.decorView.postDelayed({
                    if (recording && SystemClock.uptimeMillis() - recordStartedMs >= 12_000L) {
                        onRecordClicked()
                    }
                }, 12_500L)
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun finishWizard() {
        val svc = imuService
        if (connected && svc != null && currentIndex > 0) {
            svc.vibroArm()
        }
        val json = lastRefListJson
        if (json != null) {
            Thread {
                val r = CloudUploader(applicationContext).uploadReferenceProfiles(json)
                runOnUiThread {
                    statusBanner.show(
                        if (r.ok) StatusBannerLevel.OK else StatusBannerLevel.ERROR,
                        r.message,
                    )
                    if (r.ok) finish()
                }
            }.start()
        } else {
            finish()
        }
    }

    private fun refreshUi() {
        val done = currentIndex >= totalCount
        recordButton.isEnabled = connected && imuService != null && !done
        finishButton.isEnabled = currentIndex > 0
        recordButton.text = when {
            !connected -> getString(R.string.connect_ble_first)
            recording -> getString(R.string.vibro_ref_wizard_stop)
            done -> getString(R.string.vibro_ref_wizard_done)
            else -> getString(R.string.vibro_ref_wizard_record, currentIndex + 1, totalCount)
        }
        if (!recording && !done && connected) {
            statusText.text = getString(R.string.vibro_ref_wizard_progress, currentIndex, totalCount)
        }
    }

    private val serviceEvents = object : ImuServiceController.Events {
        override fun onServiceReady(service: IImuBleService) {
            imuService = service
            runOnUiThread { refreshUi() }
        }

        override fun onServiceLost() {
            imuService = null
            connected = false
            runOnUiThread { refreshUi() }
        }

        override fun onConnectionChanged(connected: Boolean) {
            this@VibroRefWizardActivity.connected = connected
            runOnUiThread { refreshUi() }
        }

        override fun onRelayState(
            state: RelayFsmState,
            caption: String,
            bleConnected: Boolean,
            showDisconnect: Boolean,
        ) {
            /* requestState() (called right after binding, see ImuServiceController) replies with
             * this — NOT onConnectionChanged, which only fires on a *transition*. Without this,
             * an already-connected BLE link from before this activity opened would leave
             * `connected` stuck at its false default forever (see MainActivity's identical use
             * of bleConnected via updateConnectedUi() for the same reason). */
            this@VibroRefWizardActivity.connected = bleConnected
            runOnUiThread { refreshUi() }
        }

        override fun onStatus(text: String) {}

        override fun onPowerStatus(power: ImuProtocol.PowerStatus) {}

        override fun onBatchJson(batchJson: String) {}

        override fun onConfigBlob(blob: ByteArray) {}

        override fun onOtaProgress(percent: Int) {}

        override fun onOtaDone(ok: Boolean, message: String) {}

        override fun onVibroRefList(json: String) {
            lastRefListJson = json
        }
    }

    companion object {
        fun open(context: Context) {
            context.startActivity(Intent(context, VibroRefWizardActivity::class.java))
        }
    }
}
