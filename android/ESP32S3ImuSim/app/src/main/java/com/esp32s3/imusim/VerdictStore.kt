package com.esp32s3.imusim

import android.content.ContentValues
import android.content.Context
import android.database.sqlite.SQLiteDatabase
import android.database.sqlite.SQLiteOpenHelper

/** M-relay-C MVP: local verdict history + offload ACK tracking (F5-rotate). */
class VerdictStore(context: Context) {
    private val dbHelper = Db(context.applicationContext)

    fun record(status: ImuProtocol.Status): Long {
        return recordSeq(status, status.seq)
    }

    fun recordOfflineSession(status: ImuProtocol.Status, sessionSeq: Long): Long {
        return recordSeq(status, sessionSeq)
    }

    private fun recordSeq(status: ImuProtocol.Status, seq: Long): Long {
        val level = status.vibroVerdictLevel ?: ImuProtocol.VERDICT_OK
        val cv = ContentValues().apply {
            put("ts_ms", System.currentTimeMillis())
            put("seq", seq)
            put("level", level)
            put("rms", status.vibroRmsG ?: 0f)
            put("peak", status.vibroPeakG ?: 0f)
            put("corr", status.vibroCorr ?: status.bandCorr ?: 1f)
            put("rms_delta", status.vibroRmsDelta ?: status.bandDeltaMax ?: 0f)
            put("pct", status.percent)
            put("voltage", status.voltageV)
            status.powerProfile?.let { put("power_profile", it) }
            status.chipTempC?.let { put("chip_temp_c", it) }
            put("acked", 0)
        }
        val id = dbHelper.writableDatabase.insert("verdicts", null, cv)
        trimOldRows()
        return id
    }

    fun pendingAckSeqs(limit: Int = 32): List<Long> {
        val out = ArrayList<Long>()
        val c = dbHelper.readableDatabase.rawQuery(
            "SELECT seq FROM verdicts WHERE acked=0 ORDER BY id ASC LIMIT ?",
            arrayOf(limit.toString()),
        )
        c.use {
            while (it.moveToNext()) {
                out.add(it.getLong(0))
            }
        }
        return out
    }

    fun markAcked(seq: Long) {
        val cv = ContentValues().apply { put("acked", 1) }
        dbHelper.writableDatabase.update("verdicts", cv, "seq=?", arrayOf(seq.toString()))
    }

    fun recent(limit: Int = 20): List<Row> {
        val out = ArrayList<Row>()
        val c = dbHelper.readableDatabase.rawQuery(
            "SELECT ts_ms, seq, level, rms, peak, corr, rms_delta, acked FROM verdicts ORDER BY id DESC LIMIT ?",
            arrayOf(limit.toString()),
        )
        c.use {
            while (it.moveToNext()) {
                out.add(
                    Row(
                        tsMs = it.getLong(0),
                        seq = it.getLong(1),
                        level = it.getInt(2),
                        rmsG = it.getFloat(3),
                        peakG = it.getFloat(4),
                        corr = it.getFloat(5),
                        rmsDelta = it.getFloat(6),
                        acked = it.getInt(7) != 0,
                    ),
                )
            }
        }
        return out
    }

    fun count(): Int {
        val c = dbHelper.readableDatabase.rawQuery("SELECT COUNT(*) FROM verdicts", null)
        c.use {
            return if (it.moveToFirst()) it.getInt(0) else 0
        }
    }

    /** Oldest-first rows for cloud backfill when the JSONL queue was drained/lost. */
    fun forCloudExport(limit: Int = 200): List<Row> {
        val out = ArrayList<Row>()
        val c = dbHelper.readableDatabase.rawQuery(
            """
            SELECT ts_ms, seq, level, rms, peak, corr, rms_delta, acked,
                   pct, voltage, power_profile, chip_temp_c
            FROM verdicts ORDER BY id ASC LIMIT ?
            """.trimIndent(),
            arrayOf(limit.toString()),
        )
        c.use {
            while (it.moveToNext()) {
                out.add(
                    Row(
                        tsMs = it.getLong(0),
                        seq = it.getLong(1),
                        level = it.getInt(2),
                        rmsG = it.getFloat(3),
                        peakG = it.getFloat(4),
                        corr = it.getFloat(5),
                        rmsDelta = it.getFloat(6),
                        acked = it.getInt(7) != 0,
                        pct = if (it.isNull(8)) 0 else it.getInt(8),
                        voltageV = if (it.isNull(9)) 0f else it.getFloat(9),
                        powerProfile = if (it.isNull(10)) null else it.getInt(10),
                        chipTempC = if (it.isNull(11)) null else it.getFloat(11),
                    ),
                )
            }
        }
        return out
    }

    private fun trimOldRows() {
        dbHelper.writableDatabase.execSQL(
            "DELETE FROM verdicts WHERE id NOT IN (SELECT id FROM verdicts ORDER BY id DESC LIMIT $MAX_ROWS)",
        )
    }

    data class Row(
        val tsMs: Long,
        val seq: Long,
        val level: Int,
        val rmsG: Float,
        val peakG: Float,
        val corr: Float,
        val rmsDelta: Float,
        val acked: Boolean,
        val pct: Int = 0,
        val voltageV: Float = 0f,
        val powerProfile: Int? = null,
        val chipTempC: Float? = null,
    )

    private class Db(context: Context) : SQLiteOpenHelper(context, DB_NAME, null, 3) {
        override fun onCreate(db: SQLiteDatabase) {
            db.execSQL(
                """
                CREATE TABLE verdicts (
                  id INTEGER PRIMARY KEY AUTOINCREMENT,
                  ts_ms INTEGER NOT NULL,
                  seq INTEGER NOT NULL,
                  level INTEGER NOT NULL,
                  rms REAL,
                  peak REAL,
                  corr REAL,
                  rms_delta REAL,
                  pct INTEGER,
                  voltage REAL,
                  acked INTEGER NOT NULL DEFAULT 0
                )
                """.trimIndent(),
            )
        }

        override fun onUpgrade(db: SQLiteDatabase, oldVersion: Int, newVersion: Int) {
            if (oldVersion < 2) {
                db.execSQL("ALTER TABLE verdicts ADD COLUMN acked INTEGER NOT NULL DEFAULT 0")
            }
            if (oldVersion < 3) {
                db.execSQL("ALTER TABLE verdicts ADD COLUMN power_profile INTEGER")
                db.execSQL("ALTER TABLE verdicts ADD COLUMN chip_temp_c REAL")
            }
        }
    }

    companion object {
        private const val DB_NAME = "imu_verdicts.db"
        private const val MAX_ROWS = 5000
    }
}
