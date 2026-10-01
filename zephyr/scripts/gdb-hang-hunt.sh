#!/usr/bin/env bash
# Autonomous hang hunt: OpenOCD + GDB (no Python — Zephyr SDK GDB lacks it).
#
#   zephyr/scripts/gdb-hang-hunt.sh           # poll forever
#   zephyr/scripts/gdb-hang-hunt.sh 600       # poll 10 minutes
#   zephyr/scripts/gdb-hang-hunt.sh dump      # one-shot halt+dump+resume
#
# Arms HW breakpoints on z_fatal_error / assert_post_action and a conditional
# flush_fb break (nested flush). Polls breadcrumb symbols every few seconds.
# On hang: leaves target HALTED and OpenOCD up on :3333.
#
# Log: /tmp/esp32s3-hang-hunt.log
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
GDB_CMDS="$SCRIPT_DIR/gdb-hang-hunt.gdb"
GDB_PORT="${GDB_PORT:-3333}"
LOG="${HANG_HUNT_LOG:-/tmp/esp32s3-hang-hunt.log}"
POLL_SEC="${POLL_SEC:-3}"
STUCK_FLUSH_SAMPLES="${STUCK_FLUSH_SAMPLES:-4}"
STUCK_MAIN_SEC="${STUCK_MAIN_SEC:-20}"
STUCK_BUSY_SEC="${STUCK_BUSY_SEC:-12}"

OPENOCD="${OPENOCD:-/opt/openocd-esp32/bin/openocd}"
OPENOCD_SCRIPTS="${OPENOCD_SCRIPTS:-/opt/openocd-esp32/share/openocd/scripts}"

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
	local build_elf="$HOME/zephyrproject/zephyr/build/waveshare-handshake/zephyr/zephyr.elf"
	[[ -f "$build_elf" ]] && echo "$build_elf" && return
	return 1
}

log() {
	local line="[$(date +%H:%M:%S)] $*"
	echo "$line" | tee -a "$LOG"
}

MODE="poll"
DURATION="0"
if [[ "${1:-}" == "dump" ]]; then
	MODE="dump"
	shift
