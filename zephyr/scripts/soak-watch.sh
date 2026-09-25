#!/usr/bin/env bash
# Live soak watcher: keep serial (no permanent OpenOCD), dump via GDB on silence.
#
#   zephyr/scripts/soak-watch.sh [minutes]
#
# Writes: /tmp/esp32s3-soak-watch.log
# On silence >= SILENT_SEC: one-shot gdb dump → /tmp/esp32s3-soak-hang-*.log
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
MINUTES="${1:-30}"
PORT="${PORT:-/dev/ttyACM0}"
SILENT_SEC="${SILENT_SEC:-20}"
LOG="${SOAK_LOG:-/tmp/esp32s3-soak-watch.log}"
PY="${PY:-$HOME/zephyrproject/.venv/bin/python3}"
GDB_ATTACH="$SCRIPT_DIR/gdb-attach-usb-jtag.sh"

: >"$LOG"
echo "[$(date +%H:%M:%S)] soak-watch start ${MINUTES}m silent>${SILENT_SEC}s port=$PORT" | tee -a "$LOG"

# Free any leftover OpenOCD so ACM works
pkill -f '/opt/openocd-esp32/bin/openocd' 2>/dev/null || true
sleep 0.5

exec "$PY" - "$PORT" "$MINUTES" "$SILENT_SEC" "$LOG" "$GDB_ATTACH" <<'PY'
import serial, time, sys, subprocess, os, re
from datetime import datetime

port, minutes, silent_sec, log_path, gdb_attach = sys.argv[1:6]
minutes = float(minutes)
silent_sec = float(silent_sec)
deadline = time.time() + minutes * 60
last_byte = time.time()
last_hb = None
seen_hb = False
hb_re = re.compile(rb"main_hb ok loops=(\d+)")
dumped = False
boot_grace_until = time.time() + 45  # ignore silence until first hb or 45s

def log(msg):
    line = f"[{datetime.now():%H:%M:%S}] {msg}"
    print(line, flush=True)
    with open(log_path, "a") as f:
        f.write(line + "\n")

try:
    ser = serial.Serial(port, 115200, timeout=0.5)
except Exception as e:
    log(f"FATAL open {port}: {e}")
    sys.exit(1)

log("serial open — watching heartbeats")
buf = b""
try:
    while time.time() < deadline:
        chunk = ser.read(4096)
        now = time.time()
        if chunk:
            last_byte = now
            buf += chunk
            # keep buf bounded
            if len(buf) > 65536:
                buf = buf[-32768:]
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
            with open(log_path, "ab") as f:
                f.write(chunk)
            for m in hb_re.finditer(chunk):
                last_hb = int(m.group(1))
                seen_hb = True
                log(f"hb loops={last_hb}")
        elif (
            seen_hb
            and now > boot_grace_until
            and (now - last_byte) >= silent_sec
            and not dumped
        ):
            log(f"SILENT {silent_sec:.0f}s — attaching GDB dump (last_hb={last_hb})")
            ser.close()
            dump = f"/tmp/esp32s3-soak-hang-{datetime.now():%H%M%S}.log"
            elf = ""
            out_dir = os.path.expanduser(
                "~/repos/My Projects/espXX/esp32-s3-imu-basics/out/zephyr"
            )
            try:
                elves = sorted(
                    [os.path.join(out_dir, f) for f in os.listdir(out_dir) if f.startswith("zephyr-v") and f.endswith(".elf")],
                    key=os.path.getmtime,
                    reverse=True,
                )
                elf = elves[0] if elves else ""
            except Exception:
                pass
            gdb = os.path.expanduser(
                "~/zephyr-sdk-1.0.1/gnu/xtensa-espressif_esp32s3_zephyr-elf/bin/xtensa-espressif_esp32s3_zephyr-elf-gdb"
            )
            cmds = os.path.join(os.path.dirname(gdb_attach), "gdb-hang-hunt.gdb")
            openocd = "/opt/openocd-esp32/bin/openocd"
            scripts = "/opt/openocd-esp32/share/openocd/scripts"
            ocd_log = f"/tmp/esp32s3-soak-ocd-{datetime.now():%H%M%S}.log"
            try:
                ocd = subprocess.Popen(
                    [openocd, "-s", scripts, "-f", "board/esp32s3-builtin.cfg",
                     "-c", "gdb_port 3333; tcl_port disabled; telnet_port disabled"],
                    stdout=open(ocd_log, "w"),
                    stderr=subprocess.STDOUT,
                )
                for _ in range(40):
                    if os.path.exists(ocd_log) and "Listening on port 3333" in open(ocd_log).read():
                        break
                    time.sleep(0.15)
                r = subprocess.run(
                    [
                        gdb, "-nx", "-batch",
                        "-ex", "set pagination off",
                        "-ex", "set confirm off",
                        "-ex", "target extended-remote :3333",
                        "-ex", "monitor halt",
                        "-ex", f"source {cmds}",
                        "-ex", "hang_dump",
                    ] + ([elf] if elf else []),
                    capture_output=True,
                    text=True,
                    timeout=90,
                )
                open(dump, "w").write(r.stdout + "\n" + r.stderr)
                log(f"GDB dump → {dump} rc={r.returncode} (target LEFT HALTED, openocd pid={ocd.pid})")
                for line in (r.stdout or "").splitlines():
                    if any(
                        k in line
                        for k in (
                            "g_main_loops",
                            "g_flush",
                            "g_display_busy",
                            "g_alive",
                            "#0 ",
                            "#1 ",
                            "#2 ",
                            "#3 ",
                            "spi_",
                            "flush_fb",
                            "renderer",
                        )
                    ):
                        log("  " + line[:220])
                log(f"ATTACH: {gdb} -ex 'target extended-remote :3333' '{elf}'")
                log("SOAK STOPPED ON HANG — OpenOCD kept up for interactive debug")
                sys.exit(2)
            except Exception as e:
                log(f"GDB dump failed: {e}")
                subprocess.run(["pkill", "-f", "/opt/openocd-esp32/bin/openocd"], capture_output=True)
                dumped = True
                # fall through to reset/resume only if dump failed
                esptool = os.path.expanduser(
                    "~/zephyrproject/modules/hal/espressif/tools/esptool_py/esptool.py"
                )
                subprocess.run(
                    [sys.executable, esptool, "--port", port, "--baud", "115200", "run"],
                    capture_output=True,
                )
                time.sleep(3)
                for _ in range(30):
                    try:
                        ser = serial.Serial(port, 115200, timeout=0.5)
                        break
                    except Exception:
                        time.sleep(0.2)
                last_byte = time.time()
                dumped = False
                log("resumed serial watch after failed dump+reset")
                continue
        time.sleep(0.05)
finally:
    try:
        ser.close()
    except Exception:
        pass
log("soak-watch finished clean")
PY
