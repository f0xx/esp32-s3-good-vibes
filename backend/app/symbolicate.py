"""Best-effort addr2line + heuristic root-cause hints for Zephyr crash reports.

Symbolication prefers a zephyr.elf that matches ``fw_version`` (desk
``handshake v257`` or cloud ``00.0001.0000.00072``) from:

1. Local cache ``IMU_FIRMWARE_ELF_DIR/<symvers>/zephyr.elf``
2. CDN mounts ``/mnt/cdn/{0,1,2}/good_vibes/v0/firmware/<symvers>/zephyr.elf``
3. HTTP fetch ``https://cdn.f0xx.org/good_vibes/v0/firmware/<urlenc>/zephyr.elf``
4. Legacy ``ZEPHYR_ELF_PATH`` / staged desk ELF (mismatch noted in detail)

Symbolication needs a non-zero PC (backtrace[] and/or lone ``pc``). Many task_wdt
rows have neither — empty PC is classified from uptime / thread / reason instead.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any


PRODUCT = os.getenv("OTA_CDN_PREFIX", "good_vibes").strip() or "good_vibes"
CDN_DIRECTOR = os.getenv("OTA_CDN_DIRECTOR_BASE", "https://cdn.f0xx.org").rstrip("/")
CDN_PUBLIC_BASES = [
    b.strip().rstrip("/")
    for b in os.getenv(
        "OTA_LOCAL_CDN_PUBLIC_BASES",
        "https://cdn0.f0xx.org,https://cdn1.f0xx.org,https://cdn2.f0xx.org",
    ).split(",")
    if b.strip()
]


def normalize_symvers(fw_version: str | None) -> str | None:
    """Filesystem / URL path segment for a firmware version string."""
    if not fw_version:
        return None
    s = fw_version.strip()
    if not s or len(s) > 96:
        return None
    if "/" in s or "\\" in s or ".." in s or s.startswith("."):
        return None
    return s


def firmware_elf_relpath(symvers: str) -> str:
    return f"{PRODUCT}/v0/firmware/{symvers}/zephyr.elf"


def firmware_elf_url(symvers: str, base: str | None = None) -> str:
    enc = urllib.parse.quote(symvers, safe="")
    root = (base or CDN_DIRECTOR).rstrip("/")
    return f"{root}/{PRODUCT}/v0/firmware/{enc}/zephyr.elf"


def _local_elf_dir() -> Path:
    return Path(
        os.getenv("IMU_FIRMWARE_ELF_DIR", "/opt/imu/firmware-elfs").strip()
        or "/opt/imu/firmware-elfs"
    )


def _cdn_roots() -> list[Path]:
    raw = os.getenv("OTA_LOCAL_CDN_ROOTS", "/mnt/cdn/0:/mnt/cdn/1:/mnt/cdn/2")
    out: list[Path] = []
    for part in raw.split(":"):
        part = part.strip()
        if part:
            out.append(Path(part))
    return out


def local_elf_path(symvers: str) -> Path:
    return _local_elf_dir() / symvers / "zephyr.elf"


def cdn_elf_paths(symvers: str) -> list[Path]:
    rel = firmware_elf_relpath(symvers)
    return [root / rel for root in _cdn_roots()]


def _is_elf_file(path: Path | str) -> bool:
    try:
        with open(path, "rb") as f:
            return f.read(4) == b"\x7fELF"
    except OSError:
        return False


def _legacy_elf_candidates() -> list[str]:
    paths: list[str] = []
    env = os.getenv("ZEPHYR_ELF_PATH", "").strip()
    if env:
        paths.append(env)
    ota_root = os.getenv("IMU_OTA_ELF_DIR", "").strip()
    if ota_root and os.path.isdir(ota_root):
        try:
            for name in sorted(os.listdir(ota_root), reverse=True):
                cand = os.path.join(ota_root, name, "zephyr.elf")
                if os.path.isfile(cand):
                    paths.append(cand)
                    break
        except OSError:
            pass
    for base in (
        "/opt/imu/ota-elfs",
        "/opt/imu",
        os.path.expanduser("~/esp32-imu-backend/deploy/stage"),
    ):
        cand = os.path.join(base, "zephyr.elf")
        if os.path.isfile(cand):
            paths.append(cand)
    paths.extend(
        [
            "/opt/imu/zephyr.elf",
            os.path.expanduser("~/zephyrproject/zephyr/build/zephyr/zephyr.elf"),
            os.path.expanduser(
                "~/zephyrproject/zephyr/build/waveshare-handshake/zephyr/zephyr.elf"
            ),
        ],
    )
    out: list[str] = []
    for p in paths:
        if p and os.path.isfile(p) and p not in out:
            out.append(p)
    return out


def _fetch_elf_http(symvers: str, dest: Path) -> bool:
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(".elf.partial")
    bases = [CDN_DIRECTOR] + [b for b in CDN_PUBLIC_BASES if b != CDN_DIRECTOR]
    for base in bases:
        url = firmware_elf_url(symvers, base)
        try:
            req = urllib.request.Request(url, method="GET")
            with urllib.request.urlopen(req, timeout=30) as resp:
                data = resp.read()
            if len(data) < 64 or data[:4] != b"\x7fELF":
                continue
            tmp.write_bytes(data)
            tmp.replace(dest)
            return True
        except (urllib.error.URLError, urllib.error.HTTPError, OSError, TimeoutError):
            continue
    if tmp.exists():
        try:
            tmp.unlink()
        except OSError:
            pass
    return False


def resolve_elf_for_version(
    fw_version: str | None,
) -> tuple[str | None, str | None, bool]:
    """Return (path, note, version_matched)."""
    sym = normalize_symvers(fw_version)
    if sym:
        local = local_elf_path(sym)
        if local.is_file() and _is_elf_file(local):
            return str(local), f"local cache for {sym}", True
        for cand in cdn_elf_paths(sym):
            if cand.is_file() and _is_elf_file(cand):
                try:
                    local.parent.mkdir(parents=True, exist_ok=True)
                    if not local.exists():
                        shutil.copy2(cand, local)
                except OSError:
                    pass
                return str(cand), f"CDN mount for {sym}", True
        if _fetch_elf_http(sym, local):
            return str(local), f"CDN HTTP for {sym}", True

    legacy = _legacy_elf_candidates()
    if legacy:
        note = (
            "Symbols from staged desk/cloud ELF — layout may not match "
            f"fw_version={fw_version!r}; treat as a hint only"
            if sym
            else "Symbols from staged ZEPHYR_ELF_PATH"
        )
        return legacy[0], note, False
    return None, None, False


def store_firmware_elf(symvers: str, data: bytes) -> list[str]:
    """Write zephyr.elf to local cache + any mounted CDN roots. Returns written paths."""
    sym = normalize_symvers(symvers)
    if not sym:
        raise ValueError("invalid fw_version / symvers")
    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise ValueError("not an ELF binary")

    written: list[str] = []
    targets = [local_elf_path(sym)] + cdn_elf_paths(sym)
    for path in targets:
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            tmp = path.with_suffix(".elf.partial")
            tmp.write_bytes(data)
            tmp.replace(path)
            written.append(str(path))
        except OSError:
            continue
    if not written:
        raise OSError("no writable ELF destination (cache / CDN mounts)")
    return written


# Process-local memo for addr2line — Issues board lists hundreds of rows and must
# not spawn addr2line per PC on every page load once detail_json already has frames.
_addr2line_cache: dict[tuple[str, int], str | None] = {}
_ADDR2LINE_CACHE_MAX = 4096


def _addr2line(elf: str, pc: int) -> str | None:
    key = (elf, pc & 0xFFFFFFFF)
    if key in _addr2line_cache:
        return _addr2line_cache[key]

    env_tool = os.getenv("ADDR2LINE", "").strip()
    tool = (
        env_tool
        or shutil.which("xtensa-addr2line")
        or shutil.which("xtensa-espressif_esp32s3_zephyr-elf-addr2line")
        or shutil.which("addr2line")
    )
    result: str | None = None
    if tool is not None:
        try:
            proc = subprocess.run(
                [tool, "-e", elf, "-f", "-C", f"0x{pc & 0xFFFFFFFF:08x}"],
                capture_output=True,
                text=True,
                timeout=5,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired):
            proc = None
        if proc is not None and proc.returncode == 0:
            lines = [ln.strip() for ln in proc.stdout.splitlines() if ln.strip()]
            if len(lines) >= 2:
                result = f"{lines[0]} at {lines[1]}"
            elif lines:
                result = lines[0]

    if len(_addr2line_cache) >= _ADDR2LINE_CACHE_MAX:
        _addr2line_cache.clear()
    _addr2line_cache[key] = result
    return result


def symbolicate_pcs(
    pcs: list[int],
    *,
    fw_version: str | None = None,
) -> tuple[list[dict[str, Any]], str | None, str | None, bool]:
    """Return (frames, elf_path, elf_note, version_matched)."""
    elf, note, matched = resolve_elf_for_version(fw_version)
    if elf is None or not pcs:
        return [], elf, note, matched
    frames: list[dict[str, Any]] = []
    for pc in pcs[:16]:
        if not pc:
            continue
        sym = _addr2line(elf, pc)
        frames.append({"pc": pc, "symbol": sym or f"0x{pc & 0xFFFFFFFF:08x}"})
    return frames, elf, note, matched


def symbolicate_backtrace(backtrace: list[int]) -> list[dict[str, Any]]:
    frames, _, _, _ = symbolicate_pcs(list(backtrace or []))
    return frames


def classify_root_cause(
    *,
    reason: str | None,
    pc: int | None,
    exccause: int | None,
    thread_name: str | None,
    uptime_ms: int | None,
    frames: list[dict[str, Any]] | None,
) -> str | None:
    """Human hint when stacks are missing or only partially useful."""
    r = (reason or "").strip()
    rl = r.lower()
    thr = (thread_name or "").strip()
    up = int(uptime_ms or 0)
    frames = frames or []
    top = frames[0].get("symbol") if frames else None

    if rl.startswith("soft:") or rl.startswith("soft_"):
        return f"Soft marker ({r}) — intentional reboot report, not a fault stack"

    if "task_wdt" in rl or rl in {"wdt", "tg0wdt"}:
        if 12_000 <= up <= 16_000 and (not pc) and not frames:
            base = (
                "HW task-WDT fallback (~13.5s, pre-v222): ESP32 MWDT mistimed "
                "(Zephyr passed ms as ticks) — fixed by disabling HW fallback"
            )
        elif 12_000 <= up <= 28_000:
            base = (
                "Stall task WDT during early run (~12–28s uptime): a watched thread "
                "stopped calling stall_watchdog_feed_* (often main blocked in boot "
                "IMU/I2C, or render stuck in panel SPI)"
            )
        elif up > 0 and up < 8_000:
            base = "Task WDT very early (<8s) — init path starved the feeder before main loop"
        else:
            base = "Task WDT — a stall_watchdog channel expired (or HW fallback reset)"

        if not pc and not frames and "HW task-WDT" not in base:
            base += (
                "; empty PC/backtrace is expected for CONFIG_TASK_WDT_HW_FALLBACK "
                "(pure HW reset, no Zephyr fatal capture)"
            )
        elif thr == "render":
            base += "; dying thread name render → feed_render gap (draw/flush/SPI)"
        elif thr == "main":
            base += "; dying thread name main → feed_main gap (main loop blocked)"
        elif thr:
            base += f"; captured thread was {thr} (may be whoever ran when HW fired)"

        if top and "??" not in top and not top.startswith("0x"):
            base += f"; PC symbol (ELF may not match this FW): {top}"
        return base

    # Logging-backend coredump often overwrites the real fault PC (Invalid SP path);
    # when frames point at coredump_* the useful signal is thread + assert context.
    if top and "coredump" in str(top).lower():
        base = (
            "RTC/PC captured inside coredump logging (not the original fault). "
            "Desk builds should leave CONFIG_DEBUG_COREDUMP off so RTC keeps the real PC"
        )
        if thr == "render":
            base += (
                "; thread=render — typical triggers: sched assert from ISR "
                "(!arch_is_in_isr), panel SPI/mutex, or stack smash under draw/flush"
            )
        elif thr:
            base += f"; thread={thr}"
        return base

    if top and "??" not in top and not top.startswith("0x"):
        return f"Fault at {top}" + (f" (thread {thr})" if thr else "")

    if pc:
        return f"Fault PC 0x{pc & 0xFFFFFFFF:08x}" + (
            f" exccause={exccause}" if exccause is not None else ""
        )

    if r:
        return f"Reported reason={r} (no PC/backtrace to symbolicate)"
    return None


def enrich_crash_detail(
    detail: dict | None,
    backtrace: list[int] | None,
    *,
    pc: int | None = None,
    reason: str | None = None,
    exccause: int | None = None,
    thread_name: str | None = None,
    uptime_ms: int | None = None,
    fw_version: str | None = None,
    force: bool = False,
) -> dict:
    """Fill frames / ELF metadata; persist stamps so list polls skip addr2line.

    ``sym_complete`` in ``detail_json`` means this row was already symbolicated (or
    confirmed to have nothing to resolve) for ``elf_symvers``. List/API reads must
    not re-hit CDN/addr2line until ``force=True`` (ELF upload / OTA notify).
    """
    out = dict(detail or {})
    pcs: list[int] = [int(x) for x in (backtrace or []) if x]
    if not pcs and pc:
        pcs = [int(pc)]

    sym = normalize_symvers(fw_version)
    frames = out.get("frames")
    have_frames = isinstance(frames, list) and bool(frames)
    stamped = bool(out.get("sym_complete")) and (
        not sym or out.get("elf_symvers") == sym
    )

    # Fast path: Postgres-cached detail is authoritative until artifact invalidate.
    if not force and stamped:
        hint = classify_root_cause(
            reason=reason,
            pc=pc,
            exccause=exccause,
            thread_name=thread_name,
            uptime_ms=uptime_ms,
            frames=frames if have_frames else None,
        )
        if hint:
            out["root_cause"] = hint
        return out

    matched_already = bool(sym) and out.get("elf_symvers") == sym and bool(out.get("elf_matched"))
    # Re-run addr2line only when forced, when frames were never cached, or when a
    # version-matched ELF is newly available and the cached pass used a fallback ELF.
    need_symbols = force or not have_frames
    if have_frames and sym and not matched_already and not stamped:
        _elf, _note, matched_now = resolve_elf_for_version(fw_version)
        if matched_now:
            need_symbols = True

    if need_symbols:
        frames, elf, note, matched = symbolicate_pcs(pcs, fw_version=fw_version)
        if frames:
            out["frames"] = frames
        elif force:
            out.pop("frames", None)
        if elf:
            out["elf"] = os.path.basename(elf)
            out["elf_path"] = elf
            if note:
                out["elf_note"] = note
            out["elf_matched"] = matched
            if matched:
                out["elf_url"] = firmware_elf_url(sym) if sym else None
            elif "elf_url" in out and not matched:
                out.pop("elf_url", None)
        # Stamp even when there are no PCs / no ELF — avoids CDN hammering on every poll.
        if sym:
            out["elf_symvers"] = sym
        out["sym_complete"] = True
    elif force and sym:
        # Force with nothing to resolve still refreshes the stamp for this version.
        out["elf_symvers"] = sym
        out["sym_complete"] = True
    elif have_frames and (matched_already or not sym):
        # Legacy rows already have frames — stamp so the next poll is a pure cache hit.
        if sym and not out.get("elf_symvers"):
            out["elf_symvers"] = sym
        out["sym_complete"] = True

    hint = classify_root_cause(
        reason=reason,
        pc=pc,
        exccause=exccause,
        thread_name=thread_name,
        uptime_ms=uptime_ms,
        frames=out.get("frames") if isinstance(out.get("frames"), list) else frames,
    )
    if hint:
        out["root_cause"] = hint
    return out
