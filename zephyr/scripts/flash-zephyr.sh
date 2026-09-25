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
ln -sfn "$REPO/zephyr/app/common" "$HOME/zephyrproject/waveshare-common"
ln -sfn "$REPO/zephyr" "$BOARD_ROOT"

HCI_UPSTREAM="$HOME/zephyrproject/zephyr/drivers/bluetooth/hci/hci_esp32.c"
HCI_PATCH="$REPO/zephyr/platform/hci_esp32.c"
HCI_BACKUP=""
HCI_LITERALS_APPLY="$REPO/zephyr/platform/apply-hci-text-literals.sh"
HCI_LITERALS_APPLIED=0
BT_WQ_PATCH="$REPO/zephyr/platform/patch-bt-hci-unified-wq.patch"
BT_WQ_APPLY="$REPO/zephyr/platform/apply-bt-hci-unified-wq.sh"
BT_WQ_APPLIED=0
BT_LONG_WQ_APPLY="$REPO/zephyr/platform/apply-bt-long-wq-stack.sh"
BT_LONG_WQ_APPLIED=0
WIFI_DEFER_APPLY="$REPO/zephyr/platform/apply-esp32-wifi-defer-start.sh"
WIFI_DEFER_APPLIED=0
WIFI_DMA_APPLY="$REPO/zephyr/platform/apply-esp32-wifi-dma-dram.sh"
WIFI_DMA_APPLIED=0
PSRAM_SMH_APPLY="$REPO/zephyr/platform/apply-esp32-psram-smh-defer.sh"
PSRAM_SMH_APPLIED=0
BT_CORE_PIN_APPLY="$REPO/zephyr/platform/apply-esp32-bt-ctrl-core-pin.sh"
BT_CORE_PIN_APPLIED=0
HCI_TIMEOUT_APPLY="$REPO/zephyr/platform/apply-hci-cmd-timeout-soft.sh"
HCI_TIMEOUT_APPLIED=0
HCI_SEND_POLL_APPLY="$REPO/zephyr/platform/apply-hci-esp32-send-poll.sh"
HCI_SEND_POLL_APPLIED=0
SPI_USR_TIMEOUT_APPLY="$REPO/zephyr/platform/apply-spi-usr-done-timeout.sh"
SPI_USR_TIMEOUT_APPLIED=0
# platform/hci_esp32.c is a Zephyr 3.7 deferred-VHCI overlay. On 4.x the
# upstream driver is a full DEVICE_DT_INST HCI (open/send/close) — overwriting
# it caused early DoubleExceptionVector after PSRAM init. Skip unless forced.
# WiFi defer-start is likewise in-tree on 4.4+.
ZEPHYR_MAJOR="$(awk '/^VERSION_MAJOR/{print $3; exit}' "$HOME/zephyrproject/zephyr/VERSION" 2>/dev/null || echo 0)"
if [[ "${APPLY_HCI_DEFER:-}" == "1" ]]; then
	:
elif [[ "${APPLY_HCI_DEFER:-1}" == "0" || "${ZEPHYR_MAJOR:-0}" -ge 4 ]]; then
	APPLY_HCI_DEFER=0
fi
if [[ "${APPLY_WIFI_DEFER:-}" == "1" ]]; then
	:
elif [[ "${APPLY_WIFI_DEFER:-1}" == "0" || "${ZEPHYR_MAJOR:-0}" -ge 4 ]]; then
	APPLY_WIFI_DEFER=0
fi
if [[ -f "$HCI_PATCH" && "${APPLY_HCI_DEFER:-1}" == "1" ]]; then
	HCI_BACKUP="$(mktemp)"
	cp "$HCI_UPSTREAM" "$HCI_BACKUP"
	cp "$HCI_PATCH" "$HCI_UPSTREAM"
	echo "Applied deferred VHCI HCI driver (APPLY_HCI_DEFER=0 to skip)"
elif [[ "${ZEPHYR_MAJOR:-0}" -ge 4 ]]; then
	echo "Skipping deferred HCI overlay (Zephyr ${ZEPHYR_MAJOR}.x uses upstream hci_esp32.c)"
