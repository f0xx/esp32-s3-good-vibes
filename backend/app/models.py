"""SQLAlchemy models."""

import json

from sqlalchemy import BigInteger, Float, Index, Integer, String, Text
from sqlalchemy.orm import Mapped, mapped_column

from app.db import Base


class Device(Base):
    __tablename__ = "devices"

    device_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    group_id: Mapped[str | None] = mapped_column(String(64), nullable=True)
    last_seen_ms: Mapped[int] = mapped_column(BigInteger, default=0)
    last_phone_id: Mapped[str | None] = mapped_column(String(64), nullable=True)


class Verdict(Base):
    __tablename__ = "verdicts"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    device_id: Mapped[str] = mapped_column(String(64), index=True)
    group_id: Mapped[str | None] = mapped_column(String(64), nullable=True, index=True)
    phone_id: Mapped[str | None] = mapped_column(String(64), nullable=True)
    ts_ms: Mapped[int] = mapped_column(BigInteger, index=True)
    seq: Mapped[int] = mapped_column(BigInteger)
    level: Mapped[int] = mapped_column(Integer)
    rms: Mapped[float | None] = mapped_column(Float, nullable=True)
    peak: Mapped[float | None] = mapped_column(Float, nullable=True)
    corr: Mapped[float | None] = mapped_column(Float, nullable=True)
    rms_delta: Mapped[float | None] = mapped_column(Float, nullable=True)
    pct: Mapped[int | None] = mapped_column(Integer, nullable=True)
    voltage: Mapped[float | None] = mapped_column(Float, nullable=True)
    power_profile: Mapped[int | None] = mapped_column(Integer, nullable=True)
    chip_temp_c: Mapped[float | None] = mapped_column(Float, nullable=True)
    raw_json: Mapped[str | None] = mapped_column(Text, nullable=True)

    __table_args__ = (
        Index("ix_verdicts_device_seq", "device_id", "seq", unique=True),
    )


class Spectrum(Base):
    __tablename__ = "spectra"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    device_id: Mapped[str] = mapped_column(String(64), index=True)
    group_id: Mapped[str | None] = mapped_column(String(64), nullable=True, index=True)
    phone_id: Mapped[str | None] = mapped_column(String(64), nullable=True)
    ts_ms: Mapped[int] = mapped_column(BigInteger, index=True)
    seq: Mapped[int] = mapped_column(BigInteger)
    sample_hz: Mapped[float] = mapped_column(Float)
    bin_hz: Mapped[float] = mapped_column(Float)
    bins_json: Mapped[str] = mapped_column(Text)
    peak_hz: Mapped[float | None] = mapped_column(Float, nullable=True)
    peak_mag: Mapped[float | None] = mapped_column(Float, nullable=True)
    axis: Mapped[str | None] = mapped_column(String(8), nullable=True)

    __table_args__ = (
        Index("ix_spectra_device_seq", "device_id", "seq", unique=True),
    )

    @property
    def bins(self) -> list[float]:
        try:
            data = json.loads(self.bins_json)
            if isinstance(data, list):
                return [float(x) for x in data]
        except (json.JSONDecodeError, TypeError, ValueError):
            pass
        return []


class Crash(Base):
    __tablename__ = "crashes"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    device_id: Mapped[str] = mapped_column(String(64), index=True)
    group_id: Mapped[str | None] = mapped_column(String(64), nullable=True, index=True)
    phone_id: Mapped[str | None] = mapped_column(String(64), nullable=True)
    ts_ms: Mapped[int] = mapped_column(BigInteger, index=True)
    seq: Mapped[int] = mapped_column(BigInteger)
    reason: Mapped[str | None] = mapped_column(String(128), nullable=True)
    pc: Mapped[int | None] = mapped_column(BigInteger, nullable=True)
    exccause: Mapped[int | None] = mapped_column(Integer, nullable=True)
    excvaddr: Mapped[int | None] = mapped_column(BigInteger, nullable=True)
    thread_name: Mapped[str | None] = mapped_column(String(32), nullable=True)
    fw_version: Mapped[str | None] = mapped_column(String(64), nullable=True)
    reset_reason: Mapped[int | None] = mapped_column(Integer, nullable=True)
    uptime_ms: Mapped[int | None] = mapped_column(BigInteger, nullable=True)
    backtrace_json: Mapped[str | None] = mapped_column(Text, nullable=True)
    detail_json: Mapped[str | None] = mapped_column(Text, nullable=True)

    __table_args__ = (
        Index("ix_crashes_device_seq", "device_id", "seq", unique=True),
    )

    @property
    def backtrace(self) -> list[int]:
        try:
            data = json.loads(self.backtrace_json or "[]")
            if isinstance(data, list):
                return [int(x) for x in data]
        except (json.JSONDecodeError, TypeError, ValueError):
            pass
        return []


class DeviceConfigRevision(Base):
    __tablename__ = "device_configs"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    device_id: Mapped[str] = mapped_column(String(64), index=True)
    revision: Mapped[int] = mapped_column(BigInteger, index=True)
    local_revision: Mapped[int] = mapped_column(BigInteger, default=0)
    source: Mapped[str] = mapped_column(String(16), default="esp")
    app_version: Mapped[str | None] = mapped_column(String(32), nullable=True)
    phone_id: Mapped[str | None] = mapped_column(String(64), nullable=True)
    ts_ms: Mapped[int] = mapped_column(BigInteger, index=True)
    config_json: Mapped[str] = mapped_column(Text)
    blob_b64: Mapped[str | None] = mapped_column(Text, nullable=True)

    __table_args__ = (
        Index("ix_device_configs_device_rev", "device_id", "revision", unique=True),
    )

    @property
    def config(self) -> dict:
        try:
            data = json.loads(self.config_json)
            return data if isinstance(data, dict) else {}
        except (json.JSONDecodeError, TypeError):
            return {}
