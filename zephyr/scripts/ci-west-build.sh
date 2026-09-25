#!/usr/bin/env bash
# In-container (or host) west build for handshake/smoke. No flash.
# Env:
#   IMU_REPO          git checkout of this project (default: repo root of this script)
#   ZEPHYR_PROJECT    west workspace (default: $HOME/zephyrproject)
#   APP               handshake|smoke (default: handshake)
#   OUT_DIR           where zephyr.bin is copied (default: $IMU_REPO/out/zephyr)
#   BUILD_JOBS        ninja -j (default: nproc)
#   PRISTINE          1 = west -p always (default 1)
#   CRASH_DEBUG       1 = merge prj_crash.conf; 0 = prj_release (default 0 for OTA)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMU_REPO="$(cd "${IMU_REPO:-$SCRIPT_DIR/../..}" && pwd)"
ZEPHYR_PROJECT="$(cd "${ZEPHYR_PROJECT:-${HOME}/zephyrproject}" && pwd)"
APP="${APP:-handshake}"
OUT_DIR="${OUT_DIR:-$IMU_REPO/out/zephyr}"
BOARD=esp32s3_lcd_147b/esp32s3/procpu
BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"
BUILD_DIR="${BUILD_DIR:-$ZEPHYR_PROJECT/zephyr/build}"
PRISTINE="${PRISTINE:-1}"
CRASH_DEBUG="${CRASH_DEBUG:-0}"

case "$APP" in
  smoke|handshake) ;;
  *)
    echo "Usage: APP=handshake|smoke $0" >&2
    exit 1
    ;;
esac

if [[ ! -d "$ZEPHYR_PROJECT/zephyr" ]]; then
  echo "ERROR: ZEPHYR_PROJECT=$ZEPHYR_PROJECT has no zephyr/ (west init + west update first)" >&2
  exit 1
fi

export ZEPHYR_BASE="$ZEPHYR_PROJECT/zephyr"
export ZEPHYR_SDK_INSTALL_DIR="${ZEPHYR_SDK_INSTALL_DIR:-/opt/zephyr-sdk-1.0.1}"
export ZEPHYR_TOOLCHAIN_VARIANT="${ZEPHYR_TOOLCHAIN_VARIANT:-zephyr}"
export CMAKE_BUILD_PARALLEL_LEVEL="$BUILD_JOBS"
git config --global --add safe.directory "$ZEPHYR_PROJECT/zephyr" 2>/dev/null || true
git config --global --add safe.directory "$IMU_REPO" 2>/dev/null || true

LINK="$ZEPHYR_PROJECT/waveshare-${APP}"
BOARD_ROOT="$ZEPHYR_PROJECT/waveshare-board-root"
mkdir -p "$ZEPHYR_PROJECT"
ln -sfn "$IMU_REPO/zephyr/app/${APP}" "$LINK"
# Space-free common path for CMake (Xtensa .file breaks on "My Projects").
# Must live under ZEPHYR_PROJECT — docker-west sets HOME=/tmp with no zephyrproject/.
ln -sfn "$IMU_REPO/zephyr/app/common" "$ZEPHYR_PROJECT/waveshare-common"
# Desk flash also checks $HOME/zephyrproject/waveshare-common.
mkdir -p "${HOME}/zephyrproject"
ln -sfn "$IMU_REPO/zephyr/app/common" "${HOME}/zephyrproject/waveshare-common"
ln -sfn "$IMU_REPO/zephyr" "$BOARD_ROOT"

HCI_UPSTREAM="$ZEPHYR_PROJECT/zephyr/drivers/bluetooth/hci/hci_esp32.c"
HCI_PATCH="$IMU_REPO/zephyr/platform/hci_esp32.c"
HCI_BACKUP=""
HCI_LITERALS_APPLY="$IMU_REPO/zephyr/platform/apply-hci-text-literals.sh"
HCI_LITERALS_APPLIED=0
BT_WQ_APPLY="$IMU_REPO/zephyr/platform/apply-bt-hci-unified-wq.sh"
BT_WQ_APPLIED=0
BT_LONG_WQ_APPLY="$IMU_REPO/zephyr/platform/apply-bt-long-wq-stack.sh"
BT_LONG_WQ_APPLIED=0
WIFI_DEFER_APPLY="$IMU_REPO/zephyr/platform/apply-esp32-wifi-defer-start.sh"
WIFI_DEFER_APPLIED=0
# 3.7 deferred-VHCI overlay must not replace Zephyr 4.x DEVICE_DT_INST HCI.
# WiFi defer-start is also in-tree on 4.4+.
ZEPHYR_MAJOR="$(awk '/^VERSION_MAJOR/{print $3; exit}' "$ZEPHYR_PROJECT/zephyr/VERSION" 2>/dev/null || echo 0)"
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
if [[ "${ZEPHYR_MAJOR:-0}" -ge 4 && "${APPLY_HCI_DEFER:-0}" == "0" ]]; then
  echo "Skipping deferred HCI overlay (Zephyr ${ZEPHYR_MAJOR}.x upstream HCI)"
