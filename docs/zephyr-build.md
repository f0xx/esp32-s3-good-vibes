# Zephyr build and deploy

## Prerequisites

Complete [zephyr-install.md](zephyr-install.md) first.

## Flash script (recommended)

From repo root:

```bash
PORT=/dev/ttyACM0 ./zephyr/scripts/flash-zephyr.sh handshake
```

| Variable | Default | Meaning |
|----------|---------|---------|
| `PORT` | auto `/dev/ttyACM0` | USB serial device |
| `CAPTURE_SEC` | 12 (min 35) | Post-flash log capture |

The script:

1. Symlinks `zephyr/app/handshake` → `~/zephyrproject/waveshare-handshake`
2. Symlinks `zephyr/` → `~/zephyrproject/waveshare-board-root`
3. Runs `west build -p always -b esp32s3_lcd_147b/esp32s3/procpu`
4. Flashes via `west flash` or `esptool.py` fallback
5. Captures serial and runs `verify-boot-log.sh`

**Smoke test:**

```bash
PORT=/dev/ttyACM0 ./zephyr/scripts/flash-zephyr.sh smoke
```

## Manual build

```bash
source ~/zephyrproject/.venv/bin/activate
source ~/.zephyrrc
export ZEPHYR_BASE=~/zephyrproject/zephyr

REPO="/path/to/esp32-s3-imu-basics"
ln -sfn "$REPO/zephyr/app/handshake" ~/zephyrproject/waveshare-handshake
ln -sfn "$REPO/zephyr" ~/zephyrproject/waveshare-board-root

cd ~/zephyrproject/zephyr
west build -p always -b esp32s3_lcd_147b/esp32s3/procpu ~/zephyrproject/waveshare-handshake -- \
  -DBOARD_ROOT=~/zephyrproject/waveshare-board-root

west flash --esp-device /dev/ttyACM0
```

## Serial console

115200 8N1 on USB CDC:

```bash
./zephyr/scripts/capture-serial-boot.sh /dev/ttyACM0 45
# or
python3 -c "import serial; s=serial.Serial('/dev/ttyACM0',115200); ..."
```

**Note:** After boot, logs may be quiet for ~10 s until the first telemetry line (deferred logging).

## Boot verification

```bash
SKIP_RESET=1 ./zephyr/scripts/capture-serial-boot.sh /dev/ttyACM0 45 /tmp/boot.log
./zephyr/scripts/verify-boot-log.sh /tmp/boot.log
```

Checks:

- `handshake: main()` exactly **once** (no reboot loop)
- `stage: main loop`, BLE advertising, framebuffer, backlight
- Optional `telemetry` line if early boot text missed

## BOOT button

| Action | Effect |
|--------|--------|
| Short tap (after 3.5 s grace) | Toggle screen / backlight |
| Hold 10 s (release → press → hold) | Erase WiFi NVS profiles + reboot |

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| Flash fails | Hold **BOOT**, tap **RESET**, release BOOT → download mode; retry |
| Empty serial | Wait 7 s after flash; tap RESET; increase capture time |
| Reboot loop | Check serial for repeated `handshake: main()`; ensure v12+ BOOT button fix |
| Wrong board pins | Must use `esp32s3_lcd_147b`, not generic `esp32s3_devkitm` |
| Path with spaces | Flash script uses symlinks under `~/zephyrproject/` to avoid west path issues |

## Restore Arduino firmware

```bash
cd esp32_s3_imu_basics
PORT=/dev/ttyACM0 ./scripts/build.sh production --upload
```

Or: `backups/restore-arduino-fullflash.sh`
