package com.esp32s3.imusim

import java.util.UUID

object ConfigProtocol {
    val SERVICE_UUID: UUID = UUID.fromString("4a6e0101-0000-1000-8000-00805f9b34fb")
    val CHAR_DATA_UUID: UUID = UUID.fromString("4a6e0102-0000-1000-8000-00805f9b34fb")
    val CHAR_CMD_UUID: UUID = UUID.fromString("4a6e0103-0000-1000-8000-00805f9b34fb")
    /** Read-only: compact JSON list of the 5 reference-profile slots. See vibro_ref_store.h. */
    val CHAR_REFLIST_UUID: UUID = UUID.fromString("4a6e0104-0000-1000-8000-00805f9b34fb")
    /** Read-only: flat-floor mounting calibration status JSON. See floor_calib.h. */
    val CHAR_FLOORCAL_UUID: UUID = UUID.fromString("4a6e0105-0000-1000-8000-00805f9b34fb")

    /** Must match packed `DeviceConfigV1` on the ESP32 sketch. */
    const val BLOB_SIZE = 188

    /** profiles.txt #2: up to 5 reference-profile slots, 0..VIBRO_REF_STORE_SLOTS-1. */
    const val VIBRO_REF_SLOT_COUNT = 5
    /** Must stay under firmware's VIBRO_REF_NAME_MAX (24, incl. nul terminator). */
    const val VIBRO_REF_NAME_MAX_LEN = 20

    const val CMD_RELOAD = 0
    const val CMD_COMMIT = 1
    const val CMD_FACTORY = 2
    /** Payload: opcode + slot + name bytes; slot/name optional (bare opcode == slot 0, auto-named). */
    const val CMD_VIBRO_REF_START = 3
    const val CMD_VIBRO_REF_STOP = 4
    /** Payload: opcode + uint32 LE seq — phone confirms verdict/sample offload (F5-rotate). */
    const val CMD_OFFLOAD_ACK = 5
    /** Erase Zephyr settings storage partition + reboot (fixes corrupt NVS). */
    const val CMD_ERASE_NVS = 6
    /** Payload: opcode + slot — load a stored slot as the live/active reference. */
    const val CMD_VIBRO_REF_SELECT = 7
    /** Payload: opcode + slot — erase a slot (clears live reference if it was active). */
    const val CMD_VIBRO_REF_DELETE = 8
    /** Erase all reference slots on device (recalibration wizard). */
    const val CMD_VIBRO_REF_CLEAR_ALL = 9
    /** Arm monitoring / end ref wizard — acrylic LED off (operational). */
    const val CMD_VIBRO_ARM = 10
    /** Payload: opcode + uint16 LE duration_ms (optional, bare opcode == default 3000ms).
     * Device must be held still on a true-level reference for the whole window. */
    const val CMD_FLOOR_CALIB_START = 11
    /** Discard the stored flat-floor correction (back to identity). */
    const val CMD_FLOOR_CALIB_CLEAR = 12
}

object OtaProtocol {
    val SERVICE_UUID: UUID = UUID.fromString("4a6e0201-0000-1000-8000-00805f9b34fb")
    val CHAR_CTRL_UUID: UUID = UUID.fromString("4a6e0202-0000-1000-8000-00805f9b34fb")
    val CHAR_DATA_UUID: UUID = UUID.fromString("4a6e0203-0000-1000-8000-00805f9b34fb")

    const val CHUNK_SIZE = 480
    /** Gap between DATA chunk writes — BLE stack / ESP need breathing room. */
    const val CHUNK_WRITE_DELAY_MS = 20L
}
