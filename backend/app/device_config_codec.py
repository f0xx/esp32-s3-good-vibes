"""DeviceConfigV1 blob ↔ JSON (logical view). Matches Android DeviceConfigJson.kt."""

from __future__ import annotations

import base64
import struct
import time
from typing import Any

BLOB_SIZE = 188
MAGIC = 0x31494D55
LOCAL_REV_FLAG = 0x80000000
LOCAL_TFT_OFF = 0x01


def _u32_le(data: bytes, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]


def _u16_le(data: bytes, off: int) -> int:
    return struct.unpack_from("<H", data, off)[0]


def blob_to_json(blob: bytes, *, source: str = "esp", app_version: str | None = None) -> dict[str, Any]:
    if len(blob) < BLOB_SIZE:
        raise ValueError(f"blob too short: {len(blob)} < {BLOB_SIZE}")
    magic = _u32_le(blob, 0)
    if magic != MAGIC:
        raise ValueError(f"bad magic 0x{magic:08x}")

    revision = _u32_le(blob, 161)
    local_revision = _u32_le(blob, 175)
    local_flags = blob[179]

    out: dict[str, Any] = {
        "schema": "device.config.v1",
        "revision": revision,
        "local_revision": local_revision,
        "source": source,
        "profile": {
            "power_profile": blob[148],
            "tft_policy": blob[149],
            "wake_interval_sec": _u16_le(blob, 150),
            "active_window_sec": blob[152],
            "cpu_mhz": blob[153],
            "imu_sample_hz": blob[154],
            "deep_sleep_enable": blob[156],
            "ble_poll_ms": _u16_le(blob, 136),
            "ble_default_mode": blob[138],
        },
        "vibro": {
            "schedule_mode": blob[165],
            "interval_sec": blob[166],
            "window_sec": blob[167],
            "jitter_sec": blob[168],
            "capture_tier": blob[169],
            "wifi_upload_enable": blob[170],
        },
        "mix": {
            "every": blob[171],
            "ratio": blob[172],
            "dyn_short": blob[173],
            "dyn_nested": blob[174],
        },
        "local": {
            "tft_user_off": bool(local_flags & LOCAL_TFT_OFF),
        },
        "blob_b64": base64.b64encode(blob[:BLOB_SIZE]).decode("ascii"),
    }
    if app_version:
        out["app_version"] = app_version
    return out


def json_to_blob(doc: dict[str, Any], base_blob: bytes | None = None) -> bytes:
    """Merge JSON fields into blob; bumps revision if doc.revision > current."""
    if base_blob is not None and len(base_blob) >= BLOB_SIZE:
        blob = bytearray(base_blob[:BLOB_SIZE])
    else:
        blob = bytearray(BLOB_SIZE)
        struct.pack_into("<I", blob, 0, MAGIC)
        struct.pack_into("<H", blob, 4, 1)
        struct.pack_into("<H", blob, 6, BLOB_SIZE)

    profile = doc.get("profile") or {}
    vibro = doc.get("vibro") or {}
    mix = doc.get("mix") or {}

    if "ble_poll_ms" in profile:
        struct.pack_into("<H", blob, 136, int(profile["ble_poll_ms"]))
    if "ble_default_mode" in profile:
        blob[138] = int(profile["ble_default_mode"]) & 0xFF
    if "power_profile" in profile:
        blob[148] = int(profile["power_profile"]) & 0xFF
    if "tft_policy" in profile:
        blob[149] = int(profile["tft_policy"]) & 0xFF
    if "wake_interval_sec" in profile:
        struct.pack_into("<H", blob, 150, int(profile["wake_interval_sec"]))
    if "active_window_sec" in profile:
        blob[152] = int(profile["active_window_sec"]) & 0xFF
    if "cpu_mhz" in profile:
        blob[153] = int(profile["cpu_mhz"]) & 0xFF
    if "imu_sample_hz" in profile:
        blob[154] = int(profile["imu_sample_hz"]) & 0xFF
    if "deep_sleep_enable" in profile:
        blob[156] = int(profile["deep_sleep_enable"]) & 0xFF

    if "schedule_mode" in vibro:
        blob[165] = int(vibro["schedule_mode"]) & 0xFF
    if "interval_sec" in vibro:
        blob[166] = int(vibro["interval_sec"]) & 0xFF
    if "window_sec" in vibro:
        blob[167] = int(vibro["window_sec"]) & 0xFF
    if "jitter_sec" in vibro:
        blob[168] = int(vibro["jitter_sec"]) & 0xFF
    if "capture_tier" in vibro:
        blob[169] = int(vibro["capture_tier"]) & 0xFF
    if "wifi_upload_enable" in vibro:
        blob[170] = int(vibro["wifi_upload_enable"]) & 0xFF

    if "every" in mix:
        blob[171] = int(mix["every"]) & 0xFF
    if "ratio" in mix:
        blob[172] = int(mix["ratio"]) & 0xFF
    if "dyn_short" in mix:
        blob[173] = int(mix["dyn_short"]) & 0xFF
    if "dyn_nested" in mix:
        blob[174] = int(mix["dyn_nested"]) & 0xFF

    new_rev = int(doc.get("revision") or 0)
    if new_rev <= 0:
        new_rev = int(time.time())
    struct.pack_into("<I", blob, 161, new_rev)

    return bytes(blob)


def cloud_revision(doc: dict[str, Any]) -> int:
    return int(doc.get("revision") or 0)
