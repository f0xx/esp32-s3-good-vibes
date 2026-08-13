package com.esp32s3.imusim

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.Bundle
import android.os.IBinder
import android.util.Log

/** Binds to [ImuBleForegroundService] via AIDL; BLE session outlives the activity. */
class ImuServiceController(
    private val context: Context,
    private val events: Events,
) {
    interface Events {
        fun onServiceReady(service: IImuBleService)
        fun onServiceLost()
        fun onConnectionChanged(connected: Boolean)
        fun onRelayState(state: RelayFsmState, caption: String, bleConnected: Boolean, showDisconnect: Boolean) {}
        fun onSessionRestore(snapshot: Bundle) {}
        fun onStatus(text: String)
        fun onPowerStatus(power: ImuProtocol.PowerStatus)
        fun onBatchJson(batchJson: String)
        fun onConfigBlob(blob: ByteArray)
        fun onOtaProgress(percent: Int)
        fun onOtaDone(ok: Boolean, message: String)
        fun onNetScan(json: String) {}
        fun onNetProfiles(json: String) {}
        fun onNetStatus(json: String) {}
        fun onVibroCaption(caption: String) {}
        fun onVibroRefList(json: String) {}
        fun onBanner(level: StatusBannerLevel, message: String) {}
        fun onEspScreenState(on: Boolean) {}
        fun onCaptionEpoch(epoch: Int) {}
        fun onClockState(synced: Boolean, tzMin: Int) {}
    }

    private var service: IImuBleService? = null
    private var bound = false
    private var desiredUiVisible = false

    private val callback = object : IImuBleCallback.Stub() {
        override fun onConnectionChanged(connected: Boolean) {
            events.onConnectionChanged(connected)
        }

        override fun onRelayState(
            state: Int,
            caption: String?,
            bleConnected: Boolean,
            showDisconnectButton: Boolean,
        ) {
            events.onRelayState(
                RelayFsmState.fromId(state),
                caption ?: "",
                bleConnected,
                showDisconnectButton,
            )
        }

        override fun onSessionRestore(snapshot: Bundle?) {
            if (snapshot != null) {
                events.onSessionRestore(snapshot)
            }
        }

        override fun onStatus(text: String) {
            events.onStatus(text)
        }

        override fun onPowerStatus(source: Int, voltageV: Float, percent: Int, valid: Boolean) {
            events.onPowerStatus(ImuProtocol.PowerStatus(source, voltageV, percent, valid))
        }

        override fun onBatchJson(batchJson: String) {
            events.onBatchJson(batchJson)
        }

        override fun onConfigBlob(blob: ByteArray) {
            events.onConfigBlob(blob)
        }

        override fun onOtaProgress(percent: Int) {
            events.onOtaProgress(percent)
        }

        override fun onOtaDone(ok: Boolean, message: String) {
            events.onOtaDone(ok, message)
        }

        override fun onNetScan(json: String) {
            events.onNetScan(json)
        }

        override fun onNetProfiles(json: String) {
            events.onNetProfiles(json)
        }

        override fun onNetStatus(json: String) {
            events.onNetStatus(json)
        }

        override fun onVibroCaption(caption: String) {
            events.onVibroCaption(caption)
        }

        override fun onVibroRefList(json: String) {
            events.onVibroRefList(json)
        }

        override fun onBanner(level: Int, message: String) {
            val mapped = when (level) {
                1 -> StatusBannerLevel.WARN
                2 -> StatusBannerLevel.ERROR
                else -> StatusBannerLevel.OK
            }
            events.onBanner(mapped, message)
        }

        override fun onEspScreenState(on: Boolean) {
            events.onEspScreenState(on)
        }

        override fun onCaptionEpoch(epoch: Int) {
            events.onCaptionEpoch(epoch)
        }

        override fun onClockState(synced: Boolean, tzMin: Int) {
            events.onClockState(synced, tzMin)
        }
    }

    private val connection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
            service = IImuBleService.Stub.asInterface(binder)
            bound = true
            try {
                service?.registerCallback(callback)
                service?.setUiVisible(desiredUiVisible)
                service?.let { events.onServiceReady(it) }
                requestState()
            } catch (e: Exception) {
                Log.e(TAG, "registerCallback failed", e)
            }
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            service = null
            bound = false
            events.onServiceLost()
        }
    }

    fun startAndBind() {
        val intent = Intent(context, ImuBleForegroundService::class.java).apply {
            action = ImuBleForegroundService.ACTION_BLE_RELAY
        }
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.O) {
            context.startForegroundService(intent)
        } else {
            context.startService(intent)
        }
        context.bindService(intent, connection, Context.BIND_AUTO_CREATE)
    }

    fun unbind() {
        if (!bound) return
        try {
            service?.unregisterCallback(callback)
        } catch (_: Exception) {
        }
        context.unbindService(connection)
        bound = false
        service = null
    }

    /** Async only — service posts onRelayState + onSessionRestore to this callback. */
    fun requestState() {
        try {
            service?.requestState()
        } catch (e: Exception) {
            Log.w(TAG, "requestState failed", e)
        }
    }

    /** Call from onStart()/onStop(). Stored even if the service isn't bound yet — replayed as
     *  soon as onServiceConnected fires. */
    fun setUiVisible(active: Boolean) {
        desiredUiVisible = active
        try {
            service?.setUiVisible(active)
        } catch (e: Exception) {
            Log.w(TAG, "setUiVisible failed", e)
        }
    }

    fun requireService(): IImuBleService? = service

    companion object {
        private const val TAG = "ImuServiceController"
    }
}
