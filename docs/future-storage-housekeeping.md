# Future: sensor data storage / housekeeping (proposal)

Status: **not implemented** — recorded 2026-07-23 for later revisit.

Context: ESP32-S3-LCD-1.47B IMU project. Board is a **live BLE JSON provider**; phone is the primary **recorder + renderer**. Partition today: `app3M_fat9M_16MB` (~9 MB FAT available).

## Decision summary

| Option | Verdict |
|---|---|
| RAM ring + BLE batches + phone storage | **Default — keep** |
| NVS (`Preferences`) for config/calibration | **Yes** — small, wear-leveled |
| FAT append log (batched, low rate) | **Maybe** — offline session export |
| SPIFFS | **Skip** — prefer FAT or LittleFS if needed |
| SQLite on internal flash | **Overkill** unless on-device SQL queries are required |
| SD card bulk log | **Tier 3** — hours/days of history without phone |

## Why not high-rate flash logging

- IMU poll ~100 Hz; scene ~30 Hz.
- Internal NAND flash endurance ~10k–100k erase cycles per sector (order of magnitude).
- **100 Hz writes to flash** → unacceptable wear and latency jitter.
- Safe patterns: batch writes (4–16 KB chunks), decimate before persist, rotate/circular files.

## Recommended tiers (implement in this order if needed)

### Tier 0 — current / recommended default

- **RAM ring buffer** for BLE `DATA` batches (already the model).
- **Phone** stores history and renders.
- **NVS only** on board: WiFi creds, gyro cal offsets, optional walk total, BLE poll prefs.

No filesystem DB required.

### Tier 1 — lightweight FAT housekeeping

Use when: offline logging without phone, session export, crash artifacts.

- Append-only **JSONL** or compact **binary** log per walk session.
- Write **decimated** data (e.g. computed @ 1–10 Hz, not raw @ 100 Hz).
- Flush in **large chunks** (4–16 KB), not per sample.
- **Rotate**: keep last N files or fixed-size circular log file on FAT.
- Mode guidance:
  - Raw 100 Hz → do **not** persist on internal flash (or SD + batching only).
  - Computed → OK at 1 Hz summaries.
  - Scene geometry → skip; recompute from computed state.

### Tier 2 — SQLite (only if requirements grow)

Use when: multi-hour **offline** logging **and** on-device structured queries (aggregates, filters) without phone/PC.

- Heavy RAM/flash cost; journal/WAL adds extra writes.
- Prefer **DB file on SD card**, not internal flash.
- Large transactions, infrequent commits; isolate from IMU/render loop.

### Tier 3 — SD card for bulk history

- Board has TF slot (GPIO 14/15/16/17/18/21).
- Internal flash: firmware + NVS + small config.
- SD: long raw/computed logs, optional SQLite.

## Flash wear cheat sheet

| Pattern | Wear risk |
|---|---|
| Config once at boot | negligible |
| 1 summary / minute | fine |
| 1 batch / 10 s circular log | usually fine |
| 100 Hz IMU to flash | **avoid** |

## Open questions when revisiting

1. Must the board log **without any phone connected**? For how long?
2. Need **query semantics** on device (SQL) or is append + phone-side parse enough?
3. Export path: BLE read, WiFi HTTP, USB MSC, or pull SD?
4. Retention policy: last session, last 24 h, until full?

## Suggested first implementation (if Tier 1 approved)

1. `SessionLog` module: RAM queue → FAT append `@ 1 Hz` computed records only.
2. File naming: `/walk_YYYYMMDD_HHMMSS.jsonl` or single `/log/current.bin` circular buffer.
3. BLE characteristic or WiFi endpoint: `GET /log` / `READ log_meta` for phone pull.
4. No SQLite until a concrete query requirement exists.

## Related files

- `board_config.h` — partition constants
- `ble/ble_protocol.h` — live transport (primary data path today)
- `network/network_store.cpp` — NVS pattern to mirror for small persisted state
- `sketch.txt` — hardware / partition notes
