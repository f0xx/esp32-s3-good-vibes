#!/usr/bin/env bash
# Poll OpenOCD/GDB every 5 minutes during a 1hr hang hunt.
# Does NOT start the hunt — attaches to the live :3333 session owned by
# gdb-hang-hunt.sh and runs hang_vars (halt/resume). On hang marker or stuck
# breadcrumbs, exits non-zero so the agent gets notified.
#
#   zephyr/scripts/gdb-5min-watch.sh           # 12 polls (~1hr)
#   zephyr/scripts/gdb-5min-watch.sh 6         # 6 polls (~30m)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
GDB_CMDS="$SCRIPT_DIR/gdb-hang-hunt.gdb"
GDB_PORT="${GDB_PORT:-3333}"
HUNT_LOG="${HANG_HUNT_LOG:-/tmp/esp32s3-hang-hunt.log}"
OUT="${GDB_5MIN_LOG:-/tmp/esp32s3-gdb-5min.log}"
PIDF="${HANG_HUNT_PID:-/tmp/esp32s3-hang-hunt.pid}"
POLLS="${1:-12}"
INTERVAL_SEC="${INTERVAL_SEC:-300}"

GDB="${GDB:-}"
if [[ -z "$GDB" ]]; then
	for cand in \
		"$HOME/zephyr-sdk-1.0.1/gnu/xtensa-espressif_esp32s3_zephyr-elf/bin/xtensa-espressif_esp32s3_zephyr-elf-gdb" \
		"$(command -v xtensa-espressif_esp32s3_zephyr-elf-gdb 2>/dev/null || true)"; do
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
	[[ -n "$latest" ]] && echo "$latest" && return
	return 1
}

ELF="$(pick_elf)" || {
	echo "ELF not found" >&2
	exit 1
}

: >"$OUT"
log() {
	local line="[$(date +%H:%M:%S)] $*"
	echo "$line" | tee -a "$OUT"
}

log "5min GDB watch start polls=$POLLS interval=${INTERVAL_SEC}s elf=$(basename "$ELF")"

last_loops=""
for i in $(seq 1 "$POLLS"); do
	sleep "$INTERVAL_SEC"
	ts=$(date +%H:%M:%S)

	if [[ -f "$PIDF" ]] && ! kill -0 "$(cat "$PIDF")" 2>/dev/null; then
		log "FAIL: hang-hunt process dead"
		exit 1
	fi

	if grep -aq "HANG DETECTED" "$HUNT_LOG" 2>/dev/null; then
		log "FAIL: hunt already marked HANG DETECTED"
		grep -aE "HANG DETECTED|HALTED|hang probe|g_main|g_flush|#[0-9]+ " "$HUNT_LOG" | tail -40 | tee -a "$OUT"
		date > /tmp/esp32s3-HANG-NOW
		exit 2
	fi

	if grep -aq "hang-poll finished (no hang)" "$HUNT_LOG" 2>/dev/null; then
		log "PASS: hang-poll finished (no hang)"
		exit 0
	fi

	# Direct GDB sample (halt → hang_vars → resume). timeout so a wedged
	# OpenOCD cannot block the whole hour.
	SAMPLE="$(timeout 30 "$GDB" -nx -batch \
		-ex "set pagination off" -ex "set confirm off" -ex "set print elements 0" \
		-ex "target extended-remote :$GDB_PORT" \
		-ex "source $GDB_CMDS" \
		-ex "monitor halt" \
		-ex "hang_vars" \
		-ex "monitor resume" \
		-ex "detach" -ex "quit" \
		"$ELF" 2>/dev/null || true)"

	loops="$(printf '%s' "$SAMPLE" | sed -n 's/.*g_main_loops=\([0-9]*\).*/\1/p' | head -1)"
	frames="$(printf '%s' "$SAMPLE" | sed -n 's/.*hb_render_frames=\([0-9]*\).*/\1/p' | head -1)"
	busy="$(printf '%s' "$SAMPLE" | sed -n 's/.*g_display_busy=\([0-9]*\).*/\1/p' | head -1)"
	flush_y="$(printf '%s' "$SAMPLE" | sed -n 's/.*g_flush_cur_y=\([-0-9]*\).*/\1/p' | head -1)"
	alive="$(printf '%s' "$SAMPLE" | sed -n 's/.*g_alive.uptime_ms=\([0-9]*\).*/\1/p' | head -1)"

	if [[ -z "$loops$alive" ]]; then
		log "WARN poll $i/$POLLS: GDB sample empty (OpenOCD busy?) — hunt log tail:"
		tail -3 "$HUNT_LOG" | tee -a "$OUT" || true
		continue
	fi

	log "GDB poll $i/$POLLS loops=$loops frames=$frames busy=$busy flush_y=$flush_y alive_up=$alive"

	if [[ -n "$last_loops" && "$loops" == "$last_loops" ]]; then
		log "FAIL: g_main_loops stuck at $loops across 5min GDB polls"
		date > /tmp/esp32s3-HANG-NOW
		printf '%s\n' "$SAMPLE" >>"$OUT"
		exit 3
	fi
	last_loops=$loops
done

log "5min GDB watch complete ($POLLS polls, no stuck loops)"
exit 0
