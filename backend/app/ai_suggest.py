"""Ollama-backed AI suggestions for Body / Vibro boards."""

from __future__ import annotations

import json
import os
import urllib.error
import urllib.request
from datetime import datetime, timedelta, timezone
from typing import Any

from sqlalchemy import select
from sqlalchemy.orm import Session

from app.models import Machine, Verdict, WearableSample
from app.operator_status import (
    compute_device_operator_status,
    compute_machine_operator_status,
)
from app.schemas import AiSuggestOut, WearableKindLatest, WearableLatestOut

OLLAMA_URL = os.getenv("OLLAMA_URL", "http://host.docker.internal:11434").rstrip("/")
OLLAMA_MODEL = os.getenv("OLLAMA_MODEL", "llama3.2")
OLLAMA_TIMEOUT_S = float(os.getenv("OLLAMA_TIMEOUT_S", "90"))

_BODY_SYSTEM = """You are a concise health coach for a wearable dashboard (Good Vibes Body).
Write ONE short human-readable suggestion (1-2 sentences, max ~40 words).
Base it only on the JSON context. Tone: friendly, direct, actionable.
Examples of style (do not copy): "you did a great workout on Tue 15th of September, why did you stop?"
"heart rate was looking high today — how do you feel?" "fever-ish skin temp; drink water and rest."
No markdown, no bullet lists, no preamble like "Based on the data"."""

_VIBRO_SYSTEM = """You are a concise vibration / machine-health analyst for Good Vibes Vibro.
Write ONE short human-readable suggestion (1-2 sentences, max ~40 words).
Base it only on the JSON context. Tone: calm operator radio.
Examples of style (do not copy): "the sensor is working but no samples arrived — check BLE/power."
"all looks good; forecast for the upcoming week is positive."
"slightly bad vibration signature — keep eyes on the machine."
"machine X is under damage — check its history!"
No markdown, no bullet lists, no preamble."""


def _fmt_ts(ms: int | None, tz: timezone) -> str:
    if not ms:
        return "unknown"
    return datetime.fromtimestamp(ms / 1000.0, tz=tz).strftime("%a %d %b %Y %H:%M")


def _latest_from_db(db: Session, device_id: str) -> WearableLatestOut | None:
    rows = db.scalars(
        select(WearableSample)
        .where(WearableSample.device_id == device_id)
        .order_by(WearableSample.ts_ms.desc())
        .limit(400)
    ).all()
    if not rows:
        return None
    kinds: dict[str, WearableKindLatest] = {}
    recv_ms = 0
    for row in rows:
        recv_ms = max(recv_ms, int(row.ts_ms or 0))
        if row.kind in kinds:
            continue
        kinds[row.kind] = WearableKindLatest(
            kind=row.kind,
            source=row.source or "mt200",
            ts_ms=int(row.ts_ms or 0),
            seq=int(row.seq or 0),
            value=row.value,
            extra=row.extra if isinstance(row.extra, dict) else None,
        )
    return WearableLatestOut(device_id=device_id, recv_ms=recv_ms, kinds=kinds)


def _daily_summary(db: Session, device_id: str, tz: timezone, days: int = 7) -> list[dict[str, Any]]:
    today = datetime.now(tz).date()
    start = today - timedelta(days=days - 1)
    since_ms = int(datetime(start.year, start.month, start.day, tzinfo=tz).timestamp() * 1000)
    rows = db.execute(
        select(WearableSample.kind, WearableSample.ts_ms, WearableSample.value).where(
            WearableSample.device_id == device_id,
            WearableSample.kind.in_(("steps", "kcal_x10", "hr", "spo2", "temp_c", "walk")),
            WearableSample.ts_ms >= since_ms,
            WearableSample.value.is_not(None),
        )
    ).all()
    by_day: dict[str, dict[str, tuple[int, float]]] = {}
    for kind, ts_ms, value in rows:
        day = datetime.fromtimestamp(ts_ms / 1000.0, tz=tz).date().isoformat()
        bucket = by_day.setdefault(day, {})
        prev = bucket.get(kind)
        if prev is None or int(ts_ms) >= prev[0]:
            bucket[kind] = (int(ts_ms), float(value))
    out: list[dict[str, Any]] = []
    for i in range(days):
        d = (start + timedelta(days=i)).isoformat()
        bucket = by_day.get(d, {})
        point: dict[str, Any] = {"day": d}
        for k, label in (
            ("steps", "steps"),
            ("kcal_x10", "kcal_x10"),
            ("hr", "hr"),
            ("spo2", "spo2"),
            ("temp_c", "temp_c"),
            ("walk", "walk"),
        ):
            if k in bucket:
                point[label] = bucket[k][1]
        if len(point) > 1:
            out.append(point)
    return out


