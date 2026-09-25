package com.esp32s3.imusim

import android.content.Intent
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.ProgressBar
import android.widget.TextView
import androidx.activity.addCallback
import androidx.appcompat.app.AppCompatActivity
import androidx.core.text.HtmlCompat
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.button.MaterialButton
import com.google.android.material.textfield.TextInputEditText
import com.google.android.material.textfield.TextInputLayout

/**
 * Full-screen WiFi provisioning over BLE (replaces nested AlertDialog menus).
 */
class WifiWizardActivity : AppCompatActivity() {

    private enum class Screen { HUB, SCAN, PROFILES, CONNECT, PROV }

    private lateinit var sessionStore: ImuSessionStore
    private lateinit var serviceController: ImuServiceController
    private var imuService: IImuBleService? = null
    private var bleConnected = false
    private var expectScanAcrossDisconnect = false
    private var pendingScan = false

    private lateinit var toolbar: MaterialToolbar
    private lateinit var statusLine: TextView
    private lateinit var content: ViewGroup

    private var screen = Screen.HUB
    private var scanJson: String? = null
    private var profilesJson: String? = null
    private var netStatusJson: String? = null

    private var connectSsid: String = ""
    private var connectSecured: Boolean = false
    private var connectProfileIdx: Int = -1
    private var connectFromSaved: Boolean = false
    private var provErase: Boolean = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_wifi_wizard)
        sessionStore = ImuSessionStore(this)
        scanJson = sessionStore.lastNetScanJson
        profilesJson = sessionStore.lastNetProfilesJson
        netStatusJson = sessionStore.lastNetStatusJson

        toolbar = findViewById(R.id.wifiToolbar)
        statusLine = findViewById(R.id.wifiStatusLine)
        content = findViewById(R.id.wifiContent)
        toolbar.setNavigationOnClickListener { onBackPressedDispatcher.onBackPressed() }

        serviceController = ImuServiceController(applicationContext, serviceEvents)
        serviceController.startAndBind()

        onBackPressedDispatcher.addCallback(this) {
            when (screen) {
                Screen.HUB -> finish()
                else -> showScreen(Screen.HUB)
            }
        }

        when {
            intent.wantsWifiScan() -> showScreen(Screen.SCAN, requestScan = true)
            intent.getStringExtra(EXTRA_START) == START_PROFILES ->
                showScreen(Screen.PROFILES, requestProfiles = true)
            else -> showScreen(Screen.HUB)
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        if (intent.wantsWifiScan()) {
            showScreen(Screen.SCAN, requestScan = true)
        }
    }

    override fun onStart() {
        super.onStart()
        if (::serviceController.isInitialized) {
            serviceController.setUiVisible(true)
            serviceController.requestState()
            if (screen == Screen.SCAN) {
                pullScanFromSessionOrPrefs()
            }
        }
    }

    override fun onDestroy() {
        serviceController.unbind()
        super.onDestroy()
    }

    private val serviceEvents = object : ImuServiceController.Events {
        override fun onServiceReady(service: IImuBleService) {
            imuService = service
            serviceController.setUiVisible(true)
            serviceController.requestState()
            if (pendingScan) {
                startScan()
            }
        }

        override fun onSessionRestore(snapshot: Bundle) {
            applySnapshot(snapshot)
        }

        override fun onServiceLost() {
            imuService = null
            bleConnected = false
            setStatus(getString(R.string.wifi_ble_required))
        }

        override fun onConnectionChanged(connected: Boolean) {
            bleConnected = connected
            if (!connected) {
                if (expectScanAcrossDisconnect) {
                    setStatus(getString(R.string.wifi_scan_ble_paused))
                } else {
                    setStatus(getString(R.string.wifi_ble_required))
                }
            } else if (pendingScan) {
                startScan()
            } else if (expectScanAcrossDisconnect) {
                setStatus(getString(R.string.wifi_scanning))
                /* ESP finished while we were dropped — pull cached SCAN JSON. */
                pollScanAfterReconnect()
            }
        }

        override fun onCaps(caps: Int) {}

        override fun onPowerStatus(power: ImuProtocol.PowerStatus) {}

        override fun onBatchJson(batchJson: String) {}

        override fun onConfigBlob(blob: ByteArray) {}

        override fun onOtaProgress(percent: Int) {}

        override fun onOtaDone(ok: Boolean, message: String) {}

        override fun onStatus(text: String) {
            runOnUiThread {
                if (text.contains("WiFi", ignoreCase = true) ||
                    text.contains("Net", ignoreCase = true) ||
                    text.contains("GATT", ignoreCase = true)
                ) {
                    setStatus(text)
                }
            }
        }

        override fun onNetScan(json: String) {
            runOnUiThread {
                applyScanJson(json, fromLive = true)
            }
        }

        override fun onNetProfiles(json: String) {
            runOnUiThread {
                profilesJson = json
                if (screen == Screen.PROFILES) {
                    renderProfilesScreen()
                }
            }
        }

        override fun onNetStatus(json: String) {
            runOnUiThread {
                netStatusJson = json
                updateNetStatusLine(json)
                val st = runCatching { NetProtocol.parseStatus(json).state }.getOrNull()
                /* scan_done notify can arrive while SCAN list still shows the local
                 * scanning:1 placeholder — pull the service/session cache immediately. */
                if (st == "scan_done" || st == "scan_failed") {
                    expectScanAcrossDisconnect = false
                    scanPollRunnable?.let { statusLine.removeCallbacks(it) }
                    scanPollRunnable = null
                    pullScanFromSessionOrPrefs()
                }
                if (screen == Screen.PROV) {
                    renderProvStatus()
                }
            }
        }
    }

    private fun applySnapshot(snap: Bundle) {
        bleConnected = snap.getBoolean(ImuSessionStore.KEY_CONNECTED, false)
        snap.getString(ImuSessionStore.KEY_LAST_NET_SCAN)?.let { applyScanJson(it, fromLive = false) }
        snap.getString(ImuSessionStore.KEY_LAST_NET_PROFILES)?.let { profilesJson = it }
        snap.getString(ImuSessionStore.KEY_LAST_NET_STATUS)?.let {
            netStatusJson = it
            updateNetStatusLine(it)
        }
        if (expectScanAcrossDisconnect && bleConnected) {
            setStatus(getString(R.string.wifi_scanning))
        } else if (bleConnected) {
            setStatus(
                snap.getString(ImuSessionStore.KEY_RELAY_CAPTION)
                    ?: getString(R.string.notification_connected),
            )
        } else if (expectScanAcrossDisconnect) {
            setStatus(getString(R.string.wifi_scan_ble_paused))
        } else {
            setStatus(getString(R.string.wifi_ble_required))
        }
    }

    /** Prefer non-empty AP lists over the local scanning:1 placeholder. */
    private fun applyScanJson(json: String, fromLive: Boolean) {
        val incomingAps = runCatching { NetProtocol.parseScan(json) }.getOrElse { emptyList() }
        val incomingScanning = json.contains("\"scanning\":1")
        val curAps = runCatching { NetProtocol.parseScan(scanJson ?: "") }.getOrElse { emptyList() }

        if (incomingScanning && incomingAps.isEmpty() && curAps.isNotEmpty()) {
            /* Keep prior rows visible while a new scan is in flight. */
            scanJson = scanJson!!.replace(Regex("\"scanning\"\\s*:\\s*\\d"), "\"scanning\":1").let {
                if (it.contains("\"scanning\"")) it else it.replace(Regex("\\}\\s*$"), ",\"scanning\":1}")
            }
        } else {
            scanJson = json
        }

        if (!incomingScanning) {
            expectScanAcrossDisconnect = false
            scanPollRunnable?.let { statusLine.removeCallbacks(it) }
            scanPollRunnable = null
        }

        if (screen == Screen.SCAN) {
            val aps = runCatching { NetProtocol.parseScan(scanJson ?: "") }.getOrElse { emptyList() }
            if (!incomingScanning || aps.isNotEmpty()) {
                setStatus(
                    if (aps.isEmpty()) {
                        getString(R.string.wifi_scan_empty)
                    } else if (incomingScanning) {
                        getString(R.string.wifi_scanning) + " · ${aps.size}"
                    } else {
                        getString(R.string.wifi_scan_title) + " · ${aps.size}"
                    },
                )
            }
            renderScanScreen()
        }
    }

    private fun pullScanFromSessionOrPrefs() {
        val fromPrefs = sessionStore.lastNetScanJson
        if (!fromPrefs.isNullOrBlank()) {
            applyScanJson(fromPrefs, fromLive = false)
        }
        serviceController.requestState()
    }

    private fun showScreen(
        next: Screen,
        requestScan: Boolean = false,
        requestProfiles: Boolean = false,
    ) {
        screen = next
        content.removeAllViews()
        when (next) {
            Screen.HUB -> {
                toolbar.title = getString(R.string.wifi_wizard)
                inflate(R.layout.wifi_screen_hub)
                findViewById<MaterialButton>(R.id.btnScan).setOnClickListener {
                    showScreen(Screen.SCAN, requestScan = true)
                }
                findViewById<MaterialButton>(R.id.btnProfiles).setOnClickListener {
                    showScreen(Screen.PROFILES, requestProfiles = true)
                }
                findViewById<MaterialButton>(R.id.btnProvKeep).setOnClickListener {
                    provErase = false
                    sendNetCommand("""{"op":"prov","reset":0}""", "Opening setup hotspot…")
                    showScreen(Screen.PROV)
                }
                findViewById<MaterialButton>(R.id.btnProvErase).setOnClickListener {
                    provErase = true
                    sendNetCommand("""{"op":"prov","reset":1}""", "Erasing profiles, opening AP…")
                    showScreen(Screen.PROV)
                }
            }
            Screen.SCAN -> {
                toolbar.title = getString(R.string.wifi_scan_title)
                inflate(R.layout.wifi_screen_list)
                findViewById<MaterialButton>(R.id.wifiListAction).apply {
                    text = getString(R.string.wifi_rescan)
                    setOnClickListener { startScan() }
                }
                renderScanScreen()
                if (requestScan) startScan()
            }
            Screen.PROFILES -> {
                toolbar.title = getString(R.string.wifi_profiles_title)
                inflate(R.layout.wifi_screen_list)
                findViewById<MaterialButton>(R.id.wifiListAction).apply {
                    text = getString(R.string.wifi_refresh)
                    setOnClickListener { startProfiles() }
                }
                renderProfilesScreen()
                if (requestProfiles) startProfiles()
            }
            Screen.CONNECT -> {
                toolbar.title = getString(R.string.wifi_connect_title)
                inflate(R.layout.wifi_screen_connect)
                bindConnectScreen()
            }
            Screen.PROV -> {
                toolbar.title = getString(R.string.wifi_prov_title)
                inflate(R.layout.wifi_screen_prov)
                findViewById<TextView>(R.id.provBody).text =
                    HtmlCompat.fromHtml(getString(R.string.wifi_prov_body), HtmlCompat.FROM_HTML_MODE_LEGACY)
                renderProvStatus()
            }
        }
    }

    private fun inflate(layoutId: Int) {
        LayoutInflater.from(this).inflate(layoutId, content, true)
    }

    private fun requireBle(): Boolean {
        if (bleConnected && imuService != null) return true
        setStatus(getString(R.string.wifi_ble_required))
        return false
    }

    private var scanPollRunnable: Runnable? = null

    private fun pollScanAfterReconnect() {
        scanPollRunnable?.let { statusLine.removeCallbacks(it) }
        var attempt = 0
        val poll = object : Runnable {
            override fun run() {
                if (!expectScanAcrossDisconnect || !bleConnected) return
                pullScanFromSessionOrPrefs()
                attempt++
                val haveAps = runCatching {
                    NetProtocol.parseScan(scanJson ?: "").isNotEmpty()
                }.getOrDefault(false)
                val stillScanning = scanJson?.contains("\"scanning\":1") == true
                if (haveAps && !stillScanning) {
                    expectScanAcrossDisconnect = false
                    return
                }
                if (attempt < 12 && expectScanAcrossDisconnect) {
                    statusLine.postDelayed(this, 1_200L)
                } else if (expectScanAcrossDisconnect) {
                    expectScanAcrossDisconnect = false
                    if (!haveAps) {
                        scanJson = """{"aps":[],"scanning":0}"""
                        if (screen == Screen.SCAN) renderScanScreen()
                        setStatus(getString(R.string.wifi_scan_empty))
                    } else if (screen == Screen.SCAN) {
                        renderScanScreen()
                    }
                }
            }
        }
        scanPollRunnable = poll
        statusLine.postDelayed(poll, 500L)
    }

    private fun startScan() {
        if (!requireBle()) {
            pendingScan = true
            setStatus(getString(R.string.wifi_scanning))
            return
        }
        pendingScan = false
        expectScanAcrossDisconnect = true
        /* Do not wipe prior APs — ESP drops BLE for airtime and the UI would flash empty. */
        val prior = scanJson
        scanJson = when {
            prior != null && prior.contains("\"aps\"") ->
                if (prior.contains("\"scanning\"")) {
                    prior.replace(Regex("\"scanning\"\\s*:\\s*\\d"), "\"scanning\":1")
                } else {
                    prior.replace(Regex("\\}\\s*$"), ",\"scanning\":1}")
                }
            else -> """{"aps":[],"scanning":1}"""
        }
        setStatus(getString(R.string.wifi_scanning))
        if (screen == Screen.SCAN) renderScanScreen()
        imuService?.requestNetScan()
        statusLine.postDelayed({
            if (!expectScanAcrossDisconnect) return@postDelayed
            pullScanFromSessionOrPrefs()
            if (!expectScanAcrossDisconnect) return@postDelayed
            if (scanJson != null && !scanJson!!.contains("\"scanning\":1")) return@postDelayed
            val haveAps = runCatching {
                NetProtocol.parseScan(scanJson ?: "").isNotEmpty()
            }.getOrDefault(false)
            expectScanAcrossDisconnect = false
            if (haveAps) {
                scanJson = scanJson!!.replace(Regex("\"scanning\"\\s*:\\s*\\d"), "\"scanning\":0")
                if (screen == Screen.SCAN) renderScanScreen()
                return@postDelayed
            }
            scanJson = """{"aps":[],"scanning":0}"""
            if (screen == Screen.SCAN) renderScanScreen()
            setStatus(getString(R.string.wifi_scan_empty))
        }, 35_000L)
    }

    private fun startProfiles() {
        if (!requireBle()) return
        setStatus("Loading saved profiles…")
        imuService?.requestNetProfiles()
    }

    private fun sendNetCommand(json: String, userMsg: String) {
        if (!requireBle()) return
        setStatus(userMsg)
        imuService?.sendNetCommand(json)
    }

    private fun setStatus(msg: String) {
        statusLine.text = msg
    }

    private fun updateNetStatusLine(json: String) {
        val st = runCatching { NetProtocol.parseStatus(json) }.getOrNull() ?: return
        val line = buildString {
            append("ESP: ")
            append(st.state)
            if (st.ssid.isNotEmpty()) append(" → ").append(st.ssid)
            if (st.ip.isNotEmpty()) append(" @ ").append(st.ip)
            if (st.portal) append(" (setup AP active)")
            when (st.state) {
                "connected" -> if (st.ip.isEmpty()) append(" (joined)")
                "failed" -> append(" — check password / signal")
            }
        }
        setStatus(line)
    }

    private fun renderScanScreen() {
        val json = scanJson
        val progress = findViewById<ProgressBar>(R.id.wifiListProgress) ?: return
        val hint = findViewById<TextView>(R.id.wifiListHint) ?: return
        val list = findViewById<RecyclerView>(R.id.wifiList) ?: return
        list.layoutManager = LinearLayoutManager(this)

        if (json == null) {
            progress.visibility = View.VISIBLE
            hint.visibility = View.VISIBLE
            hint.text = getString(R.string.wifi_scanning)
            list.adapter = SimpleRowAdapter(emptyList()) {}
            return
        }

        val scanning = json.contains("\"scanning\":1")
        val aps = runCatching { NetProtocol.parseScan(json) }.getOrElse { emptyList() }
        progress.visibility = if (scanning) View.VISIBLE else View.GONE
        hint.visibility = if (aps.isEmpty()) View.VISIBLE else View.GONE
        hint.text = if (scanning) getString(R.string.wifi_scanning) else getString(R.string.wifi_scan_empty)

        list.adapter = SimpleRowAdapter(
            aps.map { ap ->
                RowItem(
                    ap.ssid,
                    buildString {
                        append(NetProtocol.securityLabel(ap.secured))
                        append(" · ")
                        append(ap.rssi)
                        append(" dBm")
                        if (ap.configured) append(" · saved")
                        if (ap.active) append(" · active")
                    },
                )
            },
        ) { idx ->
            openConnectFromAp(aps[idx])
        }
    }

    private fun renderProfilesScreen() {
        val json = profilesJson
        val hint = findViewById<TextView>(R.id.wifiListHint)
        val list = findViewById<RecyclerView>(R.id.wifiList)
        findViewById<ProgressBar>(R.id.wifiListProgress).visibility = View.GONE
        list.layoutManager = LinearLayoutManager(this)

        if (json == null) {
            hint.visibility = View.VISIBLE
            hint.text = "Waiting for ESP…"
            list.adapter = SimpleRowAdapter(emptyList()) {}
            return
        }

        val profiles = runCatching { NetProtocol.parseProfiles(json) }.getOrElse { emptyList() }
        hint.visibility = if (profiles.isEmpty()) View.VISIBLE else View.GONE
        hint.text = getString(R.string.wifi_profiles_empty)

        list.adapter = SimpleRowAdapter(
            profiles.map { p ->
                RowItem(
                    p.ssid,
                    buildString {
                        append("Profile #")
                        append(p.idx)
                        if (p.active) append(" · active now")
                        append(" · tap to connect")
                    },
                )
            },
        ) { idx ->
            openConnectFromProfile(profiles[idx])
        }
    }

    private fun openConnectFromAp(ap: NetProtocol.ApEntry) {
        connectSsid = ap.ssid
        connectSecured = ap.secured
        connectProfileIdx = if (ap.configured) ap.profileIdx else -1
        connectFromSaved = ap.configured
        showScreen(Screen.CONNECT)
    }

    private fun openConnectFromProfile(profile: NetProtocol.ProfileEntry) {
        connectSsid = profile.ssid
        connectSecured = true
        connectProfileIdx = profile.idx
        connectFromSaved = true
        showScreen(Screen.CONNECT)
    }

    private fun bindConnectScreen() {
        findViewById<TextView>(R.id.connectSsid).text = connectSsid
        findViewById<TextView>(R.id.connectSecurity).text = NetProtocol.securityLabel(connectSecured)
        val passLayout = findViewById<TextInputLayout>(R.id.connectPassLayout)
        val passEdit = findViewById<TextInputEditText>(R.id.connectPass)
        val hint = findViewById<TextView>(R.id.connectHint)
        val deleteBtn = findViewById<MaterialButton>(R.id.btnDeleteProfile)

        when {
            connectFromSaved && connectProfileIdx >= 0 -> {
                hint.text = getString(R.string.wifi_saved_password_hint)
                passLayout.hint = "New password (optional)"
            }
            connectSecured -> {
                hint.text = "WPA2 — enter the network password."
                passLayout.hint = getString(R.string.wifi_password_hint)
            }
            else -> {
                hint.text = getString(R.string.wifi_open_network)
                passLayout.visibility = View.GONE
            }
        }

        deleteBtn.visibility = if (connectProfileIdx >= 0) View.VISIBLE else View.GONE
        deleteBtn.setOnClickListener {
            sendNetCommand("""{"op":"delete","idx":$connectProfileIdx}""", "Deleting profile…")
            showScreen(Screen.PROFILES, requestProfiles = true)
        }

        findViewById<MaterialButton>(R.id.btnConnect).setOnClickListener {
            val pass = passEdit.text?.toString().orEmpty()
            if (connectFromSaved && connectProfileIdx >= 0 && pass.isEmpty()) {
                sendNetCommand(
                    """{"op":"activate","idx":$connectProfileIdx}""",
                    "Connecting to $connectSsid…",
                )
            } else {
                val escSsid = escapeJson(connectSsid)
                val escPass = escapeJson(pass)
                sendNetCommand(
                    """{"op":"connect","ssid":"$escSsid","pass":"$escPass"}""",
                    "Connecting to $connectSsid…",
                )
            }
        }
    }

    private fun renderProvStatus() {
        val tv = findViewById<TextView>(R.id.provStatus)
        val json = netStatusJson
        if (json.isNullOrBlank()) {
            tv.text = if (provErase) "Requested: erase profiles + open AP" else "Requested: open setup AP"
            return
        }
        val st = runCatching { NetProtocol.parseStatus(json) }.getOrNull()
        tv.text = if (st != null) {
            buildString {
                append("Status: ")
                append(st.state)
                if (st.portal) append("\nSetup hotspot should be visible as ESP32-IMU-Setup")
            }
        } else {
            json
        }
    }

    private fun escapeJson(s: String): String =
        s.replace("\\", "\\\\").replace("\"", "\\\"")

    private data class RowItem(val title: String, val subtitle: String)

    private class SimpleRowAdapter(
        private val items: List<RowItem>,
        private val onClick: (Int) -> Unit,
    ) : RecyclerView.Adapter<SimpleRowAdapter.Holder>() {

        class Holder(v: View) : RecyclerView.ViewHolder(v) {
            val title: TextView = v.findViewById(R.id.rowTitle)
            val subtitle: TextView = v.findViewById(R.id.rowSubtitle)
        }

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): Holder {
            val v = LayoutInflater.from(parent.context).inflate(R.layout.wifi_item_row, parent, false)
            return Holder(v)
        }

        override fun onBindViewHolder(holder: Holder, position: Int) {
            val item = items[position]
            holder.title.text = item.title
            holder.subtitle.text = item.subtitle
            holder.itemView.setOnClickListener { onClick(position) }
        }

        override fun getItemCount(): Int = items.size
    }

    companion object {
        const val EXTRA_START = "wifi_start"
        const val START_SCAN = "scan"
        const val START_PROFILES = "profiles"
        const val ACTION_WIFI_SCAN = "com.esp32s3.imusim.WIFI_SCAN"
    }

    private fun Intent.wantsWifiScan(): Boolean {
        return action == ACTION_WIFI_SCAN || getStringExtra(EXTRA_START) == START_SCAN
    }
}
