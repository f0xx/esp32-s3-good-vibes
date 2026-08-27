#!/usr/bin/env bash
# In-container (or host) west build for handshake/smoke. No flash.
# Env:
#   IMU_REPO          git checkout of this project (default: repo root of this script)
#   ZEPHYR_PROJECT    west workspace (default: $HOME/zephyrproject)
#   APP               handshake|smoke (default: handshake)
#   OUT_DIR           where zephyr.bin is copied (default: $IMU_REPO/out/zephyr)
#   BUILD_JOBS        ninja -j (default: nproc)
#   PRISTINE          1 = west -p always (default 1)
#   CRASH_DEBUG       1 = merge prj_crash.conf (default 1 for handshake)
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
CRASH_DEBUG="${CRASH_DEBUG:-1}"

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
export ZEPHYR_SDK_INSTALL_DIR="${ZEPHYR_SDK_INSTALL_DIR:-/opt/zephyr-sdk-0.16.8}"
export ZEPHYR_TOOLCHAIN_VARIANT="${ZEPHYR_TOOLCHAIN_VARIANT:-zephyr}"
export CMAKE_BUILD_PARALLEL_LEVEL="$BUILD_JOBS"
git config --global --add safe.directory "$ZEPHYR_PROJECT/zephyr" 2>/dev/null || true
git config --global --add safe.directory "$IMU_REPO" 2>/dev/null || true

LINK="$ZEPHYR_PROJECT/waveshare-${APP}"
BOARD_ROOT="$ZEPHYR_PROJECT/waveshare-board-root"
mkdir -p "$ZEPHYR_PROJECT"
ln -sfn "$IMU_REPO/zephyr/app/${APP}" "$LINK"
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
if [[ -x "$BT_WQ_APPLY" && "${APPLY_BT_HCI_WQ_PATCH:-1}" == "1" ]]; then
  "$BT_WQ_APPLY" "$ZEPHYR_PROJECT/zephyr"
  BT_WQ_APPLIED=1
fi
if [[ -x "$BT_LONG_WQ_APPLY" && "${APPLY_BT_LONG_WQ_PATCH:-1}" == "1" ]]; then
  "$BT_LONG_WQ_APPLY" "$ZEPHYR_PROJECT/zephyr"
  BT_LONG_WQ_APPLIED=1
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
  KEY_FOR_WEST="$ZEPHYR_PROJECT/imu-mcuboot-root-ec-p256.pem"
  cp -a "$MCUBOOT_KEY" "$KEY_FOR_WEST"
  BUILD_EXTRA+=(-DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE="\"${KEY_FOR_WEST}\"")
  BUILD_EXTRA+=(-Dmcuboot_BOARD_ROOT="$BOARD_ROOT")
  FW_VER_H="$IMU_REPO/zephyr/app/common/fw_version.h"
  FW_CODE="$(sed -n 's/^#define[[:space:]]\+FW_VERSION_CODE[[:space:]]\+\([0-9]\+\).*/\1/p' "$FW_VER_H" | head -1)"
  if [[ -n "$FW_CODE" ]]; then
    BUILD_EXTRA+=(-DCONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION="\"0.0.${FW_CODE}\"")
    BUILD_EXTRA+=(-Dwaveshare-handshake_CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION="\"0.0.${FW_CODE}\"")
    echo "imgtool sign version 0.0.${FW_CODE}"
  fi
fi
if [[ "$APP" == "handshake" && "$CRASH_DEBUG" == "1" && -f "$LINK/prj_crash.conf" ]]; then
  echo "crash debug: merging prj_crash.conf"
  BUILD_EXTRA+=(-DEXTRA_CONF_FILE="$LINK/prj_crash.conf")
  BUILD_EXTRA+=(-Dwaveshare-handshake_EXTRA_CONF_FILE="$LINK/prj_crash.conf")
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
cp -a "$BIN" "$OUT_DIR/zephyr.bin"
if [[ -n "$OTA_BIN" ]]; then
  cp -a "$OTA_BIN" "$OUT_DIR/zephyr.signed.bin"
fi
if [[ -n "$USB_BIN" ]]; then
  cp -a "$USB_BIN" "$OUT_DIR/zephyr.signed.confirmed.bin"
fi
if [[ -n "$MCUBOOT_BIN" ]]; then
  cp -a "$MCUBOOT_BIN" "$OUT_DIR/mcuboot.bin"
fi
if [[ -n "$ELF" ]]; then
  cp -a "$ELF" "$OUT_DIR/zephyr.elf"
fi
ls -lh "$OUT_DIR/zephyr.bin" "$OUT_DIR"/zephyr.signed.bin "$OUT_DIR"/mcuboot.bin 2>/dev/null || true
echo "ci-west-build: ok -> $OUT_DIR/zephyr.bin"
