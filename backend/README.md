# ESP32 IMU Backend (G6) — VM setup

Minimal ingest service for phone-as-transmitter (Case B/C). Receives vibration **verdicts** from the Android app; ESP32 never calls this directly on battery.

## What to provision on the VM

| Item | Recommendation |
|------|----------------|
| OS | Ubuntu 24.04 LTS (or 22.04) |
| vCPU / RAM | 2 vCPU, **2 GB RAM** minimum (4 GB comfortable with Postgres) |
| Disk | **20 GB** (Postgres + logs; batches come later) |
| Network | Static LAN IP or DHCP reservation (tablet needs stable `http://IP:8080`) |
| Software | Docker Engine + Docker Compose plugin |
| Ports | **8080/tcp** open on LAN (not public internet until TLS) |
| DNS | Optional: `imu-backend.local` via router or `/etc/hosts` on tablet |

## Quick start (on the VM)

```bash
git clone <your-repo> esp32-s3-imu-basics
cd esp32-s3-imu-basics/backend

# Pick a real secret — same value goes in the Android Cloud settings
export IMU_API_KEY='pick-a-long-random-string'

docker compose up -d --build
curl -s http://127.0.0.1:8080/v1/health | jq .
```

Expected health response:

```json
{"ok": true, "schema_version": "imu.ingest.v1", "db": "postgres"}
```

## Android app configuration

On the tablet/phone:

1. **Cloud** button → default `http://artc0.f0xx.org:8090` (→ `artc0.intra.raptor.org:8080`)
2. API key: same as `IMU_API_KEY`
3. Device ID: defaults to `ESP32S3 IMU sim` target name; change if you run multiple boards
4. Group ID: e.g. `machine-press-1` for fusion later

WorkManager uploads `files/offload/verdicts.jsonl` when Wi‑Fi/cellular is up.

## Test ingest manually

```bash
curl -s -X POST http://127.0.0.1:8080/v1/ingest/verdicts \
  -H 'Content-Type: application/json' \
  -H "X-API-Key: $IMU_API_KEY" \
  -d '{
    "schema": "imu.ingest.v1",
    "device_id": "test-esp",
    "group_id": "demo",
    "phone_id": "curl",
    "sent_at_ms": 1700000000000,
    "records": [{
      "type": "verdict",
      "ts_ms": 1700000000000,
      "seq": 1,
      "level": 0,
      "rms": 0.012,
      "peak": 0.05,
      "corr": 0.98,
      "rms_delta": 0.001,
      "pct": 80,
      "voltage": 3.9
    }]
  }' | jq .

curl -s -H "X-API-Key: $IMU_API_KEY" \
  'http://127.0.0.1:8080/v1/devices/test-esp/verdicts?limit=5' | jq .
```

## Local dev (no Docker)

```bash
cd backend
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
export IMU_API_KEY=dev-change-me
export DATABASE_URL=sqlite:///./imu_backend.db
uvicorn app.main:app --reload --port 8080
```

## Grafana (verdict time-series UI)

Grafana runs on artc0 beside the API (Docker port **3000**, sub-path **`/grafana/`**).

```bash
bash deploy/ensure-grafana.sh   # RO DB user + datasource + start grafana
curl -s http://127.0.0.1:3000/api/health
```

- Dashboard: **ESP32 IMU Verdicts** — RMS, peak, correlation, level, battery, recent table
- Datasource: Postgres/Timescale `verdicts` via read-only `imu_grafana_ro`
- Public HTTPS: **monstro nginx** — [`deploy/README-grafana-monstro.md`](deploy/README-grafana-monstro.md) (you deploy FE; agent does not touch monstro)

Direct (intra/VPN): `http://artc0.intra.raptor.org:3000/grafana/`

## Desires / near-term roadmap (for your VM)

These are **not** required day one but shape how I'd grow the box:

1. **TLS** — Caddy or nginx reverse proxy with Let's Encrypt if you expose beyond LAN
2. **TimescaleDB** — swap Postgres image to `timescale/timescaledb` when you want rollups (`vrms` hourly)
3. **Batch ingest** — `POST /v1/ingest/batches` gzip JSON (phone stores more than verdicts)
4. ~~**Web UI** — minimal Grafana or a single-page verdict timeline per `group_id`~~ **Grafana deployed** (monstro FE pending)
5. **Alerts** — webhook/email on `level >= 2` (ALERT)
6. **Backups** — nightly `pg_dump` to mounted volume
7. **Fusion stub** — `GET /v1/groups/{group_id}/status` majority vote across devices (latest non-stale verdict per device; tie-break toward higher level)

```bash
curl -H "X-API-Key: $IMU_API_KEY" \
  "http://127.0.0.1:8080/v1/groups/machine-press-1/status"
```

## Files

- `schema/` — P-schema JSON + README
- `app/main.py` — FastAPI ingest + query
- `docker-compose.yml` — Postgres 16 + API + Grafana
- `grafana/` — provisioning + **ESP32 IMU Verdicts** dashboard
- `deploy/README-grafana-monstro.md` — monstro nginx handoff (FE)

## Firewall example (ufw)

```bash
sudo ufw allow from 192.168.0.0/16 to any port 8080 proto tcp
sudo ufw enable
```

Adjust subnet to match your LAN (your tablet farm is on `192.168.33.x`).
