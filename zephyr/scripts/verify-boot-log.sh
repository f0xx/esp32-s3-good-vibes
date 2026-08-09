#!/usr/bin/env bash
# Sanity-check captured Zephyr boot log text on stdin or file.
set -euo pipefail

LOG="${1:-}"
if [[ -n "$LOG" && -f "$LOG" ]]; then
	text="$(cat "$LOG")"
else
	text="$(cat)"
fi

fail=0
check() {
	local needle="$1"
	if grep -qF "$needle" <<<"$text"; then
		echo "OK  $needle"
	else
		echo "MISS $needle"
		fail=1
	fi
}

echo "boot-log verify:"
check "handshake: main()"
check "stage: main loop"
check "BLE advertising started"
check "framebuffer"
check "backlight on"

# Post-boot heartbeat (10s) when USB CDC misses early boot text.
if ! grep -qF "handshake: main()" <<<"$text"; then
	if grep -qF "telemetry" <<<"$text"; then
		echo "OK  telemetry (late capture)"
	else
		echo "MISS telemetry"
		fail=1
	fi
fi

if grep -qF "BOOT held" <<<"$text"; then
	echo "FAIL spurious BOOT long-press reboot"
	fail=1
fi

boots="$(grep -cF "handshake: main()" <<<"$text" || true)"
if [[ "$boots" -gt 1 ]]; then
	echo "FAIL reboot loop ($boots boots in capture)"
	fail=1
fi

if grep -Ei 'panic|Guru Meditation|abort\(\)|FATAL ERROR|failed to init' <<<"$text"; then
	echo "FAIL suspicious error strings in log"
	fail=1
fi

if [[ "$fail" != "0" ]]; then
	exit 1
fi
echo "boot-log verify: PASS"