def _recent_kind_stats(
    db: Session, device_id: str, kinds: tuple[str, ...], limit: int = 80
) -> dict[str, Any]:
    rows = db.scalars(
        select(WearableSample)
        .where(
            WearableSample.device_id == device_id,
            WearableSample.kind.in_(kinds),
            WearableSample.value.is_not(None),
        )
        .order_by(WearableSample.ts_ms.desc())
        .limit(limit)
    ).all()
    by_kind: dict[str, list[float]] = {}
    for row in rows:
        by_kind.setdefault(row.kind, []).append(float(row.value))
    stats: dict[str, Any] = {}
    for kind, vals in by_kind.items():
        if not vals:
            continue
        stats[kind] = {
            "n": len(vals),
            "min": round(min(vals), 2),
            "max": round(max(vals), 2),
            "avg": round(sum(vals) / len(vals), 2),
            "latest": vals[0],
        }
    return stats


def build_body_context(db: Session, device_id: str, tz_offset_min: int) -> dict[str, Any]:
    tz = timezone(timedelta(minutes=tz_offset_min))
    latest = _latest_from_db(db, device_id)
    kinds = {}
    if latest:
        for k, v in latest.kinds.items():
            kinds[k] = {
                "value": v.value,
                "ts": _fmt_ts(v.ts_ms, tz),
                "extra": v.extra,
            }
    return {
        "board": "body",
        "device_id": device_id,
        "now": datetime.now(tz).strftime("%a %d %b %Y %H:%M"),
        "latest_kinds": kinds or None,
        "daily_7d": _daily_summary(db, device_id, tz, 7),
        "recent_stats": _recent_kind_stats(
            db, device_id, ("hr", "spo2", "temp_c", "steps", "kcal_x10", "walk")
        ),
    }


def build_vibro_context(
    db: Session,
    device_id: str,
    machine_key: str | None,
    tz_offset_min: int,
) -> dict[str, Any]:
    tz = timezone(timedelta(minutes=tz_offset_min))
    verdicts = db.scalars(
        select(Verdict)
        .where(Verdict.device_id == device_id)
        .order_by(Verdict.ts_ms.desc())
        .limit(20)
    ).all()
    level_name = {0: "OK", 1: "WARN", 2: "ALERT"}
    recent = [
        {
            "ts": _fmt_ts(r.ts_ms, tz),
            "level": level_name.get(int(r.level or 0), str(r.level)),
            "rms": r.rms,
            "peak": r.peak,
            "corr": r.corr,
            "pct": r.pct,
        }
        for r in verdicts
    ]
    op: dict[str, Any] | None = None
    try:
        if machine_key:
            machine = db.scalar(select(Machine).where(Machine.machine_key == machine_key))
            if machine is not None:
                op = compute_machine_operator_status(db, machine, samples=30)
        if op is None:
            op = compute_device_operator_status(db, device_id, samples=30, persist=False)
    except Exception:
        op = None
    op_summary = None
    if op:
        op_summary = {
            "machine_key": op.get("machine_key"),
            "name": op.get("name"),
            "operator_level": op.get("operator_level"),
            "candidate_level": op.get("candidate_level"),
            "operator_alert": op.get("operator_alert"),
            "trend": op.get("trend"),
            "score": op.get("score"),
            "early_warning": op.get("early_warning"),
            "repair_open": op.get("repair_open"),
            "fta_top": (op.get("fta") or {}).get("top_event") if isinstance(op.get("fta"), dict) else None,
        }
    last_ts = int(verdicts[0].ts_ms) if verdicts else 0
    age_s = int((datetime.now(tz).timestamp() * 1000 - last_ts) / 1000) if last_ts else None
    return {
        "board": "vibro",
        "device_id": device_id,
        "machine_key": machine_key,
        "now": datetime.now(tz).strftime("%a %d %b %Y %H:%M"),
        "verdict_count_recent": len(recent),
        "seconds_since_last_verdict": age_s,
        "recent_verdicts": recent,
        "operator": op_summary,
    }


