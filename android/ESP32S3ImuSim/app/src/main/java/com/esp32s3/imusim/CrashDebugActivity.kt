package com.esp32s3.imusim

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.button.MaterialButton

/** Dev-only ESP fault injection — triggers crash ring → reboot → phone relay → cloud. */
class CrashDebugActivity : AppCompatActivity() {

    private lateinit var statusBanner: StatusBannerController
    private lateinit var statusText: TextView
    private lateinit var injectContainer: LinearLayout
    private lateinit var bistButton: MaterialButton
    private lateinit var serviceController: ImuServiceController

    private var imuService: IImuBleService? = null
    private var connected = false
    private var debugFirmware = false
    private var sessionCaps = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_crash_debug)
        statusBanner = StatusBannerController.attach(findViewById(android.R.id.content))
        statusText = findViewById(R.id.crashDebugStatus)
        injectContainer = findViewById(R.id.crashInjectButtonContainer)
        bistButton = findViewById(R.id.crashDebugBistButton)

        findViewById<MaterialToolbar>(R.id.crashDebugToolbar).setNavigationOnClickListener { finish() }

        serviceController = ImuServiceController(applicationContext, serviceEvents)
        buildInjectButtons()
        wireBist()
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

    private fun buildInjectButtons() {
        injectContainer.removeAllViews()
        for (spec in CrashInjectKind.all) {
            val row = MaterialButton(this).apply {
                layoutParams = LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT,
                ).apply { bottomMargin = dp(8) }
                text = spec.title
                isAllCaps = false
                setOnClickListener { confirmInject(spec) }
            }
            injectContainer.addView(row)
        }
    }

    private fun wireBist() {
        bistButton.setOnClickListener {
            val svc = imuService
            if (!connected || svc == null) {
                statusBanner.show(StatusBannerLevel.WARN, getString(R.string.connect_ble_first))
                return@setOnClickListener
            }
            svc.runDeviceBist()
            statusText.text = getString(R.string.crash_debug_bist_sent)
        }
    }

    private fun confirmInject(spec: CrashInjectKind.Spec) {
        if (!connected || imuService == null) {
            statusBanner.show(StatusBannerLevel.WARN, getString(R.string.connect_ble_first))
            return
        }
        AlertDialog.Builder(this)
            .setTitle(spec.title)
            .setMessage(getString(R.string.crash_debug_inject_confirm, spec.detail))
            .setPositiveButton(R.string.crash_debug_inject_action) { _, _ ->
                imuService?.injectCrash(spec.id)
                statusText.text = getString(R.string.crash_debug_inject_sent, spec.title)
                statusBanner.show(
                    StatusBannerLevel.WARN,
                    getString(R.string.crash_debug_inject_sent, spec.title),
                )
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun refreshUi() {
        val fwReady = debugFirmware || ImuProtocol.crashDebugFromCaps(sessionCaps)
        statusText.text = when {
            !connected -> getString(R.string.crash_debug_status_disconnected)
            fwReady -> getString(R.string.crash_debug_status_ready)
            else -> getString(R.string.crash_debug_status_try_anyway)
        }
        val enabled = connected && imuService != null
        bistButton.isEnabled = enabled
        for (i in 0 until injectContainer.childCount) {
            injectContainer.getChildAt(i).isEnabled = enabled
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
            this@CrashDebugActivity.connected = connected
            runOnUiThread { refreshUi() }
        }

        override fun onSessionRestore(snapshot: Bundle) {
            sessionCaps = snapshot.getInt(ImuSessionStore.KEY_CAPS, 0)
            debugFirmware = snapshot.getBoolean(ImuSessionStore.KEY_CRASH_DEBUG, false) ||
                ImuProtocol.crashDebugFromCaps(sessionCaps)
            connected = snapshot.getBoolean(ImuSessionStore.KEY_CONNECTED, false)
            runOnUiThread { refreshUi() }
        }

        override fun onStatus(text: String) {
            if (text.contains("dbg", ignoreCase = true)) {
                debugFirmware = true
                runOnUiThread { refreshUi() }
            }
        }

        override fun onPowerStatus(power: ImuProtocol.PowerStatus) {}
        override fun onBatchJson(batchJson: String) {}
        override fun onConfigBlob(blob: ByteArray) {}
        override fun onOtaProgress(percent: Int) {}
        override fun onOtaDone(ok: Boolean, message: String) {}
        override fun onVibroCaption(caption: String) {
            if (caption.contains("dbg")) {
                debugFirmware = true
                runOnUiThread { refreshUi() }
            }
        }
    }

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()

    companion object {
        fun open(context: Context) {
            context.startActivity(Intent(context, CrashDebugActivity::class.java))
        }
    }
}
