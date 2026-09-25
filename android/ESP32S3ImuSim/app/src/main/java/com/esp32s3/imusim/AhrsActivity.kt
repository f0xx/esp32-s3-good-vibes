package com.esp32s3.imusim

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.Bundle
import android.os.Looper
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import com.google.android.material.appbar.MaterialToolbar
import org.json.JSONObject
import java.util.Locale
import java.util.concurrent.Executors
import kotlin.math.atan2
import kotlin.math.asin

/**
 * Live AHRS orientation readout — deliberately its own screen, not folded into the raw-data
 * panel, because entering it forces full CPU clock + max IMU sample rate (see onServiceReady /
 * onStop below) for the smoothest on-device attitude fusion. That is the opposite of every
 * other mode in this app, which defaults to power-saving Auto — see ahrs_help string and
 * MainActivity's showPerformanceDialog(). For the 3D cube view, see backend/web/ahrs.html
 * (relayed via ImuBleForegroundService.maybeRelayAhrs, independent of this screen).
 *
 * GPS is requested only while this screen is visible and released in onStop.
 */
class AhrsActivity : AppCompatActivity() {

    private lateinit var statusText: TextView
    private lateinit var readoutText: TextView
    private lateinit var gpsText: TextView
    private lateinit var serviceController: ImuServiceController
    private lateinit var cloudUploader: CloudUploader
    private val gpsIo = Executors.newSingleThreadExecutor()

    private var imuService: IImuBleService? = null
    private var connected = false
    private var boosted = false
    private var lastSampleMs = 0L
    private var lastGpsUploadMs = 0L
    private var lastGpsLine = ""
    private var lastCloudLine = ""
    private var gpsListening = false
    private var lastGpsAlt: Double? = null
    private var lastBleRx = 0L
    private var lastBleTx = 0L
    private var lastRssi = 0
    private var lastUiPaintMs = 0L
    private var geoDetail = ""

    private val gpsListener = object : LocationListener {
        override fun onLocationChanged(location: Location) = onGpsFix(location, fromCache = false)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_ahrs)
        statusText = findViewById(R.id.ahrsStatus)
        readoutText = findViewById(R.id.ahrsReadout)
        gpsText = findViewById(R.id.ahrsGps)
        cloudUploader = CloudUploader(this)
        findViewById<MaterialToolbar>(R.id.ahrsToolbar).setNavigationOnClickListener { finish() }

