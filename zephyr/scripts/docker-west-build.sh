#!/usr/bin/env bash
# Host wrapper: imu-zephyr-ci image + west build. Does not flash, does not
# touch androidcast-ci or Cast's runner. Safe to run next to a Cast build:
# unique container name, separate caches, BUILD_JOBS defaults to nproc/2.
#
#   ./zephyr/scripts/docker-west-build.sh handshake
# Env: ZEPHYR_PROJECT (default ~/zephyrproject), IMU_BUILD_JOBS, PRISTINE, CRASH_DEBUG
#
# Frozen runner contract (Cast PHP should docker-run this image, not grow
# androidcast-ci). Desk still flashes; cluster never sees /dev/ttyACM0.
#   image:      imu-zephyr-ci:0.16.8
#   name:       imu-zephyr-bld-$BUILD_ID   (never androidcast-bld-*)
#   jobs:       IMU_BUILD_JOBS, default nproc/2 (do not preempt Cast)
#   mounts:     repo→/src, west tree→/zephyrproject, artifacts→/out
#   west tree:  cache volume *copy* in CI — not a live desk ~/zephyrproject
#   build dir:  /tmp/zephyr-build (never the host zephyr/build)
#   HOME:       /root (CMake Zephyr-sdk registry)
#   entry:      /src/zephyr/scripts/ci-west-build.sh
#   out:        /out/zephyr.bin (OTA signed slot), zephyr.signed.confirmed.bin,
#               mcuboot.bin, zephyr.elf — never flash from the cluster
#   no flash, no BLE, no phone ADB
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMU_REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
ZEPHYR_PROJECT="${ZEPHYR_PROJECT:-$HOME/zephyrproject}"
APP="${1:-handshake}"
IMAGE="${IMU_ZEPHYR_IMAGE:-imu-zephyr-ci:0.16.8}"
DOCKERFILE="$IMU_REPO/zephyr/docker/Dockerfile.esp32s3-zephyr"
OUT_DIR="${IMU_OUT_DIR:-$IMU_REPO/out/zephyr}"
NPROC="$(nproc)"
HALF=$((NPROC / 2))
[[ "$HALF" -ge 1 ]] || HALF=1
BUILD_JOBS="${IMU_BUILD_JOBS:-$HALF}"
BUILD_ID="${BUILD_ID:-local-$$}"
CONTAINER_NAME="imu-zephyr-bld-${BUILD_ID}"

if [[ ! -d "$ZEPHYR_PROJECT/zephyr" ]]; then
  echo "ERROR: ZEPHYR_PROJECT=$ZEPHYR_PROJECT has no zephyr/ — west init/update on the host (or a cache volume) first" >&2
  exit 1
fi

if ! command -v docker >/dev/null 2>&1; then
  echo "ERROR: docker not on PATH" >&2
  exit 1
fi

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "[docker-west] building $IMAGE (first time: SDK download)"
  docker build -f "$DOCKERFILE" -t "$IMAGE" "$IMU_REPO/zephyr/docker"
fi

mkdir -p "$OUT_DIR"
echo "[docker-west] APP=$APP jobs=$BUILD_JOBS container=$CONTAINER_NAME"
docker rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
# Host uid so HCI patches on the bind-mounted west tree are not left root-owned.
docker run --rm --name "$CONTAINER_NAME" \
  --user "$(id -u):$(id -g)" \
  --network host \
  -v "$IMU_REPO:/src" \
  -v "$ZEPHYR_PROJECT:/zephyrproject" \
  -v "$OUT_DIR:/out" \
  -e IMU_REPO=/src \
  -e ZEPHYR_PROJECT=/zephyrproject \
  -e APP="$APP" \
  -e OUT_DIR=/out \
  -e BUILD_JOBS="$BUILD_JOBS" \
  -e BUILD_DIR=/tmp/zephyr-build \
  -e PRISTINE="${PRISTINE:-1}" \
  -e CRASH_DEBUG="${CRASH_DEBUG:-1}" \
  -e SB_CONFIG_BOOT_SIGNATURE_KEY_FILE=/src/zephyr/mcuboot/root-ec-p256.pem \
  -e HOME=/tmp \
  -w /src \
  "$IMAGE" \
  /src/zephyr/scripts/ci-west-build.sh