fi
if [[ "${ZEPHYR_MAJOR:-0}" -ge 4 && "${APPLY_WIFI_DEFER:-0}" == "0" ]]; then
  echo "Skipping WiFi defer-start overlay (Zephyr ${ZEPHYR_MAJOR}.x in-tree)"
fi
WIFI_DMA_APPLY="$IMU_REPO/zephyr/platform/apply-esp32-wifi-dma-dram.sh"
WIFI_DMA_APPLIED=0
PSRAM_SMH_APPLY="$IMU_REPO/zephyr/platform/apply-esp32-psram-smh-defer.sh"
PSRAM_SMH_APPLIED=0
BT_CORE_PIN_APPLY="$IMU_REPO/zephyr/platform/apply-esp32-bt-ctrl-core-pin.sh"
BT_CORE_PIN_APPLIED=0
HCI_TIMEOUT_APPLY="$IMU_REPO/zephyr/platform/apply-hci-cmd-timeout-soft.sh"
HCI_TIMEOUT_APPLIED=0
HCI_SEND_POLL_APPLY="$IMU_REPO/zephyr/platform/apply-hci-esp32-send-poll.sh"
HCI_SEND_POLL_APPLIED=0

cleanup_patches() {
  if [[ -n "$HCI_BACKUP" && -f "$HCI_BACKUP" ]]; then
    cp "$HCI_BACKUP" "$HCI_UPSTREAM"
    rm -f "$HCI_BACKUP"
  fi
  if [[ "$HCI_LITERALS_APPLIED" == "1" ]]; then
    git -C "$ZEPHYR_PROJECT/zephyr" checkout -- drivers/bluetooth/hci/CMakeLists.txt 2>/dev/null || true
  fi
  if [[ "$BT_WQ_APPLIED" == "1" ]]; then
    git -C "$ZEPHYR_PROJECT/zephyr" checkout -- \
      subsys/bluetooth/host/hci_core.c subsys/bluetooth/host/hci_core.h \
      subsys/bluetooth/host/conn.c subsys/bluetooth/host/l2cap.c 2>/dev/null || true
  fi
  if [[ "$BT_LONG_WQ_APPLIED" == "1" ]]; then
    git -C "$ZEPHYR_PROJECT/zephyr" checkout -- subsys/bluetooth/host/Kconfig 2>/dev/null || true
  fi
  if [[ "$WIFI_DEFER_APPLIED" == "1" ]]; then
    git -C "$ZEPHYR_PROJECT/zephyr" checkout -- \
      drivers/wifi/esp32/src/esp_wifi_drv.c \
      drivers/wifi/esp32/Kconfig.esp32 2>/dev/null || true
  fi
  if [[ "$WIFI_DMA_APPLIED" == "1" ]]; then
    git -C "$ZEPHYR_PROJECT/modules/hal/espressif" checkout -- \
      zephyr/port/heap/heap_caps_zephyr.c \
      zephyr/esp32s3/src/wifi/esp_wifi_adapter.c \
      components/esp_coex/esp32s3/esp_coex_adapter.c 2>/dev/null || true
  fi
  if [[ "$PSRAM_SMH_APPLIED" == "1" ]]; then
    git -C "$ZEPHYR_PROJECT/zephyr" checkout -- soc/espressif/esp32s3/soc.c 2>/dev/null || true
  fi
  if [[ "$HCI_TIMEOUT_APPLIED" == "1" ]]; then
    git -C "$ZEPHYR_PROJECT/zephyr" checkout -- \
      subsys/bluetooth/host/hci_core.c 2>/dev/null || true
  fi
  if [[ "$HCI_SEND_POLL_APPLIED" == "1" ]]; then
    git -C "$ZEPHYR_PROJECT/zephyr" checkout -- \
      drivers/bluetooth/hci/hci_esp32.c 2>/dev/null || true
  fi
}
trap cleanup_patches EXIT

