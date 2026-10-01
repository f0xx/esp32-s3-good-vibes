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
    # Wall-clock of last ESP telemetry/verdict ingest (not wearable/geo).
    last_esp_ms: Mapped[int] = mapped_column(BigInteger, default=0)


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
    cpu_mhz: Mapped[int | None] = mapped_column(Integer, nullable=True)
    apb_mhz: Mapped[int | None] = mapped_column(Integer, nullable=True)
    spool_free_b: Mapped[int | None] = mapped_column(Integer, nullable=True)
    spool_cap_b: Mapped[int | None] = mapped_column(Integer, nullable=True)
    spool_pending: Mapped[int | None] = mapped_column(Integer, nullable=True)
    sent_at_ms: Mapped[int | None] = mapped_column(BigInteger, nullable=True)
    recv_ms: Mapped[int | None] = mapped_column(BigInteger, nullable=True, index=True)
    payload_bytes: Mapped[int | None] = mapped_column(Integer, nullable=True)
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
    soft_reboot_reason: Mapped[str | None] = mapped_column(String(32), nullable=True)
    is_fatal: Mapped[bool] = mapped_column(Integer, default=1)
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


class RepairEvent(Base):
    """START/END REPAIR window. Verdicts whose ts_ms fall inside
    [started_ms, ended_ms or now] are excluded from operator trend."""

    __tablename__ = "repair_events"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    machine_id: Mapped[int] = mapped_column(Integer, index=True)
    device_ids_json: Mapped[str] = mapped_column(Text, default="[]")
    started_ms: Mapped[int] = mapped_column(BigInteger, index=True)
    ended_ms: Mapped[int | None] = mapped_column(BigInteger, nullable=True, index=True)
    fta_leaf: Mapped[str | None] = mapped_column(String(64), nullable=True)
    parts_changed: Mapped[str | None] = mapped_column(Text, nullable=True)
    notes: Mapped[str | None] = mapped_column(Text, nullable=True)
    new_ref_required: Mapped[int] = mapped_column(Integer, default=1)

    @property
    def device_ids(self) -> list[str]:
        try:
            data = json.loads(self.device_ids_json or "[]")
            if isinstance(data, list):
                return [str(x) for x in data]
        except (json.JSONDecodeError, TypeError):
            pass
        return []


class OperatorStatusCache(Base):
    """Latest cheap GET for phone / Grafana. Recomputed on ingest."""

    __tablename__ = "operator_status"

    device_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    machine_key: Mapped[str | None] = mapped_column(String(64), nullable=True, index=True)
    operator_level: Mapped[int] = mapped_column(Integer, default=0)
    operator_alert: Mapped[int] = mapped_column(Integer, default=0)
    candidate_level: Mapped[int] = mapped_column(Integer, default=0)
    trend: Mapped[str] = mapped_column(String(16), default="insufficient")
    score: Mapped[float] = mapped_column(Float, default=0.0)
    early_warning: Mapped[int] = mapped_column(Integer, default=0)
    proofs_json: Mapped[str | None] = mapped_column(Text, nullable=True)
    fta_hints_json: Mapped[str | None] = mapped_column(Text, nullable=True)
    updated_ms: Mapped[int] = mapped_column(BigInteger, default=0)


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


class BatteryBenchSession(Base):
    __tablename__ = "battery_bench_sessions"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    session_id: Mapped[int] = mapped_column(BigInteger, index=True)
    device_id: Mapped[str] = mapped_column(String(64), index=True)
    group_id: Mapped[str | None] = mapped_column(String(64), nullable=True, index=True)
    phone_id: Mapped[str | None] = mapped_column(String(64), nullable=True)
    label: Mapped[str | None] = mapped_column(String(128), nullable=True)
    cell_mah: Mapped[int] = mapped_column(Integer, default=500)
    started_ms: Mapped[int] = mapped_column(BigInteger, index=True)
    stopped_ms: Mapped[int | None] = mapped_column(BigInteger, nullable=True)
    profile_json: Mapped[str | None] = mapped_column(Text, nullable=True)

    __table_args__ = (
        Index("ix_bbench_sessions_device_sid", "device_id", "session_id", unique=True),
    )


