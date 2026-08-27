# Veepoo / H-Band / MT200 BLE protocol (reverse notes)

Living notes from descrambling the **MT200** smart clock (`25:38:22:92:C9:4E`) used with the **H Band** Android app (`com.veepoo.hband`). Update this file as more opcodes / services are proven on-device.

Related firmware: `zephyr/app/handshake/src/mt200_bridge.c` (debug-only, `CONFIG_APP_CRASH_DEBUG`). Trigger: BLE crash-ctrl write `{"op":"mt200_scan"}`.

---

## How we recovered the protocol

The phone is not rooted, so HCI snoop was unreliable. The useful path:

1. **Official SDK binaries**, not guesswork from adverts.
   - Source: [HBandSDK/Android_Ble_SDK](https://github.com/HBandSDK/Android_Ble_SDK)
   - Artifact used: `vpprotocol-2.3.80.15.aar`
   - Decompile: `jadx -d /tmp/mt200-capture/decompiled/vpprotocol <aar>`
   - UUID table + 20-byte command prefixes live in `com.veepoo.protocol.profile.vp_a`
   - Per-feature parsers live in `com.veepoo.protocol.operate.*` (`vp_az` = heart rate, `vp_cj` = SpO2, `vp_cp`/`vp_co` = steps/sport, `vp_h` = battery, `vp_at` = g-sensor sport)

2. **Live GATT scan** of the real watch from this laptop (`bluetoothctl`), while H-Band was disconnected (MT200 is a **single LE link** device — it stops advertising while the phone holds the link).

3. **ESP32 as BLE central** (`BT_CENTRAL` + `BT_GATT_CLIENT`, `BT_MAX_CONN=3`) to run the recovered write/notify round-trip on the wrist.

Do **not** treat BlueZ `bluetoothctl write` as a protocol oracle: it returned `org.bluez.Error.NotSupported` on our crash-ctrl characteristic even though `Flags: write`. Direct D-Bus `GattCharacteristic1.WriteValue` succeeded.

---

## Device facts

| Item | Value |
|------|-------|
| Name | `MT200` |
| Public address | `25:38:22:92:C9:4E` |
| Companion app | H Band / Veepoo |
| Link limit | **One LE connection**. Phone H-Band XOR ESP32 bridge XOR laptop. |
| Wrist RF | RSSI ~−70 on the table, ~−80…−86 on-wrist; first `bt_conn_le_create` often dies with `RF noise?` / HCI `0x3e`. Retry once. |
| Notify CCC | Present at `value_handle + 1`, but ATT **Find Information** (Zephyr `BT_GATT_DISCOVER_DESCRIPTOR`) comes back empty. Subscribe by hardcoded offset. |
| Frame size | Commands and notifies are **20-byte**, zero-padded. Opcode is byte `[0]`. |

---

## Primary Veepoo service (proven)

| Role | UUID | Handles on this MT200 |
|------|------|------------------------|
| Primary service | `f0080001-0451-4000-b000-000000000000` | decl 97, end 102 |
| Notify | `f0080002-0451-4000-b000-000000000000` | decl 98, value **99**, CCC **100** |
| Write (no response) | `f0080003-0451-4000-b000-000000000000` | decl 101, value **102** |

Other services seen on the same device (unexplored / not yet spoken by our bridge):

| UUID | Notes |
|------|-------|
| `0000ae00-0000-1000-8000-00805f9b34fb` | `ae01` write, `ae02` notify+CCC — common cheap-watch second pipe |
| `0000fee7-0000-1000-8000-00805f9b34fb` | Tencent / WeChat-adjacent (`fec7`…`fea2`) |
| `f0030001-0451-4000-b000-000000000000` | Veepoo sibling (`f0030002` notify, `f0030003` write) |
| `f0020001-0451-4000-b000-000000000000` | Veepoo sibling (`f0020002` notify, `f0020003` write) |
| `0000ae40-…` | `ae41` / `ae42` notify |
| `00001800` / `00001801` | GAP / GATT |

Hypothesis (not proven): `F002`/`F003` are firmware/OTA or secondary sensor pipes; `AE00` is a second vendor UART. Play with them by repeating the same CCC=`value+1` subscribe pattern.

---

## Proven opcodes (F0080003 write → F0080002 notify)

All writes are 20 bytes, prefix as listed, rest `00`.

| Op | Write prefix | Meaning | Notify parse | Live result (this repo) |
|----|--------------|---------|--------------|-------------------------|
| HR start | `D0 01` (`vp_ak`) | Start optical HR | `[0]=D0`, `[1]=bpm`, `[5]=status` | On-wrist lock after ~33 s: **92–99 bpm**. Table / no contact: `0` (not a wear-error). |
| HR stop | `D0 00` (`vp_al`) | Stop HR | — | Not yet re-tested from ESP32 |
| SpO2 start | `80 01 02` (`vp_bj`, `0x80=Byte.MIN_VALUE`) | Start SpO2 | `[1]=spState` (1=open), `[2]=deviceState` (0/4=free), `[3]=SpO2%` (`1` = unpass-wear), `[4]` checking, `[5]` progress, `[6]==6` → `[7]` pulse | On-wrist: **96–99%**. **Mutually exclusive with HR** — `D0` frames stop once `80` is running (shared PPG). |
| SpO2 stop | `80 02 02` (`vp_bk`) | Stop SpO2 | — | Not yet re-tested from ESP32 |
| Status / BT | (unsolicited) | Classic-BT info (`bt_operate`) | `BD …` | After every subscribe: `bd03010202010101…`. Not a body sensor. |

### Heart-rate status (from `vp_az.java`)

`[5]` is **not** a simple OK/fail flag:

- `[5] ∉ {0,2,3}` → `STATE_HEART_BUSY`
- `[1] ∈ {1,2}` → `STATE_HEART_WEAR_ERROR`
- `30 ≤ [1] ≤ 210` → `STATE_HEART_NORMAL`
- `[1]=0` and `[5]=0` → still initialising / no lock (what we saw for the first ~30 s on-wrist)

### SpO2 (`vp_cj.java`)

`[3]==1` means wear fail (`UNPASS_WEAR`), not 1% saturation. Real saturation we saw was 96–99.

---

## Steps / sport (proven on this MT200, firmware v149)

The watch **counts steps on-device** and shows them on its own LCD — no phone required. H-Band only *reads* the counter.

| Public API (`VPOperateManager`) | Internal send | Prefix |
|---------------------------------|---------------|--------|
| `readSportStep()` (classic) | `vp_cp.vp_aq` → `vp_am` | **`A8 00`** |
| `readSportStep()` (sport-model devices) | `vp_co.vp_ar` → `vp_ao` | **`D8 00`** |
| `startGsensorSport()` | `vp_at.vp_bf` → `vp_bp` | **`F1 20`** |
| `stopGsensorSport()` | `vp_at.vp_bx` → `vp_bq` | **`F1 00`** |

### Classic step notify (`vp_cp`, opcode `A8`)

If `[5] == 00`, step count is big-endian u32 in `[1..4]` (`FFFFFFFF` → 0).

Optional triaxial shorts (little-endian pairs):

- X = `[6] | ([11] << 8)`
- Y = `[7] | ([12] << 8)`
- Z = `[8] | ([13] << 8)`

kcal / distance in the classic `A8` frame are **computed on the phone** from steps + person height (`SportUtil`), not sent by the watch.

**Live v149:** `A8 00` returned `a800000046000000…` → **70 steps**. After `F1 20` (g-sensor start) the watch streamed `A8` at ~1 Hz with the same count (sitting still).

### Newer sport notify (`vp_co`, opcode `D8`)

Need 20 bytes. Little-endian-ish hex concatenation:

- steps = `[5][4][3][2]` as hex u32
- distance m = `[9..6]` / 1000
- kcal = `[13..10]` / 10

**Live v149:** `D8 00` returned `d800460000003b000000240000…` → **70 steps, 0.059 m, 3.6 kcal**. So this SKU speaks **both** classic `A8` and sport-model `D8`.

### Why this matters for us

ESP32 already has a walk-distance pedometer (`wdcm` on the IMU DATA notify). MT200 is an independent, always-on, wrist-worn counter. Combining them lets us:

- compare two devices' step/distance estimates on the same walk
- use MT200 as a cheap ground-truth to re-tune ESP32 step length / threshold
- treat the watch as a body-worn companion while the ESP32 stays on a machine / belt / backpack

---

## Battery read (proven on this MT200, firmware v149)

| API | Prefix |
|-----|--------|
| `readBattery()` | **`A0 00`** (`vp_ad`) |

Notify layout (`vp_h.java`): `[1]` power model, `[2]` bat, `[3]` state, `[4]` level 0–4, `[5]` “percent valid”, `[6]` percent, `[7]==2` low-battery.

**Live v149:** `A0 00` → `a00000003e013e01…` → **62%**, not low. On this SKU `[4]` is also 62 (not a 0–4 bar count) — treat `[6]` as the percent of record.

**G-sensor start (`F1 20`)** acked `f12001…` then switched the notify stream from SpO2 (`80`) to repeating `A8` step frames. Useful as a “keep steps flowing” switch; not a raw accel dump in this first capture.

---

## Tools and recipes

### 1. Decompile a newer SDK drop

```bash
mkdir -p /tmp/mt200-capture
# download vpprotocol-*.aar from HBandSDK/Android_Ble_SDK releases
jadx -d /tmp/mt200-capture/decompiled/vpprotocol /path/to/vpprotocol-x.y.z.aar
rg -n "public static final byte" \
  /tmp/mt200-capture/decompiled/vpprotocol/sources/com/veepoo/protocol/profile/vp_a.java
```

Signed Java bytes: `-48` = `0xD0`, `-88` = `0xA8`, `-40` = `0xD8`, `-96` = `0xA0`, `-15` = `0xF1`, `Byte.MIN_VALUE` = `0x80`.

### 2. Free the watch (phone BT off)

H-Band holds the only LE link. Toggle phone Bluetooth **off** (Quick Settings — `adb shell svc bluetooth disable` is flaky on the BL6000Pro). Confirm the watch is advertising:

```bash
(echo "scan on"; sleep 8; echo "quit") | bluetoothctl | grep -i MT200
```

### 3. Trigger the ESP32 bridge from the laptop

Phone BT stays off. Laptop connects to the ESP32 (peripheral) and writes the debug op; ESP32 then scans/connects to the MT200 (central).

```bash
# find ESP32
(echo "scan on"; sleep 6; echo "quit") | bluetoothctl | grep -i "IMU"
bluetoothctl connect E8:F6:0A:92:51:F0

# write {"op":"mt200_scan"} via D-Bus (bluetoothctl write often fails)
gdbus call --system --dest org.bluez \
  --object-path /org/bluez/hci0/dev_E8_F6_0A_92_51_F0/service0019/char001c \
  --method org.bluez.GattCharacteristic1.WriteValue \
  "[byte 0x7b,byte 0x22,byte 0x6f,byte 0x70,byte 0x22,byte 0x3a,byte 0x22,byte 0x6d,byte 0x74,byte 0x32,byte 0x30,byte 0x30,byte 0x5f,byte 0x73,byte 0x63,byte 0x61,byte 0x6e,byte 0x22,byte 0x7d]" \
  "{}"
```

Watch serial (`/dev/ttyACM0`, 115200). Expected sequence:

```
MT200: scanning for 25:38:22:92:C9:4E
MT200: found … rssi=…
MT200: connected — starting GATT discovery
MT200: service F0080001 found
MT200: subscribed to notify via handle=100
MT200: HR-start write handle=102 err=0
MT200: heart-rate value=97 bpm
```

Session length is `MT200_SESSION_MS` (60 s in current firmware). Disconnect laptop when done so the phone can reclaim the ESP32.

### 4. Characteristic path reminder

Crash-ctrl UUID `4a6e0303-0000-1000-8000-00805f9b34fb` is currently BlueZ path `…/service0019/char001c`. Re-check with `menu gatt` / `list-attributes` after a firmware GATT change.

---

## Integration (how we want MT200 in this stack)

**Best match: ESP32 is the BLE-central wearable bridge; the phone stays the cloud relay.**

Reasons:

1. MT200 is single-link. The phone cannot talk to H-Band *and* the ESP32 *and* the watch at once. The ESP32 already proved it can own the watch link while still being a peripheral to the phone (`BT_MAX_CONN=3`: advert slot + phone + MT200).
2. The phone already uploads IMU / crash / AHRS / geo through one authenticated path. Wearable samples should be just another ingest kind, not a second companion app.
3. Combining metrics (ESP32 `wdcm` / AHRS vs MT200 steps / HR / SpO2) only works if one process sees both clocks.

**Do not** (for now):

- Talk to MT200 from the Android app in parallel with the ESP32 (you will steal the link and the bridge will fail to scan).
- Persist high-rate PPG waveforms. We only want ~1 Hz summaries.
- Block the IMU notify path on wearable I/O. Keep the bridge on a workqueue, debug-gated until the opcode set is boringly reliable.

**Phased productization**

| Phase | What | Status |
|-------|------|--------|
| 0 | Reverse + debug `mt200_scan` + serial logs | Done: HR, SpO2, **steps 70**, **battery 62%**, g-sensor start (v149) |
| 1 | Backend ingest + web + Grafana (`/v1/ingest/wearable`) | This snapshot (schema ready; phone not yet a producer) |
| 2 | Piggyback last HR/SpO2/steps onto existing BLE DATA/STATUS JSON; Android `CloudUploader` posts the envelope | Next |
| 3 | Dedicated “wearable bridge” power profile (not crash-debug), compare ESP32 walk-distance vs MT200 steps on `/app/good_vibes/wearable` | Next |
| 4 | Optional: Android talks to MT200 *only* when the ESP32 is absent (H-Band replacement mode) | Later |

Device identity: `device_id` on ingest is the **ESP32** id (the thing we already key Grafana/AHRS on). MT200 MAC / kind go in the record `source` (`mt200`) so one ESP32 can later bridge more than one watch.

---

## Backend (already TimescaleDB)

The compose stack has been `timescale/timescaledb:latest-pg16` since the original Good Vibes deploy — this is not a new database product. Hypertables are created at API startup on `ts_ms` for the high-volume tables. Wearable samples join that list.

Grafana talks to Timescale through the existing Postgres datasource (`imu-pg`). No extra Grafana plugin is required for basic time-series panels.

Ingest (when the phone starts producing):

```bash
curl -s -X POST http://127.0.0.1:8080/v1/ingest/wearable \
  -H 'Content-Type: application/json' \
  -H "X-API-Key: $IMU_API_KEY" \
  -d '{
    "schema": "imu.ingest.v1",
    "device_id": "ESP32S3-IMU-sim",
    "sent_at_ms": 1700000000000,
    "records": [
      {"type":"wearable","ts_ms":1700000000000,"seq":1,"source":"mt200","kind":"hr","value":97},
      {"type":"wearable","ts_ms":1700000000000,"seq":1,"source":"mt200","kind":"spo2","value":98},
      {"type":"wearable","ts_ms":1700000000000,"seq":1,"source":"mt200","kind":"steps","value":4321}
    ]
  }'
```

Live page: `/app/good_vibes/wearable`. Grafana dashboard: **Wearable (MT200)**.

---

## Wear session 2026-08-24 (v153 unknown dump)

Labeled windows with watch on-wrist, H Band force-stopped, phone BT on (IMU app only).

| Window | Body | Link | New F008 opcodes |
|--------|------|------|------------------|
| Sit | still | held (quiet; HR/A8/A0 are DBG) | none |
| Walk | ~2 min; LCD **52 steps / 2 kcal** | 2× drop `0x08` then `0x3e` (RSSI −83…−91) | `BD` again on reconnect |
| Rest | still | **held** | **`C7` `96` `93`** |

| Op | Hex (first bytes) | SDK name (`vp_b`) | Notes |
|----|-------------------|-------------------|-------|
| `BD` | `bd 03 01 02 02 01 01 01` | `bt_operate` | Unsolicited after CCC. Classic-BT status, not HR/steps. |
| `C7` | `c7 01 01 03` | `screen_style_oprate` | `[1]=1 [2]=1` → setting-success; `[3]=3` screen index. Watch face, not a sensor. |
| `96` | `96 a2 00…` | `ecg_data_get_id_oprate` | ECG record-id pipe. Unsolicited at rest. |
| `93` | `93 05 a1 a0 01` | `ecg_data_app_detect_oprate` | ECG detect/app pipe. `[1]=5` is not the usual start (`93 01`). |

Maybe-sparks during walk had **no matching opcode**. `93`/`96` arrived minutes later at rest. Treat sparks as PPG/electrode/wet skin, not a BLE shock command.

Phone never logged `Wearable relay` this session (`wok` never uploaded). Watch LCD remains the step ground truth until we INF a telem snapshot or cloud relay is confirmed on.

---

## Open questions

- Does `A8 00` work on *this* MT200, or does it only speak `D8 00` (`isParseSportModel`)?
- Can g-sensor sport (`F1 20`) stream triaxial at a useful rate for AHRS cross-check, or is it a one-shot?
- What are `F002` / `F003` / `AE00` actually for on this SKU?
- Can we keep HR *and* steps without SpO2 (SpO2 stole the PPG from HR)?
- Soft pairing / bonding: we connected unencrypted. Some CCC writes on other Veepoo SKUs want a bond; this one did not.
