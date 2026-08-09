#!/usr/bin/env bash
# Read Zephyr boot log after flash/reset (115200 8N1).
set -euo pipefail

PORT="${1:-${PORT:-/dev/ttyACM0}}"
SECONDS="${2:-12}"
OUT="${3:-}"
SKIP_RESET="${SKIP_RESET:-0}"

wait_for_port() {
	local p="$1"
	local tries="${2:-30}"
	while (( tries > 0 )); do
		if [[ -e "$p" ]]; then
			return 0
		fi
		sleep 0.2
		tries=$((tries - 1))
	done
	return 1
}

if ! wait_for_port "$PORT" 40; then
	echo "capture-serial-boot: port missing: $PORT" >&2
	exit 1
fi

ESPTOOL="$HOME/zephyrproject/modules/hal/espressif/tools/esptool_py/esptool.py"
PY="$HOME/zephyrproject/.venv/bin/python3"

	if [[ "$SKIP_RESET" != "1" ]]; then
		if [[ -x "$PY" && -f "$ESPTOOL" ]]; then
			"$PY" "$ESPTOOL" --port "$PORT" --baud 115200 run >/dev/null 2>&1 || true
			sleep 3.0
			wait_for_port "$PORT" 60 || true
		fi
	else
		sleep 7.0
		wait_for_port "$PORT" 60 || true
		echo "capture-serial-boot: reading ${PORT} (tap board RESET if log stays empty)" >&2
	fi

if [[ -n "$OUT" ]]; then
	exec > >(tee "$OUT")
fi

"$PY" - <<PY
import sys
import time
import serial

port = "${PORT}"
seconds = float("${SECONDS}")
deadline = time.time() + 25.0
ser = None
while time.time() < deadline:
    try:
        ser = serial.Serial(port, 115200, timeout=0.2)
        break
    except serial.SerialException:
        time.sleep(0.2)
if ser is None:
    sys.stderr.write(f"capture-serial-boot: could not open {port}\n")
    sys.exit(1)

ser.reset_input_buffer()
buf = bytearray()
deadline = time.time() + seconds
last = time.time()
while time.time() < deadline:
    chunk = ser.read(4096)
    if chunk:
        buf.extend(chunk)
        last = time.time()
    elif len(buf) > 400 and (time.time() - last) > 15.0:
        break
    else:
        time.sleep(0.05)
ser.close()
sys.stdout.write(buf.decode("utf-8", errors="replace"))
sys.stderr.write(f"\n--- capture-serial-boot: {len(buf)} bytes from {port} ---\n")
if len(buf) < 80:
    sys.exit(2)
PY
