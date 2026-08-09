package com.esp32s3.imusim

import android.annotation.SuppressLint
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import android.os.Handler
import java.nio.charset.StandardCharsets
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Sequential BLE OTA writer — one GATT write at a time, waits for [onCharacteristicWrite].
 */
@SuppressLint("MissingPermission")
class OtaUploader(
    private val gatt: BluetoothGatt,
    private val handler: Handler,
    private val onProgress: (Int) -> Unit,
    private val onDone: (Boolean, String) -> Unit,
) {
    private val running = AtomicBoolean(false)
    private var pendingStep: (() -> Unit)? = null
    private var firmware: ByteArray = byteArrayOf()
    private var offset = 0

    private val ctrl: BluetoothGattCharacteristic?
        get() = gatt.getService(OtaProtocol.SERVICE_UUID)?.getCharacteristic(OtaProtocol.CHAR_CTRL_UUID)

    private val data: BluetoothGattCharacteristic?
        get() = gatt.getService(OtaProtocol.SERVICE_UUID)?.getCharacteristic(OtaProtocol.CHAR_DATA_UUID)

    fun start(bytes: ByteArray) {
        if (!running.compareAndSet(false, true)) {
            onDone(false, "OTA already running")
            return
        }
        firmware = bytes
        offset = 0
        handler.post { beginUpload() }
    }

    fun onCharacteristicWrite(success: Boolean) {
        if (!running.get()) return
        handler.post {
            if (!success) {
                finish(false, "GATT write failed during OTA")
                return@post
            }
            pendingStep?.invoke()
            pendingStep = null
        }
    }

    fun cancel() {
        finish(false, "cancelled")
    }

    private fun beginUpload() {
        val c = ctrl
        val d = data
        if (c == null || d == null) {
            finish(false, "OTA service missing — flash firmware with OTA enabled")
            return
        }
        writeCharacteristic(c, """{"op":"begin","size":${firmware.size}}""".toByteArray(StandardCharsets.UTF_8)) {
            sendNextChunk(d)
        }
    }

    private fun sendNextChunk(dataChar: BluetoothGattCharacteristic) {
        if (offset >= firmware.size) {
            handler.postDelayed({ sendReboot() }, 150)
            return
        }
        val end = minOf(offset + OtaProtocol.CHUNK_SIZE, firmware.size)
        val chunk = firmware.copyOfRange(offset, end)
        offset = end
        val pct = (offset * 100) / firmware.size
        onProgress(pct)
        writeCharacteristic(dataChar, chunk) {
            handler.postDelayed({ sendNextChunk(dataChar) }, OtaProtocol.CHUNK_WRITE_DELAY_MS)
        }
    }

    private fun sendReboot() {
        val c = ctrl ?: run {
            finish(true, "upload complete — reboot manually")
            return
        }
        writeCharacteristic(c, """{"op":"reboot"}""".toByteArray(StandardCharsets.UTF_8)) {
            finish(true, "OTA sent — board rebooting")
        }
    }

    private fun writeCharacteristic(
        characteristic: BluetoothGattCharacteristic,
        payload: ByteArray,
        onSuccess: () -> Unit,
    ) {
        pendingStep = onSuccess
        characteristic.value = payload
        if (gatt.writeCharacteristic(characteristic) != true) {
            pendingStep = null
            finish(false, "writeCharacteristic rejected")
        }
    }

    private fun finish(ok: Boolean, message: String) {
        if (!running.getAndSet(false)) return
        pendingStep = null
        onDone(ok, message)
    }
}