class BatteryBenchSample(Base):
    __tablename__ = "battery_bench_samples"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    session_id: Mapped[int] = mapped_column(BigInteger, index=True)
    device_id: Mapped[str] = mapped_column(String(64), index=True)
    group_id: Mapped[str | None] = mapped_column(String(64), nullable=True, index=True)
    phone_id: Mapped[str | None] = mapped_column(String(64), nullable=True)
    seq: Mapped[int] = mapped_column(BigInteger)
    ts_ms: Mapped[int] = mapped_column(BigInteger, index=True)
    voltage: Mapped[float | None] = mapped_column(Float, nullable=True)
    pct: Mapped[int | None] = mapped_column(Integer, nullable=True)
    trend_v: Mapped[float | None] = mapped_column(Float, nullable=True)
    src: Mapped[int | None] = mapped_column(Integer, nullable=True)
    cpu_mhz: Mapped[int | None] = mapped_column(Integer, nullable=True)
    imu_hz: Mapped[int | None] = mapped_column(Integer, nullable=True)
    render_hz: Mapped[int | None] = mapped_column(Integer, nullable=True)
    chip_temp_c: Mapped[float | None] = mapped_column(Float, nullable=True)
    uptime_ms: Mapped[int | None] = mapped_column(BigInteger, nullable=True)
    est_ma: Mapped[float | None] = mapped_column(Float, nullable=True)

    __table_args__ = (
        Index("ix_bbench_samples_device_sid_seq", "device_id", "session_id", "seq", unique=True),
    )


class ClockEvent(Base):
    __tablename__ = "clock_events"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    device_id: Mapped[str] = mapped_column(String(64), index=True)
    group_id: Mapped[str | None] = mapped_column(String(64), nullable=True, index=True)
    phone_id: Mapped[str | None] = mapped_column(String(64), nullable=True)
    ts_ms: Mapped[int] = mapped_column(BigInteger, index=True)
    drift_ms: Mapped[int] = mapped_column(BigInteger, default=0)
    corr_ms: Mapped[int] = mapped_column(BigInteger, default=0)
    tz_min: Mapped[int] = mapped_column(Integer, default=0)
    unix_sec: Mapped[int] = mapped_column(BigInteger, default=0)
    src: Mapped[int] = mapped_column(Integer, default=0)

    __table_args__ = (
        Index("ix_clock_device_ts", "device_id", "ts_ms"),
    )


class DeviceTelemetry(Base):
    """Periodic device STATUS samples (temp, clocks, spool)."""

    __tablename__ = "device_telemetry"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    device_id: Mapped[str] = mapped_column(String(64), index=True)
    group_id: Mapped[str | None] = mapped_column(String(64), nullable=True, index=True)
    phone_id: Mapped[str | None] = mapped_column(String(64), nullable=True)
    ts_ms: Mapped[int] = mapped_column(BigInteger, index=True)
    pct: Mapped[int | None] = mapped_column(Integer, nullable=True)
    voltage: Mapped[float | None] = mapped_column(Float, nullable=True)
    power_source: Mapped[int | None] = mapped_column(Integer, nullable=True)
    chip_temp_c: Mapped[float | None] = mapped_column(Float, nullable=True)
    cpu_mhz: Mapped[int | None] = mapped_column(Integer, nullable=True)
    apb_mhz: Mapped[int | None] = mapped_column(Integer, nullable=True)
    spool_free_b: Mapped[int | None] = mapped_column(Integer, nullable=True)
    spool_cap_b: Mapped[int | None] = mapped_column(Integer, nullable=True)
    spool_pending: Mapped[int | None] = mapped_column(Integer, nullable=True)
    dram_free_kb: Mapped[int | None] = mapped_column(Integer, nullable=True)
    fw_version: Mapped[str | None] = mapped_column(String(64), nullable=True)
    fwc: Mapped[int | None] = mapped_column(Integer, nullable=True)
    spi_mhz: Mapped[int | None] = mapped_column(Integer, nullable=True)
    i2c_khz: Mapped[int | None] = mapped_column(Integer, nullable=True)
    ble_rx_kb: Mapped[int | None] = mapped_column(Integer, nullable=True)
    ble_tx_kb: Mapped[int | None] = mapped_column(Integer, nullable=True)
    ble_rx_b: Mapped[int | None] = mapped_column(BigInteger, nullable=True)
    ble_tx_b: Mapped[int | None] = mapped_column(BigInteger, nullable=True)
    ble_rx_bps: Mapped[int | None] = mapped_column(Integer, nullable=True)
    ble_tx_bps: Mapped[int | None] = mapped_column(Integer, nullable=True)
    display_on: Mapped[int | None] = mapped_column(Integer, nullable=True)
    wifi_on: Mapped[int | None] = mapped_column(Integer, nullable=True)
    wifi_rssi: Mapped[int | None] = mapped_column(Integer, nullable=True)
    wifi_ap: Mapped[int | None] = mapped_column(Integer, nullable=True)
    wifi_ssid: Mapped[str | None] = mapped_column(String(64), nullable=True)
    sent_at_ms: Mapped[int | None] = mapped_column(BigInteger, nullable=True)
    recv_ms: Mapped[int | None] = mapped_column(BigInteger, nullable=True, index=True)
    payload_bytes: Mapped[int | None] = mapped_column(Integer, nullable=True)


