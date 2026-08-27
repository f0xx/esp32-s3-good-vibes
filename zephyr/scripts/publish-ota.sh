#!/usr/bin/env bash
# Stage signed firmware + APK + checksum manifest for the backend OTA poller.
# Does not flash. Copies into backend/ota/current/ (rsync'd by deploy-artc0.sh).
#
#   ./zephyr/scripts/publish-ota.sh
# Env: IMU_OUT_DIR (default repo/out/zephyr), APK path, OTA_DIR
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMU_REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUT_DIR="${IMU_OUT_DIR:-$IMU_REPO/out/zephyr}"
OTA_DIR="${IMU_OTA_DIR:-$IMU_REPO/backend/ota/current}"
APK="${IMU_APK:-$IMU_REPO/android/ESP32S3ImuSim/app/build/outputs/apk/debug/app-debug.apk}"
GRADLE="$IMU_REPO/android/ESP32S3ImuSim/app/build.gradle.kts"

fw_bin=""
for cand in "$OUT_DIR/zephyr.signed.bin" "$OUT_DIR/zephyr.bin"; do
	if [[ -f "$cand" ]]; then
		fw_bin="$cand"
		break
	fi
done
if [[ -z "$fw_bin" ]]; then
	echo "ERROR: no firmware under $OUT_DIR — run docker-west-build.sh or flash-zephyr.sh first" >&2
	exit 1
fi
if [[ ! -f "$APK" ]]; then
	echo "ERROR: APK missing: $APK" >&2
	exit 1
fi

sha256() { sha256sum "$1" | awk '{print $1}'; }
version_code="$(sed -n 's/.*versionCode = \([0-9][0-9]*\).*/\1/p' "$GRADLE" | head -1)"
version_name="$(sed -n 's/.*versionName = "\([^"]*\)".*/\1/p' "$GRADLE" | head -1)"
FW_H="$IMU_REPO/zephyr/app/common/fw_version.h"
fw_code="$(sed -n 's/^#define[[:space:]]\+FW_VERSION_CODE[[:space:]]\+\([0-9]\+\).*/\1/p' "$FW_H" | head -1)"
fw_version="$(sed -n 's/^#define[[:space:]]\+FW_VERSION_NAME[[:space:]]\+"\([^"]*\)".*/\1/p' "$FW_H" | head -1)"
fw_version="${fw_version:-handshake unknown}"
fw_code="${fw_code:-0}"
published_ms="$(date +%s%3N)"
# GNU date %3N may be literal on busybox; fall back to seconds*1000.
if [[ "$published_ms" == *N ]]; then
	published_ms="$(($(date +%s) * 1000))"
fi

mkdir -p "$OTA_DIR"
cp -a "$fw_bin" "$OTA_DIR/firmware.bin"
cp -a "$APK" "$OTA_DIR/app-debug.apk"
if [[ -f "$OUT_DIR/mcuboot.bin" ]]; then
	cp -a "$OUT_DIR/mcuboot.bin" "$OTA_DIR/mcuboot.bin"
fi

fw_sha="$(sha256 "$OTA_DIR/firmware.bin")"
apk_sha="$(sha256 "$OTA_DIR/app-debug.apk")"
fw_size="$(stat -c%s "$OTA_DIR/firmware.bin")"
apk_size="$(stat -c%s "$OTA_DIR/app-debug.apk")"

cat >"$OTA_DIR/manifest.json" <<EOF
{
  "schema": "imu.ota.v1",
  "published_at_ms": ${published_ms},
  "apk": {
    "versionCode": ${version_code:-0},
    "versionName": "${version_name:-unknown}",
    "sha256": "${apk_sha}",
    "size": ${apk_size},
    "url": "artifacts/apk"
  },
  "fw": {
    "versionCode": ${fw_code},
    "version": "${fw_version}",
    "sha256": "${fw_sha}",
    "size": ${fw_size},
    "url": "artifacts/fw",
    "min_apk_versionCode": ${version_code:-0}
  }
}
EOF

echo "publish-ota: $OTA_DIR"
echo "  apk ${version_name} (${version_code}) ${apk_sha:0:12}… ${apk_size} B"
echo "  fw  ${fw_version} (${fw_code}) ${fw_sha:0:12}… ${fw_size} B"
