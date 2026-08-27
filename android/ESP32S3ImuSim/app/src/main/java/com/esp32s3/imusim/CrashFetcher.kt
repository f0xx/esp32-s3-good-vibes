package com.esp32s3.imusim

import org.json.JSONArray
import org.json.JSONObject

/** Parse compact crash INFO / LIST JSON from BLE crash GATT (steps 2–3b). */
object CrashFetcher {
    data class CrashInfo(
        val pending: Boolean,
        val slot: Int,
        val seq: Long,
        val size: Int,
        val pc: Long,
        val exccause: Int,
        val excvaddr: Long,
        val reason: String,
        val fw: String,
        val reset: Int,
        val uptimeMs: Long,
        val backtrace: List<Long>,
        val softReboot: Boolean = false,
        val softRebootReason: String = "",
        val detail: JSONObject?,
    )

    fun isListJson(json: String): Boolean =
        json.contains("\"slots\"")

    fun parseListSlots(json: String): List<Int> {
        return try {
            val o = JSONObject(json)
            val arr = o.optJSONArray("slots") ?: return emptyList()
            buildList {
                for (i in 0 until arr.length()) {
                    val row = arr.optJSONObject(i) ?: continue
                    add(row.optInt("slot", -1))
                }
            }.filter { it >= 0 }
        } catch (_: Exception) {
            emptyList()
        }
    }

    /**
     * Firmware embeds full per-slot detail (slot/seq/reason/pc/bt/…) directly in the "slots"
     * array — see crash_ring_list_json(). Parsing it here avoids a per-slot write+read
     * round trip for every pending crash, which used to be the main cause of long BLE
     * disconnect/reconnect cycles ("crash clear failed") whenever 2+ crashes were pending.
     * Firmware may omit a slot here if it didn't fit the 512B ATT cap — those are simply
     * missing from this round's result and get picked up on the relay's next round.
     */
    fun parseListDetailed(json: String): List<CrashInfo> {
        return try {
            val o = JSONObject(json)
            val arr = o.optJSONArray("slots") ?: return emptyList()
            buildList {
                for (i in 0 until arr.length()) {
                    val row = arr.optJSONObject(i) ?: continue
                    parseInfoObject(row)?.let { add(it) }
                }
            }
        } catch (_: Exception) {
            emptyList()
        }
    }

    fun parseInfo(json: String): CrashInfo? {
        return try {
            if (isListJson(json)) {
                return null
            }
            parseInfoObject(JSONObject(json))
        } catch (_: Exception) {
            null
        }
    }

    private fun parseInfoObject(o: JSONObject): CrashInfo? {
        val soft = o.optInt("soft", 0) != 0
        if (o.optInt("pending", 0) == 0 && !o.has("pc") && !soft) {
            return null
        }
        val bt = mutableListOf<Long>()
        o.optJSONArray("bt")?.let { arr ->
            for (i in 0 until arr.length()) {
                bt.add(arr.optLong(i))
            }
        }
        val srr = o.optString("srr", "")
        return CrashInfo(
            pending = true,
            slot = o.optInt("slot", -1),
            seq = o.optLong("seq"),
            size = o.optInt("size"),
            pc = o.optLong("pc"),
            exccause = o.optInt("exccause"),
            excvaddr = o.optLong("excvaddr"),
            reason = o.optString("reason", "unknown"),
            fw = o.optString("fw", ""),
            reset = o.optInt("reset"),
            uptimeMs = o.optLong("uptime"),
            backtrace = bt,
            softReboot = soft,
            softRebootReason = srr.ifBlank {
                if (infoReasonIsSoft(o.optString("reason", ""))) {
                    o.optString("reason", "").removePrefix("soft:")
                } else {
                    ""
                }
            },
            detail = o.optJSONObject("detail"),
        )
    }

    private fun infoReasonIsSoft(reason: String): Boolean = reason.startsWith("soft:")

    fun toOffloadJson(info: CrashInfo): String {
        val bt = JSONArray()
        info.backtrace.forEach { bt.put(it) }
        return JSONObject().apply {
            put("ts_ms", System.currentTimeMillis())
            put("seq", info.seq)
            put("slot", info.slot)
            put("reason", info.reason)
            put("pc", info.pc)
            put("exccause", info.exccause)
            put("excvaddr", info.excvaddr)
            put("thread_name", "fault")
            put("fw_version", info.fw)
            put("reset_reason", info.reset)
            put("uptime_ms", info.uptimeMs)
            put("backtrace", bt)
            if (info.softReboot) {
                put("soft_reboot_reason", info.softRebootReason.ifBlank { "unknown" })
                put("is_fatal", false)
            }
            val detailObj = info.detail ?: JSONObject().apply { put("dump_size", info.size) }
            if (info.softReboot && !detailObj.has("fatal")) {
                detailObj.put("fatal", 0)
            }
            put("detail", detailObj)
        }.toString()
    }
}