class WearableSample(Base):
    """Low-rate companion-wearable samples (MT200 HR/SpO2/steps/…) keyed by the
    bridging ESP32 device_id. Intended as a Timescale hypertable on ts_ms."""

    __tablename__ = "wearable_samples"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    device_id: Mapped[str] = mapped_column(String(64), index=True)
    group_id: Mapped[str | None] = mapped_column(String(64), nullable=True, index=True)
    phone_id: Mapped[str | None] = mapped_column(String(64), nullable=True)
    source: Mapped[str] = mapped_column(String(16), default="mt200")
    kind: Mapped[str] = mapped_column(String(16), index=True)
    ts_ms: Mapped[int] = mapped_column(BigInteger, index=True)
    seq: Mapped[int] = mapped_column(BigInteger, default=0)
    value: Mapped[float | None] = mapped_column(Float, nullable=True)
    extra_json: Mapped[str | None] = mapped_column(Text, nullable=True)
    sent_at_ms: Mapped[int | None] = mapped_column(BigInteger, nullable=True)
    recv_ms: Mapped[int | None] = mapped_column(BigInteger, nullable=True, index=True)
    payload_bytes: Mapped[int | None] = mapped_column(Integer, nullable=True)

    __table_args__ = (
        Index("ix_wearable_device_ts_kind_seq", "device_id", "ts_ms", "kind", "seq", unique=True),
    )

    @property
    def extra(self) -> dict | None:
        if not self.extra_json:
            return None
        try:
            data = json.loads(self.extra_json)
            return data if isinstance(data, dict) else None
        except (json.JSONDecodeError, TypeError):
            return None


class CloudIngestBatch(Base):
    """Phone-reported cloud upload batches (records + bytes vs device time)."""

    __tablename__ = "cloud_ingest_batches"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    device_id: Mapped[str] = mapped_column(String(64), index=True)
    group_id: Mapped[str | None] = mapped_column(String(64), nullable=True, index=True)
    phone_id: Mapped[str | None] = mapped_column(String(64), nullable=True)
    ts_ms: Mapped[int] = mapped_column(BigInteger, index=True)
    kind: Mapped[str] = mapped_column(String(16), default="verdict")
    records: Mapped[int] = mapped_column(Integer, default=0)
    bytes: Mapped[int] = mapped_column(Integer, default=0)
    sent_at_ms: Mapped[int | None] = mapped_column(BigInteger, nullable=True)
    recv_ms: Mapped[int | None] = mapped_column(BigInteger, nullable=True, index=True)


class PresenceGap(Base):
    """Closed ingest silence: ESP telemetry/verdicts stopped then resumed."""

    __tablename__ = "presence_gaps"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    device_id: Mapped[str] = mapped_column(String(64), index=True)
    group_id: Mapped[str | None] = mapped_column(String(64), nullable=True, index=True)
    silent_from_ms: Mapped[int] = mapped_column(BigInteger, index=True)
    silent_until_ms: Mapped[int] = mapped_column(BigInteger, index=True)
    gap_ms: Mapped[int] = mapped_column(BigInteger)
    reason: Mapped[str] = mapped_column(String(32), default="ingest_gap")

    __table_args__ = (
        Index(
            "ix_presence_gaps_device_from_until",
            "device_id",
            "silent_from_ms",
            "silent_until_ms",
            unique=True,
        ),
    )
