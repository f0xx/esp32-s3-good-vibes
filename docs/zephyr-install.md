# Zephyr installation (ESP32-S3-LCD-1.47B)

One-time setup for building `zephyr/app/handshake` on Linux. Tested with **Zephyr v4.4.2** and **Zephyr SDK 1.0.1**.

## Upgrade (v3.7 → v4.4.2)

| Item | Old | Current |
|------|-----|---------|
| Zephyr | v3.7.0 LTS | **v4.4.2** |
| SDK | 0.16.8 | **1.0.1** (`gnu/<triple>/` layout) |
| Desk west tree | `~/zephyrproject` | Fresh init on `v4.4.2` (old tree backed up as `~/zephyrproject.bak-v3.7-*`) |
| CI image | `imu-zephyr-ci:0.16.8` | **`imu-zephyr-ci:1.0.3`** (SDK 1.0.1, Ubuntu 24.04 / Python 3.12; Zephyr 4.4 needs ≥3.12) |
| CI west cache (cast01) | `/home/build/build-cache/imu-zephyrproject` @ 3.7 | Same path @ **4.4.2** |
| Builder `ci_version` | `imu-0.16.8` | **`imu-1.0.3`** |

**Expectations after upgrade**

- Handshake boots on desk USB with `crash ring ready`, `handshake vNN`, `framebuffer ready`, `BOOT armed (released=1)` — see [zephyr-build.md](zephyr-build.md).
- `CONFIG_WIFI_ESP32` requires **`!SMP`**. Do not enable `CONFIG_SMP` (it disables ESP32 WiFi).
- WiFi+BLE coex uses Zephyr/HAL names (`ESP_WIFI_TASK_PINNED_TO_CORE_*`, `ESP32_BT_CTLR_PINNED_TO_CORE`, `ESP32_SW_COEXIST_ENABLE`), not raw IDF `BT_CTRL_PINNED_*` / `ESP_COEX_*` symbols — see `zephyr/app/handshake/prj.conf`.
- PSRAM: `esp_init_psram()` runs; SMH registration is skipped (`apply-esp32-psram-smh-defer.sh`) so boot does not hang. Framebuffer is `.ext_ram.bss`; BIST may report `psram=0MB`.
- Platform apply scripts in `zephyr/platform/` are required on both desk (`flash-zephyr.sh`) and CI (`ci-west-build.sh`); they are not optional “nice to have”.
- Cloud OTA still needs a **committed + pushed** ref before `./ci/cast/enqueue-imu-build.sh … --ota`.
- When syncing a desk west tree to cast01, do **not** use a blanket `--exclude build/` — that drops `zephyr/scripts/build/` and `zephyr/share/sysbuild/build/` (Zephyr 4.4 west needs them). Exclude only the desk output dir `zephyr/build/` if needed.

## Requirements

| Tool | Version |
|------|---------|
| Zephyr | v4.4.2 |
| Zephyr SDK | 1.0.1 |
| Python | ≥ 3.10 |
| west | ≥ 1.2 |
| cmake | ≥ 3.20 |
| dtc | device-tree compiler |

**Gentoo packages (example):**

```bash
emerge -av dev-build/cmake dev-python/pip dev-python/venv \
  dev-vcs/git wget curl dev-embedded/dtc ncurses
```

**Debian/Ubuntu:**

```bash
sudo apt install --no-install-recommends git cmake ninja-build gperf \
  ccache dfu-util device-tree-compiler wget python3-dev python3-venv \
  python3-tomli python3-twisted xz-utils file make gcc gcc-multilib \
  libsdl2-dev libmagic1
```

## Workspace setup

```bash
mkdir -p ~/zephyrproject && cd ~/zephyrproject

python3 -m venv .venv
source ~/zephyrproject/.venv/bin/activate
pip install -U pip wheel west

west init -m https://github.com/zephyrproject-rtos/zephyr --mr v4.4.2
cd zephyr
west update
pip install -r scripts/requirements.txt
```

## Zephyr SDK

SDK 1.0+ nests GNU toolchains under `gnu/<triple>/`.

```bash
cd ~
wget -c https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v1.0.1/zephyr-sdk-1.0.1_linux-x86_64_minimal.tar.xz
tar xf zephyr-sdk-1.0.1_linux-x86_64_minimal.tar.xz
~/zephyr-sdk-1.0.1/setup.sh -t xtensa-espressif_esp32s3_zephyr-elf -c -h
```

Add to `~/.zephyrrc`:

```bash
export ZEPHYR_SDK_INSTALL_DIR="$HOME/zephyr-sdk-1.0.1"
```

Shell init (add to `~/.bashrc`):

```bash
source ~/zephyrproject/.venv/bin/activate
source ~/.zephyrrc
export ZEPHYR_BASE=~/zephyrproject/zephyr
export PATH="$ZEPHYR_SDK_INSTALL_DIR/gnu/xtensa-espressif_esp32s3_zephyr-elf/bin:$PATH"
```

## Verify SDK

```bash
source ~/zephyrproject/.venv/bin/activate && source ~/.zephyrrc
xtensa-espressif_esp32s3_zephyr-elf-gcc --version
```

## Clone this repository

```bash
git clone <your-repo-url> esp32-s3-imu-basics
cd esp32-s3-imu-basics
```

The board definition lives **in-repo** under `zephyr/boards/waveshare/esp32s3_lcd_147b/` — no extra board pack required.

## Sanity build (display sample)

Replace `/path/to/esp32-s3-imu-basics` with your clone path:

```bash
source ~/zephyrproject/.venv/bin/activate && source ~/.zephyrrc
export ZEPHYR_BASE=~/zephyrproject/zephyr
cd ~/zephyrproject/zephyr

west build -p always -b esp32s3_lcd_147b/esp32s3/procpu \
  samples/drivers/display -- \
  -DBOARD_ROOT="/path/to/esp32-s3-imu-basics/zephyr"

west flash --esp-device /dev/ttyACM0
```

## Flash backup (recommended)

Before first Zephyr flash on a board running Arduino firmware:

```bash
PORT=/dev/ttyACM0
esptool.py --port "$PORT" read_flash 0 0x1000000 "flash_backup_$(date +%Y%m%d).bin"
```

Restore Arduino: `esp32_s3_imu_basics/scripts/build.sh production --upload`  
Or full image: `esptool.py --port "$PORT" write_flash 0 flash_backup_YYYYMMDD.bin`

## Next steps

- [zephyr-build.md](zephyr-build.md) — build and flash `handshake`
- [zephyr-hardware.md](zephyr-hardware.md) — peripheral map
