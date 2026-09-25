"""Pydantic request/response models (imu.ingest.v1)."""

from math import isfinite
from typing import Literal

from pydantic import BaseModel, Field, field_validator


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
    cpu_mhz: int | None = Field(default=None, ge=0, le=255)
    apb_mhz: int | None = Field(default=None, ge=0, le=255)
    spool_free_b: int | None = Field(default=None, ge=0)
    spool_cap_b: int | None = Field(default=None, ge=0)
    spool_pending: int | None = Field(default=None, ge=0)
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
    soft_reboot_reason: str | None = Field(default=None, max_length=32)
    is_fatal: bool = True
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


class AhrsSampleIn(BaseModel):
    """Live AHRS orientation relay (phone -> backend -> web debug page).

    Deliberately NOT persisted to the DB — this is ephemeral high-rate debug telemetry
    (roughly BLE-tick rate), not durable crash/verdict/battery data. The backend just keeps
    the single latest sample per device in memory for the web page to poll. See
    zephyr/app/common/attitude.c (complementary filter, no magnetometer — yaw drifts) and
    ble_imu_gatt.c's "rot4" DATA JSON field (int16 x10000 on the wire; phone divides back to
    float before relaying here).
    """

    device_id: str = Field(min_length=1, max_length=64)
    unix_ms: int = Field(ge=0)
    seq: int = Field(default=0, ge=0)
    rot: list[float] = Field(min_length=9, max_length=9)


class AhrsLatestOut(BaseModel):
    device_id: str
    unix_ms: int
    seq: int
    rot: list[float]
    recv_ms: int


class GeoPointIn(BaseModel):
    """Phase 3 — GPS anchor + IMU dead-reckoning relay (phone -> backend -> map debug page).

    Like AhrsSampleIn, kept in memory only (bounded ring buffer per device, see
    _geo_routes in api.py) — this is preprod-demo-quality route debug data, not a durable
    trip log. `kind="gps"` points come straight from the phone's location fix; `kind="imu"`
    points are dead-reckoned on the phone from the ESP32's yaw heading + cumulative walk
    distance (see ble_imu_gatt.c's "yawd100"/"wdcm" DATA JSON fields), anchored to the most
    recent stable GPS fix.
    """

    device_id: str = Field(min_length=1, max_length=64)
    kind: Literal["gps", "imu"]
    unix_ms: int = Field(ge=0)
    lat: float = Field(ge=-90.0, le=90.0)
    lon: float = Field(ge=-180.0, le=180.0)
    accuracy_m: float | None = None

    @field_validator("lat", "lon")
    @classmethod
    def _finite_coord(cls, v: float) -> float:
        if not isfinite(v):
            raise ValueError("coordinate must be finite")
        return v

    @field_validator("accuracy_m")
    @classmethod
    def _finite_acc(cls, v: float | None) -> float | None:
        if v is None:
            return None
        if not isfinite(v):
            return None
        return v


class GeoPointOut(BaseModel):
    unix_ms: int
    lat: float
    lon: float
    accuracy_m: float | None = None


class GeoRouteOut(BaseModel):
    device_id: str
    gps: list[GeoPointOut]
    imu: list[GeoPointOut]


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
    cpu_mhz: int | None = None
    apb_mhz: int | None = None
    spool_free_b: int | None = None
    spool_cap_b: int | None = None
    spool_pending: int | None = None
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
    soft_reboot_reason: str | None = None
    is_fatal: bool = True
    uptime_ms: int | None
    backtrace: list[int] = Field(default_factory=list)
    detail: dict | None = None

    model_config = {"from_attributes": True}


class DeviceOut(BaseModel):
    device_id: str
    group_id: str | None
    last_seen_ms: int
    last_phone_id: str | None
    last_esp_ms: int = 0
    silent_for_ms: int = 0
    silent: bool = False
    verdict_count: int = 0
    crash_count: int = 0
    latest_level: int | None = None
    latest_rms: float | None = None
    latest_crash_ts: int | None = None

    model_config = {"from_attributes": True}


