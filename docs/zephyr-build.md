# Zephyr build and deploy

## Prerequisites

Complete [zephyr-install.md](zephyr-install.md) first (**Zephyr v4.4.2**, **SDK 1.0.1**).

## Toolchain / CI pins

| Surface | Pin |
|---------|-----|
| Desk SDK | `~/zephyr-sdk-1.0.1` |
| Desk west | `~/zephyrproject` @ Zephyr **v4.4.2** |
| Docker image | `imu-zephyr-ci:1.0.3` (`ci/cast/build.config.yml`, `docker-west-build.sh`) |
| cast01 west cache | `/home/build/build-cache/imu-zephyrproject` @ **v4.4.2** |
| Builder label | `ci_version: imu-1.0.3` |

Desk flash applies `zephyr/platform/apply-*.sh` before `west build` (HCI soft timeout, BT long WQ, WiFi DMA-in-DRAM, PSRAM SMH skip, BT controller core-pin prompt, …). CI runs the same set via `zephyr/scripts/ci-west-build.sh`. The old `patch-bt-hci-unified-wq` is **obsolete on Zephyr 4.4+** and defaults off (`APPLY_BT_HCI_WQ_PATCH=0`).

**WiFi + BLE:** keep `CONFIG_SMP` off. Prefer WiFi task on core 0 and BT controller on core 1 (`prj.conf`). Net pkt pools may use SPIRAM (`ESP32_WIFI_NET_ALLOC_SPIRAM`); WiFi DMA buffers stay in DRAM.

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

Checks (`verify-boot-log.sh`):

- `crash ring ready`
- `handshake vNN` (desk version from `zephyr/app/common/fw_version.h`)
- `framebuffer ready`
- `BOOT armed (released=1)`
- Must **not** see `esp_flash_erase_region failed` or `crash ring init failed`
- Prefer a single `handshake: main()` (no reboot loop)

Capture window: flash script uses ~35 s by default.

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
| Hang before `z_prep_c` / early DoubleException | Confirm PSRAM SMH skip + **no** 3.7 HCI defer overlay on 4.x (`APPLY_HCI_DEFER=0`) |
| `WIFI_ESP32` missing / WiFi dead | Ensure `# CONFIG_SMP is not set` |
| Reboot loop | Check serial for repeated `handshake: main()`; ensure v12+ BOOT button fix |
| Wrong board pins | Must use `esp32s3_lcd_147b`, not generic `esp32s3_devkitm` |
| Path with spaces | Flash script uses symlinks under `~/zephyrproject/` to avoid west path issues |
| Cloud FW build image hang (cast01) | Docker must use **overlay2** (vfs copies ~2 GB per create); image tag `imu-zephyr-ci:1.0.3` |

## Restore Arduino firmware

Arduino overwrites MCUboot + A/B slots. USB-flash handshake again before cloud OTA.
See [dual-firmware-probing.md](dual-firmware-probing.md) and [zephyr-ota.md](zephyr-ota.md).

```bash
cd esp32_s3_imu_basics
PORT=/dev/ttyACM0 ./scripts/build.sh production --upload
```

Or: `backups/restore-arduino-fullflash.sh`
