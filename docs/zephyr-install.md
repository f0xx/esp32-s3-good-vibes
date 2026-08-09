# Zephyr installation (ESP32-S3-LCD-1.47B)

One-time setup for building `zephyr/app/handshake` on Linux. Tested with **Zephyr v3.7.0 LTS** and **Zephyr SDK 0.16.8**.

## Requirements

| Tool | Version |
|------|---------|
| Zephyr | v3.7.0 |
| Zephyr SDK | 0.16.8 |
| Python | ≥ 3.8 |
| west | ≥ 0.14 |
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

west init -m https://github.com/zephyrproject-rtos/zephyr --mr v3.7.0
cd zephyr
west update
pip install -r scripts/requirements.txt
```

## Zephyr SDK

```bash
cd ~
wget -c https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.16.8/zephyr-sdk-0.16.8_linux-x86_64.tar.xz
tar xf zephyr-sdk-0.16.8_linux-x86_64.tar.xz
~/zephyr-sdk-0.16.8/setup.sh -t xtensa-espressif_esp32s3_zephyr-elf -c -h
```

Add to `~/.zephyrrc`:

```bash
export ZEPHYR_SDK_INSTALL_DIR="$HOME/zephyr-sdk-0.16.8"
```

Shell init (add to `~/.bashrc`):

```bash
source ~/zephyrproject/.venv/bin/activate
source ~/.zephyrrc
export ZEPHYR_BASE=~/zephyrproject/zephyr
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