class PresenceGapOut(BaseModel):
    device_id: str
    silent_from_ms: int
    silent_until_ms: int
    gap_ms: int
    reason: str | None = None

    model_config = {"from_attributes": True}


class GroupDeviceVerdict(BaseModel):
    device_id: str
    level: int
    ts_ms: int
    seq: int
    rms: float | None = None
    corr: float | None = None


class GroupSilentDevice(BaseModel):
    device_id: str
    last_esp_ms: int
    silent_for_ms: int


class GroupStatusOut(BaseModel):
    group_id: str
    device_count: int
    reporting_count: int
    fused_level: int = Field(ge=0, le=2)
    confidence: float = Field(ge=0.0, le=1.0)
    devices: list[GroupDeviceVerdict]
    silent_count: int = 0
    silent_devices: list[GroupSilentDevice] = Field(default_factory=list)


class HealthOut(BaseModel):
    ok: bool
    schema_version: str = "imu.ingest.v1"
    config_schema: str = "device.config.v1"
    db: str
    timescaledb: bool = False
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


class MachineCreate(BaseModel):
    machine_key: str = Field(min_length=1, max_length=64)
    name: str = Field(min_length=1, max_length=128)
    kind: str = Field(default="generic", max_length=32)
    notes: str | None = None


class MachineOut(BaseModel):
    machine_key: str
    name: str
    kind: str
    notes: str | None
    created_ms: int
    sensor_count: int = 0

    model_config = {"from_attributes": True}


class SensorCreate(BaseModel):
    device_id: str = Field(min_length=1, max_length=64)
    label: str | None = Field(default=None, max_length=64)
    mount_note: str | None = Field(default=None, max_length=128)


class SensorOut(BaseModel):
    device_id: str
    machine_key: str
    label: str | None
    mount_note: str | None
    created_ms: int

    model_config = {"from_attributes": True}


class ReferenceProfilePut(BaseModel):
    name: str = Field(default="", max_length=64)
    duration_ms: int = Field(default=0, ge=0, le=30_000)
    sample_hz: float | None = Field(default=None, gt=0)
    format: str = Field(default="band_rms", max_length=16)
    bands: list[float] | None = Field(default=None, max_length=64)
    raw: dict | list | None = None
    active: bool = True


class ReferenceProfileOut(BaseModel):
    device_id: str
    slot: int = Field(ge=0, le=4)
    name: str
    created_ms: int
    updated_ms: int
    duration_ms: int
    sample_hz: float | None
    format: str
    bands: list[float] | None = None
    raw: dict | list | None = None
    active: bool

    model_config = {"from_attributes": True}


class TrendPoint(BaseModel):
    ts_ms: int
    value: float


class TrendOut(BaseModel):
    device_id: str
    metric: str
    samples: int
    trend: Literal["increasing", "decreasing", "none", "insufficient"]
    score: float = Field(ge=-1.0, le=1.0)
    early_warning: bool
    latest_value: float | None = None
    points: list[TrendPoint] = Field(default_factory=list)
    excluded_repair_intervals: int = 0


class RepairAction(BaseModel):
    action: Literal["start", "end"]
    device_ids: list[str] = Field(default_factory=list)
    fta_leaf: str | None = Field(default=None, max_length=64)
    parts: str | None = None
    notes: str | None = None
    new_ref_required: bool | None = None


class RepairOut(BaseModel):
    id: int
    machine_key: str
    device_ids: list[str] = Field(default_factory=list)
    started_ms: int
    ended_ms: int | None = None
    fta_leaf: str | None = None
    parts_changed: str | None = None
    notes: str | None = None
    new_ref_required: bool = True


class FtaHintOut(BaseModel):
    leaf_id: str
    label: str | None = None
    category: str | None = None
    confidence: float = 0.0
    speakable: bool = True
    evidence_flags: list[str] = Field(default_factory=list)


