package com.esp32s3.imusim

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.net.HttpURLConnection
import java.net.URL

/** Machines, repairs, and operator_status against Good Vibes. */
class CloudOperatorApi(private val context: Context) {
    private val settings = CloudSettings(context)

    data class HttpResult(
        val ok: Boolean,
        val message: String,
        val json: JSONObject? = null,
        val array: JSONArray? = null,
    )

    data class OperatorStatus(
        val operatorLevel: Int,
        val operatorAlert: Boolean,
        val candidateLevel: Int,
        val trend: String,
        val score: Double,
        val earlyWarning: Boolean,
        val machineKey: String?,
        val repairOpen: Boolean,
        val hintLabel: String?,
        val newRefRequired: Boolean,
        val raw: JSONObject,
    )

    data class MachineRow(
        val machineKey: String,
        val name: String,
        val kind: String,
        val sensorCount: Int,
    )

    data class FtaLeaf(
        val leafId: String,
        val label: String,
        val category: String,
        val speakable: Boolean,
        val newRefRequired: Boolean,
    )

    fun configured(): Boolean = settings.enabled && settings.baseUrl.isNotBlank() && settings.apiKey.isNotBlank()

    fun deviceId(): String = settings.deviceId

    fun listMachines(): HttpResult = get("/v1/machines", expectArray = true)

    fun createMachine(key: String, name: String, kind: String): HttpResult {
        val body = JSONObject().apply {
            put("machine_key", key)
            put("name", name)
            put("kind", kind)
        }
        return post("/v1/machines", body)
    }

    fun attachSensor(machineKey: String, deviceId: String, label: String?): HttpResult {
        val body = JSONObject().apply {
            put("device_id", deviceId)
            if (!label.isNullOrBlank()) put("label", label)
        }
        return post("/v1/machines/${enc(machineKey)}/sensors", body)
    }

    fun startRepair(machineKey: String, deviceId: String): HttpResult {
        val body = JSONObject().apply {
            put("action", "start")
            put("device_ids", JSONArray().put(deviceId))
        }
        return post("/v1/machines/${enc(machineKey)}/repairs", body)
    }

    fun endRepair(
        machineKey: String,
        deviceId: String,
        ftaLeaf: String,
        parts: String,
        notes: String,
        newRefRequired: Boolean,
    ): HttpResult {
        val body = JSONObject().apply {
            put("action", "end")
            put("device_ids", JSONArray().put(deviceId))
            put("fta_leaf", ftaLeaf)
            if (parts.isNotBlank()) put("parts", parts)
            if (notes.isNotBlank()) put("notes", notes)
            put("new_ref_required", newRefRequired)
        }
        return post("/v1/machines/${enc(machineKey)}/repairs", body)
    }

    fun operatorStatus(deviceId: String): OperatorStatus? {
        val r = get("/v1/devices/${enc(deviceId)}/operator_status")
        val o = r.json ?: return null
        val fta = o.optJSONObject("fta")
        val hints = fta?.optJSONArray("hints")
        val first = hints?.optJSONObject(0)
        return OperatorStatus(
            operatorLevel = o.optInt("operator_level"),
            operatorAlert = o.optBoolean("operator_alert"),
            candidateLevel = o.optInt("candidate_level"),
            trend = o.optString("trend", "insufficient"),
            score = o.optDouble("score", 0.0),
            earlyWarning = o.optBoolean("early_warning"),
            machineKey = o.optString("machine_key").takeIf { it.isNotBlank() },
            repairOpen = o.optBoolean("repair_open"),
            hintLabel = first?.optString("label")?.takeIf { it.isNotBlank() },
            newRefRequired = false,
            raw = o,
        )
    }

    fun ftaLeaves(kind: String): List<FtaLeaf> {
        val r = get("/v1/fta/templates/${enc(kind)}")
        val leaves = r.json?.optJSONArray("leaves") ?: return defaultLeaves()
        val out = mutableListOf<FtaLeaf>()
        for (i in 0 until leaves.length()) {
            val leaf = leaves.optJSONObject(i) ?: continue
            out.add(
                FtaLeaf(
                    leafId = leaf.optString("leaf_id"),
                    label = leaf.optString("label"),
                    category = leaf.optString("category"),
                    speakable = leaf.optBoolean("speakable"),
                    newRefRequired = leaf.optBoolean("new_ref_required", true),
                ),
            )
        }
        return out.ifEmpty { defaultLeaves() }
    }

    fun parseMachines(result: HttpResult): List<MachineRow> {
        val arr = result.array ?: return emptyList()
        val out = mutableListOf<MachineRow>()
        for (i in 0 until arr.length()) {
            val o = arr.optJSONObject(i) ?: continue
            out.add(
                MachineRow(
                    machineKey = o.optString("machine_key"),
                    name = o.optString("name"),
                    kind = o.optString("kind", "generic"),
                    sensorCount = o.optInt("sensor_count"),
                ),
            )
        }
        return out
    }

    fun defaultLeaves(): List<FtaLeaf> = listOf(
        FtaLeaf("bearing", "Bearing wear / defect", "mechanical", true, true),
        FtaLeaf("misalignment", "Shaft / coupling misalignment", "mechanical", true, true),
        FtaLeaf("cavitation", "Cavitation", "mechanical", true, false),
        FtaLeaf("impeller", "Impeller damage", "mechanical", true, true),
        FtaLeaf("seal", "Seal / packing", "mechanical", true, true),
        FtaLeaf("remount", "Sensor remount / loose fixture", "mechanical", true, true),
        FtaLeaf("electrical", "Electrical (motor / drive)", "electrical", false, false),
        FtaLeaf("process", "Process / flow / valve", "process", false, false),
        FtaLeaf("other", "Other / unknown", "other", false, true),
    )

    private fun enc(value: String): String = java.net.URLEncoder.encode(value, "UTF-8")

    private fun get(path: String, expectArray: Boolean = false): HttpResult {
        if (!configured()) return HttpResult(false, "cloud disabled")
        return request("GET", path, null, expectArray)
    }

    private fun post(path: String, body: JSONObject): HttpResult {
        if (!configured()) return HttpResult(false, "cloud disabled")
        return request("POST", path, body, expectArray = false)
    }

    private fun request(method: String, path: String, body: JSONObject?, expectArray: Boolean): HttpResult {
        val base = settings.baseUrl.trim().trimEnd('/')
        val url = URL("$base$path")
        val conn = url.openConnection() as HttpURLConnection
        return try {
            conn.requestMethod = method
            conn.connectTimeout = 8_000
            conn.readTimeout = 20_000
            conn.setRequestProperty("X-API-Key", settings.apiKey)
            if (body != null) {
                conn.doOutput = true
                conn.setRequestProperty("Content-Type", "application/json; charset=utf-8")
                OutputStreamWriter(conn.outputStream, Charsets.UTF_8).use { it.write(body.toString()) }
            }
            val code = conn.responseCode
            val stream = if (code in 200..299) conn.inputStream else conn.errorStream
            val text = stream?.let { BufferedReader(InputStreamReader(it, Charsets.UTF_8)).use { r -> r.readText() } }.orEmpty()
            if (code !in 200..299) {
                return HttpResult(false, "HTTP $code: $text")
            }
            if (expectArray) {
                HttpResult(true, "ok", array = JSONArray(text))
            } else {
                HttpResult(true, "ok", json = JSONObject(text))
            }
        } catch (e: Exception) {
            HttpResult(false, e.message ?: "request failed")
        } finally {
            conn.disconnect()
        }
    }
}
