package com.esp32s3.imusim

import android.bluetooth.BluetoothGattCharacteristic
import android.os.Handler
import android.util.Log
import java.nio.charset.StandardCharsets
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Sequential BLE OTA writer. Writes go through [BleImuClient]'s GATT queue.
 * OTA CTRL/DATA UUIDs collide with the WiFi NET chars — callbacks must be
 * matched by parent service UUID, not characteristic UUID alone.
 *
 * Firmware pre-erases the inactive slot with BLE down (avoids flash erase + live
 * LE → TG0WDT). First begin expects a disconnect; resume begin then streams DATA.
 */
class OtaUploader(
    private val ctrl: BluetoothGattCharacteristic,
    private val data: BluetoothGattCharacteristic,
    private val write: (BluetoothGattCharacteristic, ByteArray, (Boolean) -> Unit) -> Unit,
    private val handler: Handler,
    private val onProgress: (Int) -> Unit,
    private val onDone: (Boolean, String) -> Unit,
    private val onEraseReconnect: () -> Unit,
) {
    private val running = AtomicBoolean(false)
    private var firmware: ByteArray = byteArrayOf()
    private var offset = 0
    private var lastLoggedPct = -1
    private var writeTimeout: Runnable? = null
    private var beginAccepted = false
    private var beginSent = false
    private var expectEraseDisconnect = true
    private var dataFallback: Runnable? = null

    fun start(bytes: ByteArray, resumeAfterErase: Boolean = false) {
        if (!running.compareAndSet(false, true)) {
            onDone(false, "OTA already running")
            return
        }
        firmware = bytes
        offset = 0
        lastLoggedPct = -1
        beginAccepted = false
        beginSent = false
        expectEraseDisconnect = !resumeAfterErase
        handler.post { beginUpload() }
    }

    fun cancel(reason: String = "cancelled") {
        if (uploadFinished()) {
            finish(true, "board rebooted after last OTA chunk")
            return
        }
        /* ESP drops BLE right after begin to erase slot1. GATT often reports the
         * begin write as failed in the same tear-down — that is success, not abort. */
        if (expectEraseDisconnect && offset == 0 && (beginSent || beginAccepted) &&
            looksLikeEraseDisconnect(reason)
        ) {
            enterEraseReconnect("cancel: $reason")
            return
        }
        finish(false, reason)
    }

    private fun beginUpload() {
        if (!running.get()) return
        val begin = """{"op":"begin","size":${firmware.size}}""".toByteArray(StandardCharsets.UTF_8)
        Log.i(TAG, "OTA begin size=${firmware.size} resume=${!expectEraseDisconnect}")
        beginSent = true
        queuedWrite(ctrl, begin) { ok ->
            if (!ok) {
                if (expectEraseDisconnect && offset == 0) {
                    enterEraseReconnect("begin write incomplete (ESP dropped for erase)")
                    return@queuedWrite
                }
                finish(false, "OTA begin write failed")
                return@queuedWrite
            }
            beginAccepted = true
            if (!expectEraseDisconnect) {
                handler.postDelayed({ sendNextChunk() }, 250)
                return@queuedWrite
            }
            /* First begin: ESP should drop the link to erase. If an older image
             * stays linked, fall back to DATA after a few seconds. */
            clearDataFallback()
            dataFallback = Runnable {
                dataFallback = null
                if (running.get() && offset == 0 && beginAccepted) {
                    Log.w(TAG, "OTA no BLE drop after begin — starting DATA (legacy FW?)")
                    expectEraseDisconnect = false
                    sendNextChunk()
                }
            }
            handler.postDelayed(dataFallback!!, 3_500)
        }
    }

    private fun enterEraseReconnect(why: String) {
        if (!running.getAndSet(false)) return
        clearWriteTimeout()
        clearDataFallback()
        Log.i(TAG, "OTA erase reconnect — $why")
        onEraseReconnect()
    }

    private fun looksLikeEraseDisconnect(reason: String): Boolean {
        val r = reason.lowercase()
        return r.contains("disconnect") ||
            r.contains("write failed") ||
            r.contains("timed out") ||
            r.contains("gatt")
    }

    private fun sendNextChunk() {
        if (!running.get()) return
        if (offset >= firmware.size) {
            handler.postDelayed({ sendReboot() }, 150)
            return
        }
        val end = minOf(offset + OtaProtocol.CHUNK_SIZE, firmware.size)
        val chunk = firmware.copyOfRange(offset, end)
        offset = end
        val pct = progressPct()
        if (pct != lastLoggedPct && (pct == 1 || pct % 10 == 0 || offset >= firmware.size)) {
            lastLoggedPct = pct
            Log.i(TAG, "OTA data $offset/${firmware.size} ($pct%)")
        }
        onProgress(pct)
        queuedWrite(data, chunk) { ok ->
            if (!ok) {
                if (uploadFinished()) {
                    finish(true, "board rebooted after last OTA chunk")
                } else {
                    finish(false, "OTA data write failed at $offset/${firmware.size}")
                }
                return@queuedWrite
            }
            expectEraseDisconnect = false
            handler.postDelayed({ sendNextChunk() }, OtaProtocol.CHUNK_WRITE_DELAY_MS)
        }
    }

    private fun sendReboot() {
        if (!running.get()) return
        Log.i(TAG, "OTA reboot after ${firmware.size} B")
        queuedWrite(ctrl, """{"op":"reboot"}""".toByteArray(StandardCharsets.UTF_8)) { ok ->
            if (ok) {
                finish(true, "OTA sent — board rebooting")
            } else {
                finish(true, "upload complete — reboot manually")
            }
        }
    }

    private fun queuedWrite(
        characteristic: BluetoothGattCharacteristic,
        payload: ByteArray,
        onSuccess: (Boolean) -> Unit,
    ) {
        armWriteTimeout()
        write(characteristic, payload) { ok ->
            clearWriteTimeout()
            onSuccess(ok)
        }
    }

    private fun uploadFinished(): Boolean {
        return firmware.isNotEmpty() && offset >= firmware.size
    }

    private fun progressPct(): Int {
        if (firmware.isEmpty()) return 0
        val pct = ((offset * 100L) / firmware.size).toInt().coerceIn(0, 99)
        return if (offset > 0 && pct == 0) 1 else pct
    }

    private fun armWriteTimeout() {
        clearWriteTimeout()
        writeTimeout = Runnable {
            writeTimeout = null
            finish(false, "OTA GATT write timed out")
        }
        handler.postDelayed(writeTimeout!!, WRITE_TIMEOUT_MS)
    }

    private fun clearWriteTimeout() {
        writeTimeout?.let { handler.removeCallbacks(it) }
        writeTimeout = null
    }

    private fun clearDataFallback() {
        dataFallback?.let { handler.removeCallbacks(it) }
        dataFallback = null
    }

    private fun finish(ok: Boolean, message: String) {
        if (!running.getAndSet(false)) return
        clearWriteTimeout()
        clearDataFallback()
        Log.i(TAG, "OTA done ok=$ok $message ($offset/${firmware.size})")
        /* Never abort while the board may still be mid-erase after a false fail. */
        if (!ok && !expectEraseDisconnect) {
            write(ctrl, """{"op":"abort"}""".toByteArray(StandardCharsets.UTF_8)) { }
        }
        onDone(ok, message)
    }

    companion object {
        private const val TAG = "OtaUploader"
        private const val WRITE_TIMEOUT_MS = 12_000L
    }
}
