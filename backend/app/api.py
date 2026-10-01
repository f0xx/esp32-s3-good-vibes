"""REST API routes — mounted at /v1 and /app/good_vibes/v1."""

import json
import os
import time

from fastapi import APIRouter, Depends, File, HTTPException, Query, UploadFile
from fastapi.responses import Response
from sqlalchemy import and_, func, or_, select, text
from sqlalchemy.orm import Session
from pydantic import BaseModel, Field

from app.auth import require_api_key
from app.db import Base, engine, get_db
from app.models import (
    BatteryBenchSample,
    BatteryBenchSession,
    ClockEvent,
    CloudIngestBatch,
    Crash,
    Device,
    DeviceConfigRevision,
    DeviceTelemetry,
    Machine,
    PresenceGap,
    ReferenceProfile,
    RepairEvent,
    Sensor,
    Spectrum,
    Verdict,
    WearableSample,
)
from app.battery_bench import estimate_discharge_ma
from app.export import collect_delivery_rows, render_table
from app.edge_score import score_edge_features
from app.fta_map import default_new_ref_required, mechanic_leaves
from app.operator_status import (
    compute_device_operator_status,
    compute_machine_operator_status,
    filter_outside_repairs,
    load_repair_intervals,
    refresh_after_ingest,
)
from app.trend_score import score_trend
from app.symbolicate import (
    enrich_crash_detail,
    firmware_elf_url,
    normalize_symvers,
    resolve_elf_for_version,
    store_firmware_elf,
)
from app.device_config_codec import blob_to_json, cloud_revision, json_to_blob
from app.schemas import (
    AhrsLatestOut,
    AhrsSampleIn,
    AiSuggestOut,
    BatteryBenchIngestEnvelope,
    BatteryBenchSampleOut,
    BatteryBenchSessionOut,
    ClockIngestEnvelope,
    GeoPointIn,
    GeoPointOut,
    GeoRouteOut,
    CrashIngestEnvelope,
    CrashOut,
    OtaEventOut,
    ConfigIngestEnvelope,
    DeviceConfigOut,
    DeviceConfigPut,
    DeviceInsightsOut,
    DeviceOut,
    GroupDeviceVerdict,
    GroupSilentDevice,
    GroupStatusOut,
    PresenceGapOut,
    HealthOut,
    IngestBatchEnvelope,
    IngestEnvelope,
    IngestResult,
    MachineCreate,
    MachineOut,
    OperatorStatusOut,
    ReferenceProfileOut,
    RepairAction,
    RepairOut,
    ReferenceProfilePut,
    ReferenceProfileIngestEnvelope,
    SensorCreate,
    SensorOut,
    SpectrumIngestEnvelope,
    SpectrumOut,
    TelemetryIngestEnvelope,
    TrendOut,
    TrendPoint,
    VerdictOut,
    WearableDailyOut,
    WearableDailyPoint,
    WearableIngestEnvelope,
    WearableKindLatest,
    WearableLatestOut,
    WearableSampleOut,
)

router = APIRouter()

# Board-dead / battery-out vs normal BLE idle: 1 hour of no ESP telemetry/verdicts.
SILENCE_ALERT_MS = 60 * 60 * 1000
PRESENCE_DEDUP_MS = 5 * 60 * 1000

# Live AHRS relay — process-local, in-memory, latest-sample-only. Not a durable store (see
# AhrsSampleIn docstring): this deliberately does NOT survive a backend restart or scale past
# one process, matching the "lightweight debug relay" scope of this feature. If it ever needs
# to be durable/multi-instance, move it to Redis (already how a proper pub/sub would look) —
# don't just start writing every sample to the SQL DB, that's exactly the flood this avoids.
_ahrs_latest: dict[str, AhrsLatestOut] = {}
_wearable_latest: dict[str, WearableLatestOut] = {}

# Phase 3 — GPS + IMU dead-reckoning route buffers. Same in-memory, per-process, bounded-ring-
# buffer approach as _ahrs_latest, and for the same reason (see GeoPointIn docstring): this is
# preprod-demo route-comparison debug data, not a durable trip log.
_GEO_MAX_POINTS = 4000
_geo_routes: dict[str, dict[str, list[GeoPointOut]]] = {}


def _crash_out(row: Crash, *, persist: list | None = None) -> CrashOut:
    detail = None
    if row.detail_json:
        try:
            detail = json.loads(row.detail_json)
        except (json.JSONDecodeError, TypeError):
            detail = None
    before = json.dumps(detail, sort_keys=True) if detail else ""
    # Prefer Postgres-cached detail_json (sym_complete). enrich only fills gaps /
    # force-upgrades when a new ELF invalidates the stamp.
    detail = enrich_crash_detail(
        detail,
        row.backtrace,
        pc=row.pc,
        reason=row.reason,
        exccause=row.exccause,
        thread_name=row.thread_name,
        uptime_ms=row.uptime_ms,
        fw_version=row.fw_version,
    )
    after = json.dumps(detail, sort_keys=True) if detail else ""
    # Queue persist so Issues reloads skip addr2line.
    if persist is not None and after and after != before:
        row.detail_json = after
        persist.append(row)
    return CrashOut(
        id=row.id,
        device_id=row.device_id,
        group_id=row.group_id,
        phone_id=row.phone_id,
        ts_ms=row.ts_ms,
        seq=row.seq,
        reason=row.reason,
        pc=row.pc,
        exccause=row.exccause,
        excvaddr=row.excvaddr,
        thread_name=row.thread_name,
        fw_version=row.fw_version,
        reset_reason=row.reset_reason,
        soft_reboot_reason=row.soft_reboot_reason,
        is_fatal=bool(row.is_fatal),
        uptime_ms=row.uptime_ms,
        backtrace=row.backtrace,
        detail=detail or None,
    )


def ensure_db() -> None:
    Base.metadata.create_all(bind=engine)
    if os.getenv("DATABASE_URL", "").startswith("postgres"):
        try:
            with engine.begin() as conn:
                conn.execute(text("CREATE EXTENSION IF NOT EXISTS timescaledb"))
                for table in (
                    "verdicts",
                    "spectra",
                    "crashes",
                    "battery_bench_samples",
                    "clock_events",
                    "device_telemetry",
                    "cloud_ingest_batches",
                    "wearable_samples",
                ):
                    conn.execute(
                        text(
                            f"SELECT create_hypertable('{table}', 'ts_ms', "
                            "if_not_exists => TRUE, migrate_data => TRUE)"
                        )
                    )
        except Exception:
            pass
        try:
            with engine.begin() as conn:
                # seq alone collides when ESP crash ring resets; include pc in dedup key.
                conn.execute(text("DROP INDEX IF EXISTS ix_crashes_device_seq"))
                conn.execute(
                    text(
                        "CREATE UNIQUE INDEX IF NOT EXISTS ix_crashes_device_seq_pc "
                        "ON crashes (device_id, seq, pc)"
                    )
                )
        except Exception as exc:
            import logging

            logging.getLogger(__name__).warning("crash dedup index migration: %s", exc)
        try:
            with engine.begin() as conn:
                for col, typ in (
                    ("cpu_mhz", "INTEGER"),
                    ("apb_mhz", "INTEGER"),
                    ("spool_free_b", "INTEGER"),
                    ("spool_cap_b", "INTEGER"),
                    ("spool_pending", "INTEGER"),
                ):
                    conn.execute(text(f"ALTER TABLE verdicts ADD COLUMN IF NOT EXISTS {col} {typ}"))
                for col, typ in (
                    ("pct", "INTEGER"),
                    ("voltage", "DOUBLE PRECISION"),
                    ("power_source", "INTEGER"),
                    ("fw_version", "VARCHAR(64)"),
                    ("fwc", "INTEGER"),
                    ("spi_mhz", "INTEGER"),
                    ("i2c_khz", "INTEGER"),
                    ("ble_rx_kb", "INTEGER"),
                    ("ble_tx_kb", "INTEGER"),
                    ("ble_rx_b", "BIGINT"),
                    ("ble_tx_b", "BIGINT"),
                    ("ble_rx_bps", "INTEGER"),
                    ("ble_tx_bps", "INTEGER"),
                    ("display_on", "INTEGER"),
                    ("wifi_on", "INTEGER"),
                    ("wifi_rssi", "INTEGER"),
                    ("wifi_ap", "INTEGER"),
                    ("wifi_ssid", "VARCHAR(64)"),
                ):
                    conn.execute(
                        text(f"ALTER TABLE device_telemetry ADD COLUMN IF NOT EXISTS {col} {typ}")
                    )
        except Exception as exc:
            import logging

            logging.getLogger(__name__).warning("verdict column migration: %s", exc)
        try:
            with engine.begin() as conn:
                conn.execute(
                    text(
                        "ALTER TABLE crashes ADD COLUMN IF NOT EXISTS "
                        "soft_reboot_reason VARCHAR(32)"
                    )
                )
                conn.execute(
                    text(
                        "ALTER TABLE crashes ADD COLUMN IF NOT EXISTS "
                        "is_fatal INTEGER DEFAULT 1"
                    )
                )
        except Exception as exc:
            import logging

            logging.getLogger(__name__).warning("crash column migration: %s", exc)
        try:
            with engine.begin() as conn:
                conn.execute(
                    text(
                        "ALTER TABLE devices ADD COLUMN IF NOT EXISTS "
                        "last_esp_ms BIGINT DEFAULT 0"
                    )
                )
                conn.execute(
                    text(
                        """
                        UPDATE devices d SET last_esp_ms = s.mx
                        FROM (
                          SELECT device_id, MAX(ts_ms) AS mx
                          FROM cloud_ingest_batches
                          WHERE kind IN ('telemetry', 'verdict')
                          GROUP BY device_id
                        ) s
                        WHERE d.device_id = s.device_id
                          AND COALESCE(d.last_esp_ms, 0) = 0
                        """
                    )
                )
                conn.execute(
                    text(
                        """
                        INSERT INTO presence_gaps
                          (device_id, group_id, silent_from_ms, silent_until_ms, gap_ms, reason)
                        SELECT b.device_id, d.group_id, b.prev, b.ts_ms, b.ts_ms - b.prev, 'ingest_gap'
                        FROM (
                          SELECT device_id, ts_ms,
                                 lag(ts_ms) OVER (PARTITION BY device_id ORDER BY ts_ms) AS prev
                          FROM cloud_ingest_batches
                          WHERE kind IN ('telemetry', 'verdict')
                        ) b
                        LEFT JOIN devices d ON d.device_id = b.device_id
                        WHERE b.prev IS NOT NULL
                          AND b.ts_ms - b.prev >= 3600000
                        ON CONFLICT (device_id, silent_from_ms, silent_until_ms) DO NOTHING
                        """
                    )
                )
        except Exception as exc:
            import logging

            logging.getLogger(__name__).warning("presence gap migration: %s", exc)
        try:
            with engine.begin() as conn:
                for table in (
                    "verdicts",
                    "device_telemetry",
                    "wearable_samples",
                    "cloud_ingest_batches",
                ):
                    conn.execute(
                        text(
                            f"ALTER TABLE {table} ADD COLUMN IF NOT EXISTS "
                            "sent_at_ms BIGINT"
                        )
                    )
                    conn.execute(
                        text(
                            f"ALTER TABLE {table} ADD COLUMN IF NOT EXISTS "
                            "recv_ms BIGINT"
                        )
                    )
                    if table != "cloud_ingest_batches":
                        conn.execute(
                            text(
                                f"ALTER TABLE {table} ADD COLUMN IF NOT EXISTS "
                                "payload_bytes INTEGER"
                            )
                        )
        except Exception as exc:
            import logging

            logging.getLogger(__name__).warning("delivery stamp migration: %s", exc)


