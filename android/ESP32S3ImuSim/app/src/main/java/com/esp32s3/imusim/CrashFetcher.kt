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

    fun parseInfo(json: String): CrashInfo? {
        return try {
            if (isListJson(json)) {
                return null
            }
            val o = JSONObject(json)
            if (o.optInt("pending", 0) == 0 && !o.has("pc")) {
                return null
            }
            val bt = mutableListOf<Long>()
            o.optJSONArray("bt")?.let { arr ->
                for (i in 0 until arr.length()) {
                    bt.add(arr.optLong(i))
                }
            }
            CrashInfo(
                pending = o.optInt("pending", 0) != 0,
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
                detail = o.optJSONObject("detail"),
            )
        } catch (_: Exception) {
            null
        }
    }

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
            put("detail", info.detail ?: JSONObject().apply { put("dump_size", info.size) })
        }.toString()
    }
}