        serviceController = ImuServiceController(applicationContext, serviceEvents)
        requestLocationForMap()
        refreshUi()
        renderGpsPanel()
    }

    private fun requestLocationForMap() {
        if (hasLocationPermission()) return
        ActivityCompat.requestPermissions(
            this,
            arrayOf(
                Manifest.permission.ACCESS_FINE_LOCATION,
                Manifest.permission.ACCESS_COARSE_LOCATION,
            ),
            REQ_LOCATION,
        )
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != REQ_LOCATION) return
        lastGpsLine = ""
        startGpsIfAllowed()
        imuService?.startGeoTracking()
        renderGpsPanel()
    }

    override fun onStart() {
        super.onStart()
        serviceController.startAndBind()
        startGpsIfAllowed()
        imuService?.startGeoTracking()
    }

    override fun onResume() {
        super.onResume()
        /* After MainActivity.onStop() clears uiVisible — must be onResume, not onStart. */
        serviceController.setUiVisible(true)
        maybeBoostSpeed()
    }

    override fun onPause() {
        serviceController.setUiVisible(false)
        super.onPause()
    }

    override fun onStop() {
        stopGps()
        runCatching { imuService?.stopGeoTracking() }
        revertSpeedBoost()
        serviceController.unbind()
        super.onStop()
    }

    override fun onDestroy() {
        gpsIo.shutdownNow()
        super.onDestroy()
    }

    private fun hasLocationPermission(): Boolean {
        return ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION) ==
            PackageManager.PERMISSION_GRANTED ||
            ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_COARSE_LOCATION) ==
            PackageManager.PERMISSION_GRANTED
    }

    private fun startGpsIfAllowed() {
        if (gpsListening) return
        if (!hasLocationPermission()) {
            lastGpsLine = "GPS: permission denied — allow Location for this app"
            renderGpsPanel()
            return
        }
        val lm = getSystemService(LOCATION_SERVICE) as? LocationManager
        if (lm == null) {
            lastGpsLine = "GPS: location service missing"
            renderGpsPanel()
            return
        }
        val gpsOn = runCatching { lm.isProviderEnabled(LocationManager.GPS_PROVIDER) }.getOrDefault(false)
        val netOn = runCatching { lm.isProviderEnabled(LocationManager.NETWORK_PROVIDER) }.getOrDefault(false)
        if (!gpsOn && !netOn) {
            lastGpsLine = "GPS: location off — enable GPS or network location"
            renderGpsPanel()
        }
        seedLastKnown(lm)
        var registered = 0
        for (provider in listOf(LocationManager.GPS_PROVIDER, LocationManager.NETWORK_PROVIDER)) {
            val enabled = runCatching { lm.isProviderEnabled(provider) }.getOrDefault(false)
            if (!enabled) continue
            val ok = runCatching {
                lm.requestLocationUpdates(provider, 1000L, 0f, gpsListener, Looper.getMainLooper())
            }
            if (ok.isSuccess) registered++
        }
        gpsListening = registered > 0
        if (lastGpsLine.isBlank()) {
            lastGpsLine = if (gpsListening) {
                "GPS: searching… (gps=${onOff(gpsOn)} net=${onOff(netOn)})"
            } else {
                "GPS: no provider accepted updates (gps=${onOff(gpsOn)} net=${onOff(netOn)})"
            }
        }
        renderGpsPanel()
    }

    private fun stopGps() {
        if (!gpsListening) return
        val lm = getSystemService(LOCATION_SERVICE) as? LocationManager
        runCatching { lm?.removeUpdates(gpsListener) }
        gpsListening = false
    }

    private fun seedLastKnown(lm: LocationManager) {
        val providers = listOf(
            LocationManager.GPS_PROVIDER,
            LocationManager.NETWORK_PROVIDER,
            LocationManager.PASSIVE_PROVIDER,
        )
        var best: Location? = null
        for (provider in providers) {
            val last = runCatching { lm.getLastKnownLocation(provider) }.getOrNull() ?: continue
            if (best == null || last.accuracy < best.accuracy) best = last
        }
        best?.let { onGpsFix(it, fromCache = true) }
    }

    private fun onGpsFix(location: Location, fromCache: Boolean) {
        val acc = if (location.hasAccuracy()) String.format(Locale.US, "%.0f m", location.accuracy) else "?"
        val ageMs = (System.currentTimeMillis() - location.time).coerceAtLeast(0L)
        val age = when {
            ageMs < 2000L -> "now"
            ageMs < 60_000L -> "${ageMs / 1000}s ago"
            ageMs < 3_600_000L -> "${ageMs / 60_000}m ago"
            else -> "${ageMs / 3_600_000}h ago"
        }
        val src = location.provider ?: "?"
        val tag = if (fromCache) "cached" else src
        lastGpsLine = String.format(
            Locale.US,
            "GPS: %.5f, %.5f  ±%s  %s  via %s",
            location.latitude,
            location.longitude,
            acc,
            age,
            tag,
        )
        lastGpsAlt = if (location.hasAltitude()) location.altitude else lastGpsAlt
        renderGpsPanel()

        val now = System.currentTimeMillis()
        val first = lastGpsUploadMs == 0L
        if (!first && now - lastGpsUploadMs < GPS_UPLOAD_INTERVAL_MS) {
            imuService?.seedGeoAnchor(location.latitude, location.longitude)
            return
        }
        lastGpsUploadMs = now
        imuService?.seedGeoAnchor(location.latitude, location.longitude)
        gpsIo.execute {
            val result = runCatching {
                cloudUploader.uploadGeoPoint(
                    "gps",
                    location.latitude,
                    location.longitude,
                    now,
                    if (location.hasAccuracy()) location.accuracy.toDouble() else null,
                )
            }.getOrElse { CloudUploader.Result(false, it.message ?: "upload failed") }
            runOnUiThread {
                lastCloudLine = when {
                    result.ok -> "uploaded ok"
                    result.message.startsWith("queued") -> result.message
                    else -> "upload failed: ${result.message}"
                }
                renderGpsPanel()
            }
        }
    }

    private fun maybeBoostSpeed() {
        val svc = imuService ?: return
        if (!connected || boosted) return
        boosted = true
        svc.setCpuMhzOverride(240)
        svc.setImuHzOverride(100)
        svc.setPollIntervalMs(ImuProtocol.MIN_POLL_MS)
        statusText.text = getString(R.string.ahrs_status_boosting)
    }

    private fun revertSpeedBoost() {
        if (!boosted) return
        boosted = false
        runCatching {
            imuService?.setCpuMhzOverride(0)
            imuService?.setImuHzOverride(0)
            imuService?.setPollIntervalMs(ImuProtocol.DEFAULT_POLL_MS)
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

    private fun renderGpsPanel() {
        val settings = CloudSettings(this)
        val cloud = when {
            lastCloudLine.isNotBlank() -> lastCloudLine
            !settings.enabled -> "cloud off — set URL + API key"
            else -> "waiting to upload"
        }
        val gps = lastGpsLine.ifBlank { getString(R.string.ahrs_gps_idle) }
        val alt = lastGpsAlt?.let { String.format(Locale.US, "GPS alt: %.1f m (barometer not on ESP)", it) }
            ?: "GPS alt: n/a — AHRS has no baro; height is GPS-only"
        val ble = String.format(
            Locale.US,
            "BLE %s  rx %s  tx %s  rssi %s",
            if (connected) "up" else "down",
            fmtBytes(lastBleRx),
            fmtBytes(lastBleTx),
            if (lastRssi != 0 && lastRssi != ImuProtocol.RSSI_UNAVAIL) "${lastRssi} dBm" else "—",
        )
        val geo = geoDetail.ifBlank { "IMU geo: waiting for first GPS + walk_cm" }
        gpsText.text = "$gps\n$alt\n$geo\n$ble\nCloud device: ${settings.deviceId}\nMap: $cloud"
    }

    private fun applyBatchJson(json: String) {
        val now = System.currentTimeMillis()
        if (now - lastUiPaintMs < UI_PAINT_MIN_MS && lastSampleMs != 0L) {
            return
        }
        val root = runCatching { JSONObject(json) }.getOrNull() ?: return
        val rot = runCatching {
            val rot4 = root.optJSONArray("rot4") ?: return@runCatching null
            if (rot4.length() != 9) return@runCatching null
            DoubleArray(9) { i -> rot4.optInt(i, 0) / 10000.0 }
        }.getOrNull() ?: return

        lastUiPaintMs = now
        lastSampleMs = now
        val pitch = Math.toDegrees(asin((-rot[6]).coerceIn(-1.0, 1.0)))
        val roll = Math.toDegrees(atan2(rot[7], rot[8]))
        val yaw = Math.toDegrees(atan2(rot[3], rot[0]))
        val walkCm = root.optInt("wdcm", -1)
        val yawEsp = if (root.has("yawd100")) root.optInt("yawd100") / 100.0 else yaw
        readoutText.text = String.format(
            Locale.US,
            "Roll:  %6.1f°\nPitch: %6.1f°\nYaw:   %6.1f°  (ESP %5.1f°)",
            roll,
            pitch,
            yaw,
            yawEsp,
        )
        val origin = if (root.has("geo_olat")) {
            String.format(Locale.US, "origin %.5f, %.5f", root.optDouble("geo_olat"), root.optDouble("geo_olon"))
        } else {
            "origin —"
        }
        val imu = if (root.has("geo_ilat")) {
            String.format(Locale.US, "IMU %.5f, %.5f", root.optDouble("geo_ilat"), root.optDouble("geo_ilon"))
        } else {
            "IMU —"
        }
        val walk = if (walkCm >= 0) String.format(Locale.US, "walk %.2f m", walkCm / 100.0) else "walk —"
        val n = root.optInt("geo_imu_n", 0)
        geoDetail = "$origin\n$imu  $walk  imu pts $n"
        refreshUi()
        renderGpsPanel()
    }

    private val serviceEvents = object : ImuServiceController.Events {
        override fun onServiceReady(service: IImuBleService) {
            imuService = service
            runCatching { service.startGeoTracking() }
            runOnUiThread {
                refreshUi()
                maybeBoostSpeed()
                renderGpsPanel()
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

        override fun onCaps(caps: Int) {}

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

        override fun onBleStats(rxBytes: Long, txBytes: Long, rssiDbm: Int) {
            lastBleRx = rxBytes
            lastBleTx = txBytes
            lastRssi = rssiDbm
            runOnUiThread { renderGpsPanel() }
        }
    }

    companion object {
        private const val REQ_LOCATION = 31
        private const val GPS_UPLOAD_INTERVAL_MS = 5000L
        private const val UI_PAINT_MIN_MS = 80L

        fun open(context: Context) {
            context.startActivity(Intent(context, AhrsActivity::class.java))
        }

        private fun onOff(on: Boolean): String = if (on) "on" else "off"

        private fun fmtBytes(n: Long): String {
            return when {
                n >= 1_000_000 -> String.format(Locale.US, "%.1f MB", n / 1_000_000.0)
                n >= 1_000 -> String.format(Locale.US, "%.1f kB", n / 1_000.0)
                else -> "$n B"
            }
        }
    }
}
