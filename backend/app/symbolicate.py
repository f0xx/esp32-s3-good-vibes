"""Best-effort addr2line symbolication for Zephyr crash backtraces."""

from __future__ import annotations

import os
import re
import shutil
import subprocess
from typing import Any


def _elf_candidates() -> list[str]:
    paths: list[str] = []
    env = os.getenv("ZEPHYR_ELF_PATH", "").strip()
    if env:
        paths.append(env)
    paths.extend(
        [
            "/opt/imu/zephyr.elf",
            os.path.expanduser("~/zephyrproject/zephyr/build/zephyr/zephyr.elf"),
        ],
    )
    out: list[str] = []
    for p in paths:
        if p and os.path.isfile(p) and p not in out:
            out.append(p)
    return out


def _resolve_elf() -> str | None:
    for path in _elf_candidates():
        return path
    return None


def _addr2line(elf: str, pc: int) -> str | None:
    env_tool = os.getenv("ADDR2LINE", "").strip()
    tool = env_tool or shutil.which("xtensa-espressif_esp32s3_zephyr-elf-addr2line") or shutil.which("addr2line")
    if tool is None:
        return None
    try:
        proc = subprocess.run(
            [tool, "-e", elf, "-f", "-C", f"0x{pc & 0xFFFFFFFF:08x}"],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if proc.returncode != 0:
        return None
    lines = [ln.strip() for ln in proc.stdout.splitlines() if ln.strip()]
    if len(lines) >= 2:
        return f"{lines[0]} at {lines[1]}"
    if lines:
        return lines[0]
    return None


def symbolicate_backtrace(backtrace: list[int]) -> list[dict[str, Any]]:
    elf = _resolve_elf()
    if elf is None or not backtrace:
        return []
    frames: list[dict[str, Any]] = []
    for pc in backtrace[:16]:
        sym = _addr2line(elf, pc)
        frames.append({"pc": pc, "symbol": sym or f"0x{pc & 0xFFFFFFFF:08x}"})
    return frames


def enrich_crash_detail(detail: dict | None, backtrace: list[int]) -> dict:
    out = dict(detail or {})
    if backtrace and "frames" not in out:
        frames = symbolicate_backtrace(backtrace)
        if frames:
            out["frames"] = frames
            out["elf"] = os.path.basename(_resolve_elf() or "")
    return out
