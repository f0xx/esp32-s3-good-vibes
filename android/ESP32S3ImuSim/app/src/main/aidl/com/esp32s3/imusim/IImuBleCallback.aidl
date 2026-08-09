package com.esp32s3.imusim;

interface IImuBleCallback {
    void onConnectionChanged(boolean connected);
    /** Relay FSM node: state id [RelayFsmState], caption, BLE link, Connect/Disconnect button. */
    void onRelayState(int state, String caption, boolean bleConnected, boolean showDisconnectButton);
    /** Async session restore (replaces sync getSnapshot for UI bind). */
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
    /** 0=OK green, 1=WARN yellow, 2=ERROR red */
    void onBanner(int level, String message);
    void onEspScreenState(boolean on);
}
