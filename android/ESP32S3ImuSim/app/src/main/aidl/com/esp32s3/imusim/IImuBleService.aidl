package com.esp32s3.imusim;

import android.os.Bundle;
import com.esp32s3.imusim.IImuBleCallback;

interface IImuBleService {
    void registerCallback(IImuBleCallback callback);
    void unregisterCallback(IImuBleCallback callback);

    /** @deprecated Use requestState() — pushes onSessionRestore via callback. */
    Bundle getSnapshot();
    /** Async: onRelayState + onSessionRestore on registered callbacks (main thread). */
    void requestState();

    void connect();
    void disconnect();
    void setMode(int mode);
    void setPollIntervalMs(int ms);

    void requestConfigSync();
    void pushConfig(in byte[] blob, boolean commit);
    void uploadFirmware(in byte[] firmware);

    void requestNetScan();
    void requestNetProfiles();
    void sendNetCommand(String json);

    void vibroRefStart();
    void vibroRefStop();
    void analyzeSpectrum();
    void setEspScreenOn(boolean on);
    void toggleEspScreen();

    /** Debug firmware only (CONFIG_APP_CRASH_DEBUG) — triggers fault after ~250ms. */
    void injectCrash(String kind);
    void runDeviceBist();
    void eraseDeviceNvs();
}