fi
if [[ "${ZEPHYR_MAJOR:-0}" -ge 4 && "${APPLY_WIFI_DEFER:-0}" == "0" ]]; then
	echo "Skipping WiFi defer-start overlay (Zephyr ${ZEPHYR_MAJOR}.x in-tree)"
fi

cleanup_patches() {
	if [[ -n "$HCI_BACKUP" && -f "$HCI_BACKUP" ]]; then
		cp "$HCI_BACKUP" "$HCI_UPSTREAM"
		rm -f "$HCI_BACKUP"
	fi
	if [[ "$HCI_LITERALS_APPLIED" == "1" ]]; then
		cd "$HOME/zephyrproject/zephyr"
		git checkout -- drivers/bluetooth/hci/CMakeLists.txt 2>/dev/null || true
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
	if [[ "$WIFI_DEFER_APPLIED" == "1" ]]; then
		cd "$HOME/zephyrproject/zephyr"
		git checkout -- drivers/wifi/esp32/src/esp_wifi_drv.c \
			drivers/wifi/esp32/Kconfig.esp32 2>/dev/null || true
	fi
	if [[ "$WIFI_DMA_APPLIED" == "1" ]]; then
		git -C "$HOME/zephyrproject/modules/hal/espressif" checkout -- \
			zephyr/port/heap/heap_caps_zephyr.c \
			zephyr/esp32s3/src/wifi/esp_wifi_adapter.c \
			components/esp_coex/esp32s3/esp_coex_adapter.c 2>/dev/null || true
	fi
	if [[ "$PSRAM_SMH_APPLIED" == "1" ]]; then
		cd "$HOME/zephyrproject/zephyr"
		git checkout -- soc/espressif/esp32s3/soc.c 2>/dev/null || true
	fi
	if [[ "$HCI_TIMEOUT_APPLIED" == "1" ]]; then
		cd "$HOME/zephyrproject/zephyr"
		git checkout -- subsys/bluetooth/host/hci_core.c 2>/dev/null || true
	fi
	if [[ "$HCI_SEND_POLL_APPLIED" == "1" ]]; then
		cd "$HOME/zephyrproject/zephyr"
		git checkout -- drivers/bluetooth/hci/hci_esp32.c 2>/dev/null || true
	fi
	if [[ "$SPI_USR_TIMEOUT_APPLIED" == "1" ]]; then
		cd "$HOME/zephyrproject/zephyr"
		git checkout -- drivers/spi/spi_esp32_spim.c 2>/dev/null || true
	fi
}
if [[ -n "$HCI_BACKUP" || -f "$BT_WQ_PATCH" || -x "$BT_LONG_WQ_APPLY" || -x "$HCI_LITERALS_APPLY" || -x "$WIFI_DEFER_APPLY" || -x "$WIFI_DMA_APPLY" || -x "$PSRAM_SMH_APPLY" || -x "$HCI_TIMEOUT_APPLY" || -x "$HCI_SEND_POLL_APPLY" || -x "$SPI_USR_TIMEOUT_APPLY" ]]; then
	trap cleanup_patches EXIT
fi

if [[ -x "$HCI_LITERALS_APPLY" && "${APPLY_HCI_DEFER:-1}" == "1" ]]; then
	cd "$HOME/zephyrproject/zephyr"
	if ! "$HCI_LITERALS_APPLY" "$HOME/zephyrproject/zephyr"; then
		echo "ERROR: hci_esp32.c -mtext-section-literals patch failed" >&2
		exit 1
	fi
	HCI_LITERALS_APPLIED=1
	if ! grep -q 'mtext-section-literals' \
		"$HOME/zephyrproject/zephyr/drivers/bluetooth/hci/CMakeLists.txt"; then
		echo "ERROR: hci_esp32.c still missing -mtext-section-literals" >&2
		exit 1
	fi
	echo "Verified: hci_esp32.c compiled with -mtext-section-literals"
fi

