package com.esp32s3.imusim

import org.json.JSONArray
import org.json.JSONObject
import java.util.UUID

object NetProtocol {
    val SERVICE_UUID: UUID = UUID.fromString("4a6e0200-0000-1000-8000-00805f9b34fb")
    val CHAR_SCAN_UUID: UUID = UUID.fromString("4a6e0201-0000-1000-8000-00805f9b34fb")
    val CHAR_PROFILES_UUID: UUID = UUID.fromString("4a6e0202-0000-1000-8000-00805f9b34fb")
    val CHAR_CMD_UUID: UUID = UUID.fromString("4a6e0203-0000-1000-8000-00805f9b34fb")
    val CHAR_STATUS_UUID: UUID = UUID.fromString("4a6e0204-0000-1000-8000-00805f9b34fb")

    data class ApEntry(
        val ssid: String,
        val rssi: Int,
        val secured: Boolean,
        val configured: Boolean,
        val profileIdx: Int,
        val active: Boolean,
    )

    data class ProfileEntry(
        val idx: Int,
        val ssid: String,
        val lastOkMs: Long,
        val active: Boolean,
    )

    data class NetStatus(
        val state: String,
        val ssid: String,
        val idx: Int,
        val rssi: Int,
        val ip: String,
        val portal: Boolean,
        val profiles: Int,
    )

    fun parseScan(json: String): List<ApEntry> {
        val root = JSONObject(json)
        val aps = root.optJSONArray("aps") ?: JSONArray()
        return buildList {
            for (i in 0 until aps.length()) {
                val o = aps.getJSONObject(i)
                add(
                    ApEntry(
                        ssid = o.getString("ssid"),
                        rssi = o.optInt("rssi"),
                        secured = o.optInt("sec", 1) != 0,
                        configured = o.optInt("cfg", 0) != 0,
                        profileIdx = o.optInt("idx", -1),
                        active = o.optInt("active", 0) != 0,
                    ),
                )
            }
        }
    }

    fun parseProfiles(json: String): List<ProfileEntry> {
        val root = JSONObject(json)
        val arr = root.optJSONArray("profiles") ?: JSONArray()
        return buildList {
            for (i in 0 until arr.length()) {
                val o = arr.getJSONObject(i)
                add(
                    ProfileEntry(
                        idx = o.getInt("idx"),
                        ssid = o.getString("ssid"),
                        lastOkMs = o.optLong("last_ok"),
                        active = o.optInt("active", 0) != 0,
                    ),
                )
            }
        }
    }

    fun parseStatus(json: String): NetStatus {
        val o = JSONObject(json)
        return NetStatus(
            state = o.optString("st", "idle"),
            ssid = o.optString("ssid", ""),
            idx = o.optInt("idx", -1),
            rssi = o.optInt("rssi"),
            ip = o.optString("ip", ""),
            portal = o.optInt("portal", 0) != 0,
            profiles = o.optInt("profiles", 0),
        )
    }

    fun securityLabel(secured: Boolean): String =
        if (secured) "WPA2 (password required)" else "Open (no password)"
}
