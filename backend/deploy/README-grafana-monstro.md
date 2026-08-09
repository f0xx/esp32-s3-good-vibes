# Grafana — monstro FE handoff (YOU deploy this)

**Status:** Backend Grafana is deployed on **artc0** (`10.7.16.128`) at port **3000**.  
**Your task:** expose it via **monstro** nginx (HTTPS). Do **not** ask the agent to edit monstro.

## What is already live on artc0

| Item | Value |
|------|--------|
| Grafana URL (direct, intra/VPN) | `http://artc0.intra.raptor.org:3000/grafana/` |
| Sub-path | `/grafana/` (`GF_SERVER_SERVE_FROM_SUB_PATH=true`) |
| Admin user | `admin` |
| Admin password | `~/esp32-imu-backend/.grafana_admin_password` on artc0 |
| Datasource | Postgres/Timescale `imu` via read-only `imu_grafana_ro` |
| Dashboard | **ESP32 IMU Verdicts** (`uid=imu-verdicts`) |
| API ingest (unchanged) | `:8080` / public Pi forward `:8090` |

Verify on artc0 (already done by deploy):

```bash
curl -s http://127.0.0.1:3000/api/health
curl -s -u admin:$(cat ~/esp32-imu-backend/.grafana_admin_password) \
  http://127.0.0.1:3000/api/datasources/uid/imu-pg/health
```

## monstro nginx — suggested vhost fragment

`artc0.conf` on monstro is mostly **DEPRECATED** and **not included** from `nginx.conf` today.  
Pick one:

### Option A — Re-enable `artc0.f0xx.org` HTTPS (recommended)

Add or uncomment a `443` server for `artc0.f0xx.org` and include **only** the Grafana location (API stays on Pi `:8090`):

```nginx
# /etc/nginx/artc0-imu-grafana.conf  (new file)
server {
	listen 80;
	server_name artc0.f0xx.org;
	return 301 https://$host$request_uri;
}

server {
	listen 443 ssl http2;
	server_name artc0.f0xx.org;

	ssl_certificate     /etc/letsencrypt/live/artc0.f0xx.org/fullchain.pem;
	ssl_certificate_key /etc/letsencrypt/live/artc0.f0xx.org/privkey.pem;

	access_log /var/log/nginx/artc0.f0xx.org.access_log main;
	error_log  /var/log/nginx/artc0.f0xx.org.error_log info;

	# Grafana UI (artc0 VM Docker :3000)
	location /grafana/ {
		proxy_pass http://artc0.intra.raptor.org:3000/grafana/;
		proxy_http_version 1.1;
		proxy_set_header Host              $host;
		proxy_set_header X-Real-IP         $remote_addr;
		proxy_set_header X-Forwarded-For   $proxy_add_x_forwarded_for;
		proxy_set_header X-Forwarded-Proto $scheme;
		proxy_set_header X-Forwarded-Host  $host;
		proxy_set_header Upgrade           $http_upgrade;
		proxy_set_header Connection        $connection_upgrade;
		proxy_read_timeout 300s;
		proxy_send_timeout 300s;
	}

	# Optional: return 404 for / — API is not on monstro (Pi :8090 → VM :8080)
	location / {
		return 404;
	}
}
```

Then in `/etc/nginx/nginx.conf` `http {}` block:

```nginx
include /etc/nginx/artc0-imu-grafana.conf;
```

Reload (on monstro):

```bash
nginx -t && rc-service nginx reload
```

### Option B — Sub-path on an existing vhost

If you prefer another hostname (e.g. `apps.f0xx.org/grafana/`), reuse the same `location /grafana/` block pointing at `http://artc0.intra.raptor.org:3000/grafana/`.

**Important:** `GF_SERVER_ROOT_URL` on artc0 is set to `https://artc0.f0xx.org/grafana/`. If you use a different public URL, SSH to artc0 and update `docker-compose.yml` `GF_SERVER_ROOT_URL`, then `docker compose up -d grafana`.

## After monstro deploy — verify

```bash
curl -sI https://artc0.f0xx.org/grafana/login | head -5
# Expect HTTP/2 200

curl -s https://artc0.f0xx.org/grafana/api/health
# {"database":"ok","version":"..."}
```

Browser: `https://artc0.f0xx.org/grafana/` → login `admin` + password from artc0 `.grafana_admin_password`.

## Optional: public port without monstro TLS

Pi forward (if you want Grafana on `:8091` like API on `:8090`):

```text
134.17.5.46:8091  →  artc0.intra.raptor.org:3000
```

Not configured by this deploy — monstro HTTPS is the intended path.

## Security notes

- Grafana admin password is random on first `ensure-grafana.sh` run.
- Postgres access from Grafana uses **read-only** role `imu_grafana_ro`.
- Do not expose `:3000` on the public internet without TLS + auth.
- API key for ingest remains separate (`X-API-Key` on `:8080` / `:8090`).

## Redeploy backend + Grafana (artc0)

From dev machine:

```bash
rsync -av --exclude .venv --exclude grafana/provisioning/datasources/postgres.yml \
  backend/ foxx@artc0:~/esp32-imu-backend/
ssh foxx@artc0 'cd ~/esp32-imu-backend && export IMU_API_KEY=$(cat .api_key) && bash deploy/ensure-grafana.sh && docker compose up -d --build'
```
