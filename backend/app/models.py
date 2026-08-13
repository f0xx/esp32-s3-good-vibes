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
        Index("ix_crashes_device_seq_pc", "device_id", "seq", "pc", unique=True),
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


class Machine(Base):
    """profiles.txt #2/#3: a physical machine/turbine/motor/bearing that owns
    one or more Sensor rows (ESP32 devices), each with its own reference
    profile group."""

    __tablename__ = "machines"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    machine_key: Mapped[str] = mapped_column(String(64), unique=True, index=True)
    name: Mapped[str] = mapped_column(String(128))
    kind: Mapped[str] = mapped_column(String(32), default="generic")
    notes: Mapped[str | None] = mapped_column(Text, nullable=True)
    created_ms: Mapped[int] = mapped_column(BigInteger)


class Sensor(Base):
    """One ESP32 device acting as a sensor on a Machine. Each sensor is
    calibrated independently (own reference profile group), per profiles.txt."""

    __tablename__ = "sensors"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    machine_id: Mapped[int] = mapped_column(Integer, index=True)
    device_id: Mapped[str] = mapped_column(String(64), unique=True, index=True)
    label: Mapped[str | None] = mapped_column(String(64), nullable=True)
    mount_note: Mapped[str | None] = mapped_column(String(128), nullable=True)
    created_ms: Mapped[int] = mapped_column(BigInteger)


class ReferenceProfile(Base):
    """One of up to 5 "ideal" recordings for a device (profiles.txt #2: "the
    operator records up to 5 ideal sampling profiles of the length of 30s
    max"). Storage format is intentionally flexible (raw_json / bands_json)
    since the doc leaves the exact representation TBD."""

    __tablename__ = "reference_profiles"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    device_id: Mapped[str] = mapped_column(String(64), index=True)
    slot: Mapped[int] = mapped_column(Integer)
    name: Mapped[str] = mapped_column(String(64), default="")
    created_ms: Mapped[int] = mapped_column(BigInteger)
    updated_ms: Mapped[int] = mapped_column(BigInteger)
    duration_ms: Mapped[int] = mapped_column(Integer, default=0)
    sample_hz: Mapped[float | None] = mapped_column(Float, nullable=True)
    format: Mapped[str] = mapped_column(String(16), default="band_rms")
    bands_json: Mapped[str | None] = mapped_column(Text, nullable=True)
    raw_json: Mapped[str | None] = mapped_column(Text, nullable=True)
    active: Mapped[bool] = mapped_column(default=True)

    __table_args__ = (
        Index("ix_refprofiles_device_slot", "device_id", "slot", unique=True),
    )

    @property
    def bands(self) -> list[float] | None:
        if not self.bands_json:
            return None
        try:
            data = json.loads(self.bands_json)
            return [float(x) for x in data] if isinstance(data, list) else None
        except (json.JSONDecodeError, TypeError, ValueError):
            return None

    @property
    def raw(self) -> dict | list | None:
        if not self.raw_json:
            return None
        try:
            return json.loads(self.raw_json)
        except (json.JSONDecodeError, TypeError):
            return None


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
