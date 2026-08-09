package com.esp32s3.imusim

import android.os.Bundle
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import org.json.JSONObject

/** Step-through editor: sync ESP config → edit sections → push with revision check → cloud upload. */
class ConfigEditorActivity : AppCompatActivity() {
    private lateinit var controller: ImuServiceController
    private var imuService: IImuBleService? = null
    private var baseBlob: ByteArray? = null
    private var doc: DeviceConfigJson.Doc? = null
    private lateinit var summaryText: TextView
    private lateinit var jsonPreview: EditText

    private val steps = listOf("Power", "Vibro schedule", "Capture mix", "Review & push")
    private var stepIndex = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(32, 32, 32, 32)
        }
        summaryText = TextView(this).apply {
            textSize = 14f
            setTextColor(0xFFE6EDF3.toInt())
        }
        jsonPreview = EditText(this).apply {
            setTextColor(0xFFE6EDF3.toInt())
            setBackgroundColor(0xFF161B22.toInt())
            minLines = 8
        }
        root.addView(TextView(this).apply {
            text = getString(R.string.config_editor_title)
            textSize = 18f
            setTextColor(0xFFFFFFFF.toInt())
        })
        root.addView(summaryText)
        val scroll = ScrollView(this).apply { addView(jsonPreview) }
        root.addView(scroll, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f,
        ))
        val nav = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        val backBtn = Button(this).apply { text = "Back" }
        val nextBtn = Button(this).apply { text = "Next" }
        val syncBtn = Button(this).apply { text = "Sync from ESP" }
        nav.addView(syncBtn)
        nav.addView(backBtn)
        nav.addView(nextBtn)
        root.addView(nav)
        setContentView(root)

        controller = ImuServiceController(this, object : ImuServiceController.Events {
            override fun onServiceReady(service: IImuBleService) {
                imuService = service
                syncFromEsp()
            }
            override fun onServiceLost() { imuService = null }
            override fun onConnectionChanged(connected: Boolean) {
                if (!connected) summaryText.text = "Connect BLE first"
            }
            override fun onStatus(text: String) {}
            override fun onPowerStatus(power: ImuProtocol.PowerStatus) {}
            override fun onBatchJson(batchJson: String) {}
            override fun onConfigBlob(blob: ByteArray) {
                runOnUiThread { applyBlob(blob) }
            }
            override fun onOtaProgress(percent: Int) {}
            override fun onOtaDone(ok: Boolean, message: String) {}
        })
        controller.startAndBind()

        syncBtn.setOnClickListener { syncFromEsp() }
        backBtn.setOnClickListener {
            if (stepIndex > 0) {
                stepIndex--
                renderStep()
            }
        }
        nextBtn.setOnClickListener {
            if (stepIndex < steps.lastIndex) {
                captureStepEdits()
                stepIndex++
                renderStep()
            } else {
                confirmPush()
            }
        }
    }

    override fun onDestroy() {
        controller.unbind()
        super.onDestroy()
    }

    private fun syncFromEsp() {
        summaryText.text = "Reading config from ESP…"
        imuService?.requestConfigSync()
    }

    private fun applyBlob(blob: ByteArray) {
        baseBlob = blob
        doc = DeviceConfigJson.fromBlob(blob, "esp")
        stepIndex = 0
        renderStep()
    }

    private fun renderStep() {
        val d = doc ?: run {
            summaryText.text = "No config — tap Sync from ESP"
            return
        }
        summaryText.text = "Step ${stepIndex + 1}/${steps.size}: ${steps[stepIndex]}\nrev=${d.revision} loc=${d.localRevision}"
        when (stepIndex) {
            0 -> jsonPreview.setText(d.profile.toString(2))
            1 -> jsonPreview.setText(d.vibro.toString(2))
            2 -> jsonPreview.setText(d.mix.toString(2))
            else -> jsonPreview.setText(
                DeviceConfigJson.toJson(d, packageInfoVersion()).toString(2),
            )
        }
    }

    private fun captureStepEdits() {
        val d = doc ?: return
        val parsed = runCatching { JSONObject(jsonPreview.text.toString()) }.getOrNull() ?: return
        doc = when (stepIndex) {
            0 -> d.copy(profile = parsed)
            1 -> d.copy(vibro = parsed)
            2 -> d.copy(mix = parsed)
            else -> d
        }
    }

    private fun packageInfoVersion(): String =
        runCatching {
            packageManager.getPackageInfo(packageName, 0).versionName ?: "unknown"
        }.getOrDefault("unknown")

    private fun confirmPush() {
        captureStepEdits()
        val d = doc ?: return
        val base = baseBlob ?: return
        val svc = imuService ?: run {
            summaryText.text = "Not connected"
            return
        }
        val newRev = DeviceConfigJson.nextCloudRevision(d.revision)
        val merged = DeviceConfigJson.mergeIntoBlob(base, d, newRev)
        AlertDialog.Builder(this)
            .setTitle("Push config?")
            .setMessage(
                "Revision $newRev → ESP\n\n" +
                    "ESP rejects if cloud rev < device rev.\nLocal TFT overlay preserved on device.",
            )
            .setPositiveButton("Push") { _, _ ->
                svc.pushConfig(merged, true)
                doc = d.copy(revision = newRev, source = "phone")
                ConfigCloudSync.upload(this, doc!!)
                summaryText.text = "Pushed rev $newRev — check ESP STATUS cfgseq"
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }
}