if [[ -f "$HCI_PATCH" && "${APPLY_HCI_DEFER:-1}" == "1" ]]; then
  HCI_BACKUP="$(mktemp)"
  cp "$HCI_UPSTREAM" "$HCI_BACKUP"
  cp "$HCI_PATCH" "$HCI_UPSTREAM"
  echo "Applied deferred VHCI HCI driver"
fi
if [[ -x "$HCI_LITERALS_APPLY" && "${APPLY_HCI_DEFER:-1}" == "1" ]]; then
  "$HCI_LITERALS_APPLY" "$ZEPHYR_PROJECT/zephyr"
  HCI_LITERALS_APPLIED=1
fi
# Obsolete on Zephyr 4.4+ (in-tree TX WQ). Default off; APPLY_BT_HCI_WQ_PATCH=1 for 3.7 trees.
if [[ -x "$BT_WQ_APPLY" && "${APPLY_BT_HCI_WQ_PATCH:-0}" == "1" ]]; then
  "$BT_WQ_APPLY" "$ZEPHYR_PROJECT/zephyr"
  BT_WQ_APPLIED=1
fi
if [[ -x "$HCI_TIMEOUT_APPLY" && "${APPLY_HCI_TIMEOUT_SOFT:-1}" == "1" ]]; then
  "$HCI_TIMEOUT_APPLY" "$ZEPHYR_PROJECT/zephyr"
  HCI_TIMEOUT_APPLIED=1
fi
if [[ -x "$HCI_SEND_POLL_APPLY" && "${APPLY_HCI_SEND_POLL:-1}" == "1" && "${APPLY_HCI_DEFER:-0}" != "1" ]]; then
  "$HCI_SEND_POLL_APPLY" "$ZEPHYR_PROJECT/zephyr"
  HCI_SEND_POLL_APPLIED=1
fi
if [[ -x "$BT_LONG_WQ_APPLY" && "${APPLY_BT_LONG_WQ_PATCH:-1}" == "1" ]]; then
  "$BT_LONG_WQ_APPLY" "$ZEPHYR_PROJECT/zephyr"
  BT_LONG_WQ_APPLIED=1
fi
if [[ -x "$WIFI_DEFER_APPLY" && "${APPLY_WIFI_DEFER:-1}" == "1" ]]; then
  "$WIFI_DEFER_APPLY" "$ZEPHYR_PROJECT/zephyr"
  WIFI_DEFER_APPLIED=1
fi
if [[ -x "$WIFI_DMA_APPLY" && "${APPLY_WIFI_DMA_DRAM:-1}" == "1" ]]; then
  "$WIFI_DMA_APPLY" "$ZEPHYR_PROJECT"
  WIFI_DMA_APPLIED=1
  if ! grep -q 'heap_caps_internal_dram' \
      "$ZEPHYR_PROJECT/modules/hal/espressif/zephyr/port/heap/heap_caps_zephyr.c"; then
    echo "ERROR: heap_caps_zephyr.c still ignores MALLOC_CAP_DMA" >&2
    exit 1
  fi
  DMA_ADAPTER="$ZEPHYR_PROJECT/modules/hal/espressif/components/esp_coex/esp32s3/esp_coex_adapter.c"
  [[ -f "$DMA_ADAPTER" ]] || \
    DMA_ADAPTER="$ZEPHYR_PROJECT/modules/hal/espressif/zephyr/esp32s3/src/wifi/esp_wifi_adapter.c"
  # Comment above __real_k_malloc spans >5 lines — do not use grep -A5.
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
  "$PSRAM_SMH_APPLY" "$ZEPHYR_PROJECT/zephyr"
  PSRAM_SMH_APPLIED=1
  if grep -q 'esp_psram_smh_init();' \
      "$ZEPHYR_PROJECT/zephyr/soc/espressif/esp32s3/soc.c"; then
    echo "ERROR: soc.c still calls esp_psram_smh_init" >&2
    exit 1
  fi
  echo "Verified: PSRAM SMH registration skipped (Zephyr 4.4 S3)"
