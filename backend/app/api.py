"""REST API routes — mounted at /v1 and /app/good_vibes/v1."""

import json
import os
import time

from fastapi import APIRouter, Depends, HTTPException, Query
from sqlalchemy import func, select, text
from sqlalchemy.orm import Session

from app.auth import require_api_key
from app.db import Base, engine, get_db
from app.models import Crash, Device, DeviceConfigRevision, Machine, ReferenceProfile, Sensor, Spectrum, Verdict
from app.edge_score import score_edge_features
from app.trend_score import score_trend
from app.symbolicate import enrich_crash_detail
from app.device_config_codec import blob_to_json, cloud_revision, json_to_blob
from app.schemas import (
    CrashIngestEnvelope,
    CrashOut,
    ConfigIngestEnvelope,
    DeviceConfigOut,
    DeviceConfigPut,
    DeviceOut,
    GroupDeviceVerdict,
    GroupStatusOut,
    HealthOut,
    IngestEnvelope,
    IngestResult,
    MachineCreate,
    MachineOut,
    ReferenceProfileOut,
    ReferenceProfilePut,
    SensorCreate,
    SensorOut,
    SpectrumIngestEnvelope,
    SpectrumOut,
    TrendOut,
    TrendPoint,
    VerdictOut,
)

router = APIRouter()


def _crash_out(row: Crash) -> CrashOut:
    detail = None
    if row.detail_json:
        try:
            detail = json.loads(row.detail_json)
        except (json.JSONDecodeError, TypeError):
            detail = None
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
        uptime_ms=row.uptime_ms,
        backtrace=row.backtrace,
        detail=detail,
    )


def ensure_db() -> None:
    Base.metadata.create_all(bind=engine)
    if os.getenv("DATABASE_URL", "").startswith("postgres"):
        try:
            with engine.begin() as conn:
                conn.execute(text("CREATE EXTENSION IF NOT EXISTS timescaledb"))
                for table in ("verdicts", "spectra", "crashes"):
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


@router.get("/health", response_model=HealthOut)
def health() -> HealthOut:
    db_kind = "postgres" if os.getenv("DATABASE_URL", "").startswith("postgres") else "sqlite"
    return HealthOut(ok=True, db=db_kind)


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
                raw_json=json.dumps(edge) if edge else None,
            )
        )
        accepted += 1

    db.commit()
    return IngestResult(accepted=accepted, duplicates=duplicates, device_id=body.device_id)


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
    for rec in body.records:
        if rec.type != "crash":
            continue
        pc_val = rec.pc if rec.pc is not None else 0
        exists = db.scalar(
            select(Crash.id).where(
                Crash.device_id == body.device_id,
                Crash.seq == rec.seq,
                func.coalesce(Crash.pc, 0) == pc_val,
            )
        )
        if exists is not None:
            duplicates += 1
            continue
        detail = enrich_crash_detail(rec.detail, rec.backtrace)
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
                uptime_ms=rec.uptime_ms,
                backtrace_json=json.dumps(rec.backtrace),
                detail_json=json.dumps(detail) if detail else None,
            )
        )
        accepted += 1

    db.commit()
    return IngestResult(accepted=accepted, duplicates=duplicates, device_id=body.device_id)


@router.get("/devices", response_model=list[DeviceOut])
def list_devices(
    group_id: str | None = Query(default=None),
    _: None = Depends(require_api_key),
    db: Session = Depends(get_db),
) -> list[DeviceOut]:
    stmt = select(Device)
    if group_id:
        stmt = stmt.where(Device.group_id == group_id)
    devices = db.scalars(stmt.order_by(Device.last_seen_ms.desc())).all()
    out: list[DeviceOut] = []
    for dev in devices:
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
                verdict_count=int(verdict_count),
                crash_count=int(crash_count),
                latest_level=latest.level if latest else None,
                latest_rms=latest.rms if latest else None,
                latest_crash_ts=latest_crash.ts_ms if latest_crash else None,
            )
        )
    return out


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
    return [_crash_out(r) for r in rows]


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
    return [_crash_out(r) for r in rows]


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
    now_ms = int(time.time() * 1000)
    for device in devices:
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
    rows = db.scalars(
        select(Verdict)
        .where(Verdict.device_id == device_id)
        .order_by(Verdict.ts_ms.desc())
        .limit(samples)
    ).all()
    rows = list(reversed(rows))  # oldest -> newest for trend direction

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
    )
