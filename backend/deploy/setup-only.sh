#!/usr/bin/env bash
set -euo pipefail
INSTALL_DIR=/opt/esp32-imu-backend
ENV_FILE=/etc/esp32-imu-backend/env
API_KEY="${IMU_API_KEY:-$(openssl rand -hex 24)}"
DB_PASS="${IMU_DB_PASS:-$(openssl rand -hex 16)}"

sudo -u postgres psql -tAc "SELECT 1 FROM pg_roles WHERE rolname='imu'" | grep -q 1 || \
  sudo -u postgres psql -c "CREATE USER imu WITH PASSWORD '${DB_PASS}';"

sudo -u postgres psql -tc "SELECT 1 FROM pg_database WHERE datname='imu_backend'" | grep -q 1 || \
  sudo -u postgres psql -c "CREATE DATABASE imu_backend OWNER imu;"
sudo -u postgres psql -c "GRANT ALL PRIVILEGES ON DATABASE imu_backend TO imu;"

mkdir -p "${INSTALL_DIR}"
install -m 0644 /tmp/esp32-imu-deploy/legacy_server.py "${INSTALL_DIR}/"
chown -R foxx:foxx "${INSTALL_DIR}"

mkdir -p /etc/esp32-imu-backend
cat > "${ENV_FILE}" <<EOF
IMU_API_KEY=${API_KEY}
DATABASE_URL=dbname=imu_backend user=imu password=${DB_PASS} host=127.0.0.1
BIND_HOST=0.0.0.0
BIND_PORT=8080
EOF
chmod 0640 "${ENV_FILE}"
chown root:foxx "${ENV_FILE}"

cat > /etc/systemd/system/esp32-imu-backend.service <<EOF
[Unit]
Description=ESP32 IMU ingest backend (isolated from Android Cast)
After=network.target postgresql.service
Wants=postgresql.service

[Service]
Type=simple
User=foxx
Group=foxx
EnvironmentFile=${ENV_FILE}
WorkingDirectory=${INSTALL_DIR}
ExecStart=/usr/bin/python3 ${INSTALL_DIR}/legacy_server.py
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable esp32-imu-backend.service
systemctl restart esp32-imu-backend.service
sleep 2
systemctl is-active esp32-imu-backend.service
curl -s http://127.0.0.1:8080/v1/health
echo
echo "API_KEY=${API_KEY}"