@router.get("/health", response_model=HealthOut)
def health() -> HealthOut:
    db_kind = "postgres" if os.getenv("DATABASE_URL", "").startswith("postgres") else "sqlite"
    has_timescale = False
    if db_kind == "postgres":
        try:
            with engine.connect() as conn:
                has_timescale = bool(
                    conn.execute(text("SELECT 1 FROM pg_extension WHERE extname = 'timescaledb'")).scalar()
                )
        except Exception:
            has_timescale = False
        if has_timescale:
            db_kind = "timescaledb"
    return HealthOut(ok=True, db=db_kind, timescaledb=has_timescale)


def _repair_verdict_telemetry(existing: Verdict, rec) -> bool:
    """Backfill pct/voltage/temp and edge telemetry on duplicate seq rows."""
    repaired = False
    if (existing.voltage is None or existing.voltage <= 0) and rec.voltage and rec.voltage > 0:
        existing.voltage = rec.voltage
        existing.pct = rec.pct
        repaired = True
    if existing.chip_temp_c is None and rec.chip_temp_c is not None:
        existing.chip_temp_c = rec.chip_temp_c
        repaired = True
    if existing.cpu_mhz is None and rec.cpu_mhz is not None:
        existing.cpu_mhz = rec.cpu_mhz
        repaired = True
    if existing.apb_mhz is None and rec.apb_mhz is not None:
        existing.apb_mhz = rec.apb_mhz
        repaired = True
    if existing.spool_free_b is None and rec.spool_free_b is not None:
        existing.spool_free_b = rec.spool_free_b
        existing.spool_cap_b = rec.spool_cap_b
        existing.spool_pending = rec.spool_pending
        repaired = True
    if existing.power_profile is None and rec.power_profile is not None:
        existing.power_profile = rec.power_profile
        repaired = True
    edge = _edge_payload(rec)
    if edge:
        existing_edge = _edge_from_raw(existing.raw_json)
        merged = {**existing_edge, **{k: v for k, v in edge.items() if k not in existing_edge or existing_edge[k] is None}}
        if merged != existing_edge:
            existing.raw_json = json.dumps(merged)
            repaired = True
    return repaired


def _touch_device(
    db: Session,
    device_id: str,
    group_id: str | None,
    phone_id: str | None,
    seen_ms: int,
    *,
    esp: bool = False,
) -> Device:
    device = db.get(Device, device_id)
    if device is None:
        device = Device(device_id=device_id)
        db.add(device)
        db.flush()
    device.group_id = group_id or device.group_id
    if phone_id:
        device.last_phone_id = phone_id
    if seen_ms:
        device.last_seen_ms = seen_ms
    if esp and seen_ms:
        prev = int(device.last_esp_ms or 0)
        if prev > 0 and seen_ms - prev >= SILENCE_ALERT_MS:
            recent = db.scalar(
                select(PresenceGap.id).where(
                    PresenceGap.device_id == device_id,
                    PresenceGap.silent_until_ms >= seen_ms - PRESENCE_DEDUP_MS,
                ).limit(1)
            )
            if recent is None:
                db.add(
                    PresenceGap(
                        device_id=device_id,
                        group_id=device.group_id,
                        silent_from_ms=prev,
                        silent_until_ms=seen_ms,
                        gap_ms=seen_ms - prev,
                        reason="ingest_gap",
                    )
                )
        if seen_ms >= prev:
            device.last_esp_ms = seen_ms
    return device


def _ingest_meta(body) -> tuple[int, int, int]:
    recv_ms = int(time.time() * 1000)
    sent_at = int(getattr(body, "sent_at_ms", 0) or recv_ms)
    try:
        nbytes = len(body.model_dump_json().encode("utf-8"))
    except Exception:
        nbytes = 0
    return recv_ms, sent_at, nbytes


