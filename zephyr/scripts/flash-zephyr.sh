#!/usr/bin/env bash
# Flash Zephyr apps on Waveshare ESP32-S3-LCD-1.47B (avoids repo path spaces in west).
set -euo pipefail

APP="${1:-handshake}"
PORT="${PORT:-}"
BOARD=esp32s3_lcd_147b/esp32s3/procpu
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
BOARD_ROOT="$HOME/zephyrproject/waveshare-board-root"
LINK="$HOME/zephyrproject/waveshare-${APP}"

pick_port() {
	if [[ -n "${PORT:-}" && -e "$PORT" ]]; then
		echo "$PORT"
		return
	fi
	for p in /dev/ttyACM0 /dev/ttyACM1 /dev/ttyACM2; do
		if [[ -e "$p" ]]; then
			echo "$p"
			return
		fi
	done
	echo "/dev/ttyACM0"
}

PORT="$(pick_port)"

case "$APP" in
  smoke|handshake) ;;
  *)
    echo "Usage: $0 [smoke|handshake]" >&2
    exit 1
    ;;
esac

mkdir -p "$HOME/zephyrproject"
ln -sfn "$REPO/zephyr/app/${APP}" "$LINK"
ln -sfn "$REPO/zephyr" "$BOARD_ROOT"

HCI_UPSTREAM="$HOME/zephyrproject/zephyr/drivers/bluetooth/hci/hci_esp32.c"
HCI_PATCH="$REPO/zephyr/platform/hci_esp32.c"
HCI_BACKUP=""
BT_WQ_PATCH="$REPO/zephyr/platform/patch-bt-hci-unified-wq.patch"
BT_WQ_APPLY="$REPO/zephyr/platform/apply-bt-hci-unified-wq.sh"
BT_WQ_APPLIED=0
BT_LONG_WQ_APPLY="$REPO/zephyr/platform/apply-bt-long-wq-stack.sh"
BT_LONG_WQ_APPLIED=0
if [[ -f "$HCI_PATCH" && "${APPLY_HCI_DEFER:-1}" == "1" ]]; then
	HCI_BACKUP="$(mktemp)"
	cp "$HCI_UPSTREAM" "$HCI_BACKUP"
	cp "$HCI_PATCH" "$HCI_UPSTREAM"
	echo "Applied deferred VHCI HCI driver (APPLY_HCI_DEFER=0 to skip)"
fi

cleanup_patches() {
	if [[ -n "$HCI_BACKUP" && -f "$HCI_BACKUP" ]]; then
		cp "$HCI_BACKUP" "$HCI_UPSTREAM"
		rm -f "$HCI_BACKUP"
	fi
	if [[ "$BT_WQ_APPLIED" == "1" && -x "$BT_WQ_APPLY" ]]; then
		# Best-effort revert: restore from git if tree is clean enough.
		cd "$HOME/zephyrproject/zephyr"
		git checkout -- subsys/bluetooth/host/hci_core.c subsys/bluetooth/host/hci_core.h subsys/bluetooth/host/conn.c subsys/bluetooth/host/l2cap.c 2>/dev/null || true
	fi
	if [[ "$BT_LONG_WQ_APPLIED" == "1" && -x "$BT_LONG_WQ_APPLY" ]]; then
		cd "$HOME/zephyrproject/zephyr"
		git checkout -- subsys/bluetooth/host/Kconfig 2>/dev/null || true
	fi
}
if [[ -n "$HCI_BACKUP" || -f "$BT_WQ_PATCH" || -x "$BT_LONG_WQ_APPLY" ]]; then
	trap cleanup_patches EXIT
fi

if [[ -x "$BT_WQ_APPLY" && "${APPLY_BT_HCI_WQ_PATCH:-1}" == "1" ]]; then
	cd "$HOME/zephyrproject/zephyr"
	if ! "$BT_WQ_APPLY" "$HOME/zephyrproject/zephyr"; then
		echo "ERROR: BT HCI unified workqueue patch failed" >&2
		exit 1
	fi
	BT_WQ_APPLIED=1
	if ! grep -q 'bt_hci_wq_submit(&conn->tx_complete_work)' \
		"$HOME/zephyrproject/zephyr/subsys/bluetooth/host/hci_core.c"; then
		echo "ERROR: hci_core.c still submits conn TX work to sysworkq" >&2
		exit 1
	fi
	if ! grep -q 'bt_hci_wq_submit(&chan->rx_work)' \
		"$HOME/zephyrproject/zephyr/subsys/bluetooth/host/l2cap.c"; then
		echo "ERROR: l2cap.c still submits RX work to sysworkq" >&2
		exit 1
	fi
	echo "Verified: HCI TX + L2CAP RX on BT workqueue (not sysworkq)"
fi

