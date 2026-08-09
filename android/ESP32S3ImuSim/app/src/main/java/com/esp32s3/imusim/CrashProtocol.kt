package com.esp32s3.imusim

import java.util.UUID

object CrashProtocol {
    val SERVICE_UUID: UUID = UUID.fromString("4a6e0301-0000-1000-8000-00805f9b34fb")
    val CHAR_INFO_UUID: UUID = UUID.fromString("4a6e0302-0000-1000-8000-00805f9b34fb")
    val CHAR_CTRL_UUID: UUID = UUID.fromString("4a6e0303-0000-1000-8000-00805f9b34fb")
    val CHAR_DATA_UUID: UUID = UUID.fromString("4a6e0304-0000-1000-8000-00805f9b34fb")

    const val CHUNK_SIZE = 480
}
