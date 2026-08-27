package com.esp32s3.imusim

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.widget.TextView
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.button.MaterialButton
import com.google.android.material.textfield.TextInputEditText
import java.util.Locale

/** Battery discharge bench wizard — locks ESP config, samples 1 Hz, uploads to cloud. */
class BatteryBenchActivity : AppCompatActivity() {

    private lateinit var statusBanner: StatusBannerController
    private lateinit var statusText: TextView
    private lateinit var liveText: TextView
    private lateinit var labelInput: TextInputEditText
    private lateinit var startButton: MaterialButton
    private lateinit var stopButton: MaterialButton
    private lateinit var uploadButton: MaterialButton
    private lateinit var serviceController: ImuServiceController
    private lateinit var benchStore: BatteryBenchStore

    private var imuService: IImuBleService? = null
    private var connected = false
    private var benchActive = false
    private var sessionId = 0L
    private var sampleSeq = 0L
    private var voltageV = 0f
    private var pct = 0
    private var elapsedMs = 0L
    private var estMa = 0f

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_battery_bench)
        benchStore = BatteryBenchStore(this)
        statusBanner = StatusBannerController.attach(findViewById(android.R.id.content))
        statusText = findViewById(R.id.batteryBenchStatus)
        liveText = findViewById(R.id.batteryBenchLive)
        labelInput = findViewById(R.id.batteryBenchLabel)
        startButton = findViewById(R.id.batteryBenchStartButton)
        stopButton = findViewById(R.id.batteryBenchStopButton)
        uploadButton = findViewById(R.id.batteryBenchUploadButton)

        findViewById<MaterialToolbar>(R.id.batteryBenchToolbar).setNavigationOnClickListener { finish() }

        startButton.setOnClickListener { confirmStart() }
        stopButton.setOnClickListener { confirmStop() }
        uploadButton.setOnClickListener { uploadPending() }

        serviceController = ImuServiceController(applicationContext, serviceEvents)
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

    private fun confirmStart() {
        val svc = imuService
        if (!connected || svc == null) {
            statusBanner.show(StatusBannerLevel.WARN, getString(R.string.connect_ble_first))
            return
        }
        AlertDialog.Builder(this)
            .setTitle(R.string.battery_bench_start)
            .setMessage(R.string.battery_bench_start_confirm)
            .setPositiveButton(R.string.battery_bench_start) { _, _ ->
                val label = labelInput.text?.toString()?.trim().orEmpty()
                svc.startBatteryBench(label)
                statusText.text = getString(R.string.battery_bench_starting)
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun confirmStop() {
        val svc = imuService
        if (svc == null) return
        AlertDialog.Builder(this)
            .setTitle(R.string.battery_bench_stop)
            .setMessage(R.string.battery_bench_stop_confirm)
            .setPositiveButton(R.string.battery_bench_stop) { _, _ ->
                svc.stopBatteryBench()
                statusText.text = getString(R.string.battery_bench_stopping)
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun uploadPending() {
        uploadButton.isEnabled = false
        Thread {
            val uploader = CloudUploader(applicationContext)
            val result = uploader.uploadPendingBatteryBench()
            runOnUiThread {
                uploadButton.isEnabled = true
                val msg = if (result.ok) {
                    if (result.accepted > 0) {
                        getString(R.string.battery_bench_upload_ok, result.accepted)
                    } else {
                        getString(R.string.battery_bench_upload_none)
                    }
                } else {
                    result.message
                }
                statusBanner.show(
                    if (result.ok) StatusBannerLevel.OK else StatusBannerLevel.ERROR,
                    msg,
                )
                refreshUi()
            }
        }.start()
    }

    private fun refreshUi() {
        val pending = benchStore.pendingCount()
        statusText.text = when {
            !connected -> getString(R.string.battery_bench_status_disconnected)
            benchActive -> getString(R.string.battery_bench_status_running)
            pending > 0 -> getString(R.string.battery_bench_status_pending, pending)
            else -> getString(R.string.battery_bench_status_idle)
        }
        startButton.isEnabled = connected && imuService != null && !benchActive
        stopButton.isEnabled = connected && imuService != null && benchActive
        uploadButton.isEnabled = pending > 0

        liveText.text = buildString {
            appendLine(String.format(Locale.US, "session=0x%08X  seq=%d", sessionId, sampleSeq))
            appendLine(String.format(Locale.US, "V=%.3f  pct=%d%%  elapsed=%ds", voltageV, pct, elapsedMs / 1000))
            if (estMa > 0f) {
                appendLine(String.format(Locale.US, "est ~%.0f mA", estMa))
            }
            if (pending > 0) {
                appendLine(getString(R.string.battery_bench_pending_line, pending))
            }
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
            benchActive = false
            runOnUiThread { refreshUi() }
        }

        override fun onConnectionChanged(connected: Boolean) {
            this@BatteryBenchActivity.connected = connected
            if (!connected) benchActive = false
            runOnUiThread { refreshUi() }
        }

        override fun onRelayState(
            state: RelayFsmState,
            caption: String,
            bleConnected: Boolean,
            showDisconnect: Boolean,
        ) {
            /* requestState() (called right after binding) replies with this — NOT
             * onConnectionChanged, which only fires on a *transition*. Without this, a link
             * already connected before this activity opened leaves `connected` stuck false. */
            this@BatteryBenchActivity.connected = bleConnected
            if (!bleConnected) benchActive = false
            runOnUiThread { refreshUi() }
        }

        override fun onStatus(text: String) {}

        override fun onPowerStatus(power: ImuProtocol.PowerStatus) {}

        override fun onBatchJson(batchJson: String) {}

        override fun onConfigBlob(blob: ByteArray) {}

        override fun onOtaProgress(percent: Int) {}

        override fun onOtaDone(ok: Boolean, message: String) {}

        override fun onBatteryBench(
            active: Boolean,
            sessionId: Long,
            sampleSeq: Long,
            voltageV: Float,
            pct: Int,
            elapsedMs: Long,
            estMa: Float,
        ) {
            benchActive = active
            this@BatteryBenchActivity.sessionId = sessionId
            this@BatteryBenchActivity.sampleSeq = sampleSeq
            this@BatteryBenchActivity.voltageV = voltageV
            this@BatteryBenchActivity.pct = pct
            this@BatteryBenchActivity.elapsedMs = elapsedMs
            this@BatteryBenchActivity.estMa = estMa
            runOnUiThread { refreshUi() }
        }

        override fun onBanner(level: StatusBannerLevel, message: String) {
            runOnUiThread { statusBanner.show(level, message) }
        }
    }

    companion object {
        fun open(context: Context) {
            context.startActivity(Intent(context, BatteryBenchActivity::class.java))
        }
    }
}
