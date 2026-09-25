"""Create vs cloud-receive export for offline gap correlation."""

from __future__ import annotations

import csv
import io
from datetime import datetime, timezone

from sqlalchemy import or_, select
from sqlalchemy.orm import Session

from app.models import CloudIngestBatch, DeviceTelemetry, Verdict, WearableSample

EXPORT_FIELDS = (
    "device_kind",
    "device_id",
    "phone_id",
    "source",
    "kind",
    "created_ms",
    "created_at",
    "delivered_ms",
    "delivered_at",
    "lag_ms",
    "bytes",
    "rssi_dbm",
)


def _iso(ms: int | None) -> str:
    if not ms:
        return ""
    return datetime.fromtimestamp(ms / 1000.0, tz=timezone.utc).isoformat(timespec="milliseconds")


def _row(
    *,
    device_kind: str,
    device_id: str,
    phone_id: str | None,
    source: str,
    kind: str,
    created_ms: int | None,
    delivered_ms: int | None,
    nbytes: int | None,
    rssi: float | None,
) -> dict:
    lag = ""
    if created_ms and delivered_ms:
        lag = int(delivered_ms) - int(created_ms)
    return {
        "device_kind": device_kind,
        "device_id": device_id or "",
        "phone_id": phone_id or "",
        "source": source,
        "kind": kind,
        "created_ms": created_ms or "",
        "created_at": _iso(created_ms),
        "delivered_ms": delivered_ms or "",
        "delivered_at": _iso(delivered_ms),
        "lag_ms": lag,
        "bytes": nbytes if nbytes is not None else "",
        "rssi_dbm": rssi if rssi is not None else "",
    }


def _window(created_col, recv_col, from_ms: int, to_ms: int):
    return or_(created_col.between(from_ms, to_ms), recv_col.between(from_ms, to_ms))


def collect_delivery_rows(
    db: Session,
    device_id: str,
    from_ms: int,
    to_ms: int,
    limit: int,
) -> list[dict]:
    out: list[dict] = []
    remaining = max(1, limit)

    wear = db.scalars(
        select(WearableSample)
        .where(
            WearableSample.device_id == device_id,
            _window(WearableSample.ts_ms, WearableSample.recv_ms, from_ms, to_ms),
        )
        .order_by(WearableSample.ts_ms.asc())
        .limit(remaining)
    ).all()
    for row in wear:
        rssi = row.value if row.kind.startswith("rssi") else None
        out.append(
            _row(
                device_kind=row.source or "wearable",
                device_id=row.device_id,
                phone_id=row.phone_id,
                source=row.source or "",
                kind=row.kind,
                created_ms=row.ts_ms,
                delivered_ms=row.recv_ms,
                nbytes=row.payload_bytes,
                rssi=rssi,
            )
        )
    remaining = max(0, limit - len(out))
    if remaining <= 0:
        out.sort(key=lambda r: (int(r["created_ms"] or 0), str(r["kind"])))
        return out[:limit]

    for model, kind, source, device_kind in (
        (DeviceTelemetry, "telemetry", "esp32", "esp32"),
        (Verdict, "verdict", "esp32", "esp32"),
        (CloudIngestBatch, "ingest_batch", "phone", "phone"),
    ):
        if remaining <= 0:
            break
        rows = db.scalars(
            select(model)
            .where(
                model.device_id == device_id,
                _window(model.ts_ms, model.recv_ms, from_ms, to_ms),
            )
            .order_by(model.ts_ms.asc())
            .limit(remaining)
        ).all()
        for row in rows:
            row_kind = getattr(row, "kind", kind) if kind == "ingest_batch" else kind
            nbytes = getattr(row, "bytes", None) if kind == "ingest_batch" else row.payload_bytes
            out.append(
                _row(
                    device_kind=device_kind,
                    device_id=row.device_id,
                    phone_id=row.phone_id,
                    source=source,
                    kind=str(row_kind),
                    created_ms=row.ts_ms,
                    delivered_ms=row.recv_ms,
                    nbytes=nbytes,
                    rssi=None,
                )
            )
        remaining = max(0, limit - len(out))

    out.sort(key=lambda r: (int(r["created_ms"] or 0), str(r["kind"])))
    return out[:limit]


def render_table(rows: list[dict], *, tsv: bool = False) -> str:
    buf = io.StringIO()
    writer = csv.DictWriter(
        buf,
        fieldnames=EXPORT_FIELDS,
        extrasaction="ignore",
        dialect="excel-tab" if tsv else "excel",
        lineterminator="\n",
    )
    writer.writeheader()
    writer.writerows(rows)
    return buf.getvalue()
