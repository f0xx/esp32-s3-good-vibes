package com.esp32s3.imusim

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.widget.ArrayAdapter
import android.widget.CheckBox
import android.widget.Spinner
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.button.MaterialButton
import com.google.android.material.textfield.TextInputEditText
import java.util.concurrent.Executors

/** Attach this ESP to a machine, then START/END REPAIR with an FTA leaf. */
class RepairWizardActivity : AppCompatActivity() {

    private lateinit var statusBanner: StatusBannerController
    private lateinit var statusText: TextView
    private lateinit var machineKeyInput: TextInputEditText
    private lateinit var machineNameInput: TextInputEditText
    private lateinit var kindSpinner: Spinner
    private lateinit var leafSpinner: Spinner
    private lateinit var partsInput: TextInputEditText
    private lateinit var notesInput: TextInputEditText
    private lateinit var newRefCheck: CheckBox
    private lateinit var serviceController: ImuServiceController
    private val io = Executors.newSingleThreadExecutor()
    private val api by lazy { CloudOperatorApi(this) }

    private var imuService: IImuBleService? = null
    private var connected = false
    private var leaves: List<CloudOperatorApi.FtaLeaf> = emptyList()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_repair_wizard)
        statusBanner = StatusBannerController.attach(findViewById(android.R.id.content))
        statusText = findViewById(R.id.repairWizardStatus)
        machineKeyInput = findViewById(R.id.repairMachineKey)
        machineNameInput = findViewById(R.id.repairMachineName)
        kindSpinner = findViewById(R.id.repairMachineKind)
        leafSpinner = findViewById(R.id.repairFtaLeaf)
        partsInput = findViewById(R.id.repairParts)
        notesInput = findViewById(R.id.repairNotes)
        newRefCheck = findViewById(R.id.repairNewRef)

        findViewById<MaterialToolbar>(R.id.repairWizardToolbar).setNavigationOnClickListener { finish() }
        kindSpinner.adapter = ArrayAdapter(
            this,
            android.R.layout.simple_spinner_dropdown_item,
            listOf("pump", "generic"),
        )
        findViewById<MaterialButton>(R.id.repairAttachButton).setOnClickListener { attachMachine() }
        findViewById<MaterialButton>(R.id.repairStartButton).setOnClickListener { startRepair() }
        findViewById<MaterialButton>(R.id.repairEndButton).setOnClickListener { endRepair() }

        serviceController = ImuServiceController(applicationContext, serviceEvents)
        loadCloud()
    }

    override fun onStart() {
        super.onStart()
        serviceController.startAndBind()
    }

    override fun onStop() {
        serviceController.unbind()
        super.onStop()
    }

    override fun onDestroy() {
        io.shutdownNow()
        super.onDestroy()
    }

    private fun loadCloud() {
        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        machineKeyInput.setText(prefs.getString(KEY_MACHINE, ""))
        machineNameInput.setText(prefs.getString(KEY_NAME, ""))
        io.execute {
            val machines = api.parseMachines(api.listMachines())
            val kind = kindSpinner.selectedItem?.toString() ?: "pump"
            val leafList = runCatching { api.ftaLeaves(kind) }.getOrDefault(api.defaultLeaves())
            val op = runCatching { api.operatorStatus(api.deviceId()) }.getOrNull()
            runOnUiThread {
                leaves = leafList
                leafSpinner.adapter = ArrayAdapter(
                    this,
                    android.R.layout.simple_spinner_dropdown_item,
                    leafList.map { "${it.label} (${it.category})" },
                )
                leafSpinner.setOnItemSelectedListener(
                    object : android.widget.AdapterView.OnItemSelectedListener {
                        override fun onItemSelected(
                            parent: android.widget.AdapterView<*>?,
                            view: android.view.View?,
                            position: Int,
                            id: Long,
                        ) {
                            leaves.getOrNull(position)?.let { newRefCheck.isChecked = it.newRefRequired }
                        }

                        override fun onNothingSelected(parent: android.widget.AdapterView<*>?) {}
                    },
                )
                if (machineKeyInput.text.isNullOrBlank() && machines.isNotEmpty()) {
                    val first = machines.first()
                    machineKeyInput.setText(first.machineKey)
                    machineNameInput.setText(first.name)
                }
                statusText.text = formatStatus(op, machines)
                op?.let { showOperatorBanner(it) }
            }
        }
    }

    private fun attachMachine() {
        val key = machineKeyInput.text?.toString()?.trim().orEmpty()
        val name = machineNameInput.text?.toString()?.trim().orEmpty().ifBlank { key }
        val kind = kindSpinner.selectedItem?.toString() ?: "pump"
        if (key.isBlank()) {
            statusBanner.show(StatusBannerLevel.WARN, "Enter a machine key")
            return
        }
        persistMachine(key, name)
        io.execute {
            var created = api.createMachine(key, name, kind)
            if (!created.ok && created.message.contains("409")) {
                created = CloudOperatorApi.HttpResult(true, "machine exists")
            }
            val attach = if (created.ok) api.attachSensor(key, api.deviceId(), name) else created
            val ok = attach.ok || attach.message.contains("409")
            runOnUiThread {
                statusBanner.show(
                    if (ok) StatusBannerLevel.OK else StatusBannerLevel.ERROR,
                    if (ok) "Sensor attached to $key" else attach.message,
                )
                statusText.text = if (ok) "Attached ${api.deviceId()} → $key" else attach.message
            }
        }
    }

    private fun startRepair() {
        val key = requireMachineKey() ?: return
        persistMachine(key, machineNameInput.text?.toString().orEmpty())
        imuService?.vibroSetSensingPaused(true)
        io.execute {
            val r = api.startRepair(key, api.deviceId())
            runOnUiThread {
                statusBanner.show(
                    if (r.ok) StatusBannerLevel.OK else StatusBannerLevel.ERROR,
                    if (r.ok) "START REPAIR — candidate trend paused" else r.message,
                )
                if (r.ok) statusText.text = "Repair open on $key. Mechanic work, then END REPAIR."
            }
        }
    }

    private fun endRepair() {
        val key = requireMachineKey() ?: return
        val leaf = leaves.getOrNull(leafSpinner.selectedItemPosition) ?: api.defaultLeaves().first()
        val parts = partsInput.text?.toString()?.trim().orEmpty()
        val notes = notesInput.text?.toString()?.trim().orEmpty()
        val newRef = newRefCheck.isChecked
        io.execute {
            val r = api.endRepair(key, api.deviceId(), leaf.leafId, parts, notes, newRef)
            runOnUiThread {
                if (!r.ok) {
                    statusBanner.show(StatusBannerLevel.ERROR, r.message)
                    return@runOnUiThread
                }
                if (newRef) {
                    statusBanner.show(StatusBannerLevel.WARN, "Re-record references after mechanical work")
                    VibroRefWizardActivity.open(this)
                } else {
                    imuService?.vibroArm()
                    statusBanner.show(StatusBannerLevel.OK, "END REPAIR — armed, same refs")
                }
                finish()
            }
        }
    }

    private fun requireMachineKey(): String? {
        val key = machineKeyInput.text?.toString()?.trim().orEmpty()
        if (key.isBlank()) {
            statusBanner.show(StatusBannerLevel.WARN, "Attach a machine first")
            return null
        }
        if (!api.configured()) {
            statusBanner.show(StatusBannerLevel.ERROR, "Enable Cloud first")
            return null
        }
        return key
    }

    private fun persistMachine(key: String, name: String) {
        getSharedPreferences(PREFS, MODE_PRIVATE).edit()
            .putString(KEY_MACHINE, key)
            .putString(KEY_NAME, name)
            .apply()
    }

    private fun formatStatus(op: CloudOperatorApi.OperatorStatus?, machines: List<CloudOperatorApi.MachineRow>): String {
        val device = api.deviceId().ifBlank { "(set device id in Cloud)" }
        val opLine = if (op == null) {
            "Operator: (cloud unreachable)"
        } else {
            val page = if (op.operatorAlert) "PAGE mechanic" else "no page"
            "Operator ${levelName(op.operatorLevel)} · $page · cand=${levelName(op.candidateLevel)} · ${op.trend}"
        }
        val hint = op?.hintLabel?.let { "\nFTA hint: $it" }.orEmpty()
        val machinesLine = if (machines.isEmpty()) "No machines yet." else
            machines.joinToString(" · ") { "${it.machineKey} (${it.sensorCount})" }
        return "Device $device\n$opLine$hint\n$machinesLine"
    }

    private fun showOperatorBanner(op: CloudOperatorApi.OperatorStatus) {
        if (op.operatorAlert) {
            statusBanner.show(StatusBannerLevel.ERROR, "Operator ALARM — page the mechanic")
        } else if (op.operatorLevel >= 1) {
            statusBanner.show(StatusBannerLevel.WARN, "Operator WARN — trend, not a single window")
        }
    }

    private fun levelName(level: Int): String = when (level) {
        2 -> "ALARM"
        1 -> "WARN"
        else -> "OK"
    }

    private val serviceEvents = object : ImuServiceController.Events {
        override fun onServiceReady(service: IImuBleService) {
            imuService = service
        }

        override fun onServiceLost() {
            imuService = null
            connected = false
        }

        override fun onConnectionChanged(connected: Boolean) {
            this@RepairWizardActivity.connected = connected
        }

        override fun onCaps(caps: Int) {}

        override fun onRelayState(
            state: RelayFsmState,
            caption: String,
            bleConnected: Boolean,
            showDisconnect: Boolean,
        ) {
            this@RepairWizardActivity.connected = bleConnected
        }

        override fun onStatus(text: String) {}
        override fun onPowerStatus(power: ImuProtocol.PowerStatus) {}
        override fun onBatchJson(batchJson: String) {}
        override fun onConfigBlob(blob: ByteArray) {}
        override fun onOtaProgress(percent: Int) {}
        override fun onOtaDone(ok: Boolean, message: String) {}
        override fun onVibroRefList(json: String) {}

        override fun onBanner(level: StatusBannerLevel, message: String) {
            runOnUiThread { statusBanner.show(level, message) }
        }
    }

    companion object {
        private const val PREFS = "repair_wizard"
        private const val KEY_MACHINE = "machine_key"
        private const val KEY_NAME = "machine_name"

        fun open(context: Context) {
            context.startActivity(Intent(context, RepairWizardActivity::class.java))
        }
    }
}