elif [[ "${1:-}" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
	DURATION="$1"
	shift
fi

ELF="$(pick_elf)" || {
	echo "ELF not found" >&2
	exit 1
}
[[ -x "$GDB" ]] || {
	echo "GDB not found" >&2
	exit 1
}
[[ -x "$OPENOCD" ]] || {
	echo "OpenOCD not found: $OPENOCD" >&2
	exit 1
}

OCD_LOG="${TMPDIR:-/tmp}/esp32s3-openocd-hanghunt-$$.log"
OCD_PID=""
KEEP_OPENOCD=0
cleanup() {
	if [[ -n "${OCD_PID}" ]] && kill -0 "$OCD_PID" 2>/dev/null; then
		if [[ "$KEEP_OPENOCD" != "1" ]]; then
			kill "$OCD_PID" 2>/dev/null || true
			wait "$OCD_PID" 2>/dev/null || true
		fi
	fi
}
trap cleanup EXIT

log "ELF: $ELF"
if [[ "${REUSE_OPENOCD:-0}" == "1" ]]; then
	# Attach to the OpenOCD already on $GDB_PORT. Starting a new one
	# USB-resets the chip and throws away the uptime under test.
	OCD_PID="$(ss -ltnp 2>/dev/null | sed -n "s/.*:${GDB_PORT} .*pid=\\([0-9]*\\).*/\\1/p" | head -1)"
	if [[ -z "$OCD_PID" ]] || ! kill -0 "$OCD_PID" 2>/dev/null; then
		echo "REUSE_OPENOCD=1 but nothing is listening on :$GDB_PORT" >&2
		exit 1
	fi
	KEEP_OPENOCD=1
	log "Reusing OpenOCD pid $OCD_PID (no USB reset)"
else
	fuser -k /dev/ttyACM0 2>/dev/null || true
	sleep 0.3
	log "OpenOCD starting (serial console will go quiet)…"
	"$OPENOCD" -s "$OPENOCD_SCRIPTS" \
		-f board/esp32s3-builtin.cfg \
		-c "gdb_port $GDB_PORT; tcl_port disabled; telnet_port disabled" \
		>"$OCD_LOG" 2>&1 &
	OCD_PID=$!

	for _ in $(seq 1 50); do
		if ! kill -0 "$OCD_PID" 2>/dev/null; then
			echo "OpenOCD died — $OCD_LOG" >&2
			tail -40 "$OCD_LOG" >&2 || true
			exit 1
		fi
		if rg -q "Listening on port ${GDB_PORT} for gdb" "$OCD_LOG" 2>/dev/null; then
			break
		fi
		sleep 0.15
	done
	if ! rg -q "Listening on port ${GDB_PORT}" "$OCD_LOG" 2>/dev/null; then
		echo "OpenOCD gdb port not ready — $OCD_LOG" >&2
		tail -40 "$OCD_LOG" >&2 || true
		exit 1
	fi
fi

gdb_batch() {
	# Args: extra -ex commands… then always ends with quit.
	# timeout: wedged USB-JTAG halt can block forever (desk: 9+ min).
	timeout --signal=KILL "${GDB_BATCH_TIMEOUT:-25}" \
		"$GDB" -nx -batch \
		-ex "set pagination off" \
		-ex "set confirm off" \
		-ex "target extended-remote :$GDB_PORT" \
		-ex "source $GDB_CMDS" \
		"$@" \
		-ex "quit" \
		"$ELF" || true
}

if [[ "$MODE" == "dump" ]]; then
	gdb_batch \
		-ex "monitor halt" \
		-ex "hang_dump" \
		-ex "monitor resume" \
		-ex "detach" | tee -a "$LOG"
	exit 0
fi

# Arm breakpoints once, then resume.
log "Arming breakpoints (z_fatal_error, assert_post_action)…"
ARM_OUT="$(gdb_batch \
	-ex "monitor halt" \
	-ex "hang_arm" \
	-ex "monitor resume" \
	-ex "detach" 2>&1 || true)"
echo "$ARM_OUT" | tee -a "$LOG" >/dev/null
if [[ "${ARM_FLUSH:-0}" == "1" ]]; then
	log "Arming nested-flush conditional (ARM_FLUSH=1)…"
	gdb_batch \
		-ex "monitor halt" \
		-ex "hang_arm_flush" \
		-ex "monitor resume" \
		-ex "detach" 2>&1 | tee -a "$LOG" >/dev/null || true
fi

log "hang-poll start duration=${DURATION}s poll=${POLL_SEC}s → $LOG"

t0=$(date +%s)
last_loops=""
last_loops_t=$t0
last_alive_up=""
last_alive_up_t=$t0
last_alive_ticks=""
flush_stuck=0
busy_since=""
frames_zero_streak=0
last_frames=""
SAMPLE_RAW=""

extract() {
	# $1 = regex group name from hang_vars printf lines
	local key="$1"
	printf '%s' "$SAMPLE_RAW" | sed -n "s/.*${key}=\\([^ ]*\\).*/\\1/p" | head -1
}

while true; do
	now=$(date +%s)
	if [[ "$DURATION" != "0" ]] && (( now - t0 >= ${DURATION%.*} )); then
		log "hang-poll finished (no hang)"
		exit 0
	fi

	sleep "$POLL_SEC"

	SAMPLE_RAW="$(gdb_batch \
		-ex "monitor halt" \
		-ex "printf \"live_pc 0x%x\\n\", \$pc" \
		-ex "hang_vars" \
		-ex "monitor resume" \
		-ex "detach" 2>&1 || true)"

	# A rejected GDB connection still prints the ELF's boot initializers
	# (loops=0, step=boot). $pc is only valid once the target is halted.
	if ! printf '%s' "$SAMPLE_RAW" | rg -q 'live_pc 0x[0-9a-fA-F]'; then
		log "poll skipped (no live PC)"
		continue
	fi

	loops="$(extract g_main_loops)"
	frames="$(extract g_render_frames_total)"
	if [[ -z "$frames" ]]; then
		frames="$(extract hb_render_frames)"
	fi
	stage="$(extract g_render_stage)"
	busy="$(printf '%s' "$SAMPLE_RAW" | sed -n 's/.*g_display_busy=\([0-9]*\).*/\1/p' | head -1)"
	flush_y="$(printf '%s' "$SAMPLE_RAW" | sed -n 's/.*g_flush_cur_y=\([-0-9]*\).*/\1/p' | head -1)"
	alive_up="$(printf '%s' "$SAMPLE_RAW" | sed -n 's/.*g_alive.uptime_ms=\([0-9]*\).*/\1/p' | head -1)"
	alive_ticks="$(printf '%s' "$SAMPLE_RAW" | sed -n 's/.*timer_ticks=\([0-9]*\).*/\1/p' | head -1)"
	step="$(printf '%s' "$SAMPLE_RAW" | sed -n "s/.*g_main_step=\\(.*\\)/\\1/p" | head -1)"
	spi_trip="$(printf '%s' "$SAMPLE_RAW" | sed -n 's/.*g_spi_tripped=\([0-9]*\).*/\1/p' | head -1)"
	mt_want="$(printf '%s' "$SAMPLE_RAW" | sed -n 's/.*mt200 want=\([0-9]*\).*/\1/p' | head -1)"
	# MUST match mt200 line — g_display_busy= also contains "busy="
	mt_busy="$(printf '%s' "$SAMPLE_RAW" | sed -n 's/.*mt200 want=[0-9]* busy=\([0-9]*\).*/\1/p' | head -1)"
	mt_phase="$(printf '%s' "$SAMPLE_RAW" | sed -n 's/.*mt200 want=[0-9]* busy=[0-9]* phase=\([0-9]*\).*/\1/p' | head -1)"
	flush_ret="$(printf '%s' "$SAMPLE_RAW" | sed -n 's/.*g_flush_last_ret=\([-0-9]*\).*/\1/p' | head -1)"
	spi_en="$(printf '%s' "$SAMPLE_RAW" | sed -n 's/.*g_spi_enabled=\([0-9]*\).*/\1/p' | head -1)"

	# Full state line every poll — inspect, don't just glance at loops.
	log "ok loops=${loops:-?} frames=${frames:-?} stage=${stage:-?} busy=${busy:-?} flush_y=${flush_y:-?} flush_ret=${flush_ret:-?} spi_en=${spi_en:-?} spi_trip=${spi_trip:-?} alive_up=${alive_up:-?} ticks=${alive_ticks:-?} step=${step:-?} mt200(want=${mt_want:-?} busy=${mt_busy:-?} phase=${mt_phase:-?})"
	# Also keep raw hang_vars snippet for forensics
	printf '%s\n' "$SAMPLE_RAW" | sed -n '/=== hang probe ===/,/^$/p' >>"$LOG" || true

	hang_reason=""
	now=$(date +%s)

	# Mid-flush is normal (~30 Hz). Only treat as hung if flush_y stays set
	# AND main loops freeze (USR busy-wait wedges the whole core).
	if [[ -n "$flush_y" && "$flush_y" =~ ^[0-9]+$ ]]; then
		flush_stuck=$((flush_stuck + 1))
	else
		flush_stuck=0
	fi
	if (( flush_stuck >= STUCK_FLUSH_SAMPLES )) && \
		[[ -n "$last_loops" && -n "$loops" && "$loops" == "$last_loops" ]] && \
		(( now - last_loops_t >= POLL_SEC )); then
		hang_reason="flush strip stuck y=$flush_y + loops frozen at $loops"
	fi

	if [[ "$busy" == "1" ]]; then
		if [[ -z "$busy_since" ]]; then
			busy_since=$now
		elif (( now - busy_since >= STUCK_BUSY_SEC )) && [[ -n "$last_loops" && "$loops" == "$last_loops" ]]; then
			hang_reason="${hang_reason:-g_display_busy stuck + main loops frozen}"
		fi
	else
		busy_since=""
	fi

	if [[ -n "$loops" ]]; then
		if [[ "$loops" != "$last_loops" ]]; then
			last_loops=$loops
			last_loops_t=$now
		elif (( now - last_loops_t >= STUCK_MAIN_SEC )); then
			hang_reason="${hang_reason:-g_main_loops frozen at $loops for ${STUCK_MAIN_SEC}s}"
		fi
	fi

	if [[ -n "$alive_up" ]]; then
		# Reboot / crash reset: uptime went backwards by >30s.
		if [[ -n "$last_alive_up" && "$alive_up" =~ ^[0-9]+$ && "$last_alive_up" =~ ^[0-9]+$ ]]; then
			if (( alive_up + 30000 < last_alive_up )); then
				hang_reason="reboot/crash: alive_up ${last_alive_up}->${alive_up}"
			fi
		fi
		if [[ "$alive_up" != "$last_alive_up" ]]; then
			last_alive_up=$alive_up
			last_alive_up_t=$now
		elif (( now - last_alive_up_t >= STUCK_MAIN_SEC )); then
			if [[ -n "$last_alive_ticks" && -n "$alive_ticks" && "$alive_ticks" != "$last_alive_ticks" ]]; then
				hang_reason="${hang_reason:-alive uptime frozen while timer_ticks still advance}"
			else
				hang_reason="${hang_reason:-alive uptime + timer frozen (whole OS?)}"
			fi
		fi
	fi
	[[ -n "$alive_ticks" ]] && last_alive_ticks=$alive_ticks

	# Also treat main_loops dropping as a reboot signal.
	if [[ -n "$loops" && -n "$last_loops" && "$loops" =~ ^[0-9]+$ && "$last_loops" =~ ^[0-9]+$ ]]; then
		if (( loops + 1000 < last_loops )); then
			hang_reason="${hang_reason:-reboot/crash: g_main_loops ${last_loops}->${loops}}"
		fi
	fi

	# Frame progress: g_render_frames_total must advance when SPI should run.
	FRAMES_STARVE_POLLS="${FRAMES_STARVE_POLLS:-3}"
	mt_busy_n="${mt_busy:-0}"
	spi_trip_n="${spi_trip:-0}"
	if [[ -n "$frames" && "$frames" =~ ^[0-9]+$ ]]; then
		if [[ "$mt_busy_n" != "1" && "$spi_trip_n" != "1" ]]; then
			if [[ -n "${last_frames:-}" && "$frames" == "$last_frames" ]]; then
				frames_zero_streak=$((frames_zero_streak + 1))
			else
				frames_zero_streak=0
			fi
		else
			frames_zero_streak=0
		fi
		last_frames=$frames
		if (( frames_zero_streak >= FRAMES_STARVE_POLLS )) && \
			[[ -n "$loops" && -n "$last_loops" && "$loops" != "$last_loops" ]]; then
			hang_reason="${hang_reason:-panel SPI starved (frames stuck at $frames for ${FRAMES_STARVE_POLLS} polls while SPI should run)}"
		fi
	fi

	if [[ -n "$hang_reason" ]]; then
		log "*** HANG DETECTED: $hang_reason ***"
		KEEP_OPENOCD=1
		gdb_batch \
			-ex "monitor halt" \
			-ex "hang_dump" \
			-ex "detach" 2>&1 | tee -a "$LOG"
		log "target HALTED — OpenOCD still on :$GDB_PORT (pid $OCD_PID)"
		log "  $GDB -ex 'target extended-remote :$GDB_PORT' '$ELF'"
		log "Kill OpenOCD when done: kill $OCD_PID"
		OCD_PID=""
		trap - EXIT
		exit 0
	fi
done
