# Good Vibes — monstro FE install (operator handoff)

Public dashboard: **https://apps.f0xx.org/app/good_vibes/**

Pattern matches [Android Cast](/app/androidcast_project/) — TLS on monstro, backend on artc0 Alpine VM.

## Files in this repo

| File | Install target on monstro |
|------|---------------------------|
| `fe/monstro-good-vibes-upstream.conf` | `/etc/nginx/good-vibes-upstream.conf` |
| `fe/monstro-apps-good-vibes.fragment` | include inside `apps.f0xx.org` server block |

## Steps (you run on monstro)

```bash
# 1. Upstream (http {} block — once)
sudo cp monstro-good-vibes-upstream.conf /etc/nginx/good-vibes-upstream.conf
# Add to /etc/nginx/nginx.conf inside http { }:
#   include /etc/nginx/good-vibes-upstream.conf;

# 2. Locations (apps.f0xx.org server block)
sudo cat monstro-apps-good-vibes.fragment >> /etc/nginx/apps.conf
# Or paste the location blocks before the final catch-all location / { }

# 3. Verify + reload
sudo nginx -t && sudo rc-service nginx reload
```

## Verify

```bash
curl -sS https://apps.f0xx.org/app/good_vibes/health
curl -sS -H "X-API-Key: $IMU_API_KEY" https://apps.f0xx.org/app/good_vibes/v1/health
```

Grafana: **https://apps.f0xx.org/app/good_vibes/grafana/**

## Android Cloud URL

After monstro is configured:

- **HTTPS (recommended):** `https://apps.f0xx.org/app/good_vibes`
- **Legacy Pi forwarder:** `http://artc0.f0xx.org:8090` (paths `/v1/…` only; no web UI)

## artc0 backend

Deployed separately — see `README-artc0.md` and `deploy-artc0.sh`.

Backend listens on VM `10.7.16.128:8080` (API + static UI) and `:3000` (Grafana).
