#!/usr/bin/env python3
"""Flask ingest API for armv7 / Python 3.5 hosts (artc0 RPi). Same routes as app/main.py."""

from __future__ import print_function

import json
import os
import sys
import time

try:
    from flask import Flask, jsonify, request
except ImportError:
    print("Install: python3-flask python3-psycopg2", file=sys.stderr)
    raise

try:
    import psycopg2
    import psycopg2.extras
except ImportError:
    print("Install: python3-psycopg2", file=sys.stderr)
    raise

API_KEY = os.environ.get("IMU_API_KEY", "dev-change-me")
DATABASE_URL = os.environ.get(
    "DATABASE_URL",
    "dbname=imu_backend user=imu password=imu host=127.0.0.1",
)
BIND_HOST = os.environ.get("BIND_HOST", "0.0.0.0")
BIND_PORT = int(os.environ.get("BIND_PORT", "8080"))

app = Flask(__name__)


def db_conn():
    return psycopg2.connect(DATABASE_URL)


def init_schema():
    conn = db_conn()
    cur = conn.cursor()
    cur.execute(
        """
        CREATE TABLE IF NOT EXISTS devices (
          device_id VARCHAR(64) PRIMARY KEY,
          group_id VARCHAR(64),
          last_seen_ms BIGINT DEFAULT 0,
          last_phone_id VARCHAR(64)
        )
        """
    )
    cur.execute(
        """
        CREATE TABLE IF NOT EXISTS verdicts (
          id SERIAL PRIMARY KEY,
          device_id VARCHAR(64) NOT NULL,
          group_id VARCHAR(64),
          phone_id VARCHAR(64),
          ts_ms BIGINT NOT NULL,
          seq BIGINT NOT NULL,
          level INTEGER NOT NULL,
          rms REAL,
          peak REAL,
          corr REAL,
          rms_delta REAL,
          pct INTEGER,
          voltage REAL,
          power_profile INTEGER,
          chip_temp_c REAL,
          UNIQUE (device_id, seq)
        )
        """
    )
    cur.execute(
        "CREATE INDEX IF NOT EXISTS ix_verdicts_device_ts ON verdicts (device_id, ts_ms DESC)"
    )
    cur.execute(
        """
        CREATE TABLE IF NOT EXISTS crashes (
          id SERIAL PRIMARY KEY,
          device_id VARCHAR(64) NOT NULL,
          group_id VARCHAR(64),
          phone_id VARCHAR(64),
          ts_ms BIGINT NOT NULL,
          seq BIGINT NOT NULL,
          reason VARCHAR(128),
          pc BIGINT,
          exccause INTEGER,
          excvaddr BIGINT,
          thread_name VARCHAR(32),
          fw_version VARCHAR(64),
          reset_reason INTEGER,
          uptime_ms BIGINT,
          backtrace_json TEXT,
          detail_json TEXT,
          UNIQUE (device_id, seq)
        )
        """
    )
    cur.execute(
        "CREATE INDEX IF NOT EXISTS ix_crashes_device_ts ON crashes (device_id, ts_ms DESC)"
    )
    conn.commit()
    cur.close()
    conn.close()


def check_api_key():
    if not API_KEY:
        return True
    key = request.headers.get("X-API-Key")
    return key == API_KEY


@app.route("/health", methods=["GET"])
@app.route("/v1/health", methods=["GET"])
def health():
    return jsonify({"ok": True, "schema_version": "imu.ingest.v1", "db": "postgres"})


@app.route("/v1/ingest/verdicts", methods=["POST"])
def ingest_verdicts():
    if not check_api_key():
        return jsonify({"detail": "invalid api key"}), 401

    body = request.get_json(silent=True)
    if not body or body.get("schema") != "imu.ingest.v1":
        return jsonify({"detail": "unsupported schema"}), 400

    device_id = body.get("device_id")
    records = body.get("records") or []
    if not device_id or not records:
        return jsonify({"detail": "device_id and records required"}), 400

    group_id = body.get("group_id")
    phone_id = body.get("phone_id")
    sent_at_ms = body.get("sent_at_ms") or int(time.time() * 1000)

    conn = db_conn()
    cur = conn.cursor()
    cur.execute(
        """
        INSERT INTO devices (device_id, group_id, last_seen_ms, last_phone_id)
        VALUES (%s, %s, %s, %s)
        ON CONFLICT (device_id) DO UPDATE SET
          group_id = COALESCE(EXCLUDED.group_id, devices.group_id),
          last_seen_ms = EXCLUDED.last_seen_ms,
          last_phone_id = EXCLUDED.last_phone_id
        """,
        (device_id, group_id, sent_at_ms, phone_id),
    )

    accepted = 0
    duplicates = 0
    for rec in records:
        if rec.get("type") != "verdict":
            continue
        cur.execute(
            "SELECT 1 FROM verdicts WHERE device_id = %s AND seq = %s",
            (device_id, rec.get("seq")),
        )
        if cur.fetchone():
            duplicates += 1
            continue
        cur.execute(
            """
            INSERT INTO verdicts (
              device_id, group_id, phone_id, ts_ms, seq, level,
              rms, peak, corr, rms_delta, pct, voltage, power_profile, chip_temp_c
            ) VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)
            """,
            (
                device_id,
                group_id,
                phone_id,
                rec.get("ts_ms"),
                rec.get("seq"),
                rec.get("level"),
                rec.get("rms"),
                rec.get("peak"),
                rec.get("corr"),
                rec.get("rms_delta"),
                rec.get("pct"),
                rec.get("voltage"),
                rec.get("power_profile"),
                rec.get("chip_temp_c"),
            ),
        )
        accepted += 1

    conn.commit()
    cur.close()
    conn.close()
    return jsonify({"accepted": accepted, "duplicates": duplicates, "device_id": device_id})


@app.route("/v1/devices/<device_id>/verdicts", methods=["GET"])
def list_verdicts(device_id):
    if not check_api_key():
        return jsonify({"detail": "invalid api key"}), 401

    limit = min(int(request.args.get("limit", 50)), 500)
    conn = db_conn()
    cur = conn.cursor(cursor_factory=psycopg2.extras.RealDictCursor)
    cur.execute(
        """
        SELECT id, device_id, group_id, ts_ms, seq, level, rms, peak, corr,
               rms_delta, pct, voltage, power_profile, chip_temp_c
        FROM verdicts WHERE device_id = %s ORDER BY ts_ms DESC LIMIT %s
        """,
        (device_id, limit),
    )
    rows = cur.fetchall()
    cur.close()
    conn.close()
    return jsonify(rows)


if __name__ == "__main__":
    init_schema()
    app.run(host=BIND_HOST, port=BIND_PORT, threaded=True)
