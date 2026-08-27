# ESP32-S3 IMU Sensor Platform

Live IMU visualization, BLE phone relay, and cloud vibration ingest for the **Waveshare ESP32-S3-LCD-1.47B** — a compact ESP32-S3 board with 172×320 TFT, QMI8658 6-axis IMU, LiPo battery input, and integrated BLE/WiFi.

This repository bundles everything needed to build, flash, and operate the full stack:

**Field operators:** start with **[docs/operator-howto.md](docs/operator-howto.md)** (printable PDF: [docs/operator-howto.pdf](docs/operator-howto.pdf)).

| Component | Path | Role |
|-----------|------|------|
| **Zephyr firmware** (primary) | [`zephyr/`](zephyr/) | On-device app: IMU pipeline, live scene, BLE GATT, WiFi, power management |
| **Android app** | [`android/ESP32S3ImuSim/`](android/ESP32S3ImuSim/) | BLE client, live scene, vibration verdicts, cloud upload |
| **Backend** | [`backend/`](backend/) | FastAPI ingest + TimescaleDB + Grafana dashboards |

System overview: **[docs/architecture.md](docs/architecture.md)**

---

## Feature highlights (since last snapshot)

Beyond the original live-IMU/BLE/cloud pipeline, the firmware and app now cover:

| Area | What's new |
|------|-----------|
| **Crash reliability** | Ping-pong flash-backed crash ring (survives power loss mid-write), RTC-retained-memory capture of PC/EXCCAUSE/backtrace on hard faults (`crash_rtc_capture.c`) so genuine CPU exceptions — not just watchdog resets — are recorded for next-boot upload, and a repo-wide "safe flash erase" pattern (`flash_safety.c`) applied to every flash-backed store to stop watchdog resets from erasing flash mid-BLE-connection |
| **Attitude / AHRS** | On-device AHRS fusion (`attitude.c`) streaming a quaternion + Euler angles over BLE, computed on the ESP32 itself; separated into its own high-power mobile-app mode (full CPU + IMU rate) since it isn't a battery-saving profile |
| **Floor / mounting calibration** | `floor_calib.c` — BLE-triggered "bubble level" style calibration that averages gravity and stores a correction quaternion in flash NVS, applied on top of existing bias calibration and persistent across reboots |
| **Vibration analysis** | On-device FFT (`vibro_fft.c`) and band-RMS scoring, multi-slot flash-backed reference profiles (`vibro_ref_store.c`), verdict history (`vibro_verdict_store.c`), and a wizard-style Android recording UI (`VibroRefWizardActivity.kt`) |
| **Battery bench** | `battery_bench.c/.h` + Android `BatteryBenchActivity`/`BatteryBenchEstimator`/`BatteryBenchStore` — structured battery-life measurement/estimation flow |
| **Geo / dead-reckoning** | Walk-distance + yaw fields streamed from firmware, phone GPS capture fused with IMU dead-reckoning (`GeoTracker.kt`), backend geo-ingest + route endpoints, and a Leaflet/OpenStreetMap route-comparison web page (`backend/web/map.html`) |
| **AHRS debug web page** | `backend/web/ahrs.html` — live WebSocket-fed 3D/orientation debug view |
| **OTA / recovery** | `ota_ab.c/.h` (A/B image slots) and `soft_reboot.c/.h` |
| **Compact telemetry** | `metrics_compact.c/.h` — smaller wire format for STATUS/metrics fields |
| **RGB status LED** | `vibro_led.c/.h` on top of the existing WS2812 driver, driven by vibration verdicts |
| **Experimental: MT200 BLE-central bridge** | `mt200_bridge.c/.h` — ESP32 as BLE *central* to a Veepoo/H-Band MT200 clock. Proven live: **HR 92–99 bpm**, **SpO2 96–99%**, **steps (70 on this capture)**, **battery 62%**. G-sensor start (`F1 20`) switches the watch to streaming the step counter at ~1 Hz. Debug op `{"op":"mt200_scan"}`. Single-LE-link — phone H-Band must be off. Notes: **[docs/veepoo-proto-ble-reverse.md](docs/veepoo-proto-ble-reverse.md)** |
| **Wearable ingest (prep)** | Dedicated `/v1/ingest/wearable` + latest/history APIs, `/app/good_vibes/wearable` live page, Grafana **Wearable (MT200)** dashboard. Rows land in TimescaleDB (`wearable_samples` hypertable). Phone is not a producer yet — schema is ready for the ESP32→phone relay |
| **Backend model** | `Machine`/`Sensor`/`ReferenceProfile` data model, monotonic-trend early-warning scoring (`trend_score.py`), battery-bench ingest, AHRS/geo ingest, expanded Grafana dashboards (verdicts + battery bench + wearable). Database is already **TimescaleDB** (`timescale/timescaledb:latest-pg16`) — Grafana uses the existing Postgres datasource |

