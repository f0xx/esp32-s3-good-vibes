# Arduino firmware

Production/reference firmware in `esp32_s3_imu_basics/`.

## Requirements

- [arduino-cli](https://arduino.github.io/arduino-cli/) with **esp32** core ≥ 3.x
- Board: **ESP32S3 Dev Module** with PSRAM OPI, 16 MB flash, USB CDC on boot

## Build profiles

```bash
cd esp32_s3_imu_basics
./scripts/build.sh <profile> [--verify|--upload]
```

| Profile | TFT | Debug | Use |
|---------|-----|-------|-----|
| `full` | yes | yes | Development |
| `headless` | no | yes | IMU/BLE without display |
| `production` | yes | no | Field deploy |
| `all` | — | — | Build every profile |

## Flash

```bash
PORT=/dev/ttyACM0 ./scripts/build.sh production --upload
```

FQBN (from script):

```
esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,FlashMode=qio,CDCOnBoot=cdc,PartitionScheme=app3M_fat9M_16MB
```

## Hardware test

```bash
PORT=/dev/ttyACM0 DO_UPLOAD=1 ./scripts/hw-test.sh
```

## Battery monitor (reference for Zephyr)

Implementation: `battery_monitor.cpp` + `board_config.h`

- Pin: **GPIO1**, `analogReadMilliVolts()` with 12-bit resolution
- Divider: 200k / 100k → multiply by 3.0, offset 0.992857
- EMA smoothing, trend-based DC vs battery detection
- No dedicated VBUS pin

Zephyr port mirrors this in `zephyr/app/common/battery_monitor.c` and `battery_adc_esp32.c`.

## NVS / WiFi

Hold **BOOT 10 s** at runtime to erase stored WiFi profiles (same convention as Zephyr).

## Switch back to Zephyr

```bash
PORT=/dev/ttyACM0 ../zephyr/scripts/flash-zephyr.sh handshake
```
