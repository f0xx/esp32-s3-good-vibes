"""Simple shared-secret API key auth."""

import os

from fastapi import Header, HTTPException, status

API_KEY = os.getenv("IMU_API_KEY", "dev-change-me")


def require_api_key(x_api_key: str | None = Header(default=None, alias="X-API-Key")) -> None:
    if not API_KEY:
        return
    if not x_api_key or x_api_key != API_KEY:
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="invalid api key")
