package com.esp32s3.imusim;

import android.os.Bundle;

/** Service -> UI events. All methods are oneway — never blocks the service's main thread
 *  waiting on a (possibly slow/backgrounded) UI process. */
oneway interface IImuBleCallback {
    void onConnectionChanged(boolean connected);
    /** Relay FSM node: state id [RelayFsmState], caption, BLE link, Connect/Disconnect button. */
    void onRelayState(int state, String caption, boolean bleConnected, boolean showDisconnectButton);
    /** Async session restore — pushed right after registerCallback() and on requestState(). */
    void onSessionRestore(in Bundle snapshot);
    void onStatus(String text);
    void onPowerStatus(int source, float voltageV, int percent, boolean valid);
    void onBatchJson(String batchJson);
    void onConfigBlob(in byte[] blob);
    void onOtaProgress(int percent);
    void onOtaDone(boolean ok, String message);
    void onNetScan(String json);
    void onNetProfiles(String json);
    void onNetStatus(String json);
    void onVibroCaption(String caption);
    /** Compact JSON list of the 5 reference-profile slots — see vibro_ref_store.h. */
    void onVibroRefList(String json);
    /** 0=OK green, 1=WARN yellow, 2=ERROR red */
    void onBanner(int level, String message);
    void onEspScreenState(boolean on);
    /** Incremented on each stable BLE connect — UI clears stale pre-connect captions. */
    void onCaptionEpoch(int epoch);
    void onClockState(boolean synced, int tzMin);
    /** Battery bench wizard live update (active, sessionId, seq, V, %, elapsedMs, estMa). */
    void onBatteryBench(boolean active, long sessionId, long sampleSeq, float voltageV, int pct, long elapsedMs, float estMa);
    /** Flat-floor mounting calibration status JSON — see floor_calib.h. */
    void onFloorCalStatus(String json);
}
