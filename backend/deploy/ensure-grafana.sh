#!/usr/bin/env bash
# Render Grafana datasource + ensure read-only DB role. Run on artc0 before compose up.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [[ ! -f .api_key ]]; then
  echo "Missing .api_key — run setup-only.sh first" >&2
  exit 1
fi

if [[ ! -f .grafana_db_password ]]; then
  openssl rand -hex 16 > .grafana_db_password
  chmod 600 .grafana_db_password
fi
if [[ ! -f .grafana_admin_password ]]; then
  openssl rand -base64 18 > .grafana_admin_password
  chmod 600 .grafana_admin_password
fi

DB_PASS="$(cat .grafana_db_password)"
ADMIN_PASS="$(cat .grafana_admin_password)"
export GRAFANA_ADMIN_PASSWORD="$ADMIN_PASS"
export GRAFANA_DB_PASSWORD="$DB_PASS"

mkdir -p grafana/provisioning/datasources
sed "s/__GRAFANA_DB_PASSWORD__/${DB_PASS}/g" deploy/datasource.yml.template \
  > grafana/provisioning/datasources/postgres.yml
chmod 644 grafana/provisioning/datasources/postgres.yml

# DB must be up (compose db healthy)
docker compose up -d db
docker compose exec -T db pg_isready -U imu -d imu >/dev/null

sed "s/__GRAFANA_DB_PASSWORD__/${DB_PASS}/g" deploy/grafana-ro-user.sql \
  | docker compose exec -T db psql -U imu -d imu -v ON_ERROR_STOP=1

echo "Grafana admin password: ${ADMIN_PASS}"
echo "Grafana DB RO user: imu_grafana_ro (password in .grafana_db_password)"

docker compose up -d grafana
echo "Grafana: http://127.0.0.1:3000/grafana/"