if [[ -x "$BT_LONG_WQ_APPLY" && "${APPLY_BT_LONG_WQ_PATCH:-1}" == "1" ]]; then
	cd "$HOME/zephyrproject/zephyr"
	if ! "$BT_LONG_WQ_APPLY" "$HOME/zephyrproject/zephyr"; then
		echo "ERROR: BT_LONG_WQ_STACK_SIZE patch failed" >&2
		exit 1
	fi
	BT_LONG_WQ_APPLIED=1
	if ! grep -q 'default 3072 if BT_GATT_CACHING' \
		"$HOME/zephyrproject/zephyr/subsys/bluetooth/host/Kconfig"; then
		echo "ERROR: BT_LONG_WQ_STACK_SIZE still defaults to 1300/2048" >&2
		exit 1
	fi
	echo "Verified: BT_LONG_WQ_STACK_SIZE bumped to 3072 (zephyr #92224)"
fi

source "$HOME/zephyrproject/.venv/bin/activate"
source "$HOME/.zephyrrc"
cd "$HOME/zephyrproject/zephyr"

BUILD_EXTRA=(-DBOARD_ROOT="$BOARD_ROOT")
if [[ "$APP" == "handshake" && "${CRASH_DEBUG:-1}" == "1" && -f "$LINK/prj_crash.conf" ]]; then
	echo "crash debug: merging prj_crash.conf (CRASH_DEBUG=0 to skip)"
	BUILD_EXTRA+=(-DEXTRA_CONF_FILE="$LINK/prj_crash.conf")
fi

west build -p always -b "$BOARD" "$LINK" -- "${BUILD_EXTRA[@]}"

ESPTOOL="$HOME/zephyrproject/modules/hal/espressif/tools/esptool_py/esptool.py"
PY="$HOME/zephyrproject/.venv/bin/python3"
BIN="$HOME/zephyrproject/zephyr/build/zephyr/zephyr.bin"

flash_esptool() {
	local before="$1"
	local after="$2"
	"$PY" "$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 921600 \
		--before "$before" --after "$after" write_flash -u \
		--flash_mode dio --flash_freq 40m --flash_size 16MB \
		0x0 "$BIN"
}

flash_ok=0
if west flash --esp-device "$PORT" 2>/dev/null; then
	flash_ok=1
elif [[ -f "$ESPTOOL" && -f "$BIN" ]]; then
	echo "west flash failed — trying esptool (usb_reset) on ${PORT}" >&2
	if flash_esptool usb_reset hard_reset 2>/dev/null; then
		flash_ok=1
	else
		echo "" >&2
		echo ">>> ESP32-S3: hold BOOT, tap RESET, release BOOT (download mode)" >&2
		echo ">>> Waiting 8s — perform boot+reset sequence now..." >&2
		sleep 8
		if flash_esptool no_reset hard_reset; then
			flash_ok=1
		fi
	fi
fi

if [[ "$flash_ok" != "1" ]]; then
	echo "ERROR: flash failed on ${PORT}" >&2
	exit 1
fi

echo "Flashed zephyr/app/${APP} to ${PORT}"

# ESP32-S3 native USB CDC often misses the first boot after esptool hard_reset.
# A second reset via esptool run matches the manual BOOT+RESET recovery users do.
if [[ -f "$ESPTOOL" ]]; then
	echo "Post-flash app reset (esptool run) — avoids USB/display wedge until BOOT+RESET" >&2
	sleep 2.0
	"$PY" "$ESPTOOL" --port "$PORT" --baud 115200 run >/dev/null 2>&1 || true
	sleep 1.5
fi

CAPTURE_SEC="${CAPTURE_SEC:-60}"
if [[ -x "$SCRIPT_DIR/capture-serial-boot.sh" ]]; then
	CAPTURE_LEN="$((CAPTURE_SEC > 35 ? CAPTURE_SEC : 35))"
	echo "Capturing boot log (${CAPTURE_LEN}s on ${PORT})..."
	LOG_FILE="$(mktemp /tmp/zephyr-boot-XXXXXX.log)"
	"$SCRIPT_DIR/capture-serial-boot.sh" "$PORT" "$CAPTURE_LEN" "$LOG_FILE" || true
	if [[ -s "$LOG_FILE" ]]; then
		if [[ -x "$SCRIPT_DIR/verify-boot-log.sh" ]]; then
			"$SCRIPT_DIR/verify-boot-log.sh" "$LOG_FILE" || {
				echo "WARN: boot log verification failed — see ${LOG_FILE}" >&2
			}
		fi
	else
		echo "WARN: boot log capture failed — tap RESET during capture and re-run:" >&2
		echo "  SKIP_RESET=1 $SCRIPT_DIR/capture-serial-boot.sh $PORT 15" >&2
	fi
fi