# Obsolete on Zephyr 4.4+ (in-tree TX WQ). Default off; APPLY_BT_HCI_WQ_PATCH=1 for 3.7 trees.
if [[ -x "$BT_WQ_APPLY" && "${APPLY_BT_HCI_WQ_PATCH:-0}" == "1" ]]; then
	cd "$HOME/zephyrproject/zephyr"
	if ! "$BT_WQ_APPLY" "$HOME/zephyrproject/zephyr"; then
		echo "ERROR: BT HCI unified workqueue patch failed" >&2
		exit 1
	fi
	BT_WQ_APPLIED=1
	if grep -q 'tx_notify_workqueue_get' \
		"$HOME/zephyrproject/zephyr/subsys/bluetooth/host/conn.c" && \
	   grep -q 'bt_tx_processor_workq' \
		"$HOME/zephyrproject/zephyr/subsys/bluetooth/host/hci_core.c" && \
	   ! grep -q 'bt_hci_wq_submit' \
		"$HOME/zephyrproject/zephyr/subsys/bluetooth/host/hci_core.h"; then
		echo "Verified: BT TX workqueue in-tree (Zephyr 4.4+)"
	elif ! grep -q 'bt_hci_wq_submit(&chan->rx_work)' \
		"$HOME/zephyrproject/zephyr/subsys/bluetooth/host/l2cap.c"; then
		echo "ERROR: l2cap.c still submits RX work to sysworkq" >&2
		exit 1
	elif ! grep -q 'bt_hci_wq_submit' \
		"$HOME/zephyrproject/zephyr/subsys/bluetooth/host/hci_core.h"; then
		echo "ERROR: BT HCI unified workqueue patch incomplete" >&2
		exit 1
	else
		echo "Verified: HCI TX + L2CAP RX on BT workqueue (not sysworkq)"
	fi
fi

if [[ -x "$HCI_TIMEOUT_APPLY" && "${APPLY_HCI_TIMEOUT_SOFT:-1}" == "1" ]]; then
	cd "$HOME/zephyrproject/zephyr"
	if ! "$HCI_TIMEOUT_APPLY" "$HOME/zephyrproject/zephyr"; then
		echo "ERROR: HCI cmd timeout soft-fail patch failed" >&2
		exit 1
	fi
	HCI_TIMEOUT_APPLIED=1
	if ! grep -q 'HCI cmd timeout is non-fatal' \
		"$HOME/zephyrproject/zephyr/subsys/bluetooth/host/hci_core.c"; then
		echo "ERROR: hci_core.c still asserts on HCI command timeout" >&2
		exit 1
	fi
	echo "Verified: HCI command timeout returns -ETIMEDOUT (no kernel oops)"
fi

if [[ -x "$HCI_SEND_POLL_APPLY" && "${APPLY_HCI_SEND_POLL:-1}" == "1" && "${APPLY_HCI_DEFER:-0}" != "1" ]]; then
	cd "$HOME/zephyrproject/zephyr"
	if ! "$HCI_SEND_POLL_APPLY" "$HOME/zephyrproject/zephyr"; then
		echo "ERROR: HCI ESP32 send-poll patch failed" >&2
		exit 1
	fi
	HCI_SEND_POLL_APPLIED=1
	if ! grep -q 'HCI TX polls VHCI ready' \
		"$HOME/zephyrproject/zephyr/drivers/bluetooth/hci/hci_esp32.c"; then
		echo "ERROR: hci_esp32.c still uses blocking k_sem_take for TX" >&2
		exit 1
	fi
	echo "Verified: HCI TX polls VHCI (no ISR k_sem_take)"
fi

if [[ -x "$SPI_USR_TIMEOUT_APPLY" && "${APPLY_SPI_USR_TIMEOUT:-1}" == "1" ]]; then
	cd "$HOME/zephyrproject/zephyr"
	if ! "$SPI_USR_TIMEOUT_APPLY" "$HOME/zephyrproject/zephyr"; then
		echo "ERROR: SPI USR-done timeout patch failed" >&2
		exit 1
	fi
	SPI_USR_TIMEOUT_APPLIED=1
	if ! grep -q 'SPI bus quiet v288' \
		"$HOME/zephyrproject/zephyr/drivers/spi/spi_esp32_spim.c"; then
		echo "ERROR: spi_esp32_spim.c still has unbounded USR busy-wait" >&2
		exit 1
	fi
	if ! grep -q 'SPI USR-done outer polled v286' \
		"$HOME/zephyrproject/zephyr/drivers/spi/spi_esp32_spim.c"; then
		echo "ERROR: polled SPI outer loop still ignores transfer() ret" >&2
		exit 1
	fi
	echo "Verified: SPI USR-done wait v286 + polled outer ret check"
