# artc0 deployment — READ THIS FIRST

## Two different hosts share similar names

| How you connect | Where you land | Specs |
|-----------------|----------------|-------|
| **`ssh foxx@artc0`** (SSH config + ProxyJump) | **Alpine VM** `10.7.16.128` | 32 CPU, 16 GB RAM, 44 GB disk — **use this** |
| **`ssh foxx@artc0.f0xx.org`** or public DNS | **Raspberry Pi** `134.17.5.46` | 874 MB RAM, Stretch — Cast tunnel box, **not the IMU backend** |

Public DNS `artc0.f0xx.org` → `f0xx.org` → **134.17.5.46** (Pi).  
Your `~/.ssh/config` Host `artc0` → `10.7.16.128` via `f0xx-jump`.

**Always deploy with:** `rsync … foxx@artc0:…` and `ssh foxx@artc0` — never rely on `artc0.f0xx.org` for SSH.

## Correct backend (Alpine VM)

```bash
# From dev machine (uses SSH config ProxyJump)
rsync -av --exclude .venv backend/ foxx@artc0:~/esp32-imu-backend/
ssh foxx@artc0 'cd ~/esp32-imu-backend && export IMU_API_KEY=$(cat .api_key) && docker compose up -d --build'
```

- **Path:** `~/esp32-imu-backend`
- **Stack:** Docker Compose — FastAPI + Postgres 16
- **Port:** `8080` (does not use nginx 80/443 — Cast/stream apps keep those)
- **Health:** `curl http://127.0.0.1:8080/v1/health` on the VM
- **Grafana:** `http://127.0.0.1:3000/grafana/` (public HTTPS via monstro — see `deploy/README-grafana-monstro.md`)
- **API key:** `~/esp32-imu-backend/.api_key`
- **Grafana admin:** `~/esp32-imu-backend/.grafana_admin_password`

### Android tablet Cloud URL

Default in the app: **`http://artc0.f0xx.org:8090`**

Public forwarder (Pi `134.17.5.46`):

```text
http://artc0.f0xx.org:8090/  →  http://artc0.intra.raptor.org:8080/
```

Verify: `curl http://artc0.f0xx.org:8090/v1/health`

Direct on intra/VPN (no Pi): `http://artc0.intra.raptor.org:8080`

SSH/deploy still uses **`ssh foxx@artc0`** → VM `10.7.16.128` (same host as `artc0.intra.raptor.org`).

## Mistaken Pi deploy (can remove)

A Flask + Postgres 9.6 service was accidentally installed on the **Pi** via `artc0.f0xx.org`:

```bash
ssh foxx@artc0.f0xx.org 'sudo systemctl disable --now esp32-imu-backend.service; sudo rm -rf /opt/esp32-imu-backend'
```

Cast SSH tunnels on the Pi (`134.17.5.46:80/443` → `10.7.0.10`) were not modified.

## Isolation from Android Cast (Alpine)

- IMU uses Docker network `esp32-imu-backend_default` and port **8080** only
- Existing nginx on **80/443** (`apps.conf`, `stream.conf.disabled`) untouched
- Monitoring on 9090/9100/etc. untouched

## TimescaleDB

Current compose uses **TimescaleDB on Postgres 16** (`timescale/timescaledb:latest-pg16`). Hypertable on `verdicts.ts_ms` is created at API startup when the extension is available.
