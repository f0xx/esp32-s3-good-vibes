# Backend deploy

FastAPI ingest service for vibration **verdicts** uploaded by the Android app (Case B/C — phone as relay).

Path: `backend/`

## Stack

| Component | Technology |
|-----------|------------|
| API | FastAPI + Uvicorn |
| Database | TimescaleDB (PostgreSQL 16) |
| Dashboard | Grafana 11.5 |
| Orchestration | Docker Compose |

## Quick start (Docker)

```bash
cd backend
cp .env.example .env   # optional — or export vars directly
export IMU_API_KEY='pick-a-long-random-string'

docker compose up -d --build
curl -s http://127.0.0.1:8080/v1/health | jq .
```

Expected:

```json
{"ok": true, "schema_version": "imu.ingest.v1", "db": "postgres"}
```

## Services (docker-compose.yml)

| Service | Port | Image |
|---------|------|-------|
| `api` | **8080** | built from `Dockerfile` |
| `db` | internal | `timescale/timescaledb:latest-pg16` |
| `grafana` | **3000** | `grafana/grafana:11.5.2` |

## Environment variables

| Variable | Default | Purpose |
|----------|---------|---------|
| `IMU_API_KEY` | `dev-change-me` | `X-API-Key` header auth |
| `DATABASE_URL` | `postgresql+psycopg2://imu:imu@db:5432/imu` | SQLAlchemy DSN |
| `GRAFANA_ADMIN_PASSWORD` | `change-me-grafana` | Grafana admin |

See `.env.example`.

## Android configuration

In the app **Cloud** screen:

- Base URL: `http://<server>:8080`
- API key: matches `IMU_API_KEY`
- Device ID: e.g. board BLE name
- Group ID: e.g. `machine-press-1`

WorkManager uploads `files/offload/verdicts.jsonl` when network is available.

## Test ingest

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
```

Query:

```bash
curl -s -H "X-API-Key: $IMU_API_KEY" \
  'http://127.0.0.1:8080/v1/devices/test-esp/verdicts?limit=5' | jq .
```

## Local dev (SQLite, no Docker)

```bash
cd backend
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
export IMU_API_KEY=dev-change-me
export DATABASE_URL=sqlite:///./imu_backend.db
uvicorn app.main:app --reload --port 8080
```

## Grafana

Dashboard **`imu-verdicts`** — RMS, correlation, battery, verdict level, plus **band/edge panels** (v46) reading `verdicts.raw_json`:

- Band reference compare: `band_corr`, `band_delta_max`
- Edge features: `edge_crest`, `edge_zcr_hz`, `edge_hf_ratio`

Redeploy dashboards: `./backend/deploy/deploy-artc0.sh` (copies `backend/grafana/dashboards/`).

Dashboards: **ESP32 IMU Verdicts** (`uid=imu-verdicts`), **ESP32 IMU Crashes** (`uid=imu-crashes`).

```bash
bash deploy/ensure-grafana.sh
```

- UI: `http://127.0.0.1:3000` (or `/grafana/` behind nginx)
- Dashboard: **ESP32 IMU Verdicts**
- See `deploy/README-grafana-monstro.md` for reverse-proxy setup

## VM sizing

| Resource | Minimum |
|----------|---------|
| vCPU | 2 |
| RAM | 2 GB (4 GB comfortable) |
| Disk | 20 GB |
| LAN port | **8080/tcp** open to tablets |

## Firewall example

```bash
sudo ufw allow from 192.168.0.0/16 to any port 8080 proto tcp
```

## Good Vibes web UI (production)

| URL | Role |
|-----|------|
| `https://apps.f0xx.org/app/good_vibes/` | Verdict + spectrum dashboard (after monstro FE install) |
| `https://apps.f0xx.org/app/good_vibes/v1/…` | Same API as `/v1/…` |
| `https://apps.f0xx.org/app/good_vibes/grafana/` | Grafana dashboards |

**Monstro (you apply):** see `backend/deploy/fe/INSTALL-good-vibes.md`

**artc0 backend:** `./backend/deploy/deploy-artc0.sh` — API on VM `:8080`, Grafana `:3000`.

New endpoints:

- `POST /v1/ingest/spectra` — phone FFT bins
- `GET /v1/devices` — device list with latest level
- `GET /v1/devices/{id}/spectra` — spectrum history

Android default cloud URL: `https://apps.f0xx.org/app/good_vibes`

## Files

| Path | Role |
|------|------|
| `app/main.py` | FastAPI app + static UI mount |
| `app/api.py` | REST routes |
| `web/index.html` | Good Vibes dashboard |
| `schema/` | JSON schema docs |
| `grafana/` | Dashboard provisioning |
| `deploy/` | Bare-metal and nginx helpers |

More detail: [backend/README.md](../backend/README.md)