class OperatorStatusOut(BaseModel):
    device_id: str | None = None
    machine_key: str | None = None
    name: str | None = None
    kind: str | None = None
    candidate_level: int = 0
    operator_level: int = 0
    operator_alert: bool = False
    trend: str | None = None
    score: float | None = None
    early_warning: bool | None = None
    samples: int | None = None
    latest_value: float | None = None
    metric: str | None = None
    points: list[dict] = Field(default_factory=list)
    proofs: dict | None = None
    fta: dict | None = None
    sensors: list[dict] = Field(default_factory=list)
    repairs: list[RepairOut] = Field(default_factory=list)
    updated_ms: int = 0
    repair_open: bool = False


class DeviceConfigPut(BaseModel):
    revision: int = Field(ge=0)
    source: str = Field(default="be", max_length=16)
    app_version: str | None = Field(default=None, max_length=32)
    config: dict
    blob_b64: str | None = None
    force: bool = False


class BatteryBenchSampleRecord(BaseModel):
    type: Literal["battery_bench_sample"] = "battery_bench_sample"
    session_id: int = Field(ge=1)
    ts_ms: int = Field(ge=0)
    seq: int = Field(ge=0)
    voltage: float | None = None
    pct: int | None = Field(default=None, ge=0, le=100)
    trend_v: float | None = None
    src: int | None = Field(default=None, ge=0, le=2)
    cpu_mhz: int | None = Field(default=None, ge=0, le=255)
    imu_hz: int | None = Field(default=None, ge=0, le=255)
    render_hz: int | None = Field(default=None, ge=0, le=255)
    chip_temp_c: float | None = None
    uptime_ms: int | None = Field(default=None, ge=0)
    session_started_ms: int | None = Field(default=None, ge=0)
    session_stopped: bool = False
    label: str | None = Field(default=None, max_length=128)
    cell_mah: int | None = Field(default=500, ge=50, le=5000)
    profile_snapshot: dict | None = None


class BatteryBenchIngestEnvelope(BaseModel):
    schema: Literal["imu.ingest.v1"] = "imu.ingest.v1"
    device_id: str = Field(min_length=1, max_length=64)
    group_id: str | None = Field(default=None, max_length=64)
    phone_id: str | None = Field(default=None, max_length=64)
    sent_at_ms: int = Field(ge=0)
    records: list[BatteryBenchSampleRecord] = Field(min_length=1, max_length=500)


class BatteryBenchSessionOut(BaseModel):
    id: int
    session_id: int
    device_id: str
    group_id: str | None
    label: str | None
    cell_mah: int
    started_ms: int
    stopped_ms: int | None
    sample_count: int = 0

    model_config = {"from_attributes": True}


class BatteryBenchSampleOut(BaseModel):
    id: int
    session_id: int
    device_id: str
    seq: int
    ts_ms: int
    voltage: float | None
    pct: int | None
    trend_v: float | None
    src: int | None
    cpu_mhz: int | None
    imu_hz: int | None
    render_hz: int | None
    chip_temp_c: float | None
    uptime_ms: int | None
    est_ma: float | None

    model_config = {"from_attributes": True}


class ClockEventRecord(BaseModel):
    type: Literal["clock"] = "clock"
    ts_ms: int = Field(ge=0)
    drift_ms: int = 0
    corr_ms: int = 0
    tz_min: int = 0
    unix_sec: int = Field(ge=0, default=0)
    src: int = Field(ge=0, le=2, default=0)


class ClockIngestEnvelope(BaseModel):
    schema: Literal["imu.ingest.v1"] = "imu.ingest.v1"
    device_id: str = Field(min_length=1, max_length=64)
    group_id: str | None = Field(default=None, max_length=64)
    phone_id: str | None = Field(default=None, max_length=64)
    sent_at_ms: int = Field(ge=0)
    records: list[ClockEventRecord] = Field(min_length=1, max_length=100)


