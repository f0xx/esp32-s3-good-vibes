package com.esp32s3.imusim

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.Bundle
import android.os.Looper
import androidx.core.content.ContextCompat
import java.util.concurrent.ExecutorService
import kotlin.math.cos
import kotlin.math.sin

/**
 * Phase 3 — GPS anchor + IMU dead-reckoning route tracker (preprod-demo quality; see
 * ROADMAP.md "Phase 6 — GPS + geo position").
 *
 * Establishes a moving "IMU anchor" from every sufficiently-accurate GPS fix (re-anchoring on
 * each new fix so IMU drift doesn't accumulate indefinitely between fixes), relays each stable
 * GPS fix to the backend as a `kind=gps` point, and separately dead-reckons a `kind=imu` trace
 * from the ESP32's yaw heading + cumulative walk distance (see ble_imu_gatt.c's "yawd100"/
 * "wdcm" DATA JSON fields, added in firmware v143). Heading is relative to the device's boot
 * orientation (gyro-integrated, no magnetometer — see attitude.c), not true compass north, so
 * the IMU trace's absolute direction is only meaningful relative to the last GPS re-anchor —
 * this is explicitly a demo-quality comparison, not a navigation-grade fusion.
 */
class GeoTracker(
    private val context: Context,
    private val cloudUploader: CloudUploader,
    private val ioExecutor: ExecutorService,
) {
    companion object {
        private const val STABLE_ACCURACY_M = 20f
        private const val MIN_GPS_UPLOAD_INTERVAL_MS = 5000L
        private const val EARTH_RADIUS_M = 6371000.0
        /** Guards against a walk-distance counter reset (reboot) or bogus single-sample jump
         *  being dead-reckoned into a wild teleport. */
        private const val MAX_PLAUSIBLE_STEP_M = 5.0
    }

    private var locationManager: LocationManager? = null
    private var listening = false

    private var anchorLat: Double? = null
    private var anchorLon: Double? = null
    private var lastWalkCm: Int? = null
    private var lastGpsUploadMs = 0L

    private val listener = object : LocationListener {
        override fun onLocationChanged(location: Location) = handleFix(location)

        @Deprecated("legacy LocationListener callback, unused on API 26+")
        override fun onStatusChanged(provider: String?, status: Int, extras: Bundle?) = Unit
        override fun onProviderEnabled(provider: String) = Unit
        override fun onProviderDisabled(provider: String) = Unit
    }

    fun start() {
        if (listening) return
        if (ContextCompat.checkSelfPermission(context, Manifest.permission.ACCESS_FINE_LOCATION) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            return
        }
        val lm = context.getSystemService(Context.LOCATION_SERVICE) as? LocationManager ?: return
        locationManager = lm
        for (provider in listOf(LocationManager.GPS_PROVIDER, LocationManager.NETWORK_PROVIDER)) {
            if (runCatching { lm.isProviderEnabled(provider) }.getOrDefault(false)) {
                runCatching {
                    lm.requestLocationUpdates(provider, 3000L, 3f, listener, Looper.getMainLooper())
                }
            }
        }
        listening = true
    }

    fun stop() {
        if (!listening) return
        runCatching { locationManager?.removeUpdates(listener) }
        listening = false
    }

    private fun handleFix(location: Location) {
        if (location.hasAccuracy() && location.accuracy > STABLE_ACCURACY_M) return
        anchorLat = location.latitude
        anchorLon = location.longitude

        val now = System.currentTimeMillis()
        if (now - lastGpsUploadMs < MIN_GPS_UPLOAD_INTERVAL_MS) return
        lastGpsUploadMs = now
        val accuracy = if (location.hasAccuracy()) location.accuracy.toDouble() else null
        ioExecutor.execute {
            runCatching {
                cloudUploader.uploadGeoPoint("gps", location.latitude, location.longitude, now, accuracy)
            }
        }
    }

    /** Feed the ESP32's cumulative walk distance (cm) and yaw heading (deg) from the latest BLE
     *  DATA JSON. No-op until a GPS fix has established an anchor. Best-effort: relay failures
     *  are swallowed (see CloudUploader.uploadGeoPoint doc). */
    fun onImuSample(walkCm: Int, yawDeg: Double, unixMs: Long) {
        val lat0 = anchorLat ?: return
        val lon0 = anchorLon ?: return
        val prevCm = lastWalkCm
        lastWalkCm = walkCm
        if (prevCm == null) return

        val deltaM = (walkCm - prevCm) / 100.0
        if (deltaM <= 0.0 || deltaM > MAX_PLAUSIBLE_STEP_M) return

        val headingRad = Math.toRadians(yawDeg)
        val dLat = deltaM * cos(headingRad) / EARTH_RADIUS_M
        val dLon = deltaM * sin(headingRad) / (EARTH_RADIUS_M * cos(Math.toRadians(lat0)))
        val newLat = lat0 + Math.toDegrees(dLat)
        val newLon = lon0 + Math.toDegrees(dLon)
        anchorLat = newLat
        anchorLon = newLon

        ioExecutor.execute {
            runCatching { cloudUploader.uploadGeoPoint("imu", newLat, newLon, unixMs, null) }
        }
    }
}