def _share_bytes(total: int, n: int) -> int:
    if n <= 0:
        return max(0, total)
    return max(0, total // n)


def _device_silence(device: Device, now_ms: int) -> tuple[int, int, bool]:
    last_esp = int(device.last_esp_ms or 0)
    heartbeat = last_esp if last_esp > 0 else int(device.last_seen_ms or 0)
    silent_for = max(0, now_ms - heartbeat) if heartbeat > 0 else 0
    return last_esp, silent_for, heartbeat > 0 and silent_for >= SILENCE_ALERT_MS


def _edge_payload(rec) -> dict:
    out: dict = {}
    for key, val in (
        ("band_corr", rec.band_corr),
        ("band_delta_max", rec.band_delta_max),
        ("bands", rec.bands),
        ("edge_crest", rec.edge_crest),
        ("edge_zcr_hz", rec.edge_zcr_hz),
        ("edge_hf_ratio", rec.edge_hf_ratio),
        ("session_seq", rec.session_seq),
        ("cap_mix_sec", rec.cap_mix_sec),
    ):
        if val is not None:
            out[key] = val
    scored = score_edge_features(
        band_corr=rec.band_corr,
        band_delta_max=rec.band_delta_max,
        bands=rec.bands,
        edge_crest=rec.edge_crest,
        edge_zcr_hz=rec.edge_zcr_hz,
        edge_hf_ratio=rec.edge_hf_ratio,
        level=rec.level,
    )
    out.update(scored)
    return out


def _edge_from_raw(raw_json: str | None) -> dict:
    if not raw_json:
        return {}
    try:
        data = json.loads(raw_json)
        return data if isinstance(data, dict) else {}
    except (json.JSONDecodeError, TypeError):
        return {}


def _verdict_out(row: Verdict) -> VerdictOut:
    edge = _edge_from_raw(row.raw_json)
    return VerdictOut(
        id=row.id,
        device_id=row.device_id,
        group_id=row.group_id,
        ts_ms=row.ts_ms,
        seq=row.seq,
        level=row.level,
        rms=row.rms,
        peak=row.peak,
        corr=row.corr,
        rms_delta=row.rms_delta,
        pct=row.pct,
        voltage=row.voltage,
        power_profile=row.power_profile,
        chip_temp_c=row.chip_temp_c,
        band_corr=edge.get("band_corr"),
        band_delta_max=edge.get("band_delta_max"),
        bands=edge.get("bands"),
        edge_crest=edge.get("edge_crest"),
        edge_zcr_hz=edge.get("edge_zcr_hz"),
        edge_hf_ratio=edge.get("edge_hf_ratio"),
        edge_score=edge.get("edge_score"),
        edge_risk=edge.get("edge_risk"),
    )


@router.post("/ingest/verdicts", response_model=IngestResult)
def ingest_verdicts(
    body: IngestEnvelope,
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> IngestResult:
    if body.schema != "imu.ingest.v1":
        raise HTTPException(status_code=400, detail="unsupported schema")

    recv_ms, sent_at_ms, nbytes = _ingest_meta(body)
    _touch_device(
        db, body.device_id, body.group_id, body.phone_id, sent_at_ms, esp=True
    )

    accepted = 0
    duplicates = 0
    per_bytes = _share_bytes(nbytes, len(body.records))
    for rec in body.records:
        if rec.type != "verdict":
            continue
        existing_id = db.scalar(
            select(Verdict.id).where(
                Verdict.device_id == body.device_id,
                Verdict.seq == rec.seq,
            )
        )
        if existing_id is not None:
            existing = db.get(Verdict, existing_id)
            if existing is not None and _repair_verdict_telemetry(existing, rec):
                accepted += 1
            else:
                duplicates += 1
            continue
        edge = _edge_payload(rec)
        db.add(
            Verdict(
                device_id=body.device_id,
                group_id=body.group_id,
                phone_id=body.phone_id,
                ts_ms=rec.ts_ms,
                seq=rec.seq,
                level=rec.level,
                rms=rec.rms,
                peak=rec.peak,
                corr=rec.corr,
                rms_delta=rec.rms_delta,
                pct=rec.pct,
                voltage=rec.voltage,
                power_profile=rec.power_profile,
                chip_temp_c=rec.chip_temp_c,
                cpu_mhz=rec.cpu_mhz,
                apb_mhz=rec.apb_mhz,
                spool_free_b=rec.spool_free_b,
                spool_cap_b=rec.spool_cap_b,
                spool_pending=rec.spool_pending,
                sent_at_ms=sent_at_ms,
                recv_ms=recv_ms,
                payload_bytes=per_bytes,
                raw_json=json.dumps(edge) if edge else None,
            )
        )
        accepted += 1

    db.commit()
    if accepted > 0:
        refresh_after_ingest(db, body.device_id)
        db.commit()
    return IngestResult(accepted=accepted, duplicates=duplicates, device_id=body.device_id)


@router.post("/ingest/telemetry", response_model=IngestResult)
def ingest_telemetry(
    body: TelemetryIngestEnvelope,
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> IngestResult:
    recv_ms, sent_at_ms, nbytes = _ingest_meta(body)
    _touch_device(
        db, body.device_id, body.group_id, body.phone_id, sent_at_ms, esp=True
    )

    accepted = 0
    per_bytes = _share_bytes(nbytes, len(body.records))
    for rec in body.records:
        if rec.type != "telemetry":
            continue
        db.add(
            DeviceTelemetry(
                device_id=body.device_id,
                group_id=body.group_id,
                phone_id=body.phone_id,
                ts_ms=rec.ts_ms,
                pct=rec.pct,
                voltage=rec.voltage,
                power_source=rec.power_source,
                chip_temp_c=rec.chip_temp_c,
                cpu_mhz=rec.cpu_mhz,
                apb_mhz=rec.apb_mhz,
                spool_free_b=rec.spool_free_b,
                spool_cap_b=rec.spool_cap_b,
                spool_pending=rec.spool_pending,
                dram_free_kb=rec.dram_free_kb,
                fw_version=rec.fw_version,
                fwc=rec.fwc,
                spi_mhz=rec.spi_mhz,
                i2c_khz=rec.i2c_khz,
                ble_rx_kb=rec.ble_rx_kb,
                ble_tx_kb=rec.ble_tx_kb,
                ble_rx_b=rec.ble_rx_b,
                ble_tx_b=rec.ble_tx_b,
                ble_rx_bps=rec.ble_rx_bps,
                ble_tx_bps=rec.ble_tx_bps,
                display_on=rec.display_on,
                wifi_on=rec.wifi_on,
                wifi_rssi=rec.wifi_rssi,
                wifi_ap=rec.wifi_ap,
                wifi_ssid=rec.wifi_ssid,
                sent_at_ms=sent_at_ms,
                recv_ms=recv_ms,
                payload_bytes=per_bytes,
            )
        )
        accepted += 1

    db.commit()
    return IngestResult(accepted=accepted, duplicates=0, device_id=body.device_id)


@router.post("/ingest/batches", response_model=IngestResult)
def ingest_batches(
    body: IngestBatchEnvelope,
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> IngestResult:
    device = db.get(Device, body.device_id)
    if device is None:
        device = Device(device_id=body.device_id)
        db.add(device)
    device.group_id = body.group_id or device.group_id
    recv_ms, sent_at_ms, _nbytes = _ingest_meta(body)
    device.last_seen_ms = sent_at_ms
    device.last_phone_id = body.phone_id

    accepted = 0
    for batch in body.batches:
        db.add(
            CloudIngestBatch(
                device_id=body.device_id,
                group_id=body.group_id,
                phone_id=body.phone_id,
                ts_ms=batch.ts_ms,
                kind=batch.kind,
                records=batch.records,
                bytes=batch.bytes,
                sent_at_ms=sent_at_ms,
                recv_ms=recv_ms,
            )
        )
        accepted += 1

    db.commit()
    return IngestResult(accepted=accepted, duplicates=0, device_id=body.device_id)


@router.post("/ingest/spectra", response_model=IngestResult)
def ingest_spectra(
    body: SpectrumIngestEnvelope,
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> IngestResult:
    device = db.get(Device, body.device_id)
    if device is None:
        device = Device(device_id=body.device_id)
        db.add(device)
    device.group_id = body.group_id or device.group_id
    device.last_seen_ms = body.sent_at_ms
    device.last_phone_id = body.phone_id

    accepted = 0
    duplicates = 0
    for rec in body.records:
        exists = db.scalar(
            select(Spectrum.id).where(
                Spectrum.device_id == body.device_id,
                Spectrum.seq == rec.seq,
            )
        )
        if exists is not None:
            duplicates += 1
            continue
        db.add(
            Spectrum(
                device_id=body.device_id,
                group_id=body.group_id,
                phone_id=body.phone_id,
                ts_ms=rec.ts_ms,
                seq=rec.seq,
                sample_hz=rec.sample_hz,
                bin_hz=rec.bin_hz,
                bins_json=json.dumps(rec.bins),
                peak_hz=rec.peak_hz,
                peak_mag=rec.peak_mag,
                axis=rec.axis,
            )
        )
        accepted += 1

    db.commit()
    return IngestResult(accepted=accepted, duplicates=duplicates, device_id=body.device_id)


@router.post("/ingest/crashes", response_model=IngestResult)
def ingest_crashes(
    body: CrashIngestEnvelope,
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> IngestResult:
    if body.schema != "imu.ingest.v1":
        raise HTTPException(status_code=400, detail="unsupported schema")

    device = db.get(Device, body.device_id)
    if device is None:
        device = Device(device_id=body.device_id)
        db.add(device)
    device.group_id = body.group_id or device.group_id
    device.last_seen_ms = body.sent_at_ms
    device.last_phone_id = body.phone_id

    accepted = 0
    duplicates = 0
    seen: set[tuple[int, int]] = set()
    for rec in body.records:
        if rec.type != "crash":
            continue
        pc_val = rec.pc if rec.pc is not None else 0
        key = (rec.seq, pc_val)
        if key in seen:
            duplicates += 1
            continue
        exists = db.scalar(
            select(Crash.id).where(
                Crash.device_id == body.device_id,
                Crash.seq == rec.seq,
                func.coalesce(Crash.pc, 0) == pc_val,
            )
        )
        if exists is not None:
            duplicates += 1
            seen.add(key)
            continue
        seen.add(key)
        detail = enrich_crash_detail(
            rec.detail,
            rec.backtrace,
            pc=rec.pc,
            reason=rec.reason,
            exccause=rec.exccause,
            thread_name=rec.thread_name,
            uptime_ms=rec.uptime_ms,
            fw_version=rec.fw_version,
        )
        if rec.soft_reboot_reason:
            detail = detail or {}
            detail["soft_reboot_reason"] = rec.soft_reboot_reason
            detail["fatal"] = rec.is_fatal
        db.add(
            Crash(
                device_id=body.device_id,
                group_id=body.group_id,
                phone_id=body.phone_id,
                ts_ms=rec.ts_ms,
                seq=rec.seq,
                reason=rec.reason,
                pc=rec.pc,
                exccause=rec.exccause,
                excvaddr=rec.excvaddr,
                thread_name=rec.thread_name,
                fw_version=rec.fw_version,
                reset_reason=rec.reset_reason,
                soft_reboot_reason=rec.soft_reboot_reason,
                is_fatal=1 if rec.is_fatal else 0,
                uptime_ms=rec.uptime_ms,
                backtrace_json=json.dumps(rec.backtrace),
                detail_json=json.dumps(detail) if detail else None,
            )
        )
        accepted += 1

    db.commit()
    return IngestResult(accepted=accepted, duplicates=duplicates, device_id=body.device_id)


@router.post("/ingest/battery_bench", response_model=IngestResult)
def ingest_battery_bench(
    body: BatteryBenchIngestEnvelope,
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> IngestResult:
    if body.schema != "imu.ingest.v1":
        raise HTTPException(status_code=400, detail="unsupported schema")

    device = db.get(Device, body.device_id)
    if device is None:
        device = Device(device_id=body.device_id)
        db.add(device)
    device.group_id = body.group_id or device.group_id
    device.last_seen_ms = body.sent_at_ms
    device.last_phone_id = body.phone_id

    accepted = 0
    duplicates = 0
    for rec in body.records:
        if rec.type != "battery_bench_sample":
            continue
        sess = db.scalar(
            select(BatteryBenchSession).where(
                BatteryBenchSession.device_id == body.device_id,
                BatteryBenchSession.session_id == rec.session_id,
            )
        )
        if sess is None:
            started = rec.session_started_ms or rec.ts_ms
            sess = BatteryBenchSession(
                session_id=rec.session_id,
                device_id=body.device_id,
                group_id=body.group_id,
                phone_id=body.phone_id,
                label=rec.label,
                cell_mah=rec.cell_mah or 500,
                started_ms=started,
                profile_json=json.dumps(rec.profile_snapshot)
                if rec.profile_snapshot
                else None,
            )
            db.add(sess)
            db.flush()
        elif rec.label and not sess.label:
            sess.label = rec.label
        if rec.session_stopped:
            sess.stopped_ms = rec.ts_ms

        exists = db.scalar(
            select(BatteryBenchSample.id).where(
                BatteryBenchSample.device_id == body.device_id,
                BatteryBenchSample.session_id == rec.session_id,
                BatteryBenchSample.seq == rec.seq,
            )
        )
        if exists is not None:
            duplicates += 1
            continue

        prev = db.scalar(
            select(BatteryBenchSample)
            .where(
                BatteryBenchSample.device_id == body.device_id,
                BatteryBenchSample.session_id == rec.session_id,
                BatteryBenchSample.seq < rec.seq,
            )
            .order_by(BatteryBenchSample.seq.desc())
            .limit(1)
        )
        est_ma = None
        if prev is not None and prev.voltage is not None and rec.voltage is not None:
            est_ma = estimate_discharge_ma(
                rec.voltage,
                prev.voltage,
                rec.ts_ms - prev.ts_ms,
                sess.cell_mah,
            )

        db.add(
            BatteryBenchSample(
                session_id=rec.session_id,
                device_id=body.device_id,
                group_id=body.group_id,
                phone_id=body.phone_id,
                seq=rec.seq,
                ts_ms=rec.ts_ms,
                voltage=rec.voltage,
                pct=rec.pct,
                trend_v=rec.trend_v,
                src=rec.src,
                cpu_mhz=rec.cpu_mhz,
                imu_hz=rec.imu_hz,
                render_hz=rec.render_hz,
                chip_temp_c=rec.chip_temp_c,
                uptime_ms=rec.uptime_ms,
                est_ma=est_ma,
            )
        )
        accepted += 1

    db.commit()
    return IngestResult(accepted=accepted, duplicates=duplicates, device_id=body.device_id)


@router.post("/ingest/clock", response_model=IngestResult)
def ingest_clock(
    body: ClockIngestEnvelope,
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> IngestResult:
    device = db.get(Device, body.device_id)
    if device is None:
        device = Device(device_id=body.device_id)
        db.add(device)
    device.group_id = body.group_id or device.group_id
    device.last_seen_ms = body.sent_at_ms
    device.last_phone_id = body.phone_id

    accepted = 0
    for rec in body.records:
        if rec.type != "clock":
            continue
        db.add(
            ClockEvent(
                device_id=body.device_id,
                group_id=body.group_id,
                phone_id=body.phone_id,
                ts_ms=rec.ts_ms,
                drift_ms=rec.drift_ms,
                corr_ms=rec.corr_ms,
                tz_min=rec.tz_min,
                unix_sec=rec.unix_sec,
                src=rec.src,
            )
        )
        accepted += 1
    db.commit()
    return IngestResult(accepted=accepted, duplicates=0, device_id=body.device_id)


@router.post("/ingest/ahrs")
def ingest_ahrs(
    body: AhrsSampleIn,
    _: None = Depends(require_api_key),
) -> dict:
    prev = _ahrs_latest.get(body.device_id)
    if prev is not None and body.seq != 0 and body.seq <= prev.seq:
        return {"ok": True, "stale": True}
    _ahrs_latest[body.device_id] = AhrsLatestOut(
        device_id=body.device_id,
        unix_ms=body.unix_ms,
        seq=body.seq,
        rot=body.rot,
        recv_ms=int(time.time() * 1000),
    )
    return {"ok": True}


@router.get("/ahrs/{device_id}/latest", response_model=AhrsLatestOut)
def ahrs_latest(
    device_id: str,
    _: None = Depends(require_api_key),
) -> AhrsLatestOut:
    sample = _ahrs_latest.get(device_id)
    if sample is None:
        raise HTTPException(status_code=404, detail="no AHRS sample received yet")
    return sample


@router.get("/live")
def live_streams(_: None = Depends(require_api_key)) -> dict:
    """In-memory AHRS + geo keys currently held by this process (lost on container recreate)."""
    now = int(time.time() * 1000)
    return {
        "ahrs": [
            {
                "device_id": device_id,
                "unix_ms": sample.unix_ms,
                "recv_ms": sample.recv_ms,
                "age_ms": now - sample.recv_ms,
            }
            for device_id, sample in _ahrs_latest.items()
        ],
        "geo": [
            {
                "device_id": device_id,
                "gps_n": len(buckets["gps"]),
                "imu_n": len(buckets["imu"]),
            }
            for device_id, buckets in _geo_routes.items()
        ],
    }


@router.post("/ingest/geo")
def ingest_geo(
    body: GeoPointIn,
    _: None = Depends(require_api_key),
) -> dict:
    try:
        routes = _geo_routes.setdefault(body.device_id, {"gps": [], "imu": []})
        bucket = routes[body.kind]
        acc = body.accuracy_m
        if acc is not None and (acc != acc or acc == float("inf") or acc == float("-inf")):
            acc = None
        bucket.append(
            GeoPointOut(unix_ms=body.unix_ms, lat=body.lat, lon=body.lon, accuracy_m=acc)
        )
        if len(bucket) > _GEO_MAX_POINTS:
            del bucket[: len(bucket) - _GEO_MAX_POINTS]
        return {"ok": True}
    except Exception as exc:
        import logging

        logging.getLogger(__name__).exception("ingest_geo failed")
        raise HTTPException(status_code=400, detail=f"geo ingest rejected: {exc}") from exc


@router.get("/geo/{device_id}/route", response_model=GeoRouteOut)
def geo_route(
    device_id: str,
    _: None = Depends(require_api_key),
) -> GeoRouteOut:
    routes = _geo_routes.get(device_id) or {"gps": [], "imu": []}
    return GeoRouteOut(device_id=device_id, gps=routes["gps"], imu=routes["imu"])


@router.delete("/geo/{device_id}/route")
def geo_route_clear(
    device_id: str,
    _: None = Depends(require_api_key),
) -> dict:
    _geo_routes.pop(device_id, None)
    return {"ok": True}


@router.post("/ingest/wearable", response_model=IngestResult)
def ingest_wearable(
    body: WearableIngestEnvelope,
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> IngestResult:
    recv_ms, sent_at_ms, nbytes = _ingest_meta(body)
    _touch_device(
        db, body.device_id, body.group_id, body.phone_id, sent_at_ms, esp=True
    )
    accepted = 0
    duplicates = 0
    latest = _wearable_latest.get(body.device_id)
    if latest is None:
        latest = WearableLatestOut(device_id=body.device_id, recv_ms=0, kinds={})
    now_ms = recv_ms
    per_bytes = _share_bytes(nbytes, len(body.records))
    for rec in body.records:
        extra_json = json.dumps(rec.extra) if rec.extra else None
        existing = db.execute(
            select(WearableSample).where(
                WearableSample.device_id == body.device_id,
                WearableSample.ts_ms == rec.ts_ms,
                WearableSample.kind == rec.kind,
                WearableSample.seq == rec.seq,
            )
        ).scalar_one_or_none()
        if rec.kind in ("rssi_esp", "rssi_mt200") and (
            rec.value is None or rec.value <= -120 or rec.value >= 20
        ):
            continue
        if existing is not None:
            duplicates += 1
        else:
            db.add(
                WearableSample(
                    device_id=body.device_id,
                    group_id=body.group_id,
                    phone_id=body.phone_id,
                    source=rec.source,
                    kind=rec.kind,
                    ts_ms=rec.ts_ms,
                    seq=rec.seq,
                    value=rec.value,
                    extra_json=extra_json,
                    sent_at_ms=sent_at_ms,
                    recv_ms=now_ms,
                    payload_bytes=per_bytes,
                )
            )
            accepted += 1
        prev = latest.kinds.get(rec.kind)
        if prev is None or rec.ts_ms >= prev.ts_ms:
            latest.kinds[rec.kind] = WearableKindLatest(
                kind=rec.kind,
                source=rec.source,
                ts_ms=rec.ts_ms,
                seq=rec.seq,
                value=rec.value,
                extra=rec.extra,
            )
    latest.recv_ms = now_ms
    _wearable_latest[body.device_id] = latest
    db.commit()
    return IngestResult(accepted=accepted, duplicates=duplicates, device_id=body.device_id)


def _wearable_latest_from_db(db: Session, device_id: str) -> WearableLatestOut | None:
    """Rebuild per-kind latest from Timescale after process restart (RAM cache cold)."""
    # DISTINCT ON (kind) … ORDER BY kind, ts_ms DESC — Postgres/Timescale.
    rows = db.execute(
        select(WearableSample)
        .where(WearableSample.device_id == device_id)
        .distinct(WearableSample.kind)
        .order_by(WearableSample.kind, WearableSample.ts_ms.desc())
    ).scalars().all()
    if not rows:
        return None
    kinds: dict[str, WearableKindLatest] = {}
    recv_ms = 0
    for row in rows:
        kinds[row.kind] = WearableKindLatest(
            kind=row.kind,
            source=row.source,
            ts_ms=row.ts_ms,
            seq=row.seq,
            value=row.value,
            extra=row.extra,
        )
        if row.recv_ms is not None and row.recv_ms > recv_ms:
            recv_ms = row.recv_ms
        elif row.ts_ms > recv_ms:
            recv_ms = row.ts_ms
    return WearableLatestOut(device_id=device_id, recv_ms=recv_ms, kinds=kinds)


@router.get("/wearable/{device_id}/latest", response_model=WearableLatestOut)
def wearable_latest(
    device_id: str,
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> WearableLatestOut:
    sample = _wearable_latest.get(device_id)
    if sample is None:
        sample = _wearable_latest_from_db(db, device_id)
        if sample is None:
            raise HTTPException(status_code=404, detail="no wearable sample received yet")
        _wearable_latest[device_id] = sample
    return sample


@router.get("/wearable/{device_id}/samples", response_model=list[WearableSampleOut])
def wearable_samples(
    device_id: str,
    kind: str | None = Query(default=None, max_length=16),
    since_ms: int = Query(default=0, ge=0),
    limit: int = Query(default=200, ge=1, le=2000),
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> list[WearableSampleOut]:
    stmt = select(WearableSample).where(WearableSample.device_id == device_id)
    if kind:
        stmt = stmt.where(WearableSample.kind == kind)
    if since_ms:
        stmt = stmt.where(WearableSample.ts_ms >= since_ms)
    stmt = stmt.order_by(WearableSample.ts_ms.desc()).limit(limit)
    rows = db.execute(stmt).scalars().all()
    return [
        WearableSampleOut(
            device_id=row.device_id,
            source=row.source,
            kind=row.kind,
            ts_ms=row.ts_ms,
            seq=row.seq,
            value=row.value,
            extra=row.extra,
        )
        for row in rows
    ]


def _veepoo_stride_m(height_cm: float = 170.0) -> float:
    """Veepoo SportUtil.getStepLength — stride length in metres."""
    h = float(height_cm)
    if h < 155.0:
        return (h * 20.0) / 42.0 / 100.0
    if h < 174.0:
        return (h * 13.0) / 28.0 / 100.0
    return (h * 19.0) / 42.0 / 100.0


@router.get("/wearable/{device_id}/daily", response_model=WearableDailyOut)
def wearable_daily(
    device_id: str,
    range_days: int = Query(default=7, alias="range", ge=1, le=366),
    tz_offset_min: int = Query(default=0, ge=-840, le=840),
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> WearableDailyOut:
    """Daily steps/kcal for the local calendar; distance_m from steps×stride.
    HR / SpO2 are per-day averages of samples (not cumulative counters).

    Watch step/kcal counters are cumulative within a day and reset at local midnight.
    Using the latest sample per day keeps the pre-reset total. HR/SpO2 use the mean
    of in-range samples for that local day.
    """
    from datetime import datetime, timedelta, timezone

    if range_days not in (7, 30, 365):
        raise HTTPException(status_code=400, detail="range must be 7, 30, or 365")

    tz = timezone(timedelta(minutes=tz_offset_min))
    today_local = datetime.now(tz).date()
    start_day = today_local - timedelta(days=range_days - 1)
    start_dt = datetime(start_day.year, start_day.month, start_day.day, tzinfo=tz)
    since_ms = int(start_dt.timestamp() * 1000)

    rows = db.execute(
        select(WearableSample.kind, WearableSample.ts_ms, WearableSample.value).where(
            WearableSample.device_id == device_id,
            WearableSample.kind.in_(("steps", "kcal_x10", "hr", "spo2")),
            WearableSample.ts_ms >= since_ms,
            WearableSample.value.is_not(None),
        )
    ).all()

    # day -> kind -> (ts_ms, value) for cumulative counters (newest wins)
    by_day: dict[str, dict[str, tuple[int, float]]] = {}
    # day -> kind -> list of values for averages
    by_day_avg: dict[str, dict[str, list[float]]] = {}
    for kind, ts_ms, value in rows:
        day = datetime.fromtimestamp(ts_ms / 1000.0, tz=tz).date().isoformat()
        v = float(value)
        if kind in ("hr", "spo2"):
            # Drop obvious out-of-range / wear-fail sentinels
            if kind == "hr" and (v < 30.0 or v > 220.0):
                continue
            if kind == "spo2" and (v < 70.0 or v > 100.0):
                continue
            by_day_avg.setdefault(day, {}).setdefault(kind, []).append(v)
            continue
        bucket = by_day.setdefault(day, {})
        prev = bucket.get(kind)
        if prev is None or int(ts_ms) >= prev[0]:
            bucket[kind] = (int(ts_ms), v)

    stride = _veepoo_stride_m(170.0)
    days: list[WearableDailyPoint] = []
    for i in range(range_days):
        d = (start_day + timedelta(days=i)).isoformat()
        bucket = by_day.get(d, {})
        avgs = by_day_avg.get(d, {})
        steps = float(bucket["steps"][1]) if "steps" in bucket else 0.0
        kcal_x10 = float(bucket["kcal_x10"][1]) if "kcal_x10" in bucket else 0.0
        hr_list = avgs.get("hr") or []
        spo2_list = avgs.get("spo2") or []
        days.append(
            WearableDailyPoint(
                day=d,
                steps=steps,
                distance_m=round(steps * stride, 1),
                kcal=round(kcal_x10 / 10.0, 1),
                hr_avg=round(sum(hr_list) / len(hr_list), 1) if hr_list else 0.0,
                spo2_avg=round(sum(spo2_list) / len(spo2_list), 1) if spo2_list else 0.0,
            )
        )

    return WearableDailyOut(
        device_id=device_id,
        range_days=range_days,
        tz_offset_min=tz_offset_min,
        days=days,
    )


@router.post("/ingest/reference_profiles", response_model=IngestResult)
def ingest_reference_profiles(
    body: ReferenceProfileIngestEnvelope,
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> IngestResult:
    now_ms = int(time.time() * 1000)
    accepted = 0
    for rec in body.records:
        if rec.type != "reference_profile":
            continue
        row = db.scalar(
            select(ReferenceProfile).where(
                ReferenceProfile.device_id == body.device_id,
                ReferenceProfile.slot == rec.slot,
            )
        )
        if row is None:
            row = ReferenceProfile(
                device_id=body.device_id,
                slot=rec.slot,
                created_ms=now_ms,
            )
            db.add(row)
        row.name = rec.name
        row.updated_ms = now_ms
        row.duration_ms = rec.duration_ms
        row.sample_hz = rec.sample_hz
        row.bands_json = json.dumps(rec.bands) if rec.bands else None
        row.raw_json = json.dumps(rec.raw) if rec.raw else None
        row.active = rec.active
        accepted += 1
    db.commit()
    return IngestResult(accepted=accepted, duplicates=0, device_id=body.device_id)


@router.get("/devices/{device_id}/battery_bench/sessions", response_model=list[BatteryBenchSessionOut])
def list_battery_bench_sessions(
    device_id: str,
    limit: int = Query(default=20, ge=1, le=100),
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> list[BatteryBenchSessionOut]:
    rows = db.scalars(
        select(BatteryBenchSession)
        .where(BatteryBenchSession.device_id == device_id)
        .order_by(BatteryBenchSession.started_ms.desc())
        .limit(limit)
    ).all()
    out: list[BatteryBenchSessionOut] = []
    for row in rows:
        count = db.scalar(
            select(func.count())
            .select_from(BatteryBenchSample)
            .where(
                BatteryBenchSample.device_id == device_id,
                BatteryBenchSample.session_id == row.session_id,
            )
        ) or 0
        out.append(
            BatteryBenchSessionOut(
                id=row.id,
                session_id=row.session_id,
                device_id=row.device_id,
                group_id=row.group_id,
                label=row.label,
                cell_mah=row.cell_mah,
                started_ms=row.started_ms,
                stopped_ms=row.stopped_ms,
                sample_count=int(count),
            )
        )
    return out


@router.get(
    "/devices/{device_id}/battery_bench/sessions/{session_id}/samples",
    response_model=list[BatteryBenchSampleOut],
)
def list_battery_bench_samples(
    device_id: str,
    session_id: int,
    limit: int = Query(default=500, ge=1, le=5000),
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> list[BatteryBenchSampleOut]:
    rows = db.scalars(
        select(BatteryBenchSample)
        .where(
            BatteryBenchSample.device_id == device_id,
            BatteryBenchSample.session_id == session_id,
        )
        .order_by(BatteryBenchSample.seq.asc())
        .limit(limit)
    ).all()
    return [BatteryBenchSampleOut.model_validate(r) for r in rows]


@router.get("/devices", response_model=list[DeviceOut])
def list_devices(
    group_id: str | None = Query(default=None),
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> list[DeviceOut]:
    stmt = select(Device)
    if group_id:
        stmt = stmt.where(Device.group_id == group_id)
    now_ms = int(time.time() * 1000)
    devices = db.scalars(stmt.order_by(Device.last_seen_ms.desc())).all()
    out: list[DeviceOut] = []
    for dev in devices:
        last_esp, silent_for, silent = _device_silence(dev, now_ms)
        verdict_count = db.scalar(
            select(func.count()).select_from(Verdict).where(Verdict.device_id == dev.device_id)
        ) or 0
        crash_count = db.scalar(
            select(func.count()).select_from(Crash).where(Crash.device_id == dev.device_id)
        ) or 0
        latest = db.scalar(
            select(Verdict)
            .where(Verdict.device_id == dev.device_id)
            .order_by(Verdict.ts_ms.desc())
            .limit(1)
        )
        latest_crash = db.scalar(
            select(Crash)
            .where(Crash.device_id == dev.device_id)
            .order_by(Crash.ts_ms.desc())
            .limit(1)
        )
        out.append(
            DeviceOut(
                device_id=dev.device_id,
                group_id=dev.group_id,
                last_seen_ms=dev.last_seen_ms,
                last_phone_id=dev.last_phone_id,
                last_esp_ms=last_esp,
                silent_for_ms=silent_for,
                silent=silent,
                verdict_count=int(verdict_count),
                crash_count=int(crash_count),
                latest_level=latest.level if latest else None,
                latest_rms=latest.rms if latest else None,
                latest_crash_ts=latest_crash.ts_ms if latest_crash else None,
            )
        )
    return out


def _crash_detail(row: Crash) -> dict:
    if not row.detail_json:
        return {}
    try:
        data = json.loads(row.detail_json)
        return data if isinstance(data, dict) else {}
    except (json.JSONDecodeError, TypeError):
        return {}


def _ota_event_from_crash(row: Crash) -> OtaEventOut | None:
    detail = _crash_detail(row)
    reason = (row.reason or "") + " " + (row.soft_reboot_reason or "")
    outcome = detail.get("ota_outcome") or None
    boot = detail.get("boot_part")
    target = detail.get("target_part")
    is_ota = bool(outcome) or "fw_upgrade" in reason.lower() or (row.soft_reboot_reason or "") == "fw_upgrade"
    if not is_ota:
        return None
    outcome_s = str(outcome) if outcome is not None else None
    ok = None
    if outcome_s:
        low = outcome_s.lower()
        if "ok" in low or "transition_ok" in low:
            ok = True
        elif "non_operable" in low or "fail" in low or "nok" in low:
            ok = False
    return OtaEventOut(
        ts_ms=row.ts_ms,
        fw_version=row.fw_version,
        outcome=outcome_s,
        boot_part=str(boot) if boot is not None else None,
        target_part=str(target) if target is not None else None,
        ok=ok,
        reason=row.reason or row.soft_reboot_reason,
        uptime_ms=row.uptime_ms,
    )


def _avg(values: list[float]) -> float | None:
    if not values:
        return None
    return sum(values) / len(values)


def _lipo_pct_from_v(voltage: float | None) -> int | None:
    """STATUS / UI curve: 3.30 V empty … 4.20 V peak. USB/DC rails (>= 4.50 V) are not SoC."""
    if voltage is None or voltage < 2.8:
        return None
    if voltage >= 4.50:
        return None
    return int(max(0, min(100, round((voltage - 3.30) / (4.20 - 3.30) * 100.0))))


def _insight_voltage(tel: DeviceTelemetry | None, verdict: Verdict | None) -> float | None:
    for src in (tel, verdict):
        if src is None:
            continue
        v = getattr(src, "voltage", None)
        if v is not None and v > 0.4:
            return float(v)
    return None


def _insight_battery_pct(tel: DeviceTelemetry | None, verdict: Verdict | None) -> int | None:
    for src in (tel, verdict):
        if src is None:
            continue
        pct = getattr(src, "pct", None)
        # Firmware 0 means "not computed", not empty. Estimate from V instead.
        if pct is not None and 1 <= int(pct) <= 100:
            return int(pct)
    return _lipo_pct_from_v(_insight_voltage(tel, verdict))


@router.get("/devices/{device_id}/insights", response_model=DeviceInsightsOut)
def device_insights(
    device_id: str,
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> DeviceInsightsOut:
    device = db.get(Device, device_id)
    if device is None:
        raise HTTPException(status_code=404, detail="unknown device")
    now_ms = int(time.time() * 1000)
    last_esp, silent_for, silent = _device_silence(device, now_ms)
    latest_verdict = db.scalar(
        select(Verdict).where(Verdict.device_id == device_id).order_by(Verdict.ts_ms.desc()).limit(1)
    )
    latest_tel = db.scalar(
        select(DeviceTelemetry)
        .where(DeviceTelemetry.device_id == device_id)
        .order_by(
            DeviceTelemetry.recv_ms.desc().nulls_last(),
            DeviceTelemetry.id.desc(),
        )
        .limit(1)
    )
    bat_tel = db.scalar(
        select(DeviceTelemetry)
        .where(
            DeviceTelemetry.device_id == device_id,
            or_(DeviceTelemetry.voltage > 0.4, DeviceTelemetry.pct > 0),
        )
        .order_by(
            DeviceTelemetry.recv_ms.desc().nulls_last(),
            DeviceTelemetry.id.desc(),
        )
        .limit(1)
    )
    # Prefer highest fwc among fw-bearing STATUS rows (recv_ms alone kept stale v260
    # while desk/cloud already ran v308+). Fall back to latest crash fw.
    latest_fw_tel = db.scalar(
        select(DeviceTelemetry)
        .where(
            DeviceTelemetry.device_id == device_id,
            DeviceTelemetry.fw_version.is_not(None),
            DeviceTelemetry.fw_version != "",
        )
        .order_by(
            DeviceTelemetry.fwc.desc().nulls_last(),
            DeviceTelemetry.recv_ms.desc().nulls_last(),
            DeviceTelemetry.id.desc(),
        )
        .limit(1)
    )
    latest_crash = db.scalar(
        select(Crash).where(Crash.device_id == device_id).order_by(Crash.ts_ms.desc()).limit(1)
    )
    crashes = db.scalars(
        select(Crash).where(Crash.device_id == device_id).order_by(Crash.ts_ms.desc()).limit(80)
    ).all()
    ota_events = [ev for ev in (_ota_event_from_crash(c) for c in crashes) if ev is not None]
    # -127 / 127 are "not measured", not a weak hop. Drop them before the window
    # so a quiet stretch cannot erase the last real readings from the average.
    rssi_ok = and_(WearableSample.value > -120, WearableSample.value < 20)
    rssi_esp_rows = db.scalars(
        select(WearableSample)
        .where(
            WearableSample.device_id == device_id,
            WearableSample.kind == "rssi_esp",
            rssi_ok,
        )
        .order_by(WearableSample.ts_ms.desc())
        .limit(40)
    ).all()
    rssi_mt_rows = db.scalars(
        select(WearableSample)
        .where(
            WearableSample.device_id == device_id,
            WearableSample.kind == "rssi_mt200",
            rssi_ok,
        )
        .order_by(WearableSample.ts_ms.desc())
        .limit(40)
    ).all()
    rssi_esp_vals = [float(r.value) for r in rssi_esp_rows if r.value is not None]
    rssi_mt_vals = [float(r.value) for r in rssi_mt_rows if r.value is not None]
    verdict_count = db.scalar(
        select(func.count()).select_from(Verdict).where(Verdict.device_id == device_id)
    ) or 0
    crash_count = db.scalar(
        select(func.count()).select_from(Crash).where(Crash.device_id == device_id)
    ) or 0

    def _fwc_of(name: str | None, code: int | None = None) -> int:
        if code is not None and int(code) > 0:
            return int(code)
        if not name:
            return 0
        import re

        m = re.search(r"(?:^|[^\d])v?(\d{2,4})\b", name, re.I)
        if m:
            return int(m.group(1))
        m = re.search(r"(\d+)$", name.strip())
        return int(m.group(1)) if m else 0

    candidates: list[tuple[int, str | None, int | None]] = []
    if latest_fw_tel and latest_fw_tel.fw_version:
        candidates.append(
            (_fwc_of(latest_fw_tel.fw_version, latest_fw_tel.fwc), latest_fw_tel.fw_version, latest_fw_tel.fwc)
        )
    if latest_tel and latest_tel.fw_version:
        candidates.append(
            (_fwc_of(latest_tel.fw_version, latest_tel.fwc), latest_tel.fw_version, latest_tel.fwc)
        )
    if latest_crash and latest_crash.fw_version:
        candidates.append((_fwc_of(latest_crash.fw_version, None), latest_crash.fw_version, None))
    candidates.sort(key=lambda t: (t[0],), reverse=True)
    best_fw = candidates[0][1] if candidates else None
    best_fwc = candidates[0][0] if candidates and candidates[0][0] > 0 else (
        candidates[0][2] if candidates else None
    )

    return DeviceInsightsOut(
        device_id=device.device_id,
        group_id=device.group_id,
        last_seen_ms=device.last_seen_ms,
        last_esp_ms=last_esp,
        silent_for_ms=silent_for,
        silent=silent,
        last_phone_id=device.last_phone_id,
        latest_fw=best_fw,
        latest_fwc=best_fwc if best_fwc and best_fwc > 0 else None,
        battery_pct=_insight_battery_pct(bat_tel or latest_tel, latest_verdict),
        voltage=_insight_voltage(bat_tel or latest_tel, latest_verdict),
        chip_temp_c=(
            latest_verdict.chip_temp_c if latest_verdict and latest_verdict.chip_temp_c is not None
            else (latest_tel.chip_temp_c if latest_tel else None)
        ),
        cpu_mhz=(
            latest_verdict.cpu_mhz if latest_verdict and latest_verdict.cpu_mhz is not None
            else (latest_tel.cpu_mhz if latest_tel else None)
        ),
        apb_mhz=(
            latest_verdict.apb_mhz if latest_verdict and latest_verdict.apb_mhz is not None
            else (latest_tel.apb_mhz if latest_tel else None)
        ),
        spool_free_b=(
            latest_verdict.spool_free_b if latest_verdict and latest_verdict.spool_free_b is not None
            else (latest_tel.spool_free_b if latest_tel else None)
        ),
        spool_pending=(
            latest_verdict.spool_pending if latest_verdict and latest_verdict.spool_pending is not None
            else (latest_tel.spool_pending if latest_tel else None)
        ),
        spi_mhz=latest_tel.spi_mhz if latest_tel else None,
        i2c_khz=latest_tel.i2c_khz if latest_tel else None,
        ble_rx_kb=latest_tel.ble_rx_kb if latest_tel else None,
        ble_tx_kb=latest_tel.ble_tx_kb if latest_tel else None,
        ble_rx_b=latest_tel.ble_rx_b if latest_tel else None,
        ble_tx_b=latest_tel.ble_tx_b if latest_tel else None,
        ble_rx_bps=latest_tel.ble_rx_bps if latest_tel else None,
        ble_tx_bps=latest_tel.ble_tx_bps if latest_tel else None,
        display_on=latest_tel.display_on if latest_tel else None,
        wifi_on=latest_tel.wifi_on if latest_tel else None,
        wifi_rssi=latest_tel.wifi_rssi if latest_tel else None,
        wifi_ap=latest_tel.wifi_ap if latest_tel else None,
        wifi_ssid=latest_tel.wifi_ssid if latest_tel else None,
        avg_rssi_esp=_avg(rssi_esp_vals),
        avg_rssi_mt200=_avg(rssi_mt_vals),
        last_rssi_esp=rssi_esp_vals[0] if rssi_esp_vals else None,
        last_rssi_mt200=rssi_mt_vals[0] if rssi_mt_vals else None,
        verdict_count=int(verdict_count),
        crash_count=int(crash_count),
        last_uptime_ms=latest_crash.uptime_ms if latest_crash else None,
        ota=ota_events[0] if ota_events else None,
        recent_ota=ota_events[:8],
    )


@router.get("/devices/{device_id}/presence_gaps", response_model=list[PresenceGapOut])
def list_presence_gaps(
    device_id: str,
    limit: int = Query(default=20, ge=1, le=200),
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> list[PresenceGapOut]:
    rows = db.scalars(
        select(PresenceGap)
        .where(PresenceGap.device_id == device_id)
        .order_by(PresenceGap.silent_until_ms.desc())
        .limit(limit)
    ).all()
    return [
        PresenceGapOut(
            device_id=r.device_id,
            silent_from_ms=r.silent_from_ms,
            silent_until_ms=r.silent_until_ms,
            gap_ms=r.gap_ms,
            reason=r.reason,
        )
        for r in rows
    ]


@router.get("/export/delivery")
def export_delivery(
    device_id: str = Query(min_length=1, max_length=64),
    from_ms: int = Query(ge=0),
    to_ms: int = Query(ge=0),
    limit: int = Query(default=50000, ge=1, le=200000),
    format: str = Query(default="csv", pattern="^(csv|tsv|json)$"),
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
):
    if to_ms < from_ms:
        raise HTTPException(status_code=400, detail="to_ms must be >= from_ms")
    rows = collect_delivery_rows(db, device_id, from_ms, to_ms, limit)
    if format == "json":
        return {"device_id": device_id, "from_ms": from_ms, "to_ms": to_ms, "count": len(rows), "rows": rows}
    body = render_table(rows, tsv=(format == "tsv"))
    ext = "tsv" if format == "tsv" else "csv"
    filename = f"delivery-{device_id}-{from_ms}-{to_ms}.{ext}"
    media = "text/tab-separated-values" if format == "tsv" else "text/csv"
    return Response(
        content=body,
        media_type=f"{media}; charset=utf-8",
        headers={"Content-Disposition": f'attachment; filename="{filename}"'},
    )


@router.get("/devices/{device_id}/verdicts", response_model=list[VerdictOut])
def list_verdicts(
    device_id: str,
    limit: int = Query(default=50, ge=1, le=500),
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> list[VerdictOut]:
    rows = db.scalars(
        select(Verdict)
        .where(Verdict.device_id == device_id)
        .order_by(Verdict.ts_ms.desc())
        .limit(limit)
    ).all()
    return [_verdict_out(r) for r in rows]


@router.get("/devices/{device_id}/spectra", response_model=list[SpectrumOut])
def list_spectra(
    device_id: str,
    limit: int = Query(default=20, ge=1, le=100),
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> list[SpectrumOut]:
    rows = db.scalars(
        select(Spectrum)
        .where(Spectrum.device_id == device_id)
        .order_by(Spectrum.ts_ms.desc())
        .limit(limit)
    ).all()
    return [
        SpectrumOut(
            id=r.id,
            device_id=r.device_id,
            group_id=r.group_id,
            ts_ms=r.ts_ms,
            seq=r.seq,
            sample_hz=r.sample_hz,
            bin_hz=r.bin_hz,
            bins=r.bins,
            peak_hz=r.peak_hz,
            peak_mag=r.peak_mag,
            axis=r.axis,
        )
        for r in rows
    ]


@router.get("/devices/{device_id}/crashes", response_model=list[CrashOut])
def list_crashes(
    device_id: str,
    limit: int = Query(default=50, ge=1, le=500),
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> list[CrashOut]:
    rows = db.scalars(
        select(Crash)
        .where(Crash.device_id == device_id)
        .order_by(Crash.ts_ms.desc())
        .limit(limit)
    ).all()
    persist: list = []
    out = [_crash_out(r, persist=persist) for r in rows]
    if persist:
        try:
            db.commit()
        except Exception:
            db.rollback()
    return out


@router.get("/crashes", response_model=list[CrashOut])
def list_crashes_all(
    device_id: str | None = Query(default=None),
    group_id: str | None = Query(default=None),
    limit: int = Query(default=50, ge=1, le=500),
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> list[CrashOut]:
    stmt = select(Crash)
    if device_id:
        stmt = stmt.where(Crash.device_id == device_id)
    if group_id:
        stmt = stmt.where(Crash.group_id == group_id)
    rows = db.scalars(stmt.order_by(Crash.ts_ms.desc()).limit(limit)).all()
    persist: list = []
    out = [_crash_out(r, persist=persist) for r in rows]
    if persist:
        try:
            db.commit()
        except Exception:
            db.rollback()
    return out


class FirmwareElfUploadOut(BaseModel):
    fw_version: str
    written: list[str]
    url: str
    resymbolicated: int = Field(description="Crash rows re-enriched for this fw_version")
    elf_matched: bool = True


class FirmwareElfStatusOut(BaseModel):
    fw_version: str
    available: bool
    matched: bool
    path: str | None = None
    url: str | None = None
    note: str | None = None


def _resymbolicate_fw_version(db: Session, fw_version: str) -> int:
    rows = db.scalars(select(Crash).where(Crash.fw_version == fw_version)).all()
    n = 0
    for row in rows:
        detail = None
        if row.detail_json:
            try:
                detail = json.loads(row.detail_json)
            except (json.JSONDecodeError, TypeError):
                detail = None
        detail = enrich_crash_detail(
            detail,
            row.backtrace,
            pc=row.pc,
            reason=row.reason,
            exccause=row.exccause,
            thread_name=row.thread_name,
            uptime_ms=row.uptime_ms,
            fw_version=row.fw_version,
            force=True,
        )
        row.detail_json = json.dumps(detail) if detail else None
        n += 1
    if n:
        db.commit()
    return n


@router.get("/firmware-elfs/{fw_version:path}", response_model=FirmwareElfStatusOut)
def firmware_elf_status(
    fw_version: str,
    _: None = Depends(require_api_key),
) -> FirmwareElfStatusOut:
    sym = normalize_symvers(fw_version)
    if not sym:
        raise HTTPException(status_code=400, detail="invalid fw_version")
    path, note, matched = resolve_elf_for_version(sym)
    return FirmwareElfStatusOut(
        fw_version=sym,
        available=bool(path and matched),
        matched=matched,
        path=path if matched else None,
        url=firmware_elf_url(sym) if matched else None,
        note=note,
    )


@router.post(
    "/firmware-elfs/resymbolicate",
    response_model=FirmwareElfUploadOut,
)
def resymbolicate_firmware_elf(
    fw_version: str = Query(..., min_length=1, max_length=96),
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> FirmwareElfUploadOut:
    """Invalidate cached symbolication for ``fw_version`` and re-enrich from current ELF.

    Called after OTA publishes a new ``zephyr.elf`` (or when an operator wants a refresh
    without re-uploading). Only rows for this fw_version are touched.
    """
    sym = normalize_symvers(fw_version)
    if not sym:
        raise HTTPException(status_code=400, detail="invalid fw_version")
    path, _note, matched = resolve_elf_for_version(sym)
    n = _resymbolicate_fw_version(db, sym)
    return FirmwareElfUploadOut(
        fw_version=sym,
        written=[path] if path else [],
        url=firmware_elf_url(sym),
        resymbolicated=n,
        elf_matched=bool(matched),
    )


@router.post("/firmware-elfs/{fw_version:path}", response_model=FirmwareElfUploadOut)
async def upload_firmware_elf(
    fw_version: str,
    file: UploadFile = File(...),
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> FirmwareElfUploadOut:
    """Upload zephyr.elf for a fw_version; mirror to CDN mounts; re-symbolicate matching crashes."""
    sym = normalize_symvers(fw_version)
    if not sym:
        raise HTTPException(status_code=400, detail="invalid fw_version")
    data = await file.read()
    try:
        written = store_firmware_elf(sym, data)
    except ValueError as e:
        raise HTTPException(status_code=400, detail=str(e)) from e
    except OSError as e:
        raise HTTPException(status_code=503, detail=str(e)) from e
    n = _resymbolicate_fw_version(db, sym)
    return FirmwareElfUploadOut(
        fw_version=sym,
        written=written,
        url=firmware_elf_url(sym),
        resymbolicated=n,
        elf_matched=True,
    )


def _fuse_levels(levels: list[int]) -> tuple[int, float]:
    if not levels:
        return 0, 0.0
    counts = {0: 0, 1: 0, 2: 0}
    for level in levels:
        counts[max(0, min(2, level))] += 1
    best_level = max(counts, key=lambda lvl: (counts[lvl], lvl))
    return best_level, counts[best_level] / len(levels)


@router.get("/groups/{group_id}/status", response_model=GroupStatusOut)
def group_status(
    group_id: str,
    stale_ms: int = Query(default=3_600_000, ge=60_000, le=86_400_000),
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> GroupStatusOut:
    devices = db.scalars(select(Device).where(Device.group_id == group_id)).all()
    summaries: list[GroupDeviceVerdict] = []
    silent_devices: list[GroupSilentDevice] = []
    now_ms = int(time.time() * 1000)
    for device in devices:
        last_esp, silent_for, silent = _device_silence(device, now_ms)
        if silent:
            silent_devices.append(
                GroupSilentDevice(
                    device_id=device.device_id,
                    last_esp_ms=last_esp,
                    silent_for_ms=silent_for,
                )
            )
        row = db.scalar(
            select(Verdict)
            .where(Verdict.device_id == device.device_id)
            .order_by(Verdict.ts_ms.desc())
            .limit(1)
        )
        if row is None or now_ms - row.ts_ms > stale_ms:
            continue
        summaries.append(
            GroupDeviceVerdict(
                device_id=row.device_id,
                level=row.level,
                ts_ms=row.ts_ms,
                seq=row.seq,
                rms=row.rms,
                corr=row.corr,
            )
        )

    fused_level, confidence = _fuse_levels([s.level for s in summaries])
    return GroupStatusOut(
        group_id=group_id,
        device_count=len(devices),
        reporting_count=len(summaries),
        fused_level=fused_level,
        confidence=confidence,
        devices=summaries,
        silent_count=len(silent_devices),
        silent_devices=silent_devices,
    )


def _config_out(row: DeviceConfigRevision) -> DeviceConfigOut:
    return DeviceConfigOut(
        device_id=row.device_id,
        revision=row.revision,
        local_revision=row.local_revision,
        source=row.source,
        app_version=row.app_version,
        ts_ms=row.ts_ms,
        config=row.config,
        blob_b64=row.blob_b64,
    )


def _latest_config(db: Session, device_id: str) -> DeviceConfigRevision | None:
    return db.scalar(
        select(DeviceConfigRevision)
        .where(DeviceConfigRevision.device_id == device_id)
        .order_by(DeviceConfigRevision.revision.desc())
        .limit(1)
    )


def _store_config(
    db: Session,
    *,
    device_id: str,
    doc: dict,
    source: str,
    ts_ms: int,
    app_version: str | None = None,
    phone_id: str | None = None,
    blob_b64: str | None = None,
    force: bool = False,
) -> DeviceConfigRevision:
    rev = cloud_revision(doc)
    latest = _latest_config(db, device_id)
    if latest is not None and rev < latest.revision and not force:
        raise HTTPException(
            status_code=409,
            detail=f"stale revision {rev} < latest {latest.revision}",
        )
    existing = db.scalar(
        select(DeviceConfigRevision.id).where(
            DeviceConfigRevision.device_id == device_id,
            DeviceConfigRevision.revision == rev,
        )
    )
    if existing is not None:
        row = db.get(DeviceConfigRevision, existing)
        if row is None:
            raise HTTPException(status_code=500, detail="config row missing")
        return row

    row = DeviceConfigRevision(
        device_id=device_id,
        revision=rev,
        local_revision=int(doc.get("local_revision") or 0),
        source=source,
        app_version=app_version,
        phone_id=phone_id,
        ts_ms=ts_ms,
        config_json=json.dumps(doc),
        blob_b64=blob_b64 or doc.get("blob_b64"),
    )
    db.add(row)
    device = db.get(Device, device_id)
    if device is None:
        device = Device(device_id=device_id)
        db.add(device)
    device.last_seen_ms = ts_ms
    device.last_phone_id = phone_id or device.last_phone_id
    return row


@router.get("/devices/{device_id}/config", response_model=DeviceConfigOut)
def get_device_config(
    device_id: str,
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> DeviceConfigOut:
    row = _latest_config(db, device_id)
    if row is None:
        raise HTTPException(status_code=404, detail="no config stored")
    return _config_out(row)


@router.get("/devices/{device_id}/config/history", response_model=list[DeviceConfigOut])
def list_device_config_history(
    device_id: str,
    limit: int = Query(default=20, ge=1, le=100),
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> list[DeviceConfigOut]:
    rows = db.scalars(
        select(DeviceConfigRevision)
        .where(DeviceConfigRevision.device_id == device_id)
        .order_by(DeviceConfigRevision.revision.desc())
        .limit(limit)
    ).all()
    return [_config_out(r) for r in rows]


@router.put("/devices/{device_id}/config", response_model=DeviceConfigOut)
def put_device_config(
    device_id: str,
    body: DeviceConfigPut,
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> DeviceConfigOut:
    doc = dict(body.config)
    doc["schema"] = "device.config.v1"
    doc["revision"] = body.revision
    doc["source"] = body.source
    if body.app_version:
        doc["app_version"] = body.app_version

    blob_b64 = body.blob_b64
    if blob_b64 is None and doc:
        latest = _latest_config(db, device_id)
        base = None
        if latest and latest.blob_b64:
            import base64

            base = base64.b64decode(latest.blob_b64)
        blob = json_to_blob(doc, base)
        import base64

        blob_b64 = base64.b64encode(blob).decode("ascii")
        doc["blob_b64"] = blob_b64

    row = _store_config(
        db,
        device_id=device_id,
        doc=doc,
        source=body.source,
        ts_ms=int(time.time() * 1000),
        app_version=body.app_version,
        blob_b64=blob_b64,
        force=body.force,
    )
    db.commit()
    db.refresh(row)
    return _config_out(row)


@router.post("/ingest/config", response_model=IngestResult)
def ingest_config(
    body: ConfigIngestEnvelope,
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> IngestResult:
    if body.schema != "imu.ingest.v1":
        raise HTTPException(status_code=400, detail="unsupported schema")

    accepted = 0
    duplicates = 0
    for rec in body.records:
        if rec.type != "config":
            continue
        doc = dict(rec.config)
        doc["schema"] = "device.config.v1"
        doc["revision"] = rec.revision
        doc["local_revision"] = rec.local_revision
        doc["source"] = rec.source
        if rec.app_version:
            doc["app_version"] = rec.app_version
        if rec.blob_b64:
            doc["blob_b64"] = rec.blob_b64
        try:
            _store_config(
                db,
                device_id=body.device_id,
                doc=doc,
                source=rec.source,
                ts_ms=rec.ts_ms,
                app_version=rec.app_version,
                phone_id=body.phone_id,
                blob_b64=rec.blob_b64,
            )
            accepted += 1
        except HTTPException as exc:
            if exc.status_code == 409:
                duplicates += 1
            else:
                raise

    device = db.get(Device, body.device_id)
    if device is None:
        device = Device(device_id=body.device_id)
        db.add(device)
    device.group_id = body.group_id or device.group_id
    device.last_seen_ms = body.sent_at_ms
    device.last_phone_id = body.phone_id
    db.commit()
    return IngestResult(accepted=accepted, duplicates=duplicates, device_id=body.device_id)


# --- Machines / Sensors / Reference profiles (profiles.txt #2/#3) ---------


def _machine_out(row: Machine, sensor_count: int) -> MachineOut:
    return MachineOut(
        machine_key=row.machine_key,
        name=row.name,
        kind=row.kind,
        notes=row.notes,
        created_ms=row.created_ms,
        sensor_count=sensor_count,
    )


@router.post("/machines", response_model=MachineOut, status_code=201)
def create_machine(
    body: MachineCreate,
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> MachineOut:
    existing = db.scalar(select(Machine).where(Machine.machine_key == body.machine_key))
    if existing is not None:
        raise HTTPException(status_code=409, detail="machine_key already exists")
    row = Machine(
        machine_key=body.machine_key,
        name=body.name,
        kind=body.kind,
        notes=body.notes,
        created_ms=int(time.time() * 1000),
    )
    db.add(row)
    db.commit()
    db.refresh(row)
    return _machine_out(row, 0)


@router.get("/machines", response_model=list[MachineOut])
def list_machines(
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> list[MachineOut]:
    rows = db.scalars(select(Machine).order_by(Machine.created_ms.desc())).all()
    out: list[MachineOut] = []
    for row in rows:
        count = db.scalar(
            select(func.count()).select_from(Sensor).where(Sensor.machine_id == row.id)
        ) or 0
        out.append(_machine_out(row, int(count)))
    return out


def _get_machine_or_404(db: Session, machine_key: str) -> Machine:
    row = db.scalar(select(Machine).where(Machine.machine_key == machine_key))
    if row is None:
        raise HTTPException(status_code=404, detail="machine not found")
    return row


@router.get("/machines/{machine_key}", response_model=MachineOut)
def get_machine(
    machine_key: str,
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> MachineOut:
    row = _get_machine_or_404(db, machine_key)
    count = db.scalar(
        select(func.count()).select_from(Sensor).where(Sensor.machine_id == row.id)
    ) or 0
    return _machine_out(row, int(count))


def _sensor_out(row: Sensor, machine_key: str) -> SensorOut:
    return SensorOut(
        device_id=row.device_id,
        machine_key=machine_key,
        label=row.label,
        mount_note=row.mount_note,
        created_ms=row.created_ms,
    )


@router.post("/machines/{machine_key}/sensors", response_model=SensorOut, status_code=201)
def add_sensor(
    machine_key: str,
    body: SensorCreate,
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> SensorOut:
    machine = _get_machine_or_404(db, machine_key)
    existing = db.scalar(select(Sensor).where(Sensor.device_id == body.device_id))
    if existing is not None:
        raise HTTPException(status_code=409, detail="device_id already assigned to a sensor")
    row = Sensor(
        machine_id=machine.id,
        device_id=body.device_id,
        label=body.label,
        mount_note=body.mount_note,
        created_ms=int(time.time() * 1000),
    )
    db.add(row)
    device = db.get(Device, body.device_id)
    if device is None:
        db.add(Device(device_id=body.device_id))
    db.commit()
    db.refresh(row)
    return _sensor_out(row, machine_key)


@router.get("/machines/{machine_key}/sensors", response_model=list[SensorOut])
def list_sensors(
    machine_key: str,
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> list[SensorOut]:
    machine = _get_machine_or_404(db, machine_key)
    rows = db.scalars(
        select(Sensor).where(Sensor.machine_id == machine.id).order_by(Sensor.created_ms)
    ).all()
    return [_sensor_out(r, machine_key) for r in rows]


def _ref_profile_out(row: ReferenceProfile) -> ReferenceProfileOut:
    return ReferenceProfileOut(
        device_id=row.device_id,
        slot=row.slot,
        name=row.name,
        created_ms=row.created_ms,
        updated_ms=row.updated_ms,
        duration_ms=row.duration_ms,
        sample_hz=row.sample_hz,
        format=row.format,
        bands=row.bands,
        raw=row.raw,
        active=row.active,
    )


@router.put(
    "/devices/{device_id}/reference_profiles/{slot}",
    response_model=ReferenceProfileOut,
)
def put_reference_profile(
    device_id: str,
    slot: int,
    body: ReferenceProfilePut,
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> ReferenceProfileOut:
    if slot < 0 or slot > 4:
        raise HTTPException(status_code=400, detail="slot must be 0..4 (max 5 profiles)")
    now_ms = int(time.time() * 1000)
    row = db.scalar(
        select(ReferenceProfile).where(
            ReferenceProfile.device_id == device_id,
            ReferenceProfile.slot == slot,
        )
    )
    if row is None:
        row = ReferenceProfile(device_id=device_id, slot=slot, created_ms=now_ms)
        db.add(row)
    row.name = body.name
    row.updated_ms = now_ms
    row.duration_ms = body.duration_ms
    row.sample_hz = body.sample_hz
    row.format = body.format
    row.bands_json = json.dumps(body.bands) if body.bands is not None else None
    row.raw_json = json.dumps(body.raw) if body.raw is not None else None
    row.active = body.active

    device = db.get(Device, device_id)
    if device is None:
        db.add(Device(device_id=device_id))

    db.commit()
    db.refresh(row)
    return _ref_profile_out(row)


@router.get(
    "/devices/{device_id}/reference_profiles",
    response_model=list[ReferenceProfileOut],
)
def list_reference_profiles(
    device_id: str,
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> list[ReferenceProfileOut]:
    rows = db.scalars(
        select(ReferenceProfile)
        .where(ReferenceProfile.device_id == device_id)
        .order_by(ReferenceProfile.slot)
    ).all()
    return [_ref_profile_out(r) for r in rows]


@router.delete("/devices/{device_id}/reference_profiles/{slot}", status_code=204)
def delete_reference_profile(
    device_id: str,
    slot: int,
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> None:
    row = db.scalar(
        select(ReferenceProfile).where(
            ReferenceProfile.device_id == device_id,
            ReferenceProfile.slot == slot,
        )
    )
    if row is None:
        raise HTTPException(status_code=404, detail="reference profile not found")
    db.delete(row)
    db.commit()


# --- Trend / monotonic early-warning (profiles.txt #2) ---------------------


@router.get("/devices/{device_id}/trend", response_model=TrendOut)
def device_trend(
    device_id: str,
    metric: str = Query(default="edge_score", pattern="^(edge_score|rms_delta|band_delta_max)$"),
    samples: int = Query(default=30, ge=5, le=200),
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> TrendOut:
    raw = list(
        db.scalars(
            select(Verdict)
            .where(Verdict.device_id == device_id)
            .order_by(Verdict.ts_ms.desc())
            .limit(max(samples * 4, 80))
        ).all()
    )
    raw.reverse()
    machine = db.scalar(select(Sensor).where(Sensor.device_id == device_id))
    intervals = load_repair_intervals(
        db, device_id, machine.machine_id if machine is not None else None
    )
    now_ms = int(time.time() * 1000)
    rows = filter_outside_repairs(raw, intervals, now_ms)
    if len(rows) > samples:
        rows = rows[-samples:]

    points: list[TrendPoint] = []
    for row in rows:
        if metric == "rms_delta":
            val = row.rms_delta
        elif metric == "band_delta_max":
            val = _edge_from_raw(row.raw_json).get("band_delta_max")
        else:
            val = _edge_from_raw(row.raw_json).get("edge_score")
        if val is not None:
            points.append(TrendPoint(ts_ms=row.ts_ms, value=float(val)))

    values = [p.value for p in points]
    latest_high = bool(rows) and rows[-1].level >= 1
    result = score_trend(values, latest_high=latest_high)

    return TrendOut(
        device_id=device_id,
        metric=metric,
        samples=len(values),
        trend=result.trend,
        score=result.score,
        early_warning=result.early_warning,
        latest_value=values[-1] if values else None,
        points=points,
        excluded_repair_intervals=len(intervals),
    )


def _repair_out(row: RepairEvent, machine_key: str) -> RepairOut:
    return RepairOut(
        id=row.id,
        machine_key=machine_key,
        device_ids=row.device_ids,
        started_ms=row.started_ms,
        ended_ms=row.ended_ms,
        fta_leaf=row.fta_leaf,
        parts_changed=row.parts_changed,
        notes=row.notes,
        new_ref_required=bool(row.new_ref_required),
    )


@router.post("/machines/{machine_key}/repairs", response_model=RepairOut)
def post_machine_repair(
    machine_key: str,
    body: RepairAction,
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> RepairOut:
    machine = _get_machine_or_404(db, machine_key)
    now_ms = int(time.time() * 1000)
    if body.action == "start":
        open_row = db.scalar(
            select(RepairEvent)
            .where(RepairEvent.machine_id == machine.id, RepairEvent.ended_ms.is_(None))
            .order_by(RepairEvent.started_ms.desc())
        )
        if open_row is not None:
            return _repair_out(open_row, machine_key)
        device_ids = body.device_ids
        if not device_ids:
            device_ids = list(
                db.scalars(select(Sensor.device_id).where(Sensor.machine_id == machine.id)).all()
            )
        row = RepairEvent(
            machine_id=machine.id,
            device_ids_json=json.dumps(device_ids),
            started_ms=now_ms,
            ended_ms=None,
            notes=body.notes,
            new_ref_required=1,
        )
        db.add(row)
        db.commit()
        db.refresh(row)
        return _repair_out(row, machine_key)

    open_row = db.scalar(
        select(RepairEvent)
        .where(RepairEvent.machine_id == machine.id, RepairEvent.ended_ms.is_(None))
        .order_by(RepairEvent.started_ms.desc())
    )
    if open_row is None:
        raise HTTPException(status_code=404, detail="no open repair")
    leaf = body.fta_leaf
    require_ref = (
        body.new_ref_required
        if body.new_ref_required is not None
        else default_new_ref_required(leaf)
    )
    open_row.ended_ms = now_ms
    open_row.fta_leaf = leaf
    open_row.parts_changed = body.parts
    open_row.notes = body.notes if body.notes is not None else open_row.notes
    open_row.new_ref_required = 1 if require_ref else 0
    if body.device_ids:
        open_row.device_ids_json = json.dumps(body.device_ids)
    db.commit()
    db.refresh(open_row)
    for did in open_row.device_ids:
        refresh_after_ingest(db, did)
    db.commit()
    return _repair_out(open_row, machine_key)


@router.get("/machines/{machine_key}/repairs", response_model=list[RepairOut])
def list_machine_repairs(
    machine_key: str,
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> list[RepairOut]:
    machine = _get_machine_or_404(db, machine_key)
    rows = db.scalars(
        select(RepairEvent)
        .where(RepairEvent.machine_id == machine.id)
        .order_by(RepairEvent.started_ms.desc())
        .limit(50)
    ).all()
    return [_repair_out(r, machine_key) for r in rows]


@router.get("/devices/{device_id}/operator_status", response_model=OperatorStatusOut)
def device_operator_status(
    device_id: str,
    samples: int = Query(default=30, ge=5, le=200),
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> OperatorStatusOut:
    payload = compute_device_operator_status(db, device_id, samples=samples, persist=True)
    db.commit()
    return OperatorStatusOut.model_validate(payload)


@router.get("/machines/{machine_key}/operator_status", response_model=OperatorStatusOut)
def machine_operator_status(
    machine_key: str,
    samples: int = Query(default=30, ge=5, le=200),
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> OperatorStatusOut:
    machine = _get_machine_or_404(db, machine_key)
    payload = compute_machine_operator_status(db, machine, samples=samples)
    db.commit()
    return OperatorStatusOut.model_validate(payload)


@router.get("/fta/templates/{kind}")
def get_fta_template(
    kind: str,
    _: None = Depends(require_api_key),
) -> dict:
    if kind not in ("pump", "generic"):
        raise HTTPException(status_code=404, detail="unknown FTA template")
    from app.fta_map import load_template

    tmpl = load_template(kind)
    return {**tmpl, "leaves": mechanic_leaves(kind)}


@router.get("/ai/suggest", response_model=AiSuggestOut)
async def ai_suggest(
    board: str = Query(..., min_length=3, max_length=16),
    device_id: str = Query(..., min_length=1, max_length=64),
    machine_key: str | None = Query(default=None, max_length=64),
    tz_offset_min: int = Query(default=0, ge=-840, le=840),
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> AiSuggestOut:
    """Generate a short human-readable suggestion via local Ollama (artc0)."""
    from starlette.concurrency import run_in_threadpool

    from app.ai_suggest import (
        build_body_context,
        build_vibro_context,
        complete_from_context,
    )

    board_n = board.strip().lower()
    if board_n not in ("body", "vibro"):
        return AiSuggestOut(
            ok=False,
            board=board_n,
            device_id=device_id,
            machine_key=machine_key,
            suggestion="AI suggests is only available on Body and Vibro.",
            generated_at_ms=int(time.time() * 1000),
            error="unsupported_board",
        )
    if board_n == "body":
        ctx = build_body_context(db, device_id, tz_offset_min)
    else:
        ctx = build_vibro_context(db, device_id, machine_key, tz_offset_min)
    return await run_in_threadpool(
        complete_from_context,
        board=board_n,
        device_id=device_id,
        machine_key=machine_key or None,
        ctx=ctx,
    )
