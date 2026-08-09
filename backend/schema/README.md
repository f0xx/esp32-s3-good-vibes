# P-schema — phone → cloud message formats (v1)

Phone is the **transmitter** (Case B/C). ESP32 never talks to the cloud directly on battery.

## Envelope (`imu.ingest.v1`)

All ingest POST bodies use this wrapper:

| Field | Type | Required | Notes |
|-------|------|----------|-------|
| `schema` | string | yes | Always `"imu.ingest.v1"` |
| `device_id` | string | yes | Stable ESP identity (MAC, user label, or BLE name hash) |
| `group_id` | string | no | MACHINE/HUMAN group for fusion (`machine-1`, `body-alice`) |
| `phone_id` | string | no | Android install id (relay attribution) |
| `sent_at_ms` | int | yes | Phone wall clock when upload started |
| `records` | array | yes | One or more typed records (see below) |

## Record: `verdict`

Edge vibration health score from ESP32 (`vd` in BLE STATUS) + phone timestamp.

| Field | Type | Notes |
|-------|------|-------|
| `type` | string | `"verdict"` |
| `ts_ms` | int | Phone time when verdict was stored |
| `seq` | int | ESP batch sequence (`s` in STATUS) |
| `level` | int | 0=OK, 1=WARN, 2=ALERT |
| `rms` | float | g |
| `peak` | float | g |
| `corr` | float | Pearson vs reference |
| `rms_delta` | float | ΔRMS vs reference |
| `pct` | int | Battery % |
| `voltage` | float | V |
| `power_profile` | int | optional `pp` from STATUS |
| `chip_temp_c` | float | optional `tc` |

## Record: `batch` (future)

Raw/computed/scene BLE batch JSON. Large — gzip recommended; phone-side only for now.

| Field | Type | Notes |
|-------|------|-------|
| `type` | string | `"batch"` |
| `ts_ms` | int | First sample time in batch |
| `seq` | int | ESP sequence |
| `mode` | int | 0 raw, 1 computed, 2 scene |
| `payload` | object | Parsed batch header + records (or opaque JSON string) |

## Record: `crash`

Post-mortem from ESP32 fatal error or watchdog (phone relay). See `crash.v1.json` and `docs/crash-reporting-roadmap.md`.

| Field | Type | Notes |
|-------|------|-------|
| `type` | string | `"crash"` |
| `ts_ms` | int | Phone or device time of report |
| `seq` | int | Monotonic crash sequence per device |
| `reason` | string | e.g. `panic`, `stack_overflow`, `render_stall` |
| `pc` | int | Xtensa program counter |
| `exccause` | int | optional exception cause |
| `excvaddr` | int | optional fault address |
| `thread_name` | string | Zephyr thread name |
| `fw_version` | string | e.g. `handshake v26` |
| `reset_reason` | int | ESP-IDF reset reason enum |
| `uptime_ms` | int | `k_uptime_get()` at fault |
| `backtrace` | int[] | Hex PCs (best-effort) |
| `detail` | object | Last telemetry snapshot, etc. |

## Record: `profile` (future)

DeviceConfigV1 blob snapshot + metadata for cloud sync.

## HTTP API (MVP)

```
POST /v1/ingest/verdicts
POST /v1/ingest/crashes
  Header: X-API-Key: <shared secret>
  Body: imu.ingest.v1 envelope with typed records

GET  /v1/health
GET  /v1/devices/{device_id}/verdicts?limit=50
GET  /v1/devices/{device_id}/crashes?limit=50
GET  /v1/crashes?group_id=…&limit=50
```

## Storage (VM)

- **Postgres** — verdict time-series, device registry, group membership
- **TimescaleDB** optional extension for rollups (add when graphing)
- Retention: 90 d verdicts default; batches tiered separately later

JSON Schema files in this directory validate payloads at ingest.
