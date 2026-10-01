#!/usr/bin/env bash
# Attach OpenOCD + GDB to Waveshare ESP32-S3 via built-in USB-JTAG (no external probe).
#
# Desk board enumerates as 303a:1001 "Espressif USB JTAG/serial debug unit" on the
# same cable as /dev/ttyACM0. Attaching OpenOCD usually takes over that USB
# interface — serial console goes quiet until you detach.
#
# Usage:
#   zephyr/scripts/gdb-attach-usb-jtag.sh              # halt, dump regs+bt, resume
#   zephyr/scripts/gdb-attach-usb-jtag.sh dump [elf]
#   zephyr/scripts/gdb-attach-usb-jtag.sh -i [elf]     # interactive GDB (OpenOCD stays up)
#   zephyr/scripts/gdb-attach-usb-jtag.sh server       # OpenOCD only (:3333)
#
# When USB goes silent on a hang: do NOT RTS-reset first — run `dump` while the
# chip is still powered. Match ELF to the running image (out/zephyr/zephyr-vNNN.elf).
#
# Env overrides:
#   OPENOCD, OPENOCD_SCRIPTS, GDB, ELF, GDB_PORT (default 3333)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"

OPENOCD="${OPENOCD:-/opt/openocd-esp32/bin/openocd}"
OPENOCD_SCRIPTS="${OPENOCD_SCRIPTS:-/opt/openocd-esp32/share/openocd/scripts}"
GDB_PORT="${GDB_PORT:-3333}"

GDB="${GDB:-}"
if [[ -z "$GDB" ]]; then
	for cand in \
		"$HOME/zephyr-sdk-1.0.1/gnu/xtensa-espressif_esp32s3_zephyr-elf/bin/xtensa-espressif_esp32s3_zephyr-elf-gdb" \
		"$(command -v xtensa-espressif_esp32s3_zephyr-elf-gdb 2>/dev/null || true)" \
		"$(command -v xtensa-esp32s3-elf-gdb 2>/dev/null || true)"; do
		if [[ -n "$cand" && -x "$cand" ]]; then
			GDB="$cand"
			break
		fi
	done
fi

pick_elf() {
	if [[ -n "${ELF:-}" && -f "$ELF" ]]; then
		echo "$ELF"
		return
	fi
	local latest
	latest="$(ls -t "$REPO/out/zephyr"/zephyr-v*.elf 2>/dev/null | head -1 || true)"
	if [[ -n "$latest" ]]; then
		echo "$latest"
		return
	fi
	local build_elf="$HOME/zephyrproject/zephyr/build/waveshare-handshake/zephyr/zephyr.elf"
	if [[ -f "$build_elf" ]]; then
		echo "$build_elf"
		return
	fi
	return 1
}

usage() {
	cat <<EOF
Usage: $(basename "$0") [dump|server|-i|--interactive] [zephyr.elf]

  dump (default)  Halt both cores, print registers + backtrace, resume, exit.
  server          Run OpenOCD only (gdb on :${GDB_PORT}); Ctrl-C to stop.
  -i              Interactive GDB (starts OpenOCD in the background).

Requires Espressif OpenOCD (default /opt/openocd-esp32) and USB-JTAG (lsusb 303a:1001).
Serial console on the same cable will typically stop while OpenOCD is attached.
EOF
	exit 2
}

MODE="dump"
ELF_ARG=""
while [[ $# -gt 0 ]]; do
	case "$1" in
	dump | server) MODE="$1"; shift ;;
	-i | --interactive) MODE="interactive"; shift ;;
	-h | --help) usage ;;
	-*)
		echo "unknown option: $1" >&2
		usage
		;;
	*)
		ELF_ARG="$1"
		shift
		;;
	esac
done

[[ -x "$OPENOCD" ]] || {
	echo "OpenOCD not found: $OPENOCD" >&2
	echo "Install Espressif OpenOCD or set OPENOCD= / OPENOCD_SCRIPTS=" >&2
	exit 1
}
[[ -d "$OPENOCD_SCRIPTS" ]] || {
	echo "OpenOCD scripts missing: $OPENOCD_SCRIPTS" >&2
	exit 1
}
[[ -f "$OPENOCD_SCRIPTS/board/esp32s3-builtin.cfg" ]] || {
	echo "missing board/esp32s3-builtin.cfg under $OPENOCD_SCRIPTS" >&2
	exit 1
}

