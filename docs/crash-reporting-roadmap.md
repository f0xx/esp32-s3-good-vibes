# Crash reporting roadmap

Lightweight post-mortem pipeline (DIY Memfault alternative) for Waveshare ESP32-S3-LCD-1.47B / Zephyr `handshake`.

## Goals

- Detect **stalls** (render/IMU heartbeats) and **hard faults** (assert, stack overflow, WDT)
- Capture **PC + backtrace** where possible
- Upload compact records **BLE → Android → cloud**
- View reports in **Good Vibes web UI** (`/app/good_vibes/` → Crash reports table)

## Architecture

```
Device (Zephyr)          Phone (Android)           Backend
─────────────────        ───────────────         ─────────────
heartbeat / WDT    →     (future)           →     (stall metrics — step 3)
fatal / coredump   →     CloudUploader      →     POST /v1/ingest/crashes
NVS crash record   →     BLE GATT read      →     GET  …/crashes
flash coredump     →     (optional bulk)    →     symbolicate with zephyr.elf
```

## Implementation phases

| Step | Status | What |
|------|--------|------|
| **0** | ✅ | Backend `crashes` table, ingest/list API, web UI (empty OK) |
| **1** | ✅ | Dev: `CONFIG_DEBUG_COREDUMP` + `CONFIG_STACK_SENTINEL` via `prj_crash.conf` |
| **2** | ✅ | Flash coredump partition, BLE export, Android upload |
| **3a** | ✅ | Task WDT + render stall → panic / coredump / reboot |
| **3b** | ✅ | NVS crash ring (5 slots), 16 KiB coredump, telemetry in detail, multi-slot BLE upload |
| **3c** | ✅ | addr2line symbolication on crash ingest + web UI frames |
| **4** | ✅ | Debug-only crash inject + BIST (`CONFIG_APP_CRASH_DEBUG`), web UI crash detail modal |

---

## Step 4 — Debug inject + BIST (non-production)

Enabled only when `prj_crash.conf` sets `CONFIG_APP_CRASH_DEBUG=y` (default with `CRASH_DEBUG=1` flash).

**Production strip:** `CRASH_DEBUG=0 bash zephyr/scripts/flash-zephyr.sh handshake` — omits `prj_crash.conf` entirely; inject/BIST code is `#if 0`.

### BLE crash GATT CTRL (debug builds)

| JSON | Action |
|------|--------|
| `{"op":"inject","kind":"panic"}` | `k_panic("debug_inject")` after 250 ms |
| `{"op":"inject","kind":"assert"}` | `__ASSERT(false, …)` |
| `{"op":"inject","kind":"null"}` | Null pointer write |
| `{"op":"inject","kind":"div0"}` | Integer divide by zero |
| `{"op":"inject","kind":"stack"}` | Stack overflow (recursive) |
| `{"op":"inject","kind":"wdt"}` | Infinite sleep → task/HW WDT |
| `{"op":"bist"}` | Re-run built-in self-test |

STATUS adds `"dbg":1` and `"bist":"ok"` or `"bist":"fail:imu,…"`.

### BIST checks (smoke)

| Flag | Test |
|------|------|
| IMU | QMI8658 WHO_AM_I == 0x05 |
| heap | `k_malloc(512)` / `k_free` |
| cfg | `device_config_defaults()` magic |
| crash | `crash_ring_init()` |

Boot runs BIST once after IMU init on debug builds.

### Phone UI

**Device → Crash debug (dev)…** — inject menu + BIST. Requires debug firmware; production images ignore inject writes.

### Web UI

Good Vibes dashboard: **Latest crash** card, clickable rows → modal with full backtrace/symbols/`detail` JSON, group-wide crash scope toggle.

---

## Step 3c — Symbolication (done)

- **`backend/app/symbolicate.py`** — `addr2line` on ingest when `ZEPHYR_ELF_PATH` set (or default build path)
- Crash **`detail.frames`**: `[{pc, symbol}, …]` stored with each ingest
- Web UI backtrace column shows symbols when present

Deploy: copy `zephyr.elf` to server (`/opt/imu/zephyr.elf`) or set `ZEPHYR_ELF_PATH` in backend env.

---

## Step 0 — Cloud + UI (done)

### API

```
POST /v1/ingest/crashes          # same envelope as verdicts (imu.ingest.v1)
GET  /v1/devices/{id}/crashes    # per-device list
GET  /v1/crashes?group_id=…      # fleet view
```

Record type: `crash` — see `backend/schema/crash.v1.json`.

### Web UI

Dashboard section **Crash reports** — shows empty hint until first ingest.

---

