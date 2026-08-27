package com.esp32s3.imusim

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.appbar.MaterialToolbar
import org.json.JSONObject
import java.util.Locale
import kotlin.math.atan2
import kotlin.math.asin

/**
 * Live AHRS orientation readout — deliberately its own screen, not folded into the raw-data
 * panel, because entering it forces full CPU clock + max IMU sample rate (see onServiceReady /
 * onStop below) for the smoothest on-device attitude fusion. That is the opposite of every
 * other mode in this app, which defaults to power-saving Auto — see ahrs_help string and
 * MainActivity's showPerformanceDialog(). For the 3D cube view, see backend/web/ahrs.html
 * (relayed via ImuBleForegroundService.maybeRelayAhrs, independent of this screen).
 */
class AhrsActivity : AppCompatActivity() {

    private lateinit var statusText: TextView
    private lateinit var readoutText: TextView
    private lateinit var serviceController: ImuServiceController

    private var imuService: IImuBleService? = null
    private var connected = false
    private var boosted = false
    private var lastSampleMs = 0L

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_ahrs)
        statusText = findViewById(R.id.ahrsStatus)
        readoutText = findViewById(R.id.ahrsReadout)
        findViewById<MaterialToolbar>(R.id.ahrsToolbar).setNavigationOnClickListener { finish() }

        serviceController = ImuServiceController(applicationContext, serviceEvents)
        refreshUi()
    }

    override fun onStart() {
        super.onStart()
        serviceController.startAndBind()
    }

    override fun onStop() {
        revertSpeedBoost()
        serviceController.unbind()
        super.onStop()
    }

    private fun maybeBoostSpeed() {
        val svc = imuService ?: return
        if (!connected || boosted) return
        boosted = true
        svc.setCpuMhzOverride(240)
        svc.setImuHzOverride(100)
        statusText.text = getString(R.string.ahrs_status_boosting)
    }

    private fun revertSpeedBoost() {
        if (!boosted) return
        boosted = false
        runCatching {
            imuService?.setCpuMhzOverride(0)
            imuService?.setImuHzOverride(0)
        }
    }

    private fun refreshUi() {
        if (!connected) {
            statusText.text = getString(R.string.connect_ble_first)
            return
        }
        val ageMs = System.currentTimeMillis() - lastSampleMs
        statusText.text = if (lastSampleMs == 0L || ageMs > 4000L) {
            getString(R.string.ahrs_status_stale)
        } else {
            getString(R.string.ahrs_status_live) + " (240 MHz / 100 Hz)"
        }
    }

    private fun applyBatchJson(json: String) {
        val rot = runCatching {
            val root = JSONObject(json)
            val rot4 = root.optJSONArray("rot4") ?: return@runCatching null
            if (rot4.length() != 9) return@runCatching null
            DoubleArray(9) { i -> rot4.optInt(i, 0) / 10000.0 }
        }.getOrNull() ?: return

        lastSampleMs = System.currentTimeMillis()
        val pitch = Math.toDegrees(asin((-rot[6]).coerceIn(-1.0, 1.0)))
        val roll = Math.toDegrees(atan2(rot[7], rot[8]))
        val yaw = Math.toDegrees(atan2(rot[3], rot[0]))
        readoutText.text = String.format(
            Locale.US,
            "Roll:  %6.1f°\nPitch: %6.1f°\nYaw:   %6.1f°",
            roll,
            pitch,
            yaw,
        )
        refreshUi()
    }

    private val serviceEvents = object : ImuServiceController.Events {
        override fun onServiceReady(service: IImuBleService) {
            imuService = service
            runOnUiThread {
                refreshUi()
                maybeBoostSpeed()
            }
        }

        override fun onServiceLost() {
            imuService = null
            connected = false
            boosted = false
            runOnUiThread { refreshUi() }
        }

        override fun onConnectionChanged(connected: Boolean) {
            this@AhrsActivity.connected = connected
            runOnUiThread {
                refreshUi()
                if (connected) maybeBoostSpeed()
            }
        }

        override fun onRelayState(
            state: RelayFsmState,
            caption: String,
            bleConnected: Boolean,
            showDisconnect: Boolean,
        ) {
            /* requestState() (called right after binding) replies with this — NOT
             * onConnectionChanged, which only fires on a *transition*. */
            this@AhrsActivity.connected = bleConnected
            runOnUiThread {
                refreshUi()
                if (bleConnected) maybeBoostSpeed()
            }
        }

        override fun onStatus(text: String) {}

        override fun onPowerStatus(power: ImuProtocol.PowerStatus) {}

        override fun onBatchJson(batchJson: String) {
            runOnUiThread { applyBatchJson(batchJson) }
        }

        override fun onConfigBlob(blob: ByteArray) {}

        override fun onOtaProgress(percent: Int) {}

        override fun onOtaDone(ok: Boolean, message: String) {}

        override fun onFloorCalStatus(json: String) {}

        override fun onBanner(level: StatusBannerLevel, message: String) {}
    }

    companion object {
        fun open(context: Context) {
            context.startActivity(Intent(context, AhrsActivity::class.java))
        }
    }
}
