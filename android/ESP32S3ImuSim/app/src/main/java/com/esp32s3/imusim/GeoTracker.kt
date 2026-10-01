package com.esp32s3.imusim

import org.json.JSONObject
import java.util.concurrent.Executor
import kotlin.math.cos
import kotlin.math.sin

/**
 * GPS-anchored IMU dead-reckoning. GPS is owned by [AhrsActivity] (AHRS screen only).
 * The first GPS fix of the session is the origin; later GPS does not re-anchor.
 * Distance comes from ESP `wdcm`; heading from `yawd100` (gyro yaw, not compass).
 */
class GeoTracker(
    private val cloudUploader: CloudUploader,
    private val ioExecutor: Executor,
) {
    companion object {
        private const val EARTH_RADIUS_M = 6371000.0
        /** ~10 km/h — used with sample dt so a slow BLE poll cannot drop a whole stride. */
        private const val MAX_SPEED_M_S = 2.8
        private const val MIN_STEP_M = 0.03
    }

    private var sessionActive = false
    private var originLat: Double? = null
    private var originLon: Double? = null
    private var imuLat: Double? = null
    private var imuLon: Double? = null
    private var lastWalkCm: Int? = null
    private var lastYawDeg: Double? = null
    private var lastSampleMs = 0L
    private var imuEmitted = 0

    fun start(force: Boolean = false): Boolean {
        if (sessionActive && !force) return true
        sessionActive = true
        return true
    }

    fun stop() {
        sessionActive = false
        lastWalkCm = null
        originLat = null
        originLon = null
        imuLat = null
        imuLon = null
        lastYawDeg = null
        lastSampleMs = 0L
        imuEmitted = 0
    }

    /** First GPS fix of this AHRS session becomes the IMU origin. */
    fun seedAnchor(lat: Double, lon: Double) {
        if (!sessionActive) return
        if (originLat != null) return
        originLat = lat
        originLon = lon
        imuLat = lat
        imuLon = lon
        emitImu(lat, lon, System.currentTimeMillis())
    }

    fun onImuSample(walkCm: Int, yawDeg: Double, unixMs: Long) {
        if (!sessionActive) return
        val lat0 = imuLat ?: return
        val lon0 = imuLon ?: return
        lastYawDeg = yawDeg
        val prevCm = lastWalkCm
        lastWalkCm = walkCm
        if (prevCm == null) {
            lastSampleMs = unixMs
            return
        }
        val dtSec = if (lastSampleMs == 0L) {
            1.0
        } else {
            ((unixMs - lastSampleMs) / 1000.0).coerceIn(0.2, 20.0)
        }
        lastSampleMs = unixMs

        var deltaM = (walkCm - prevCm) / 100.0
        if (deltaM < 0.0) {
            /* Counter reset (ESP reboot) — resync, do not teleport. */
            return
        }
        val maxM = (MAX_SPEED_M_S * dtSec).coerceAtLeast(MIN_STEP_M)
        if (deltaM > maxM) {
            deltaM = maxM
        }
        if (deltaM < MIN_STEP_M) return

        val headingRad = Math.toRadians(yawDeg)
        val dLat = deltaM * cos(headingRad) / EARTH_RADIUS_M
        val dLon = deltaM * sin(headingRad) / (EARTH_RADIUS_M * cos(Math.toRadians(lat0)))
        val newLat = (lat0 + Math.toDegrees(dLat)).coerceIn(-90.0, 90.0)
        val newLon = (lon0 + Math.toDegrees(dLon)).coerceIn(-180.0, 180.0)
        imuLat = newLat
        imuLon = newLon
        emitImu(newLat, newLon, unixMs)
    }

    fun appendSnapshot(root: JSONObject) {
        root.put("geo_on", sessionActive)
        originLat?.let { root.put("geo_olat", it) }
        originLon?.let { root.put("geo_olon", it) }
        imuLat?.let { root.put("geo_ilat", it) }
        imuLon?.let { root.put("geo_ilon", it) }
        lastWalkCm?.let { root.put("geo_wdcm", it) }
        lastYawDeg?.let { root.put("geo_yaw", it) }
        root.put("geo_imu_n", imuEmitted)
    }

    private fun emitImu(lat: Double, lon: Double, unixMs: Long) {
        imuEmitted++
        ioExecutor.execute {
            runCatching { cloudUploader.uploadGeoPoint("imu", lat, lon, unixMs, null) }
        }
    }
}
