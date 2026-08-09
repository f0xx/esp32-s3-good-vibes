"""ESP32 IMU Good Vibes — cloud ingest API + web dashboard."""

import json
import os
from pathlib import Path

from fastapi import FastAPI
from fastapi.responses import HTMLResponse
from fastapi.staticfiles import StaticFiles

from app.api import ensure_db, router as api_router
from app.auth import API_KEY

WEB_DIR = Path(__file__).resolve().parent.parent / "web"

app = FastAPI(title="Good Vibes IMU Backend", version="0.2.0")


def _demo_hints_json() -> str:
    hints = {
        "api_key": API_KEY,
        "grafana_url": "grafana/",
        "grafana_user": "admin",
    }
    grafana_pass = os.getenv("GRAFANA_ADMIN_PASSWORD", "").strip()
    if grafana_pass:
        hints["grafana_password"] = grafana_pass
    return json.dumps(hints)


def _render_index_html() -> str:
    template = (WEB_DIR / "index.html").read_text(encoding="utf-8")
    return template.replace("__DEMO_HINTS_JSON__", _demo_hints_json())


@app.on_event("startup")
def on_startup() -> None:
    ensure_db()

# Legacy + public UI paths (monstro proxies /app/good_vibes/ → artc0:8080)
app.include_router(api_router, prefix="/v1")
app.include_router(api_router, prefix="/app/good_vibes/v1")


@app.get("/health")
@app.get("/app/good_vibes/health")
def health_root():
    return {"ok": True, "ui": "/app/good_vibes/"}


@app.get("/app/good_vibes")
@app.get("/app/good_vibes/")
def good_vibes_index():
    index = WEB_DIR / "index.html"
    if index.is_file():
        return HTMLResponse(_render_index_html())
    return {"detail": "web UI not installed"}


if (WEB_DIR / "static").is_dir():
    app.mount(
        "/app/good_vibes/static",
        StaticFiles(directory=WEB_DIR / "static"),
        name="good_vibes_static",
    )
