#!/usr/bin/env bash
# Deploy Good Vibes backend to artc0 Alpine VM (ssh config Host artc0 → 10.7.16.128).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BACKEND_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REMOTE="${REMOTE:-foxx@artc0}"
REMOTE_DIR="${REMOTE_DIR:-~/esp32-imu-backend}"
ZEPHYR_ELF="${ZEPHYR_ELF:-$HOME/zephyrproject/zephyr/build/zephyr/zephyr.elf}"
ZEPHYR_ADDR2LINE="${ZEPHYR_ADDR2LINE:-$HOME/zephyr-sdk-0.16.8/xtensa-espressif_esp32s3_zephyr-elf/bin/xtensa-espressif_esp32s3_zephyr-elf-addr2line}"
STAGE_DIR="${BACKEND_DIR}/deploy/stage"

echo "=== stage symbolication assets"
mkdir -p "${STAGE_DIR}"
rm -f "${STAGE_DIR}/zephyr.elf" "${STAGE_DIR}/xtensa-addr2line"
if [[ -f "${ZEPHYR_ELF}" ]]; then
	cp -a "${ZEPHYR_ELF}" "${STAGE_DIR}/zephyr.elf"
	echo "  zephyr.elf ($(du -h "${STAGE_DIR}/zephyr.elf" | cut -f1))"
else
	echo "  WARN: ${ZEPHYR_ELF} missing — crash symbolication disabled"
fi
if [[ -x "${ZEPHYR_ADDR2LINE}" ]]; then
	cp -a "${ZEPHYR_ADDR2LINE}" "${STAGE_DIR}/xtensa-addr2line"
	echo "  xtensa-addr2line"
else
	echo "  WARN: ${ZEPHYR_ADDR2LINE} missing — crash symbolication disabled"
fi

echo "=== rsync backend → ${REMOTE}:${REMOTE_DIR}"
rsync -av --delete \
	--exclude .venv \
	--exclude __pycache__ \
	--exclude '*.pyc' \
	--exclude .env \
	--exclude .api_key \
	--exclude .grafana_admin_password \
	--exclude .grafana_db_password \
	"${BACKEND_DIR}/" "${REMOTE}:${REMOTE_DIR}/"

echo "=== docker compose up (artc0)"
ssh "${REMOTE}" bash -s <<'REMOTE'
set -euo pipefail
cd ~/esp32-imu-backend
if [[ ! -f .api_key ]]; then
	openssl rand -hex 24 > .api_key
	chmod 600 .api_key
	echo "Created .api_key"
fi
if [[ ! -f .grafana_admin_password ]]; then
	openssl rand -hex 12 > .grafana_admin_password
	chmod 600 .grafana_admin_password
	echo "Created .grafana_admin_password"
fi
export IMU_API_KEY="$(cat .api_key)"
export GRAFANA_ADMIN_PASSWORD="$(cat .grafana_admin_password)"
docker compose up -d --build
sleep 5
curl -sf http://127.0.0.1:8080/v1/health || echo "WARN: health check failed (API still starting?)"
curl -sf http://127.0.0.1:8080/app/good_vibes/health || true
echo "API key: $(cat .api_key)"
REMOTE

echo "=== done — configure monstro FE (see deploy/fe/INSTALL-good-vibes.md)"
