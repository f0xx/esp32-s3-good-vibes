# Dual-firmware probing — Zephyr vs Arduino

This repo still contains two firmware trees. **Only one image runs at a time.** Flash the
track you want before scanning.

| Track | Path | BLE name | Role today |
|-------|------|----------|------------|
| **Zephyr handshake** | `zephyr/app/handshake/` | `ESP32S3 IMU sim` | **Production.** Full IMU, scene, GATT (IMU / NET / config / OTA / crash), WiFi, A/B OTA, crash ring |
| **Zephyr smoke** | `zephyr/app/smoke/` | `WS147B-Zephyr` | Hardware sanity only (LCD / BOOT / advertise). The Android app will not connect |
| **Arduino production** | `esp32_s3_imu_basics/` | `ESP32S3 IMU sim` | **Frozen reference.** Battery curve and early UI. Do not ship new features here |

Desk USB and cloud OTA both target **Zephyr handshake**. Cloud / phone / A/B detail:
**[zephyr-ota.md](zephyr-ota.md)** (PDF: [zephyr-ota.pdf](zephyr-ota.pdf)).

When Zephyr handshake and Arduino share the BLE name, power only one board nearby.

---

## Flash Zephyr handshake (default)

West does not tolerate spaces in `BOARD_ROOT`. Use the helper (symlinks under `~/zephyrproject/`):

```bash
PORT=/dev/ttyACM0 zephyr/scripts/flash-zephyr.sh handshake
```

That writes **MCUboot @ 0x0** plus a **signed** image in slot A (`image-0`). After boot, serial
must show `crash ring ready`, `handshake vNN` (or a cloud `00.0001.0000.NNNNN` stamp), and
`BOOT armed (released=1)`.

See [zephyr-build.md](zephyr-build.md) for the manual `west` equivalent.

### Boot behaviour (handshake)

- Live IMU scene on the 172×320 ST7789 (not the old RGB-corner stub).
- Acrylic WS2812 on GPIO38 is operational (vibro / status), not a boot colour cycle.
- BLE connectable, name **`ESP32S3 IMU sim`**, IMU service UUID in advertising.
- STATUS JSON includes `fw` (`FW_VERSION_NAME`), `fwc` (`FW_VERSION_CODE`), `feat` (capability
  bitmask), and `boot_part` (`A` or `B`).
- CAPS / `feat` include IMU, TFT, config, temp, vibro, WiFi, **OTA**, RSSI, time, bench
  (plus crash-debug / MT200 when those Kconfig bits are on).

**BOOT (GPIO0):** short tap toggles screen / backlight. Hold 10 s (after the grace window)
erases WiFi NVS profiles and reboots. See [zephyr-build.md](zephyr-build.md).

### Android app (handshake)

The app (`ImuProtocol.kt` / `BleImuClient.kt`) expects:

1. Scan filter: device name **or** IMU service UUID
2. Connect → MTU 517 → discover services
3. Enable NOTIFY on DATA, write MODE / POLL_MS / TIME, read CAPS
4. Poll DATA on NOTIFY; STATUS carries `fw` / `fwc` / `boot_part`

Modes (COMPUTED / RAW / SCENE / AHRS / vibro) are live QMI8658 + on-device fusion, not stubs.
If IMU init fails, batches stay empty (`n:0`) — check I2C / WHO_AM_I.

Cloud OTA: **Device… → Check for OTA**. Lab file OTA: **Device… → OTA from file**. Both write
the inactive MCUboot slot over GATT. Sequence: [zephyr-ota.md](zephyr-ota.md).

---

## Flash Zephyr smoke (hardware-only)

```bash
PORT=/dev/ttyACM0 zephyr/scripts/flash-zephyr.sh smoke
```

- BLE name **`WS147B-Zephyr`** — will **not** match the Android app scan filter.
- Use for LCD / BOOT / nRF Connect visibility. No IMU GATT, no OTA.

---

## Flash / restore Arduino (reference only)

Arduino does **not** understand the current MCUboot A/B layout. Flashing it overwrites the
Zephyr bootloader + slots. After Arduino, you must USB-flash handshake again before cloud OTA
will work.

```bash
./esp32_s3_imu_basics/scripts/build.sh production --upload
# or, if a golden 16 MB dump still exists:
./backups/restore-arduino-fullflash.sh
```

See `backups/LATEST` for the SHA256 of the last golden image (may be absent on new clones).

---

## WS2812 acrylic — control model

The red acrylic edge light is a **single WS2812** (one GRB pixel) on **GPIO38**.

| Layer | Arduino | Zephyr handshake |
|-------|---------|------------------|
| Physical | Digital NRZ ~800 kHz | Same |
| Colour order | GRB | GRB |
| Driver | RMT (`rgbLedWrite`) | `ws2812_gpio38.c` + `vibro_led.c` |
| “Brightness” | 8-bit R/G/B in the protocol frame | Same — not DAC/ADC |

There is **no** analog PWM/DAC on the data pin. **Do not** enable SPI3 CS on GPIO38 in
devicetree — that latched red on early wrong-pin builds.

---

## Protocol source of truth

| Artifact | Path |
|----------|------|
| Zephyr IMU / CAPS | `zephyr/app/common/ble_imu_protocol.h` |
| Zephyr GATT | `zephyr/app/handshake/src/ble_imu_gatt.c`, `ble_net_gatt.c`, `ble_config_gatt.c`, `ble_ota_gatt.c`, `ble_crash_gatt.c` |
| Zephyr A/B | `zephyr/app/common/ota_ab.c`, `soft_reboot.c` |
| Arduino BLE headers | `esp32_s3_imu_basics/ble/ble_protocol.h` (legacy) |
| Android | `ImuProtocol.kt`, `BleImuClient.kt`, `OtaProtocol` in `ConfigProtocol.kt` |

Keep UUIDs and CAP bitmasks in sync on the Zephyr + Android side. Arduino is not a second
implementation target.

OTA GATT (historical Arduino numbers, still wired this way):

| Role | UUID |
|------|------|
| OTA service | `4a6e0201-0000-1000-8000-00805f9b34fb` |
| CTRL (JSON begin / abort / finish) | `4a6e0202-…` |
| DATA (480-byte chunks) | `4a6e0203-…` |

Those values overlap **characteristic** UUIDs on the NET service (`4a6e0200-…`). They are
different GATT services; the phone looks up OTA by service UUID.

---

## Identifying which firmware is running

| Signal | Zephyr handshake | Zephyr smoke | Arduino |
|--------|------------------|--------------|---------|
| BLE name | ESP32S3 IMU sim | WS147B-Zephyr | ESP32S3 IMU sim |
| Serial module | `handshake:` | `smoke:` | Arduino / ESP-IDF |
| STATUS `fw` | `handshake vNN` or `00.0001.0000.NNNNN` | N/A | Arduino string |
| STATUS `fwc` | desk `NN` or cloud `1000000000+N` | N/A | n/a or old |
| STATUS `boot_part` | `A` or `B` | N/A | N/A |
| `feat` / CAPS | IMU+TFT+…+**OTA** | N/A | subset, no MCUboot A/B |
| Crash ring | `crash ring ready` | no | no |
| Boot acrylic | operational / vibro | off after boot | app-driven |

USB factory images report `handshake v191` (desk `FW_VERSION_CODE`). Cloud OTA restamps
`FW_VERSION_NAME` / `FW_VERSION_CODE` to the allocated `00.0001.0000.NNNNN` line. Same
source tree, different compare numbers — see [zephyr-ota.md](zephyr-ota.md).