if ! lsusb 2>/dev/null | grep -q '303a:1001'; then
	echo "warn: no Espressif USB JTAG (303a:1001) in lsusb — is the board plugged in?" >&2
fi

OCD_LOG="${TMPDIR:-/tmp}/esp32s3-openocd-$$.log"
OCD_PID=""

cleanup() {
	if [[ -n "${OCD_PID}" ]] && kill -0 "$OCD_PID" 2>/dev/null; then
		kill "$OCD_PID" 2>/dev/null || true
		wait "$OCD_PID" 2>/dev/null || true
	fi
}
trap cleanup EXIT

start_openocd() {
	echo "OpenOCD: $OPENOCD (scripts=$OPENOCD_SCRIPTS, gdb_port=$GDB_PORT)" >&2
	echo "log → $OCD_LOG" >&2
	"$OPENOCD" -s "$OPENOCD_SCRIPTS" \
		-f board/esp32s3-builtin.cfg \
		-c "gdb_port $GDB_PORT; tcl_port disabled; telnet_port disabled" \
		>"$OCD_LOG" 2>&1 &
	OCD_PID=$!
	local i
	for i in $(seq 1 40); do
		if ! kill -0 "$OCD_PID" 2>/dev/null; then
			echo "OpenOCD exited early — see $OCD_LOG" >&2
			tail -30 "$OCD_LOG" >&2 || true
			exit 1
		fi
		if rg -q "Listening on port ${GDB_PORT} for gdb" "$OCD_LOG" 2>/dev/null; then
			return 0
		fi
		if rg -q "Error:|Could not|LIBUSB" "$OCD_LOG" 2>/dev/null && \
			! rg -q "Listening on port ${GDB_PORT}" "$OCD_LOG" 2>/dev/null; then
			# keep waiting a bit — some errors are soft; hard fail if process dies
			:
		fi
		sleep 0.15
	done
	if ! rg -q "Listening on port ${GDB_PORT}" "$OCD_LOG" 2>/dev/null; then
		echo "OpenOCD did not open gdb port — see $OCD_LOG" >&2
		tail -40 "$OCD_LOG" >&2 || true
		exit 1
	fi
}

case "$MODE" in
server)
	trap - EXIT
	echo "OpenOCD server (Ctrl-C to stop). Then: gdb -ex 'target extended-remote :$GDB_PORT' <elf>"
	exec "$OPENOCD" -s "$OPENOCD_SCRIPTS" \
		-f board/esp32s3-builtin.cfg \
		-c "gdb_port $GDB_PORT"
	;;
esac

[[ -n "$GDB" && -x "$GDB" ]] || {
	echo "GDB not found (set GDB=… to xtensa-*-esp32s3*-gdb)" >&2
	exit 1
}

if [[ -n "$ELF_ARG" ]]; then
	ELF="$ELF_ARG"
else
	ELF="$(pick_elf)" || {
		echo "ELF not found — pass path or build so out/zephyr/zephyr-v*.elf exists" >&2
		exit 1
	}
fi
[[ -f "$ELF" ]] || {
	echo "ELF not found: $ELF" >&2
	exit 1
}

echo "ELF: $ELF" >&2
echo "GDB: $GDB" >&2
start_openocd

if [[ "$MODE" == "interactive" ]]; then
	echo "Interactive GDB — 'monitor halt' / bt / 'monitor resume' / quit" >&2
	# Keep OpenOCD until GDB exits; trap cleans up.
	"$GDB" -nx \
		-ex "set pagination off" \
		-ex "set confirm off" \
		-ex "target extended-remote :$GDB_PORT" \
		-ex "monitor halt" \
		"$ELF"
	exit 0
fi

# dump
"$GDB" -nx -batch \
	-ex "set pagination off" \
	-ex "set confirm off" \
	-ex "target extended-remote :$GDB_PORT" \
	-ex "monitor halt" \
	-ex "printf \"\\n=== registers (cpu0) ===\\n\"" \
	-ex "info registers" \
	-ex "printf \"\\n=== backtrace ===\\n\"" \
	-ex "bt 24" \
	-ex "printf \"\\n=== threads (Zephyr RTOS awareness often unavailable) ===\\n\"" \
	-ex "info threads" \
	-ex "thread apply all bt 12" \
	-ex "monitor resume" \
	-ex "detach" \
	-ex "quit" \
	"$ELF"
echo "dump done (resumed). OpenOCD log: $OCD_LOG" >&2