class ReferenceProfileIngestRecord(BaseModel):
    type: Literal["reference_profile"] = "reference_profile"
    slot: int = Field(ge=0, le=4)
    name: str = Field(default="", max_length=64)
    duration_ms: int = Field(ge=0, default=0)
    sample_hz: float | None = Field(default=None, gt=0)
    bands: list[float] | None = Field(default=None, max_length=8)
    raw: dict | None = None
    active: bool = False


class ReferenceProfileIngestEnvelope(BaseModel):
    schema: Literal["imu.ingest.v1"] = "imu.ingest.v1"
    device_id: str = Field(min_length=1, max_length=64)
    group_id: str | None = Field(default=None, max_length=64)
    phone_id: str | None = Field(default=None, max_length=64)
    sent_at_ms: int = Field(ge=0)
    records: list[ReferenceProfileIngestRecord] = Field(min_length=1, max_length=5)


class TelemetryRecord(BaseModel):
    type: Literal["telemetry"] = "telemetry"
    ts_ms: int = Field(ge=0)
    pct: int | None = Field(default=None, ge=0, le=100)
    voltage: float | None = None
    power_source: int | None = Field(default=None, ge=0, le=15)
    chip_temp_c: float | None = None
    cpu_mhz: int | None = Field(default=None, ge=0, le=255)
    apb_mhz: int | None = Field(default=None, ge=0, le=255)
    spool_free_b: int | None = Field(default=None, ge=0)
    spool_cap_b: int | None = Field(default=None, ge=0)
    spool_pending: int | None = Field(default=None, ge=0)
    dram_free_kb: int | None = Field(default=None, ge=0)
    fw_version: str | None = Field(default=None, max_length=64)
    fwc: int | None = Field(default=None, ge=0)
    spi_mhz: int | None = Field(default=None, ge=0, le=200)
    i2c_khz: int | None = Field(default=None, ge=0, le=5000)
    ble_rx_kb: int | None = Field(default=None, ge=0)
    ble_tx_kb: int | None = Field(default=None, ge=0)
    ble_rx_b: int | None = Field(default=None, ge=0)
    ble_tx_b: int | None = Field(default=None, ge=0)
    ble_rx_bps: int | None = Field(default=None, ge=0)
    ble_tx_bps: int | None = Field(default=None, ge=0)
    display_on: int | None = Field(default=None, ge=0, le=1)
    wifi_on: int | None = Field(default=None, ge=0, le=1)
    wifi_rssi: int | None = Field(default=None, ge=-127, le=20)
    wifi_ap: int | None = Field(default=None, ge=0, le=1)
    wifi_ssid: str | None = Field(default=None, max_length=64)


class TelemetryIngestEnvelope(BaseModel):
    schema: Literal["imu.ingest.v1"] = "imu.ingest.v1"
    device_id: str = Field(min_length=1, max_length=64)
    group_id: str | None = Field(default=None, max_length=64)
    phone_id: str | None = Field(default=None, max_length=64)
    sent_at_ms: int = Field(ge=0)
    records: list[TelemetryRecord] = Field(min_length=1, max_length=200)


class IngestBatchRecord(BaseModel):
    ts_ms: int = Field(ge=0)
    kind: Literal["verdict", "spectrum", "crash", "bench", "telemetry"] = "verdict"
    records: int = Field(ge=0, le=500)
    bytes: int = Field(ge=0)


class WearableRecord(BaseModel):
    """One low-rate companion-wearable sample (HR, SpO2, steps, kcal, distance, …).

    `source` is the producer (`mt200` watch, `esp32` for IMU walk_cm / MT200 RSSI,
    `phone` for ESP32↔phone RSSI);
    `device_id` on the envelope is the bridging ESP32 so Grafana/AHRS keys stay unchanged.
    RSSI kinds are `rssi_esp` (phone-measured) and `rssi_mt200` (ESP-measured); missing
    RSSI is stored as -127 dBm (HCI 127 means N/A, not a strong signal).
    """

    type: Literal["wearable"] = "wearable"
    ts_ms: int = Field(ge=0)
    seq: int = Field(default=0, ge=0)
    source: str = Field(default="mt200", min_length=1, max_length=16)
    kind: str = Field(min_length=1, max_length=16)
    value: float | None = None
    extra: dict | None = None


