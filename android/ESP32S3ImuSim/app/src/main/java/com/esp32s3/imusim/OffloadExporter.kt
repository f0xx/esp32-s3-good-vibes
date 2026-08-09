package com.esp32s3.imusim

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream

/** M-relay-C: append verdict rows to JSONL for deferred cloud upload. */
class OffloadExporter(private val context: Context) {
    private val dir = File(context.applicationContext.filesDir, "offload").also { it.mkdirs() }
    private val verdictFile = File(dir, "verdicts.jsonl")
    private val spectrumFile = File(dir, "spectra.jsonl")
    private val crashFile = File(dir, "crashes.jsonl")

    fun exportVerdict(status: ImuProtocol.Status) {
        val level = status.vibroVerdictLevel
            ?: if (status.vibroRmsG != null) ImuProtocol.VERDICT_OK else return
        val line = JSONObject().apply {
            put("ts_ms", System.currentTimeMillis())
            put("seq", status.seq)
            put("level", level)
            put("rms", status.vibroRmsG ?: 0.0)
            put("peak", status.vibroPeakG ?: 0.0)
            put("corr", status.vibroCorr ?: status.bandCorr ?: 1.0)
            put("rms_delta", status.vibroRmsDelta ?: status.bandDeltaMax ?: 0.0)
            put("pct", status.percent)
            put("voltage", status.voltageV)
            status.powerProfile?.let { put("power_profile", it) }
            status.chipTempC?.let { put("chip_temp_c", it) }
            putEdgeFeatures(status)
        }.toString() + "\n"
        FileOutputStream(verdictFile, true).use { it.write(line.toByteArray()) }
    }

    private fun JSONObject.putEdgeFeatures(status: ImuProtocol.Status) {
        status.bandCorr?.let { put("band_corr", it.toDouble()) }
        status.bandDeltaMax?.let { put("band_delta_max", it.toDouble()) }
        status.edgeCrest?.let { put("edge_crest", it.toDouble()) }
        status.edgeZcrHz?.let { put("edge_zcr_hz", it.toDouble()) }
        status.edgeHfRatio?.let { put("edge_hf_ratio", it.toDouble()) }
        status.bandRms?.let { bands ->
            if (bands.isNotEmpty()) {
                val arr = JSONArray()
                bands.forEach { arr.put(it.toDouble()) }
                put("bands", arr)
            }
        }
        status.pendingSessionSeq?.takeIf { it > 0L }?.let { put("session_seq", it) }
        status.captureMixWindowSec?.takeIf { it > 0 }?.let { put("cap_mix_sec", it) }
    }

    fun exportSpectrum(
        seq: Long,
        sampleHz: Float,
        binHz: Float,
        bins: List<Float>,
        peakHz: Float,
        peakMag: Float,
    ) {
        val arr = org.json.JSONArray()
        bins.forEach { arr.put(it.toDouble()) }
        val line = JSONObject().apply {
            put("ts_ms", System.currentTimeMillis())
            put("seq", seq)
            put("sample_hz", sampleHz.toDouble())
            put("bin_hz", binHz.toDouble())
            put("bins", arr)
            put("peak_hz", peakHz.toDouble())
            put("peak_mag", peakMag.toDouble())
            put("axis", "mag")
        }.toString() + "\n"
        FileOutputStream(spectrumFile, true).use { it.write(line.toByteArray()) }
    }

    fun exportCrashJson(line: String) {
        val payload = line.trim()
        if (payload.isEmpty()) return
        FileOutputStream(crashFile, true).use {
            it.write((payload + "\n").toByteArray())
        }
    }

    @Synchronized
    fun drainPendingCrashes(maxLines: Int): List<String> {
        if (!crashFile.exists() || maxLines <= 0) return emptyList()
        val all = crashFile.readLines().filter { it.isNotBlank() }
        if (all.isEmpty()) return emptyList()
        val take = all.take(maxLines)
        rewriteCrashes(all.drop(take.size))
        return take
    }

    @Synchronized
    fun restoreCrashes(lines: List<String>) {
        if (lines.isEmpty()) return
        val existing = if (crashFile.exists()) {
            crashFile.readLines().filter { it.isNotBlank() }
        } else {
            emptyList()
        }
        rewriteCrashes(lines + existing)
    }

    @Synchronized
    fun drainPendingLines(maxLines: Int): List<String> {
        if (!verdictFile.exists() || maxLines <= 0) return emptyList()
        val all = verdictFile.readLines().filter { it.isNotBlank() }
        if (all.isEmpty()) return emptyList()
        val take = all.take(maxLines)
        rewriteVerdicts(all.drop(take.size))
        return take
    }

    @Synchronized
    fun drainPendingSpectra(maxLines: Int): List<String> {
        if (!spectrumFile.exists() || maxLines <= 0) return emptyList()
        val all = spectrumFile.readLines().filter { it.isNotBlank() }
        if (all.isEmpty()) return emptyList()
        val take = all.take(maxLines)
        rewriteSpectra(all.drop(take.size))
        return take
    }

    @Synchronized
    fun restoreLines(lines: List<String>) {
        if (lines.isEmpty()) return
        val existing = if (verdictFile.exists()) {
            verdictFile.readLines().filter { it.isNotBlank() }
        } else {
            emptyList()
        }
        rewriteVerdicts(lines + existing)
    }

    @Synchronized
    fun restoreSpectra(lines: List<String>) {
        if (lines.isEmpty()) return
        val existing = if (spectrumFile.exists()) {
            spectrumFile.readLines().filter { it.isNotBlank() }
        } else {
            emptyList()
        }
        rewriteSpectra(lines + existing)
    }

    private fun rewriteVerdicts(lines: List<String>) {
        if (lines.isEmpty()) {
            verdictFile.delete()
            return
        }
        verdictFile.writeText(lines.joinToString("\n", postfix = "\n"))
    }

    private fun rewriteSpectra(lines: List<String>) {
        if (lines.isEmpty()) {
            spectrumFile.delete()
            return
        }
        spectrumFile.writeText(lines.joinToString("\n", postfix = "\n"))
    }

    private fun rewriteCrashes(lines: List<String>) {
        if (lines.isEmpty()) {
            crashFile.delete()
            return
        }
        crashFile.writeText(lines.joinToString("\n", postfix = "\n"))
    }

    fun lineCount(): Int {
        if (!verdictFile.exists()) return 0
        return verdictFile.readLines().count { it.isNotBlank() }
    }

    fun pendingCrashCount(): Int {
        if (!crashFile.exists()) return 0
        return crashFile.readLines().count { it.isNotBlank() }
    }
}