fi

if [[ -x "$BT_LONG_WQ_APPLY" && "${APPLY_BT_LONG_WQ_PATCH:-1}" == "1" ]]; then
	cd "$HOME/zephyrproject/zephyr"
	if ! "$BT_LONG_WQ_APPLY" "$HOME/zephyrproject/zephyr"; then
		echo "ERROR: BT_LONG_WQ_STACK_SIZE patch failed" >&2
		exit 1
	fi
	BT_LONG_WQ_APPLIED=1
	if ! grep -qE 'default 3072 if BT_ECC' \
		"$HOME/zephyrproject/zephyr/subsys/bluetooth/host/Kconfig"; then
		echo "ERROR: BT_LONG_WQ_STACK_SIZE BT_ECC default not bumped to 3072" >&2
		exit 1
	fi
	echo "Verified: BT_LONG_WQ_STACK_SIZE forced to 3072 (incl. BT_ECC; was 1400 @98%)"
fi

if [[ -x "$WIFI_DEFER_APPLY" && "${APPLY_WIFI_DEFER:-1}" == "1" ]]; then
	cd "$HOME/zephyrproject/zephyr"
	if ! "$WIFI_DEFER_APPLY" "$HOME/zephyrproject/zephyr"; then
		echo "ERROR: ESP32 WiFi defer-start patch failed" >&2
		exit 1
	fi
	WIFI_DEFER_APPLIED=1
	DRV_WIFI="$HOME/zephyrproject/zephyr/drivers/wifi/esp32/src/esp_wifi_drv.c"
	if grep -q 'not required on Zephyr 4.4+' "$DRV_WIFI"; then
		echo "Verified: WiFi start deferred in-tree (Zephyr 4.4+)"
	else
		if ! grep -q 'esp32_wifi_radio_ensure' "$DRV_WIFI"; then
			echo "ERROR: esp_wifi_drv.c still starts WiFi in iface init" >&2
			exit 1
		fi
		if ! grep -q 'esp32_wifi_radio_release' "$DRV_WIFI"; then
			echo "ERROR: esp_wifi_drv.c missing WiFi radio_release (BLE coexist)" >&2
			exit 1
		fi
		if ! grep -q 'WiFi scan start (IDF default config)' "$DRV_WIFI"; then
			echo "ERROR: esp_wifi_drv.c missing Arduino-like IDF default scan" >&2
			exit 1
		fi
		echo "Verified: WiFi radio deferred until scan/connect (+ release after scan)"
	fi
fi

if [[ -x "$WIFI_DMA_APPLY" && "${APPLY_WIFI_DMA_DRAM:-1}" == "1" ]]; then
	if ! "$WIFI_DMA_APPLY" "$HOME/zephyrproject"; then
		echo "ERROR: ESP32 WiFi DMA-in-DRAM heap patch failed" >&2
		exit 1
	fi
	WIFI_DMA_APPLIED=1
	if ! grep -q 'heap_caps_internal_dram' \
		"$HOME/zephyrproject/modules/hal/espressif/zephyr/port/heap/heap_caps_zephyr.c"; then
		echo "ERROR: heap_caps_zephyr.c still ignores MALLOC_CAP_DMA" >&2
		exit 1
	fi
	DMA_ADAPTER="$HOME/zephyrproject/modules/hal/espressif/components/esp_coex/esp32s3/esp_coex_adapter.c"
	[[ -f "$DMA_ADAPTER" ]] || \
		DMA_ADAPTER="$HOME/zephyrproject/modules/hal/espressif/zephyr/esp32s3/src/wifi/esp_wifi_adapter.c"
	if ! awk '
		/malloc_internal_wrapper|esp_coex_common_malloc_internal_wrapper/ { in_fn = 1 }
		in_fn && /__real_k_malloc/ { found = 1 }
		in_fn && /^}/ { if (found) { ok = 1; exit }; in_fn = 0; found = 0 }
		END { exit !ok }
	' "$DMA_ADAPTER"; then
		echo "ERROR: WiFi malloc_internal still uses wifi_malloc/PSRAM ($DMA_ADAPTER)" >&2
		exit 1
	fi
	echo "Verified: WiFi DMA/INTERNAL + malloc_internal stay in DRAM"
