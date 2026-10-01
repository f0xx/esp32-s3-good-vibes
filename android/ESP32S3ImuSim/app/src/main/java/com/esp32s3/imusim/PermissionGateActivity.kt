package com.esp32s3.imusim

import android.Manifest
import android.app.Activity
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.PowerManager
import android.provider.Settings
import android.util.Log
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat

/**
 * Runtime grants the always-on service cannot show itself: BLE, location, notifications,
 * battery-optimization exemption, and a Bluetooth-on prompt.
 */
class PermissionGateActivity : Activity() {
    private var askedRuntime = false
    private var askedBattery = false
    private var askedExact = false
    private var askedBluetooth = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        proceed()
    }

    override fun onResume() {
        super.onResume()
        if (askedRuntime || askedBattery || askedExact || askedBluetooth) {
            proceed()
        }
    }

    private fun proceed() {
        val missing = missingRuntime(this)
        if (missing.isNotEmpty() && !askedRuntime) {
            askedRuntime = true
            ActivityCompat.requestPermissions(this, missing.toTypedArray(), REQ_RUNTIME)
            return
        }
        if (needsExactAlarm(this) && !askedExact) {
            askedExact = true
            try {
                startActivity(Intent(Settings.ACTION_REQUEST_SCHEDULE_EXACT_ALARM).apply {
                    data = Uri.parse("package:$packageName")
                })
            } catch (e: Exception) {
                Log.w(TAG, "exact alarm: ${e.message}")
            }
            return
        }
        if (needsBatteryExemption(this) && !askedBattery) {
            askedBattery = true
            try {
                startActivity(
                    Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS).apply {
                        data = Uri.parse("package:$packageName")
                    },
                )
            } catch (e: Exception) {
                Log.w(TAG, "battery exemption: ${e.message}")
            }
            return
        }
        if (bluetoothOff(this) && !askedBluetooth && AutopilotRelay.BlePermissionGate.canUseBle(this)) {
            askedBluetooth = true
            try {
                startActivity(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE))
            } catch (e: Exception) {
                Log.w(TAG, "bluetooth enable: ${e.message}")
            }
            return
        }
        if (AutopilotRelay.BlePermissionGate.canUseBle(this)) {
            AutopilotRelay.startBleRelayService(this)
        }
        finish()
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        proceed()
    }

    companion object {
        private const val TAG = "PermissionGate"
        private const val REQ_RUNTIME = 41
        private const val PREFS = "permission_gate"
        private const val KEY_ASKED_AT = "asked_at"
        private const val DEBOUNCE_MS = 6L * 60L * 60L * 1000L

        fun missingRuntime(context: Context): List<String> {
            val needed = mutableListOf<String>()
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                if (!granted(context, Manifest.permission.BLUETOOTH_SCAN)) {
                    needed.add(Manifest.permission.BLUETOOTH_SCAN)
                }
                if (!granted(context, Manifest.permission.BLUETOOTH_CONNECT)) {
                    needed.add(Manifest.permission.BLUETOOTH_CONNECT)
                }
            }
            if (!granted(context, Manifest.permission.ACCESS_FINE_LOCATION)) {
                needed.add(Manifest.permission.ACCESS_FINE_LOCATION)
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
                !granted(context, Manifest.permission.POST_NOTIFICATIONS)
            ) {
                needed.add(Manifest.permission.POST_NOTIFICATIONS)
            }
            return needed
        }

        fun needsAttention(context: Context): Boolean {
            return missingRuntime(context).isNotEmpty() ||
                needsBatteryExemption(context) ||
                needsExactAlarm(context) ||
                bluetoothOff(context)
        }

        fun launch(context: Context) {
            if (!needsAttention(context)) return
            val prefs = context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            val now = System.currentTimeMillis()
            val last = prefs.getLong(KEY_ASKED_AT, 0L)
            if (last != 0L && now - last < DEBOUNCE_MS) {
                return
            }
            prefs.edit().putLong(KEY_ASKED_AT, now).commit()
            val intent = Intent(context, PermissionGateActivity::class.java).apply {
                addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP)
            }
            try {
                context.startActivity(intent)
            } catch (e: Exception) {
                Log.w(TAG, "launch failed: ${e.message}")
            }
        }

        private fun granted(context: Context, permission: String): Boolean {
            return ContextCompat.checkSelfPermission(context, permission) == PackageManager.PERMISSION_GRANTED
        }

        private fun needsExactAlarm(context: Context): Boolean {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return false
            val alarm = context.getSystemService(android.app.AlarmManager::class.java) ?: return false
            return !alarm.canScheduleExactAlarms()
        }

        private fun needsBatteryExemption(context: Context): Boolean {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) return false
            val power = context.getSystemService(PowerManager::class.java) ?: return false
            return !power.isIgnoringBatteryOptimizations(context.packageName)
        }

        private fun bluetoothOff(context: Context): Boolean {
            val mgr = context.getSystemService(BluetoothManager::class.java) ?: return false
            val adapter = mgr.adapter ?: return true
            return !adapter.isEnabled
        }
    }
}