def _ollama_chat(system: str, user: str) -> tuple[str, str]:
    payload = {
        "model": OLLAMA_MODEL,
        "stream": False,
        "options": {"temperature": 0.6, "num_predict": 96},
        "messages": [
            {"role": "system", "content": system},
            {"role": "user", "content": user},
        ],
    }
    req = urllib.request.Request(
        f"{OLLAMA_URL}/api/chat",
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=OLLAMA_TIMEOUT_S) as resp:
        body = json.loads(resp.read().decode("utf-8"))
    msg = (body.get("message") or {}).get("content") or body.get("response") or ""
    text = " ".join(str(msg).strip().split())
    if len(text) > 320:
        text = text[:317].rstrip() + "…"
    return text, OLLAMA_MODEL


def complete_from_context(
    *,
    board: str,
    device_id: str,
    machine_key: str | None,
    ctx: dict[str, Any],
) -> AiSuggestOut:
    now_ms = int(datetime.now(timezone.utc).timestamp() * 1000)
    if board == "body":
        system = _BODY_SYSTEM
        empty = not ctx.get("latest_kinds") and not ctx.get("daily_7d")
        fallback = "No wearable data yet — pair the watch and wait for the first samples."
    else:
        system = _VIBRO_SYSTEM
        empty = not ctx.get("recent_verdicts") and not ctx.get("operator")
        fallback = "No vibration samples yet — check the sensor is powered and ingesting."

    if empty:
        return AiSuggestOut(
            ok=True,
            board=board,
            device_id=device_id,
            machine_key=machine_key,
            suggestion=fallback,
            model=OLLAMA_MODEL,
            generated_at_ms=now_ms,
        )

    user = (
        "Context JSON follows. Reply with only the suggestion text.\n"
        + json.dumps(ctx, ensure_ascii=False, default=str)
    )
    try:
        text, model = _ollama_chat(system, user)
        if not text:
            raise RuntimeError("empty model response")
        return AiSuggestOut(
            ok=True,
            board=board,
            device_id=device_id,
            machine_key=machine_key,
            suggestion=text,
            model=model,
            generated_at_ms=now_ms,
        )
    except Exception as exc:  # noqa: BLE001 — surface to UI
        err = str(exc)
        if isinstance(exc, urllib.error.URLError):
            err = f"ollama unreachable ({OLLAMA_URL})"
        return AiSuggestOut(
            ok=False,
            board=board,
            device_id=device_id,
            machine_key=machine_key,
            suggestion="AI temporarily unavailable — try again in a minute.",
            model=OLLAMA_MODEL,
            generated_at_ms=now_ms,
            error=err[:200],
        )


def generate_suggestion(
    db: Session,
    *,
    board: str,
    device_id: str,
    machine_key: str | None = None,
    tz_offset_min: int = 0,
) -> AiSuggestOut:
    now_ms = int(datetime.now(timezone.utc).timestamp() * 1000)
    board = board.strip().lower()
    if board not in ("body", "vibro"):
        return AiSuggestOut(
            ok=False,
            board=board,
            device_id=device_id,
            machine_key=machine_key,
            suggestion="AI suggests is only available on Body and Vibro.",
            model=OLLAMA_MODEL,
            generated_at_ms=now_ms,
            error="unsupported_board",
        )
    if board == "body":
        ctx = build_body_context(db, device_id, tz_offset_min)
    else:
        ctx = build_vibro_context(db, device_id, machine_key, tz_offset_min)
    return complete_from_context(
        board=board,
        device_id=device_id,
        machine_key=machine_key,
        ctx=ctx,
    )
