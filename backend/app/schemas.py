"""Pydantic request/response models (imu.ingest.v1)."""

from typing import Literal

from pydantic import BaseModel, Field


class VerdictRecord(BaseModel):
    type: Literal["verdict"] = "verdict"
    ts_ms: int = Field(ge=0)
    seq: int = Field(ge=0)
    level: int = Field(ge=0, le=2)
    rms: float | None = None
    peak: float | None = None
    corr: float | None = None
    rms_delta: float | None = None
    pct: int | None = Field(default=None, ge=0, le=100)
    voltage: float | None = None
    power_profile: int | None = Field(default=None, ge=0, le=5)
    chip_temp_c: float | None = None
    band_corr: float | None = Field(default=None, ge=-1.0, le=1.0)
    band_delta_max: float | None = Field(default=None, ge=0.0)
    bands: list[float] | None = Field(default=None, max_length=8)
    edge_crest: float | None = None
    edge_zcr_hz: float | None = None
    edge_hf_ratio: float | None = None
    session_seq: int | None = Field(default=None, ge=0)
    cap_mix_sec: int | None = Field(default=None, ge=0)


class SpectrumRecord(BaseModel):
    type: Literal["spectrum"] = "spectrum"
    ts_ms: int = Field(ge=0)
    seq: int = Field(ge=0)
    sample_hz: float = Field(gt=0)
    bin_hz: float = Field(gt=0)
    bins: list[float] = Field(min_length=4, max_length=512)
    peak_hz: float | None = None
    peak_mag: float | None = None
    axis: str = Field(default="mag", max_length=8)


class CrashRecord(BaseModel):
    type: Literal["crash"] = "crash"
    ts_ms: int = Field(ge=0)
    seq: int = Field(ge=0)
    reason: str | None = Field(default=None, max_length=128)
    pc: int | None = Field(default=None, ge=0)
    exccause: int | None = Field(default=None, ge=0)
    excvaddr: int | None = Field(default=None, ge=0)
    thread_name: str | None = Field(default=None, max_length=32)
    fw_version: str | None = Field(default=None, max_length=64)
    reset_reason: int | None = Field(default=None, ge=0)
    uptime_ms: int | None = Field(default=None, ge=0)
    backtrace: list[int] = Field(default_factory=list, max_length=32)
    detail: dict | None = None


class CrashIngestEnvelope(BaseModel):
    schema: Literal["imu.ingest.v1"] = "imu.ingest.v1"
    device_id: str = Field(min_length=1, max_length=64)
    group_id: str | None = Field(default=None, max_length=64)
    phone_id: str | None = Field(default=None, max_length=64)
    sent_at_ms: int = Field(ge=0)
    records: list[CrashRecord] = Field(min_length=1, max_length=20)


class IngestEnvelope(BaseModel):
    schema: Literal["imu.ingest.v1"] = "imu.ingest.v1"
    device_id: str = Field(min_length=1, max_length=64)
    group_id: str | None = Field(default=None, max_length=64)
    phone_id: str | None = Field(default=None, max_length=64)
    sent_at_ms: int = Field(ge=0)
    records: list[VerdictRecord] = Field(min_length=1, max_length=500)


class SpectrumIngestEnvelope(BaseModel):
    schema: Literal["imu.ingest.v1"] = "imu.ingest.v1"
    device_id: str = Field(min_length=1, max_length=64)
    group_id: str | None = Field(default=None, max_length=64)
    phone_id: str | None = Field(default=None, max_length=64)
    sent_at_ms: int = Field(ge=0)
    records: list[SpectrumRecord] = Field(min_length=1, max_length=50)


class IngestResult(BaseModel):
    accepted: int
    duplicates: int
    device_id: str


class VerdictOut(BaseModel):
    id: int
    device_id: str
    group_id: str | None
    ts_ms: int
    seq: int
    level: int
    rms: float | None
    peak: float | None
    corr: float | None
    rms_delta: float | None
    pct: int | None
    voltage: float | None
    power_profile: int | None
    chip_temp_c: float | None
    band_corr: float | None = None
    band_delta_max: float | None = None
    bands: list[float] | None = None
    edge_crest: float | None = None
    edge_zcr_hz: float | None = None
    edge_hf_ratio: float | None = None
    edge_score: float | None = None
    edge_risk: str | None = None

    model_config = {"from_attributes": True}


class SpectrumOut(BaseModel):
    id: int
    device_id: str
    group_id: str | None
    ts_ms: int
    seq: int
    sample_hz: float
    bin_hz: float
    bins: list[float]
    peak_hz: float | None
    peak_mag: float | None
    axis: str | None

    model_config = {"from_attributes": True}


class CrashOut(BaseModel):
    id: int
    device_id: str
    group_id: str | None
    phone_id: str | None
    ts_ms: int
    seq: int
    reason: str | None
    pc: int | None
    exccause: int | None
    excvaddr: int | None
    thread_name: str | None
    fw_version: str | None
    reset_reason: int | None
    uptime_ms: int | None
    backtrace: list[int] = Field(default_factory=list)
    detail: dict | None = None

    model_config = {"from_attributes": True}


class DeviceOut(BaseModel):
    device_id: str
    group_id: str | None
    last_seen_ms: int
    last_phone_id: str | None
    verdict_count: int = 0
    crash_count: int = 0
    latest_level: int | None = None
    latest_rms: float | None = None
    latest_crash_ts: int | None = None

    model_config = {"from_attributes": True}


class GroupDeviceVerdict(BaseModel):
    device_id: str
    level: int
    ts_ms: int
    seq: int
    rms: float | None = None
    corr: float | None = None


class GroupStatusOut(BaseModel):
    group_id: str
    device_count: int
    reporting_count: int
    fused_level: int = Field(ge=0, le=2)
    confidence: float = Field(ge=0.0, le=1.0)
    devices: list[GroupDeviceVerdict]


class HealthOut(BaseModel):
    ok: bool
    schema_version: str = "imu.ingest.v1"
    config_schema: str = "device.config.v1"
    db: str
    ui_path: str = "/app/good_vibes/"


class ConfigRecord(BaseModel):
    type: Literal["config"] = "config"
    ts_ms: int = Field(ge=0)
    revision: int = Field(ge=0)
    local_revision: int = Field(default=0, ge=0)
    source: str = Field(default="phone", max_length=16)
    app_version: str | None = Field(default=None, max_length=32)
    config: dict
    blob_b64: str | None = None


class ConfigIngestEnvelope(BaseModel):
    schema: Literal["imu.ingest.v1"] = "imu.ingest.v1"
    device_id: str = Field(min_length=1, max_length=64)
    group_id: str | None = Field(default=None, max_length=64)
    phone_id: str | None = Field(default=None, max_length=64)
    sent_at_ms: int = Field(ge=0)
    records: list[ConfigRecord] = Field(min_length=1, max_length=5)


class DeviceConfigOut(BaseModel):
    device_id: str
    revision: int
    local_revision: int = 0
    source: str
    app_version: str | None = None
    ts_ms: int
    config: dict
    blob_b64: str | None = None

    model_config = {"from_attributes": True}


class DeviceConfigPut(BaseModel):
    revision: int = Field(ge=0)
    source: str = Field(default="be", max_length=16)
    app_version: str | None = Field(default=None, max_length=32)
    config: dict
    blob_b64: str | None = None
    force: bool = False
