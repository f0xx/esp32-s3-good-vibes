"""Backend operator page: trend minus repair gaps, proofs, FTA IMU hints."""

from __future__ import annotations

import json
import time
from typing import Any

from sqlalchemy import select
from sqlalchemy.orm import Session

from app.fta_map import load_template, map_imu_hints
from app.models import Machine, OperatorStatusCache, RepairEvent, Sensor, Spectrum, Verdict
from app.trend_score import TREND_THRESHOLD, score_trend

DEFAULT_SAMPLES = 30
PROOF_K = 5
LAST_K_WARN = 3
ALARM_SCORE = 0.6


def repair_covers(ts_ms: int, started_ms: int, ended_ms: int | None, now_ms: int) -> bool:
    end = ended_ms if ended_ms is not None else now_ms
    return started_ms <= ts_ms <= end


def device_ids_for_repair(row: RepairEvent) -> list[str]:
    ids = row.device_ids
    return ids


def load_repair_intervals(
    db: Session, device_id: str, machine_id: int | None = None
) -> list[tuple[int, int | None]]:
    q = select(RepairEvent)
    if machine_id is not None:
        q = q.where(RepairEvent.machine_id == machine_id)
    rows = db.scalars(q).all()
    out: list[tuple[int, int | None]] = []
    for row in rows:
        ids = device_ids_for_repair(row)
        if machine_id is None and device_id not in ids:
            continue
        if ids and device_id not in ids:
            continue
        out.append((int(row.started_ms), row.ended_ms))
    return out


def filter_outside_repairs(
    rows: list[Verdict], intervals: list[tuple[int, int | None]], now_ms: int
) -> list[Verdict]:
    if not intervals:
        return rows
    kept: list[Verdict] = []
    for row in rows:
        if any(repair_covers(int(row.ts_ms), start, end, now_ms) for start, end in intervals):
            continue
        kept.append(row)
    return kept


def _edge(row: Verdict) -> dict[str, Any]:
    if not row.raw_json:
        return {}
    try:
        data = json.loads(row.raw_json)
        return data if isinstance(data, dict) else {}
    except (json.JSONDecodeError, TypeError):
        return {}


def _metric_value(row: Verdict, metric: str) -> float | None:
    if metric == "rms_delta":
        return float(row.rms_delta) if row.rms_delta is not None else None
    edge = _edge(row)
    if metric == "band_delta_max":
        val = edge.get("band_delta_max")
        return float(val) if val is not None else None
    val = edge.get("edge_score")
    return float(val) if val is not None else None


def machine_for_device(db: Session, device_id: str) -> Machine | None:
    sensor = db.scalar(select(Sensor).where(Sensor.device_id == device_id))
    if sensor is None:
        return None
    return db.get(Machine, sensor.machine_id)


def compute_device_operator_status(
    db: Session,
    device_id: str,
    *,
    samples: int = DEFAULT_SAMPLES,
    metric: str = "edge_score",
    persist: bool = True,
) -> dict[str, Any]:
    now_ms = int(time.time() * 1000)
    machine = machine_for_device(db, device_id)
    intervals = load_repair_intervals(db, device_id, machine.id if machine else None)

    raw_rows = list(
        db.scalars(
            select(Verdict)
            .where(Verdict.device_id == device_id)
            .order_by(Verdict.ts_ms.desc())
            .limit(max(samples * 4, 80))
        ).all()
    )
    raw_rows.reverse()
    rows = filter_outside_repairs(raw_rows, intervals, now_ms)
    if len(rows) > samples:
        rows = rows[-samples:]

    points: list[dict[str, Any]] = []
    for row in rows:
        val = _metric_value(row, metric)
        if val is not None:
            points.append({"ts_ms": int(row.ts_ms), "value": float(val), "level": int(row.level)})

    values = [p["value"] for p in points]
    latest_high = bool(rows) and int(rows[-1].level) >= 1
    last_k = rows[-LAST_K_WARN:] if rows else []
    last_k_warn = bool(last_k) and all(int(r.level) >= 1 for r in last_k) and len(last_k) >= LAST_K_WARN
    result = score_trend(values, latest_high=latest_high)

    candidate_level = int(rows[-1].level) if rows else 0
    operator_alert = (
        result.trend == "increasing"
        and last_k_warn
        and result.score >= ALARM_SCORE
    )
    if operator_alert:
        operator_level = 2
    elif result.early_warning or (result.trend == "increasing" and latest_high):
        operator_level = 1
    else:
        operator_level = 0

    kind = machine.kind if machine is not None else "generic"
    last_edge = _edge(rows[-1]) if rows else {}
    flags = last_edge.get("edge_flags")
    if not isinstance(flags, list):
        flags = []
    fta_hints = map_imu_hints(
        edge_flags=[str(x) for x in flags],
        edge_score=last_edge.get("edge_score") if isinstance(last_edge.get("edge_score"), (int, float)) else None,
        kind=kind if kind == "pump" else "generic",
    )
    template = load_template("pump" if kind == "pump" else "generic")

    proof_rows = rows[-PROOF_K:]
    last_spectrum = db.scalar(
        select(Spectrum.id)
        .where(Spectrum.device_id == device_id)
        .order_by(Spectrum.ts_ms.desc())
        .limit(1)
    )
    proofs = {
        "window": [
            {
                "ts_ms": int(r.ts_ms),
                "seq": int(r.seq),
                "candidate_level": int(r.level),
                "rms": r.rms,
                "corr": r.corr,
                "rms_delta": r.rms_delta,
                "band_corr": _edge(r).get("band_corr"),
                "band_delta_max": _edge(r).get("band_delta_max"),
                "bands": _edge(r).get("bands"),
                "edge_flags": _edge(r).get("edge_flags"),
                "edge_score": _edge(r).get("edge_score"),
            }
            for r in proof_rows
        ],
        "last_spectrum_id": int(last_spectrum) if last_spectrum is not None else None,
        "excluded_repair_intervals": [
            {"started_ms": a, "ended_ms": b} for a, b in intervals
        ],
        "last_k_warn": last_k_warn,
        "thresholds": {
            "trend": TREND_THRESHOLD,
            "alarm_score": ALARM_SCORE,
            "last_k": LAST_K_WARN,
        },
    }

    machine_key = machine.machine_key if machine is not None else None
    payload = {
        "device_id": device_id,
        "machine_key": machine_key,
        "candidate_level": candidate_level,
        "operator_level": operator_level,
        "operator_alert": operator_alert,
        "trend": result.trend,
        "score": result.score,
        "early_warning": result.early_warning,
        "samples": len(values),
        "latest_value": values[-1] if values else None,
        "metric": metric,
        "points": points,
        "proofs": proofs,
        "fta": {
            "template_id": template.get("id"),
            "top_event": template.get("top_event"),
            "kind": template.get("kind"),
            "hints": fta_hints,
        },
        "updated_ms": now_ms,
        "repair_open": any(end is None for _, end in intervals),
    }

    if persist:
        row = db.get(OperatorStatusCache, device_id)
        if row is None:
            row = OperatorStatusCache(device_id=device_id)
            db.add(row)
        row.machine_key = machine_key
        row.operator_level = operator_level
        row.operator_alert = 1 if operator_alert else 0
        row.candidate_level = candidate_level
        row.trend = result.trend
        row.score = result.score
        row.early_warning = 1 if result.early_warning else 0
        row.proofs_json = json.dumps(proofs)
        row.fta_hints_json = json.dumps(fta_hints)
        row.updated_ms = now_ms

    return payload


