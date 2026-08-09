package com.esp32s3.imusim

/** Background relay connection loop (service-owned). IDs are stable for AIDL. */
enum class RelayFsmState(val id: Int) {
    /** Service process up; foreground notification. */
    STARTING(0),
    /** Wait for ESP BLE stack / radio settle after boot. */
    BT_WARMUP(1),
    /** BLE scan + GATT connect attempt. */
    SCAN_CONNECT(2),
    /** Backoff after fail or successful relay — await manual Connect or auto retry. */
    PAUSE(3),
    /** Link up — IMU poll / crash drain / config. */
    CONNECTED(4),
    /** Upload phone queue to cloud. */
    CLOUD_SYNC(5),
    ;

    companion object {
        fun fromId(id: Int): RelayFsmState = entries.firstOrNull { it.id == id } ?: STARTING
    }
}