## Step 1 — Dev coredump on serial (done)

Extra Kconfig fragment merged by `flash-zephyr.sh` when `CRASH_DEBUG=1` (default for `handshake`):

| Option | Purpose |
|--------|---------|
| `CONFIG_DEBUG_COREDUMP=y` | Snapshot on fatal error |
| `CONFIG_DEBUG_COREDUMP_BACKEND_LOGGING=y` | Print `#CD:BEGIN#…#CD:END#` on USB CDC |
| `CONFIG_DEBUG_COREDUMP_MEMORY_DUMP_MIN=y` | Smaller dump (fault thread stack) |
| `CONFIG_STACK_SENTINEL=y` | Magic at stack base → graceful fatal on overflow |
| `CONFIG_ASSERT=y` | Catch invariant violations |

### Trigger a test crash (manual)

Add temporarily in `main.c`:

```c
k_panic("crash test");
```

Capture serial log, then offline:

```bash
python $ZEPHYR_BASE/scripts/coredump/coredump_serial_log_parser.py crash.log crash.bin
python $ZEPHYR_BASE/scripts/coredump/coredump_gdbserver.py \
  ~/zephyrproject/zephyr/build/zephyr/zephyr.elf crash.bin
# (gdb) bt
```

Disable coredump for size-sensitive builds:

```bash
CRASH_DEBUG=0 bash zephyr/scripts/flash-zephyr.sh handshake
```

---

## Step 2 — Flash coredump + BLE (implemented)

1. **DTS** — `coredump-partition` 4 KiB @ flash; `CONFIG_DEBUG_COREDUMP_BACKEND_FLASH_PARTITION=y` via `prj_crash.conf`
2. **BLE GATT** — service `4a6e0301`: INFO (compact JSON), CTRL (`clear` / `read` chunk), DATA (raw dump bytes)
3. **Android** — on BLE connect: read INFO → `crashes.jsonl` → `POST /v1/ingest/crashes` → clear ESP after ack
4. **Backend** — `POST /v1/ingest/crashes`, web UI crash table (step 0)

Optional later: `POST /v1/crashes/{id}/symbolicate` with stored ELF build id.

---

## Step 3 — Production watchdog + compact record

### Step 3a — Task WDT + render stall panic (done)

- `stall_watchdog.c` — `CONFIG_TASK_WDT` + HW fallback (`watchdog0`), main feed 15 s, render feed 12 s
- Two consecutive 10 s windows with `hb_render_frames==0` while screen on → `k_panic("render_stall")`
- Total CPU freeze → task/HW WDT → `k_panic("task_wdt")` → coredump flash → reboot

### Step 3b — NVS crash ring + telemetry (done)

- **Flash layout** — `crash-ring` 2 KiB (5×256 B records + header), `coredump-partition` 16 KiB
- **`crash_ring_store.c`** — append on panic/WDT boot, survives weeks/months offline
- **Telemetry** — last `render_hz`, `imu_hz`, `bat_mv`, `bat_pct`, `power_profile` in `detail`
- **BLE** — INFO (single or list), CTRL `info`/`list`/`clear` + optional slot
- **Android** — `fetchAllPendingCrashes()`, per-slot clear after cloud ack

### Step 3c+ (done)

1. ~~**CI symbolication**~~ — step 3c (`symbolicate.py` on ingest)

---

## Step 3 (legacy checklist)

1. ~~**Task watchdog**~~ — step 3a
2. **`crash_report.c`** — reset reason at boot ✅
3. ~~**Fatal hook**~~ — step 3b (`crash_ring_store.c`)
4. ~~**Telemetry link**~~ — step 3b (`power_manager_telemetry_snapshot`)
5. ~~**CI**~~ — step 3c

---

## Memfault comparison

| | Memfault | This roadmap |
|--|----------|--------------|
| SDK size | Full fleet SDK + HTTP | Zephyr coredump + thin BLE upload |
| Cost | Paid tiers / 100-device free cap | Your infra only |
| Backtrace | Cloud symbolication | GDB + addr2line in CI |
| Best for | Large fleets, OTA analytics | Learning + Good Vibes scale |

Memfault remains useful as a **comparison spike** on a branch; not required for the pipeline above.

---

## Related files

| Path | Role |
|------|------|
| `backend/app/models.py` | `Crash` SQLAlchemy model |
| `backend/app/api.py` | ingest + list routes |
| `backend/web/index.html` | Crash reports table |
| `zephyr/app/handshake/prj_crash.conf` | Step 1 Kconfig |
| `zephyr/scripts/flash-zephyr.sh` | Merges `prj_crash.conf` when `CRASH_DEBUG=1` |