def compute_machine_operator_status(
    db: Session, machine: Machine, *, samples: int = DEFAULT_SAMPLES
) -> dict[str, Any]:
    sensors = list(
        db.scalars(select(Sensor).where(Sensor.machine_id == machine.id)).all()
    )
    devices = [s.device_id for s in sensors]
    children = [compute_device_operator_status(db, did, samples=samples, persist=True) for did in devices]
    if not children:
        now_ms = int(time.time() * 1000)
        return {
            "machine_key": machine.machine_key,
            "name": machine.name,
            "kind": machine.kind,
            "operator_level": 0,
            "operator_alert": False,
            "candidate_level": 0,
            "sensors": [],
            "fta": {
                "template_id": load_template(machine.kind if machine.kind == "pump" else "generic").get("id"),
                "top_event": load_template(machine.kind if machine.kind == "pump" else "generic").get("top_event"),
                "kind": machine.kind,
                "hints": [],
            },
            "updated_ms": now_ms,
            "repair_open": False,
        }

    operator_level = max(int(c["operator_level"]) for c in children)
    operator_alert = any(bool(c["operator_alert"]) for c in children)
    candidate_level = max(int(c["candidate_level"]) for c in children)
    hints: list[dict[str, Any]] = []
    seen: set[str] = set()
    for child in children:
        for hint in child.get("fta", {}).get("hints", []):
            lid = str(hint.get("leaf_id"))
            if lid in seen:
                continue
            seen.add(lid)
            hints.append(hint)
    template = load_template("pump" if machine.kind == "pump" else "generic")
    repairs = list(
        db.scalars(
            select(RepairEvent)
            .where(RepairEvent.machine_id == machine.id)
            .order_by(RepairEvent.started_ms.desc())
            .limit(20)
        ).all()
    )
    return {
        "machine_key": machine.machine_key,
        "name": machine.name,
        "kind": machine.kind,
        "operator_level": operator_level,
        "operator_alert": operator_alert,
        "candidate_level": candidate_level,
        "sensors": children,
        "fta": {
            "template_id": template.get("id"),
            "top_event": template.get("top_event"),
            "kind": template.get("kind"),
            "hints": hints,
        },
        "repairs": [
            {
                "id": r.id,
                "device_ids": r.device_ids,
                "started_ms": r.started_ms,
                "ended_ms": r.ended_ms,
                "fta_leaf": r.fta_leaf,
                "parts_changed": r.parts_changed,
                "notes": r.notes,
                "new_ref_required": bool(r.new_ref_required),
            }
            for r in repairs
        ],
        "updated_ms": int(time.time() * 1000),
        "repair_open": any(r.ended_ms is None for r in repairs),
    }


def refresh_after_ingest(db: Session, device_id: str) -> None:
    compute_device_operator_status(db, device_id, persist=True)
    machine = machine_for_device(db, device_id)
    if machine is not None:
        for sensor in db.scalars(select(Sensor).where(Sensor.machine_id == machine.id)).all():
            if sensor.device_id != device_id:
                compute_device_operator_status(db, sensor.device_id, persist=True)