fi

if [[ -x "$PSRAM_SMH_APPLY" && "${APPLY_PSRAM_SMH_DEFER:-1}" == "1" ]]; then
	if ! "$PSRAM_SMH_APPLY" "$HOME/zephyrproject/zephyr"; then
		echo "ERROR: ESP32 PSRAM SMH defer patch failed" >&2
		exit 1
	fi
	PSRAM_SMH_APPLIED=1
	if grep -q 'esp_psram_smh_init();' \
		"$HOME/zephyrproject/zephyr/soc/espressif/esp32s3/soc.c"; then
		echo "ERROR: soc.c still calls esp_psram_smh_init" >&2
		exit 1
	fi
	echo "Verified: PSRAM SMH registration skipped (Zephyr 4.4 S3)"
fi

if [[ -x "$BT_CORE_PIN_APPLY" && "${APPLY_BT_CORE_PIN:-1}" == "1" ]]; then
	if ! "$BT_CORE_PIN_APPLY" "$HOME/zephyrproject/zephyr"; then
		echo "ERROR: ESP32 BT controller core-pin prompt patch failed" >&2
		exit 1
	fi
	BT_CORE_PIN_APPLIED=1
	if ! grep -q 'BT controller CPU core (0 or 1)' \
		"$HOME/zephyrproject/modules/hal/espressif/zephyr/Kconfig"; then
		echo "ERROR: ESP32_BT_CTLR_PINNED_TO_CORE still has no prompt" >&2
		exit 1
	fi
	echo "Verified: BT controller core pin is user-configurable"
fi

source "$HOME/zephyrproject/.venv/bin/activate"
source "$HOME/.zephyrrc"
cd "$HOME/zephyrproject/zephyr"

# Re-apply critical SPI timeout immediately before build. Earlier apply can be
# lost if something restores upstream spi_esp32_spim.c (or a parallel west
# tree sync). Unbounded USR busy-wait → silent MSPI wedge / DoubleException.
if [[ -x "$SPI_USR_TIMEOUT_APPLY" && "${APPLY_SPI_USR_TIMEOUT:-1}" == "1" ]]; then
	if ! "$SPI_USR_TIMEOUT_APPLY" "$HOME/zephyrproject/zephyr"; then
		echo "ERROR: SPI USR-done timeout re-apply before build failed" >&2
		exit 1
	fi
	if ! grep -q 'SPI bus quiet v288' \
		"$HOME/zephyrproject/zephyr/drivers/spi/spi_esp32_spim.c"; then
		echo "ERROR: spi_esp32_spim.c missing USR timeout before west build" >&2
		exit 1
	fi
fi

BUILD_DIR="$HOME/zephyrproject/zephyr/build"
MCUBOOT_KEY="$REPO/zephyr/mcuboot/root-ec-p256.pem"
BUILD_EXTRA=(-DBOARD_ROOT="$BOARD_ROOT")
WEST_SYSBUILD=()
if [[ "$APP" == "handshake" ]]; then
	if [[ ! -f "$MCUBOOT_KEY" ]]; then
		echo "ERROR: missing $MCUBOOT_KEY (see zephyr/mcuboot/KEYS)" >&2
		exit 1
	fi
	WEST_SYSBUILD=(--sysbuild)
	# Kconfig rejects spaces in SB_CONFIG_BOOT_SIGNATURE_KEY_FILE; repo path has spaces.
	KEY_FOR_WEST="$HOME/zephyrproject/imu-mcuboot-root-ec-p256.pem"
	cp -a "$MCUBOOT_KEY" "$KEY_FOR_WEST"
	BUILD_EXTRA+=(-DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE="\"${KEY_FOR_WEST}\"")
	BUILD_EXTRA+=(-Dmcuboot_BOARD_ROOT="$BOARD_ROOT")
	FW_VER_H="$REPO/zephyr/app/common/fw_version.h"
	FW_CODE="$(sed -n 's/^#define[[:space:]]\+FW_VERSION_CODE[[:space:]]\+\([0-9]\+\).*/\1/p' "$FW_VER_H" | head -1)"
	if [[ -n "$FW_CODE" ]]; then
		BUILD_EXTRA+=(-DCONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION="\"0.0.${FW_CODE}\"")
		BUILD_EXTRA+=(-Dwaveshare-handshake_CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION="\"0.0.${FW_CODE}\"")
		echo "imgtool sign version 0.0.${FW_CODE}"
	fi