fi
if [[ -x "$BT_CORE_PIN_APPLY" && "${APPLY_BT_CORE_PIN:-1}" == "1" ]]; then
  "$BT_CORE_PIN_APPLY" "$ZEPHYR_PROJECT/zephyr"
  BT_CORE_PIN_APPLIED=1
  if ! grep -q 'BT controller CPU core (0 or 1)' \
      "$ZEPHYR_PROJECT/modules/hal/espressif/zephyr/Kconfig"; then
    echo "ERROR: ESP32_BT_CTLR_PINNED_TO_CORE still has no prompt" >&2
    exit 1
  fi
  echo "Verified: BT controller core pin is user-configurable"
fi

MCUBOOT_KEY="${SB_CONFIG_BOOT_SIGNATURE_KEY_FILE:-$IMU_REPO/zephyr/mcuboot/root-ec-p256.pem}"
BUILD_EXTRA=(-DBOARD_ROOT="$BOARD_ROOT")
WEST_SYSBUILD=()
if [[ "$APP" == "handshake" ]]; then
  if [[ ! -f "$MCUBOOT_KEY" ]]; then
    echo "ERROR: missing MCUboot key $MCUBOOT_KEY" >&2
    exit 1
  fi
  WEST_SYSBUILD=(--sysbuild)
  # /tmp is writable by the container uid. Do not cp -a into the west cache:
  # that tree is nobody:nginx and utime/chown fail (build 203).
  KEY_FOR_WEST="/tmp/imu-mcuboot-root-ec-p256.pem"
  cp -f "$MCUBOOT_KEY" "$KEY_FOR_WEST"
  BUILD_EXTRA+=(-DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE="\"${KEY_FOR_WEST}\"")
  BUILD_EXTRA+=(-Dmcuboot_BOARD_ROOT="$BOARD_ROOT")
  FW_VER_H="$IMU_REPO/zephyr/app/common/fw_version.h"
  if [[ -n "${FW_OTA_VERSION_CODE:-}" && -n "${FW_OTA_VERSION_NAME:-}" ]]; then
    # Isolated CI clone only — stamp cloud-allocated 00.0001.0000.00001 (not Cast).
    cat >"$FW_VER_H" <<EOF
