"""Cloud OTA artifacts — phone polls GET /v1/ota/manifest then downloads blobs."""

from __future__ import annotations

import json
import os
from pathlib import Path

from fastapi import APIRouter, Depends, HTTPException
from fastapi.responses import FileResponse

from app.auth import require_api_key

router = APIRouter()

_ARTIFACTS = {
    "apk": "app-debug.apk",
    "fw": "firmware.bin",
}


def _ota_dir() -> Path:
    raw = os.getenv("IMU_OTA_DIR", "").strip()
    if raw:
        return Path(raw)
    return Path(__file__).resolve().parent.parent / "ota" / "current"


def _manifest_path() -> Path:
    return _ota_dir() / "manifest.json"


@router.get("/ota/manifest")
def ota_manifest(_: None = Depends(require_api_key)) -> dict:
    path = _manifest_path()
    if not path.is_file():
        return {"schema": "imu.ota.v1", "available": False}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise HTTPException(status_code=500, detail=f"ota manifest unreadable: {exc}") from exc
    if not isinstance(data, dict):
        raise HTTPException(status_code=500, detail="ota manifest is not an object")
    data["available"] = bool(data.get("apk") or data.get("fw"))
    data.setdefault("schema", "imu.ota.v1")
    return data


@router.get("/ota/artifacts/{name}")
def ota_artifact(name: str, _: None = Depends(require_api_key)) -> FileResponse:
    filename = _ARTIFACTS.get(name)
    if filename is None:
        raise HTTPException(status_code=404, detail="unknown ota artifact")
    path = _ota_dir() / filename
    if not path.is_file():
        raise HTTPException(status_code=404, detail="ota artifact not published")
    media = "application/vnd.android.package-archive" if name == "apk" else "application/octet-stream"
    return FileResponse(path, media_type=media, filename=filename)