fi
if [[ "$APP" == "handshake" && "${CRASH_DEBUG:-1}" == "1" && -f "$LINK/prj_crash.conf" ]]; then
	echo "crash debug: merging prj_crash.conf (CRASH_DEBUG=0 for release)"
	BUILD_EXTRA+=(-DEXTRA_CONF_FILE="$LINK/prj_crash.conf")
	BUILD_EXTRA+=(-Dwaveshare-handshake_EXTRA_CONF_FILE="$LINK/prj_crash.conf")
elif [[ "$APP" == "handshake" && "${CRASH_DEBUG:-1}" != "1" && -f "$LINK/prj_release.conf" ]]; then
	echo "release: merging prj_release.conf"
	BUILD_EXTRA+=(-DEXTRA_CONF_FILE="$LINK/prj_release.conf")
	BUILD_EXTRA+=(-Dwaveshare-handshake_EXTRA_CONF_FILE="$LINK/prj_release.conf")
	if [[ -f "$LINK/panel_40mhz.overlay" ]]; then
		echo "release: panel SPI 40 MHz overlay"
		BUILD_EXTRA+=(-DEXTRA_DTC_OVERLAY_FILE="$LINK/panel_40mhz.overlay")
		BUILD_EXTRA+=(-Dwaveshare-handshake_EXTRA_DTC_OVERLAY_FILE="$LINK/panel_40mhz.overlay")
	fi
fi

west build -p always "${WEST_SYSBUILD[@]}" -d "$BUILD_DIR" -b "$BOARD" "$LINK" -- "${BUILD_EXTRA[@]}"

ESPTOOL="$HOME/zephyrproject/modules/hal/espressif/tools/esptool_py/esptool.py"
PY="$HOME/zephyrproject/.venv/bin/python3"
# Prefer `python -m esptool` (venv package) over the thin wrapper script.
if "$PY" -c 'import esptool' 2>/dev/null; then
	ESPTOOL_CMD=("$PY" -m esptool)
elif [[ -f "$ESPTOOL" ]]; then
	ESPTOOL_CMD=("$PY" "$ESPTOOL")
else
	ESPTOOL_CMD=()
fi
APP_FLASH_ADDR=0x10000
MCUBOOT_FLASH_ADDR=0x0

find_build_file() {
	local name="$1"
	local under="${2:-}"
	if [[ -n "$under" && -d "$BUILD_DIR/$under" ]]; then
		find "$BUILD_DIR/$under" -name "$name" -type f -print | head -1
		return
	fi
	find "$BUILD_DIR" \( -path '*mcuboot*' -prune \) -o \( -name "$name" -type f -print \) | head -1
}

BIN="$(find_build_file zephyr.bin)"
MCUBOOT_BIN="$(find_build_file zephyr.bin mcuboot)"
SIGNED_CONFIRMED="$(find_build_file zephyr.signed.confirmed.bin)"
if [[ -n "$SIGNED_CONFIRMED" ]]; then
	BIN="$SIGNED_CONFIRMED"
fi

