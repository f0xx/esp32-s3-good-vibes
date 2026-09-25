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
heartbeat / soft WDT →   (future)           →     (stall metrics)
fatal / #CD logging  →   CloudUploader      →     POST /v1/ingest/crashes
NVS crash ring       →   BLE GATT read      →     GET  …/crashes
desk USB #CD         →   esp_coredump.sh    →     GDB bt (coredump_gdbserver --pipe)
```

**Do not reinvent stall isolation with draw/IMU rate knobs.** Desk diagnosis uses the settings below (thread analyzer, soft WDT, logging coredump). Field keeps soft WDT + RTC crash ring.

---

## Implementation phases

| Step | Status | What |
|------|--------|------|
| **0** | ✅ | Backend `crashes` table, ingest/list API, web UI (empty OK) |
| **1** | ✅ | Desk: logging coredump + stack sentinel + assert via `prj_crash.conf` |
| **2** | ✅ | NVS crash ring + BLE export + Android upload (flash coredump backend **unsafe** on ESP32-S3) |
| **3a** | ✅ | Soft task WDT + render stall → panic / reboot (`TASK_WDT_HW_FALLBACK=n`) |
| **3b** | ✅ | NVS crash ring (5 slots), telemetry in detail, multi-slot BLE upload |
| **3c** | ✅ | addr2line symbolication on crash ingest + web UI frames |
| **4** | ✅ | Debug-only crash inject + BIST (`CONFIG_APP_CRASH_DEBUG`), web UI crash detail modal |
| **5** | 🔧 | Desk SMP visibility: thread analyzer (in `prj_crash.conf`); mutex/SystemView optional |

---

## Canonical desk Kconfig (`prj_crash.conf`)

Merged by `flash-zephyr.sh` when `CRASH_DEBUG=1` (default for `handshake`):

| Option | Purpose |
|--------|---------|
| `CONFIG_DEBUG_COREDUMP=y` | Snapshot on fatal |
| `CONFIG_DEBUG_COREDUMP_BACKEND_LOGGING=y` | `#CD:BEGIN#…#CD:END#` on USB CDC |
| `CONFIG_DEBUG_COREDUMP_MEMORY_DUMP_MIN=y` | Smaller dump (fault thread stack) |
| `CONFIG_STACK_SENTINEL=y` | Magic at stack base → graceful fatal on overflow |
| `CONFIG_STACK_CANARIES_STRONG=y` | GCC `-fstack-protector-strong` (desk only; needs entropy) |
| `CONFIG_ASSERT=y` | Catch invariant violations |
| `CONFIG_THREAD_NAME` / `MONITOR` | Named threads in dumps / analyzer |
| `CONFIG_TASK_WDT=y` + `HW_FALLBACK=n` | Soft stall detect only (ESP MWDT mistimes if HW fallback on) |
| `CONFIG_APP_CRASH_DEBUG=y` | Inject + BIST |
| `CONFIG_APP_FUNC_TRACE=y` | Bring-up/recover enter/leave (not hot loops) |
| `CONFIG_THREAD_ANALYZER=y` + `AUTO` @ 30 s | Stack/CPU per thread on serial (`AUTO_STACK_SIZE=2048` — 1024 overflows with LOG + BT/WiFi) |

### Versioned `zephyr.elf` (symbolication)

CDN layout (director + `cdn0`–`cdn2`):

```
https://cdn.f0xx.org/good_vibes/v0/firmware/<symvers>/zephyr.elf
```

- Desk: `symvers` = `handshake v257` (space → `%20` in URL).
- Cloud OTA: `symvers` = `FW_OTA_VERSION_NAME` (e.g. `00.0001.0000.00072`) via `ci/cast/publish-firmware-elf-cdn.sh`.
- Issues UI: per-row **Upload** → `POST /v1/firmware-elfs/{fw_version}` then re-symbolicates all matching crashes.
- Resolver: `/opt/imu/firmware-elfs/` → CDN mounts → HTTP CDN → legacy `ZEPHYR_ELF_PATH`.

**Never** enable `CONFIG_DEBUG_COREDUMP_BACKEND_FLASH_PARTITION` on this SoC (fatal-path flash write double-faults).

**Production strip:** `CRASH_DEBUG=0 bash zephyr/scripts/flash-zephyr.sh handshake` — uses `prj_release.conf` (no logging coredump / analyzer / assert storms).

### Desk post-mortem (USB)

```bash
# Capture serial, then:
./esp_coredump.sh /path/to/capture.log
# or pipe #CD: lines into coredump_gdbserver --pipe with zephyr.elf
```

### Desk SMP / hang visibility (Step 5)

Use stock Zephyr tooling before inventing A/B rate caps:

1. **Thread analyzer** (enabled in `prj_crash.conf`) — stack high-water + CPU % every 30 s. Docs: [Thread analyzer](http://docs.zephyrproject.org/latest/services/debugging/thread-analyzer.html).
2. **Mutex / sync tracing** — only when contention is the hypothesis: [Mutex tracing APIs](https://docs.zephyrproject.org/apidoc/latest/group__subsys__tracing__apis__mutex.html). Heavy on UART; prefer RTT/SystemView if we add a probe path.
3. **Multicore race context** — [Edgebench: multicore debugging / race conditions](https://www.edgebench.io/posts/multicore-debugging-challenges-in-zephyr-rtos-part-1-race-conditions) (SystemView + per-core RTT; not required for USB-desk day-to-day).
4. **`CONFIG_SCHED_CPU_MASK`** — **debated / off**. Do not enable until we have a concrete pin plan (e.g. BT vs app cores) and thread-analyzer evidence that migration is the problem.
5. **Host serial tooling** — optional: [embedded-agent-bridge](https://github.com/wangwei357/embedded-agent-bridge) (`eabctl` daemon for non-blocking serial/GDB). Our `esp_coredump.sh` + flash capture already cover the desk loop; EAB is an alternative agent interface, not a firmware change.

### Platform patches we do **not** need on Zephyr 4.4+

| Patch | Status |
|-------|--------|
| `zephyr/platform/patch-bt-hci-unified-wq.patch` | **Obsolete.** 4.4+ has in-tree TX WQ (`tx_notify_workqueue_get` / `bt_tx_processor_workq`). Apply scripts default `APPLY_BT_HCI_WQ_PATCH=0`. Set `=1` only on old 3.7 trees. |

---

## Step 4 — Debug inject + BIST (non-production)

Enabled only when `prj_crash.conf` sets `CONFIG_APP_CRASH_DEBUG=y` (default with `CRASH_DEBUG=1` flash).

**Production strip:** `CRASH_DEBUG=0` — omits desk coredump/analyzer; release overlay still keeps soft WDT + crash ring paths gated as documented in `prj_release.conf`.

### BLE crash GATT CTRL (debug builds)

| JSON | Action |
|------|--------|
| `{"op":"inject","kind":"panic"}` | `k_panic("debug_inject")` after 250 ms |
| `{"op":"inject","kind":"assert"}` | `__ASSERT(false, …)` |
| `{"op":"inject","kind":"null"}` | Null pointer write |
| `{"op":"inject","kind":"div0"}` | Integer divide by zero |
| `{"op":"inject","kind":"stack"}` | Stack overflow (recursive) |
| `{"op":"inject","kind":"wdt"}` | Infinite sleep → soft task WDT |
| `{"op":"bist"}` | Re-run built-in self-test |

STATUS adds `"dbg":1` and `"bist":"ok"` or `"bist":"fail:imu,…"`.

### BIST checks

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

See **Canonical desk Kconfig** above. Manual inject still works via BLE CTRL or temporary `k_panic` in `main.c`.

Offline (or use `./esp_coredump.sh`):

```bash
python $ZEPHYR_BASE/scripts/coredump/coredump_serial_log_parser.py crash.log crash.bin
python $ZEPHYR_BASE/scripts/coredump/coredump_gdbserver.py \
  path/to/zephyr.elf crash.bin
# (gdb) bt
```

---

## Step 2 — Crash ring + BLE (implemented; flash coredump abandoned)

1. **NVS / flash layout** — `crash-ring` for compact records; **no** flash coredump backend on ESP32-S3
2. **BLE GATT** — service `4a6e0301`: INFO (compact JSON), CTRL (`clear` / `read` chunk), DATA when used
3. **Android** — on BLE connect: read INFO → `crashes.jsonl` → `POST /v1/ingest/crashes` → clear ESP after ack
4. **Backend** — ingest + web UI (step 0)

---

## Step 3 — Soft watchdog + compact record

### Step 3a — Soft task WDT + render stall (done)

- `stall_watchdog.c` — `CONFIG_TASK_WDT` with **`HW_FALLBACK=n`** (ESP32-S3 MWDT tick bug → empty-PC floods if HW on)
- Main / render soft channels + consecutive windows with `hb_render_frames==0` while screen on → `k_panic("render_stall")`
- Soft WDT cannot catch a **total CPU freeze** (no thread runs to expire channels) — that needs HW WDT (broken here) or external observation (USB silence + thread analyzer last print)

### Step 3b — NVS crash ring + telemetry (done)

- **`crash_ring_store.c`** — append on panic/WDT boot
- **Telemetry** — last `render_hz`, `imu_hz`, `bat_mv`, `bat_pct`, `power_profile` in `detail`
- **BLE** — INFO / CTRL list+clear; Android `fetchAllPendingCrashes()`

### Step 3c+ (done)

1. ~~**CI symbolication**~~ — step 3c (`symbolicate.py` on ingest)

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
| `zephyr/app/handshake/prj_crash.conf` | Desk crash Kconfig (canonical) |
| `zephyr/app/handshake/prj_release.conf` | Field overlay (`CRASH_DEBUG=0`) |
| `zephyr/scripts/flash-zephyr.sh` | Merges `prj_crash.conf` when `CRASH_DEBUG=1` |
| `esp_coredump.sh` | Desk `#CD:` → GDB helper |
| `zephyr/platform/patch-bt-hci-unified-wq.patch` | Obsolete on 4.4+; default not applied |
