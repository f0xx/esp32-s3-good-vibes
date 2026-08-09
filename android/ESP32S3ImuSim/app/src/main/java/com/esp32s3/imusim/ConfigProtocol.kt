package com.esp32s3.imusim

import java.util.UUID

object ConfigProtocol {
    val SERVICE_UUID: UUID = UUID.fromString("4a6e0101-0000-1000-8000-00805f9b34fb")
    val CHAR_DATA_UUID: UUID = UUID.fromString("4a6e0102-0000-1000-8000-00805f9b34fb")
    val CHAR_CMD_UUID: UUID = UUID.fromString("4a6e0103-0000-1000-8000-00805f9b34fb")

    /** Must match packed `DeviceConfigV1` on the ESP32 sketch. */
    const val BLOB_SIZE = 188

    const val CMD_RELOAD = 0
    const val CMD_COMMIT = 1
    const val CMD_FACTORY = 2
    const val CMD_VIBRO_REF_START = 3
    const val CMD_VIBRO_REF_STOP = 4
    /** Payload: [5] + uint32 LE seq — phone confirms verdict/sample offload (F5-rotate). */
    const val CMD_OFFLOAD_ACK = 5
    /** Erase Zephyr settings storage partition + reboot (fixes corrupt NVS). */
    const val CMD_ERASE_NVS = 6
}

object OtaProtocol {
    val SERVICE_UUID: UUID = UUID.fromString("4a6e0201-0000-1000-8000-00805f9b34fb")
    val CHAR_CTRL_UUID: UUID = UUID.fromString("4a6e0202-0000-1000-8000-00805f9b34fb")
    val CHAR_DATA_UUID: UUID = UUID.fromString("4a6e0203-0000-1000-8000-00805f9b34fb")

    const val CHUNK_SIZE = 480
    /** Gap between DATA chunk writes — BLE stack / ESP need breathing room. */
    const val CHUNK_WRITE_DELAY_MS = 20L
}