flash_esptool() {
	local before="$1"
	local after="$2"
	shift 2
	if [[ ${#ESPTOOL_CMD[@]} -eq 0 ]]; then
		return 1
	fi
	"${ESPTOOL_CMD[@]}" --chip esp32s3 --port "$PORT" --baud 921600 \
		--before "$before" --after "$after" write-flash -u \
		--flash-mode dio --flash-freq 40m --flash-size 16MB \
		"$@"
}

try_esptool() {
	local before="$1"
	local after="$2"
	if [[ -n "$MCUBOOT_BIN" && -f "$MCUBOOT_BIN" && -n "$BIN" && -f "$BIN" ]]; then
		echo "esptool mcuboot @${MCUBOOT_FLASH_ADDR} + app @${APP_FLASH_ADDR}" >&2
		flash_esptool "$before" "$after" "$MCUBOOT_FLASH_ADDR" "$MCUBOOT_BIN" \
			"$APP_FLASH_ADDR" "$BIN"
	elif [[ -n "$BIN" && -f "$BIN" ]]; then
		echo "esptool app only @${APP_FLASH_ADDR} (mcuboot already on device?)" >&2
		flash_esptool "$before" "$after" "$APP_FLASH_ADDR" "$BIN"
	else
		return 1
	fi
}

flash_ok=0
# Prefer explicit esptool layout: mcuboot@0x0 + signed app@0x10000.
# `west flash` on sysbuild used to inherit slot0's FLASH_LOAD_OFFSET for mcuboot
# (board chosen code-partition), so the bootloader at 0x0 never got rewritten and
# kept an old 2MB ESP image header (spi_flash "Detected size 16384k … header 2048k").
if [[ ${#ESPTOOL_CMD[@]} -gt 0 ]] && try_esptool usb_reset hard_reset; then
	flash_ok=1
elif west flash -d "$BUILD_DIR" --esp-device "$PORT"; then
	flash_ok=1
elif [[ ${#ESPTOOL_CMD[@]} -gt 0 ]]; then
	echo "west flash failed — trying esptool download-mode on ${PORT}" >&2
	echo "" >&2
	echo ">>> ESP32-S3: hold BOOT, tap RESET, release BOOT (download mode)" >&2
	echo ">>> Waiting 8s — perform boot+reset sequence now..." >&2
	sleep 8
	if try_esptool no_reset hard_reset; then
		flash_ok=1
	fi
fi

if [[ "$flash_ok" != "1" ]]; then
	echo "ERROR: flash failed on ${PORT}" >&2
	exit 1
fi

echo "Flashed zephyr/app/${APP} to ${PORT}"

# Build-time integrity fingerprint (MCUboot TLV already signed; this is desk audit).
if command -v sha256sum >/dev/null 2>&1; then
	echo "image SHA256 (local artifact, pre-flash):" >&2
	sha256sum "$BIN" >&2 || true
	if [[ -n "$MCUBOOT_BIN" && -f "$MCUBOOT_BIN" ]]; then
		sha256sum "$MCUBOOT_BIN" >&2 || true
	fi
fi

OUT_DIR="$REPO/out/zephyr"
mkdir -p "$OUT_DIR"
OTA_BIN="$(find_build_file zephyr.signed.bin)"
if [[ -n "$OTA_BIN" && -f "$OTA_BIN" ]]; then
	cp -a "$OTA_BIN" "$OUT_DIR/zephyr.signed.bin"
	cp -a "$OTA_BIN" "$OUT_DIR/zephyr.bin"
fi
if [[ -n "$SIGNED_CONFIRMED" && -f "$SIGNED_CONFIRMED" ]]; then
	cp -a "$SIGNED_CONFIRMED" "$OUT_DIR/zephyr.signed.confirmed.bin"
fi
if [[ -n "$MCUBOOT_BIN" && -f "$MCUBOOT_BIN" ]]; then
	cp -a "$MCUBOOT_BIN" "$OUT_DIR/mcuboot.bin"
fi
ELF="$(find_build_file zephyr.elf)"
if [[ -n "$ELF" && -f "$ELF" ]]; then
	cp -a "$ELF" "$OUT_DIR/zephyr.elf"
	# Keep a version-pinned ELF so overnight dumps are not re-symbolicated
	# against a later desk rebuild (same PC → wrong function names).
	FW_CODE_PIN="$(sed -n 's/^#define[[:space:]]\+FW_VERSION_CODE[[:space:]]\+\([0-9]\+\).*/\1/p' \
		"$REPO/zephyr/app/common/fw_version.h" | head -1)"
	if [[ -n "$FW_CODE_PIN" ]]; then
		cp -a "$ELF" "$OUT_DIR/zephyr-v${FW_CODE_PIN}.elf"
		echo "archived ELF → out/zephyr/zephyr-v${FW_CODE_PIN}.elf" >&2
	fi
fi

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