#ifndef IMU_FW_VERSION_H
#define IMU_FW_VERSION_H
#define FW_VERSION_CODE ${FW_OTA_VERSION_CODE}
#define FW_VERSION_NAME "${FW_OTA_VERSION_NAME}"
#endif
EOF
    echo "stamped FW_VERSION ${FW_OTA_VERSION_NAME} (${FW_OTA_VERSION_CODE})"
  fi
  FW_CODE="$(sed -n 's/^#define[[:space:]]\+FW_VERSION_CODE[[:space:]]\+\([0-9]\+\).*/\1/p' "$FW_VER_H" | head -1)"
  # MCUboot iv_revision is uint16. Desk images are 0.0.{code<=65535}.
  # Cloud era starts at 1.0.{patch} so it is newer than 0.0.186.
  IMGTOOL_VER="0.0.${FW_CODE}"
  if [[ -n "${FW_OTA_VERSION_NAME:-}" && "${FW_OTA_VERSION_NAME}" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
    img_minor=$((10#${BASH_REMATCH[3]}))
    img_patch=$((10#${BASH_REMATCH[4]}))
    [[ "$img_patch" -gt 65535 ]] && img_patch=65535
    [[ "$img_minor" -gt 255 ]] && img_minor=255
    IMGTOOL_VER="1.${img_minor}.${img_patch}"
  elif [[ -n "$FW_CODE" && "$FW_CODE" -gt 65535 ]]; then
    IMGTOOL_VER="1.0.1"
  fi
  if [[ -n "$FW_CODE" ]]; then
    BUILD_EXTRA+=(-DCONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION="\"${IMGTOOL_VER}\"")
    BUILD_EXTRA+=(-Dwaveshare-handshake_CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION="\"${IMGTOOL_VER}\"")
    echo "imgtool sign version ${IMGTOOL_VER}"
  fi
fi
if [[ "$APP" == "handshake" && "$CRASH_DEBUG" == "1" && -f "$LINK/prj_crash.conf" ]]; then
  echo "crash debug: merging prj_crash.conf"
  BUILD_EXTRA+=(-DEXTRA_CONF_FILE="$LINK/prj_crash.conf")
  BUILD_EXTRA+=(-Dwaveshare-handshake_EXTRA_CONF_FILE="$LINK/prj_crash.conf")
elif [[ "$APP" == "handshake" && "$CRASH_DEBUG" != "1" && -f "$LINK/prj_release.conf" ]]; then
  echo "release: merging prj_release.conf (CRASH_DEBUG=$CRASH_DEBUG)"
  BUILD_EXTRA+=(-DEXTRA_CONF_FILE="$LINK/prj_release.conf")
  BUILD_EXTRA+=(-Dwaveshare-handshake_EXTRA_CONF_FILE="$LINK/prj_release.conf")
  if [[ -f "$LINK/panel_40mhz.overlay" ]]; then
    echo "release: panel SPI 40 MHz overlay"
    BUILD_EXTRA+=(-DEXTRA_DTC_OVERLAY_FILE="$LINK/panel_40mhz.overlay")
    BUILD_EXTRA+=(-Dwaveshare-handshake_EXTRA_DTC_OVERLAY_FILE="$LINK/panel_40mhz.overlay")
  fi
fi

WEST_P=()
if [[ "$PRISTINE" == "1" ]]; then
  WEST_P=(-p always)
fi

echo "west build APP=$APP BOARD=$BOARD jobs=$BUILD_JOBS pristine=$PRISTINE dir=$BUILD_DIR sysbuild=${#WEST_SYSBUILD[@]}"
cd "$ZEPHYR_PROJECT/zephyr"
west build "${WEST_P[@]}" "${WEST_SYSBUILD[@]}" -d "$BUILD_DIR" -b "$BOARD" "$LINK" -- "${BUILD_EXTRA[@]}"

find_build_file() {
  local name="$1"
  local under="${2:-}"
  if [[ -n "$under" && -d "$BUILD_DIR/$under" ]]; then
    find "$BUILD_DIR/$under" -name "$name" -type f -print | head -1
    return
  fi
  find "$BUILD_DIR" \( -path '*mcuboot*' -prune \) -o \( -name "$name" -type f -print \) | head -1
}

# Phone OTA writes the signed-but-unconfirmed slot image (test swap + ota_ab confirm).
OTA_BIN="$(find_build_file zephyr.signed.bin)"
USB_BIN="$(find_build_file zephyr.signed.confirmed.bin)"
ELF="$(find_build_file zephyr.elf)"
MCUBOOT_BIN="$(find_build_file zephyr.bin mcuboot)"
BIN="${OTA_BIN:-$(find_build_file zephyr.bin)}"
if [[ -z "$BIN" || ! -f "$BIN" ]]; then
  echo "ERROR: no zephyr.bin / signed image under $BUILD_DIR" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"
cp -f "$BIN" "$OUT_DIR/zephyr.bin"
if [[ -n "$OTA_BIN" ]]; then
  cp -f "$OTA_BIN" "$OUT_DIR/zephyr.signed.bin"
fi
if [[ -n "$USB_BIN" ]]; then
  cp -f "$USB_BIN" "$OUT_DIR/zephyr.signed.confirmed.bin"
fi
if [[ -n "$MCUBOOT_BIN" ]]; then
  cp -f "$MCUBOOT_BIN" "$OUT_DIR/mcuboot.bin"
fi
if [[ -n "$ELF" ]]; then
  cp -f "$ELF" "$OUT_DIR/zephyr.elf"
fi
ls -lh "$OUT_DIR/zephyr.bin" "$OUT_DIR"/zephyr.signed.bin "$OUT_DIR"/mcuboot.bin 2>/dev/null || true
{
  echo "# SHA256 of build artifacts $(date -Iseconds)"
  for f in zephyr.bin zephyr.signed.bin zephyr.signed.confirmed.bin mcuboot.bin zephyr.elf; do
    if [[ -f "$OUT_DIR/$f" ]]; then
      sha256sum "$OUT_DIR/$f"
    fi
  done
} | tee "$OUT_DIR/SHA256SUMS"
echo "ci-west-build: ok -> $OUT_DIR/zephyr.bin"
