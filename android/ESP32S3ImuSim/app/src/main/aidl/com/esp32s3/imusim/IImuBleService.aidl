package com.esp32s3.imusim;

import com.esp32s3.imusim.IImuBleCallback;

/** UI -> service commands. All methods are oneway (fire-and-forget); results/state arrive
 *  exclusively via IImuBleCallback on the caller's registered callback. Never blocks the
 *  calling thread on service-side work. */
oneway interface IImuBleService {
    void registerCallback(IImuBleCallback callback);
    void unregisterCallback(IImuBleCallback callback);

    /** Async: onRelayState + onSessionRestore pushed to the caller's callback (main thread). */
    void requestState();

    /** Call with true from onStart()/onServiceReady and false from onStop(). Lets the always-on
     *  relay FSM connect in full (notify-enabled) mode when the UI can actually render live data,
     *  and upgrades an already-connected background (minimal) session in place. */
    void setUiVisible(boolean active);

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

    /** `name` may be empty for an auto-generated "slot N" name. */
    void vibroRefStart(int slot, String name);
    void vibroRefStop();
    void vibroRefSelect(int slot);
    void vibroRefDelete(int slot);
    void vibroRefClearAll();
    /** End ref wizard — device acrylic LED off (operational). */
    void vibroArm();
    /** Async: result arrives via onVibroRefList. */
    void requestVibroRefList();
    void analyzeSpectrum();
    void setEspScreenOn(boolean on);
    void toggleEspScreen();

    /** 0 = auto (mode/battery-derived); nonzero = manual override, rounded firmware-side to
     *  the nearest supported tier (80/160/240). Persists on-device across reboots. */
    void setCpuMhzOverride(int mhz);
    /** 0 = auto; nonzero clamped firmware-side to [1,120] Hz. Persists across reboots. */
    void setImuHzOverride(int hz);

    /** Start/stop ESP battery-bench mode (config locked on device while active). */
    void startBatteryBench(String label);
    void stopBatteryBench();

    /** Debug firmware only (CONFIG_APP_CRASH_DEBUG) — triggers fault after ~250ms. */
    void injectCrash(String kind);
    void runDeviceBist();
    void eraseDeviceNvs();

    /** Bubble-level-style flat-floor mounting calibration. Device must sit still on a
     *  true-level reference for durationMs (0 = firmware default 3000ms). Result/progress
     *  arrives via onFloorCalStatus. */
    void floorCalibStart(int durationMs);
    /** Discard the stored flat-floor correction (back to identity). */
    void floorCalibClear();
    /** Async: result arrives via onFloorCalStatus. */
    void requestFloorCalStatus();
}