Detailed phase-by-phase history: **[ROADMAP.md](ROADMAP.md)**. Raw feature wishlist / vision doc: **[features.list](features.list)**.

---

## Hardware at a glance

**Board:** [Waveshare ESP32-S3-LCD-1.47B](https://www.waveshare.com/esp32-s3-lcd-1.47b.htm) (rev B3)  
**Module:** ESP32-S3-WROOM-1 **N8R8** — 8 MB flash, 8 MB octal PSRAM  
**USB:** Type-C CDC console @ **115200** baud (typically `/dev/ttyACM0`)

| Peripheral | Chip / signal | Zephyr driver / code |
|------------|---------------|----------------------|
| CPU | ESP32-S3 Xtensa LX7 @ 240 MHz | `clock_control` + `power_manager.c` |
| Display | ST7789V 172×320 SPI | `sitronix,st7789v` + MIPI DBI SPI |
| IMU | QMI8658 @ I2C 0x6B | `i2c_esp32` + `qmi8658.c` |
| Bluetooth | ESP32-S3 integrated BT | Zephyr BT host + `esp32_bt_hci` |
| WiFi | ESP32-S3 integrated | Zephyr `CONFIG_WIFI` + ESP32 shim |
| Battery | GPIO1, 200k/100k divider | `adc_esp32` + `battery_adc_esp32.c` |
| Backlight | GPIO46 PWM | LEDC via `panel_backlight.c` |
| BOOT | GPIO0 | `boot_button.c` |
| RGB edge | WS2812 on GPIO38 | Bit-bang `ws2812_gpio38.c` |

Full pin map and Kconfig: **[docs/zephyr-hardware.md](docs/zephyr-hardware.md)**

---

## Soldering the accumulator (taken from electronic cigarette)

Accu rated as 3.7V / 420mAh LiPo

![Battery soldering pins](backend/assets/simg0000.jpg)

Accu +3.7v (red) <-> BAT pin (ETA6098)

Accu Gnd (black) <-> G pin (common ground)

---

## Prerequisites

| Task | Guide |
|------|-------|
| Install Zephyr v3.7 + SDK 0.16.8 | [docs/zephyr-install.md](docs/zephyr-install.md) |
| Install arduino-cli + esp32 core | [docs/arduino-firmware.md](docs/arduino-firmware.md) |
| Android SDK + JDK 21 | [docs/android-app.md](docs/android-app.md) |
| Docker (backend) | [docs/backend.md](docs/backend.md) |

---

## Quick start — Zephyr (recommended)

### 1. One-time toolchain

Follow **[docs/zephyr-install.md](docs/zephyr-install.md)**. You need Zephyr **v3.7.0**, SDK **0.16.8**, target `xtensa-espressif_esp32s3_zephyr-elf`.

### 2. Build and flash

```bash
cd /path/to/esp32-s3-imu-basics
PORT=/dev/ttyACM0 ./zephyr/scripts/flash-zephyr.sh handshake
```

| App | Command | Purpose |
|-----|---------|---------|
| `handshake` | `flash-zephyr.sh handshake` | Full product firmware |
| `smoke` | `flash-zephyr.sh smoke` | BLE/LCD hardware test only |

The script symlinks the repo under `~/zephyrproject/` (avoids path-space issues), builds, flashes, captures serial, and verifies boot.

### 3. Expected boot output

```
handshake v14 — battery ADC curve-fit (Arduino parity)
QMI8658 ready at 0x6B
stage: main loop
BLE advertising started
telemetry screen=on target(cpu=240 render=30Hz imu=10Hz) actual(cpu=240 ...) bat=...V ...% src=...
```

Manual serial capture:

```bash
SKIP_RESET=1 ./zephyr/scripts/capture-serial-boot.sh /dev/ttyACM0 45 /tmp/boot.log
./zephyr/scripts/verify-boot-log.sh /tmp/boot.log
```

More detail: **[docs/zephyr-build.md](docs/zephyr-build.md)**

---

## Quick start — Android app

```bash
cd android/ESP32S3ImuSim
export ANDROID_HOME="$HOME/Android/Sdk"
export JAVA_HOME="/usr/lib/jvm/openjdk-21"   # adjust for your system
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

1. Open **ESP32S3ImuSim** → scan → connect to **ESP32S3 IMU sim**
2. Optional: **Cloud** → set backend URL + API key

Full guide: **[docs/android-app.md](docs/android-app.md)**

---

## Quick start — Backend

```bash
cd backend
export IMU_API_KEY='your-long-random-secret'
docker compose up -d --build
curl -s http://127.0.0.1:8080/v1/health
```

Services: API **8080**, Grafana **3000**, TimescaleDB internal.

Full guide: **[docs/backend.md](docs/backend.md)**

---

## Quick start — Arduino (reference / fallback)

Mature battery calibration and production UI:

```bash
cd esp32_s3_imu_basics
PORT=/dev/ttyACM0 ./scripts/build.sh production --upload
```

Before first Zephyr flash, consider a full 16 MB backup — see **[backups/README.md](backups/README.md)**.

Guide: **[docs/arduino-firmware.md](docs/arduino-firmware.md)**

---

## Power modes (Zephyr `handshake`)

| State | CPU | IMU | Render | Backlight |
|-------|-----|-----|--------|-----------|
| Screen **ON** (default) | 240 MHz | 10 Hz | ~30 Hz target (~21 Hz measured) | 40% |
| Screen **OFF** (BOOT tap) | 160 MHz | 100 Hz | off | off |

Telemetry prints every **10 s** on USB serial.

**BOOT button (GPIO0):**

| Action | Effect |
|--------|--------|
| Short tap (after 3.5 s boot grace) | Toggle screen / backlight |
| Hold 10 s (release → press → hold) | Erase WiFi NVS profiles + reboot |

---

## Battery HUD

| Label | Meaning |
|-------|---------|
| `BAT 82%` | Running from battery |
| `DC 4.05V` | USB/charge path, cell visible on ADC |
| `DC ext` | USB power but ADC below 3.25 V (no cell on JST or empty header) |

Zephyr mirrors Arduino logic: GPIO1, 200k/100k divider, eFuse curve-fit ADC, trend-based DC/BAT detection.

Details: **[docs/battery.md](docs/battery.md)**

---

## Repository layout

```
esp32-s3-imu-basics/
├── README.md                      ← this file
├── docs/                          ← detailed guides (see index below)
├── zephyr/
│   ├── boards/waveshare/esp32s3_lcd_147b/   ← out-of-tree board definition
│   ├── app/handshake/             ← main firmware
│   ├── app/smoke/                 ← hardware smoke test
│   └── scripts/                   ← flash-zephyr.sh, serial capture
├── esp32_s3_imu_basics/           ← Arduino sketch + scripts
├── android/ESP32S3ImuSim/         ← Kotlin BLE client
├── backend/                       ← FastAPI + docker-compose
├── backups/                       ← full-flash recovery (local .bin, not in git)
├── ROADMAP.md                     ← phase-by-phase implementation history
└── features.list                  ← raw feature wishlist / vision doc
```

---

## Documentation index

| Document | Contents |
|----------|----------|
| [docs/architecture.md](docs/architecture.md) | System diagram, use cases, BLE services |
| [docs/zephyr-install.md](docs/zephyr-install.md) | Zephyr + SDK install (Linux/Gentoo/Debian) |
| [docs/zephyr-hardware.md](docs/zephyr-hardware.md) | Ported peripherals, drivers, CPU scaling, partitions |
| [docs/zephyr-build.md](docs/zephyr-build.md) | Build, flash, serial, troubleshooting |
| [docs/arduino-firmware.md](docs/arduino-firmware.md) | Arduino profiles, upload, battery reference |
| [docs/android-app.md](docs/android-app.md) | Build/install APK, BLE/cloud settings |
| [docs/backend.md](docs/backend.md) | Docker deploy, API, Grafana, firewall |
| [docs/battery.md](docs/battery.md) | ADC path, HUD labels, validation |
| [docs/dual-firmware-probing.md](docs/dual-firmware-probing.md) | Switching Arduino ↔ Zephyr, protocol parity |
| [docs/zephyr-experiment.md](docs/zephyr-experiment.md) | Zephyr port notes and history |
| [docs/metrics.md](docs/metrics.md) | On-device/backend metrics reference |
| [docs/veepoo-proto-ble-reverse.md](docs/veepoo-proto-ble-reverse.md) | Veepoo/MT200 BLE protocol, tools, live HR/SpO2 results, step opcodes, integration plan |

---

## Troubleshooting (common)

| Problem | Check |
|---------|-------|
| Flash fails | Hold **BOOT**, tap **RESET**, release BOOT → download mode |
| No serial output | Wait 7 s after flash; increase capture time; tap RESET |
| Reboot loop | Serial shows repeated `handshake: main()` — update to v12+ BOOT fix |
| Wrong pins / blank LCD | Board must be `esp32s3_lcd_147b`, not generic devkit |
| App won't connect | Only one board named **ESP32S3 IMU sim** nearby; flash `handshake` not `smoke` |
| Battery shows `DC ext` on USB | Connect LiPo to JST; see [docs/battery.md](docs/battery.md) |
| Restore Arduino | `./backups/restore-arduino-fullflash.sh` or Arduino production upload |

---

## License

Project firmware and apps — use and modify per your deployment needs. Third-party components (Zephyr, ESP-IDF HAL, Waveshare demos) retain their respective licenses.