class WearableIngestEnvelope(BaseModel):
    schema: Literal["imu.ingest.v1"] = "imu.ingest.v1"
    device_id: str = Field(min_length=1, max_length=64)
    group_id: str | None = Field(default=None, max_length=64)
    phone_id: str | None = Field(default=None, max_length=64)
    sent_at_ms: int = Field(ge=0)
    records: list[WearableRecord] = Field(min_length=1, max_length=200)


class WearableKindLatest(BaseModel):
    kind: str
    source: str
    ts_ms: int
    seq: int
    value: float | None = None
    extra: dict | None = None


class WearableLatestOut(BaseModel):
    device_id: str
    recv_ms: int
    kinds: dict[str, WearableKindLatest]


class WearableSampleOut(BaseModel):
    device_id: str
    source: str
    kind: str
    ts_ms: int
    seq: int
    value: float | None = None
    extra: dict | None = None


class WearableDailyPoint(BaseModel):
    """One local calendar day of watch totals."""

    day: str  # YYYY-MM-DD
    steps: float = 0
    distance_m: float = 0
    kcal: float = 0
    hr_avg: float = 0
    spo2_avg: float = 0


class WearableDailyOut(BaseModel):
    device_id: str
    range_days: int
    tz_offset_min: int
    days: list[WearableDailyPoint]


class AiSuggestOut(BaseModel):
    ok: bool = True
    board: str
    device_id: str
    machine_key: str | None = None
    suggestion: str
    model: str | None = None
    generated_at_ms: int = 0
    error: str | None = None


class OtaEventOut(BaseModel):
    ts_ms: int
    fw_version: str | None = None
    outcome: str | None = None
    boot_part: str | None = None
    target_part: str | None = None
    ok: bool | None = None
    reason: str | None = None
    uptime_ms: int | None = None


class DeviceInsightsOut(BaseModel):
    device_id: str
    group_id: str | None = None
    last_seen_ms: int = 0
    last_esp_ms: int = 0
    silent_for_ms: int = 0
    silent: bool = False
    last_phone_id: str | None = None
    latest_fw: str | None = None
    latest_fwc: int | None = None
    battery_pct: int | None = None
    voltage: float | None = None
    chip_temp_c: float | None = None
    cpu_mhz: int | None = None
    apb_mhz: int | None = None
    spool_free_b: int | None = None
    spool_pending: int | None = None
    spi_mhz: int | None = None
    i2c_khz: int | None = None
    ble_rx_kb: int | None = None
    ble_tx_kb: int | None = None
    ble_rx_b: int | None = None
    ble_tx_b: int | None = None
    ble_rx_bps: int | None = None
    ble_tx_bps: int | None = None
    display_on: int | None = None
    wifi_on: int | None = None
    wifi_rssi: int | None = None
    wifi_ap: int | None = None
    wifi_ssid: str | None = None
    avg_rssi_esp: float | None = None
    avg_rssi_mt200: float | None = None
    last_rssi_esp: float | None = None
    last_rssi_mt200: float | None = None
    verdict_count: int = 0
    crash_count: int = 0
    last_uptime_ms: int | None = None
    ota: OtaEventOut | None = None
    recent_ota: list[OtaEventOut] = Field(default_factory=list)


class IngestBatchEnvelope(BaseModel):
    schema: Literal["imu.ingest.v1"] = "imu.ingest.v1"
    device_id: str = Field(min_length=1, max_length=64)
    group_id: str | None = Field(default=None, max_length=64)
    phone_id: str | None = Field(default=None, max_length=64)
    sent_at_ms: int = Field(ge=0)
    batches: list[IngestBatchRecord] = Field(min_length=1, max_length=50)
