#!/usr/bin/env bash
# Install ESP32 IMU backend on artc0 (Raspbian stretch / Python 3.5).
# Does NOT touch Android Cast SSH tunnels on :80/:443 (10.7.0.10 forwards).
set -euo pipefail

INSTALL_DIR="${INSTALL_DIR:-/opt/esp32-imu-backend}"
ENV_FILE="/etc/esp32-imu-backend/env"
SERVICE="esp32-imu-backend.service"
API_KEY="${IMU_API_KEY:-$(openssl rand -hex 24)}"
DB_PASS="${IMU_DB_PASS:-$(openssl rand -hex 16)}"

if [[ "$(id -u)" -ne 0 ]]; then
  echo "Run with sudo" >&2
  exit 1
fi

echo "== packages (skip slow apt update if already installed) =="
export DEBIAN_FRONTEND=noninteractive
if ! dpkg -s postgresql python3-flask python3-psycopg2 >/dev/null 2>&1; then
  apt-get install -y --no-install-recommends -o Acquire::http::Timeout=120 \
    postgresql python3-flask python3-psycopg2 rsync
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
bash "${SCRIPT_DIR}/setup-only.sh"
