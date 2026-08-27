package com.esp32s3.imusim

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.widget.ProgressBar
import android.widget.TextView
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.button.MaterialButton
import org.json.JSONObject
import java.util.Locale

/**
 * Bubble-level-style flat-floor mounting calibration wizard. Not the per-boot bias zero
 * (imu_cal_accel_level, "keep board still") — this corrects a *fixed* mounting tilt against a
 * true-level reference, persists across reboots, and is applied on top of that bias zero. See
 * floor_calib.h.
 */
class FloorCalibActivity : AppCompatActivity() {

    private lateinit var statusBanner: StatusBannerController
    private lateinit var statusText: TextView
    private lateinit var resultText: TextView
    private lateinit var progressBar: ProgressBar
    private lateinit var startButton: MaterialButton
    private lateinit var clearButton: MaterialButton
    private lateinit var serviceController: ImuServiceController

    private var imuService: IImuBleService? = null
    private var connected = false
    private var sampling = false
    private var valid = false
    private var residualDeg = 0f
    private var lastUnix = 0L

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_floor_calib)
        statusBanner = StatusBannerController.attach(findViewById(android.R.id.content))
        statusText = findViewById(R.id.floorCalibStatus)
        resultText = findViewById(R.id.floorCalibResult)
        progressBar = findViewById(R.id.floorCalibProgress)
        startButton = findViewById(R.id.floorCalibStartButton)
        clearButton = findViewById(R.id.floorCalibClearButton)

        findViewById<MaterialToolbar>(R.id.floorCalibToolbar).setNavigationOnClickListener { finish() }
        startButton.setOnClickListener { confirmStart() }
        clearButton.setOnClickListener { confirmClear() }

        serviceController = ImuServiceController(applicationContext, serviceEvents)
        refreshUi()
    }

    override fun onStart() {
        super.onStart()
        serviceController.startAndBind()
    }

    override fun onResume() {
        super.onResume()
        imuService?.requestFloorCalStatus()
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
            .setTitle(R.string.floor_calib_start)
            .setMessage(R.string.floor_calib_start_confirm)
            .setPositiveButton(android.R.string.ok) { _, _ ->
                svc.floorCalibStart(0)
                statusText.text = getString(R.string.floor_calib_sampling)
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun confirmClear() {
        val svc = imuService
        if (!connected || svc == null) {
            statusBanner.show(StatusBannerLevel.WARN, getString(R.string.connect_ble_first))
            return
        }
        AlertDialog.Builder(this)
            .setTitle(R.string.floor_calib_clear)
            .setMessage(R.string.floor_calib_clear_confirm)
            .setPositiveButton(android.R.string.ok) { _, _ -> svc.floorCalibClear() }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun refreshUi() {
        startButton.isEnabled = connected && imuService != null && !sampling
        clearButton.isEnabled = connected && imuService != null && !sampling && valid
        statusText.text = when {
            !connected -> getString(R.string.connect_ble_first)
            sampling -> getString(R.string.floor_calib_sampling)
            valid -> getString(R.string.floor_calib_status_calibrated)
            else -> getString(R.string.floor_calib_status_none)
        }
        resultText.text = buildString {
            if (valid) {
                appendLine(
                    String.format(Locale.US, "corrected %.2f\u00b0 of mounting tilt", residualDeg),
                )
                if (lastUnix > 0L) {
                    appendLine("calibrated: " + java.text.DateFormat.getDateTimeInstance()
                        .format(java.util.Date(lastUnix * 1000L)))
                }
            } else {
                appendLine(getString(R.string.floor_calib_result_none))
            }
        }
    }

    private fun applyStatusJson(json: String) {
        try {
            val o = JSONObject(json)
            valid = o.optInt("valid", 0) != 0
            sampling = o.optInt("sampling", 0) != 0
            residualDeg = o.optDouble("residual_deg", 0.0).toFloat()
            lastUnix = o.optLong("unix", 0L)
            progressBar.progress = (o.optDouble("progress", 0.0) * 100.0).toInt().coerceIn(0, 100)
        } catch (_: Exception) {
            /* malformed JSON — keep last-known state */
        }
        refreshUi()
    }

    private val serviceEvents = object : ImuServiceController.Events {
        override fun onServiceReady(service: IImuBleService) {
            imuService = service
            service.requestFloorCalStatus()
            runOnUiThread { refreshUi() }
        }

        override fun onServiceLost() {
            imuService = null
            connected = false
            runOnUiThread { refreshUi() }
        }

        override fun onConnectionChanged(connected: Boolean) {
            this@FloorCalibActivity.connected = connected
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
            this@FloorCalibActivity.connected = bleConnected
            runOnUiThread { refreshUi() }
        }

        override fun onStatus(text: String) {}

        override fun onPowerStatus(power: ImuProtocol.PowerStatus) {}

        override fun onBatchJson(batchJson: String) {}

        override fun onConfigBlob(blob: ByteArray) {}

        override fun onOtaProgress(percent: Int) {}

        override fun onOtaDone(ok: Boolean, message: String) {}

        override fun onFloorCalStatus(json: String) {
            runOnUiThread { applyStatusJson(json) }
        }

        override fun onBanner(level: StatusBannerLevel, message: String) {
            runOnUiThread { statusBanner.show(level, message) }
        }
    }

    companion object {
        fun open(context: Context) {
            context.startActivity(Intent(context, FloorCalibActivity::class.java))
        }
    }
}
