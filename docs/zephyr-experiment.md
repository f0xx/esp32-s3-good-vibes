# Zephyr experiment — revertable fork (F8)

**Status:** frozen planning doc for a forked thread. **Does not replace** Arduino firmware on `main`.

**Goal:** evaluate Zephyr on real hardware with a path back to `./scripts/build.sh production --upload`.

---

## Revert contract (non‑negotiable)

1. **Git:** all Zephyr work on branch `experiment/zephyr` (or worktree). `main` stays Arduino.
2. **Layout:** new tree only — `zephyr/` at repo root (west workspace or app submodule). Do **not** delete/replace `esp32_s3_imu_basics/`.
3. **Flash backup** before first Zephyr flash on the Waveshare board:

```bash
PORT=/dev/ttyACM0
esptool.py --port "$PORT" chip_id
esptool.py --port "$PORT" read_flash 0 0x1000000 "flash_backup_$(date +%Y%m%d).bin"
# 16MB — matches board FlashSize=16M
```

4. **Restore Arduino** (pick one):

```bash
# A — normal (recommended)
./esp32_s3_imu_basics/scripts/build.sh production --upload

# B — full image restore
esptool.py --port "$PORT" write_flash 0 flash_backup_YYYYMMDD.bin
```

5. **Recovery if USB weird:** hold **BOOT**, tap **RESET**, release BOOT → download mode; reflash Arduino build.

ESP32-S3 ROM USB bootloader remains unless partition table/bootloader is deliberately overwritten. Zephyr uses its own partition map — **backup + branch** is enough for experiments.

---

## Reality check: “all features” migration

Current Arduino stack (~90 C++ units): QMI8658, ST7789+LVGL-style scene, BLE GATT (IMU/net/OTA/config), WiFi prov, cloud uploader, vibro verdict, power profiles, CoopTasks, Android protocol parity.

| Track | Board | Notes |
|-------|--------|--------|
| **Your hardware** | Waveshare ESP32-S3-LCD-1.47**B** on `/dev/ttyACM0` | Custom out-of-tree board `esp32s3_lcd_147b` in `zephyr/boards/waveshare/` |
| **Reference DTS** | Waveshare 1.28″ in Zephyr 3.7 | Pin template only (different GPIO map) |

**Recommendation:** build `hello_world` for `esp32s3_devkitm/esp32s3/procpu` → flash → serial sanity → custom Waveshare overlay → IMU/LCD/BLE port.

Waveshare 1.47**B** pins (from `board_config.h`, not 1.47 non-B wiki):

| Function | GPIO |
|----------|------|
| TFT MOSI/SCLK/CS/DC/RST/BL | 45/40/42/41/39/46 |
| IMU I2C SDA/SCL | 48/47 |
| WS2812 | 38 |
| BOOT | 0 |
| BAT ADC | 1 |

Reference: Zephyr has `waveshare/esp32s3_touch_lcd_1_28` — use as DTS template (GitHub zephyr#86323).

---

## Feature migration phases (forked thread backlog)

| Phase | Deliverable | Parity with today |
|-------|-------------|-------------------|
| Z0 | west build + flash + shell | — |
| Z1 | Custom board overlay `waveshare_esp32s3_lcd_147b` | pin map |
| Z2 | I2C + QMI8658 read | IMU raw |
| Z3 | SPI + ST7789 + backlight PWM | LCD |
| Z4 | BLE peripheral + one NUS/ custom UUID | IMU notify stub |
| Z5 | Full GATT service port | Android app connects — see `zephyr/app/handshake/` + `docs/dual-firmware-probing.md` |
| Z6 | WiFi + prov | wizard |
| Z7 | Vibro + verdict + cloud | machine pipeline |
| Z8 | OTA (MCUboot) | BLE OTA |

Do **not** block Z0–Z3 on full GATT parity.

---

## Gentoo — Zephyr SDK install (bash)

System deps (adjust for your profile):

```bash
# As root — names may vary on Gentoo
emerge -av dev-build/cmake dev-python-pip dev-python-venv \
  dev-vcs/git wget curl dtc ncurses

# Optional but useful
emerge -av dev-embedded/dtc  # if not pulled
```

User workspace:

```bash
export ZEPHYR_BASE="$HOME/zephyrproject/zephyr"
mkdir -p ~/zephyrproject && cd ~/zephyrproject

python3 -m venv .venv
source ~/zephyrproject/.venv/bin/activate
pip install -U pip wheel west

# LTS tag — bump in fork if you want mainline
west init -m https://github.com/zephyrproject-rtos/zephyr --mr v3.7.0
cd zephyr
west update
pip install -r scripts/requirements.txt   # pyelftools etc. (west packages needs newer west)

# Zephyr SDK — must be complete ~1.4GB download before tar xf
cd ~
wget -c https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.16.8/zephyr-sdk-0.16.8_linux-x86_64.tar.xz
tar tf zephyr-sdk-0.16.8_linux-x86_64.tar.xz >/dev/null   # verify not truncated
tar xf zephyr-sdk-0.16.8_linux-x86_64.tar.xz
~/zephyr-sdk-0.16.8/setup.sh -t xtensa-espressif_esp32s3_zephyr-elf -c -h

cat > ~/.zephyrrc <<'EOF'
export ZEPHYR_SDK_INSTALL_DIR="$HOME/zephyr-sdk-0.16.8"
EOF

source ~/zephyrproject/.venv/bin/activate
source ~/.zephyrrc
export ZEPHYR_BASE=~/zephyrproject/zephyr
echo 'source ~/zephyrproject/.venv/bin/activate' >> ~/.bashrc
echo 'source ~/.zephyrrc' >> ~/.bashrc
echo 'export ZEPHYR_BASE=~/zephyrproject/zephyr' >> ~/.bashrc
```

Sanity build (Waveshare ESP32-S3 — **correct pin map**):

```bash
source ~/zephyrproject/.venv/bin/activate && source ~/.zephyrrc
export ZEPHYR_BASE=~/zephyrproject/zephyr
cd ~/zephyrproject/zephyr

# Display corners test — ST7789 172×320, USB-CDC console, BOOT=GPIO0
west build -p always -b esp32s3_lcd_147b/esp32s3/procpu samples/drivers/display -- \
  "-DBOARD_ROOT=/home/foxx/repos/My Projects/espXX/esp32-s3-imu-basics/zephyr"

west flash --esp-device /dev/ttyACM0

# serial (115200 USB-CDC): expect "*** Booting Zephyr OS ***" + "Display sample for st7789v@0"
python3 -c "import serial,time; s=serial.Serial('/dev/ttyACM0',115200,timeout=1); time.sleep(2); print(s.read(4096).decode('utf-8','replace'))"
```

**Why generic `esp32s3_devkitm` looked broken:** wrong SPI/UART pins, SPI3 CS on GPIO38 (WS2812 → constant red), no LCD/BLE init, console on UART0 GPIO43/44 not USB-CDC.

Restore Arduino: `./esp32_s3_imu_basics/scripts/build.sh production --upload` or `backups/restore-arduino-fullflash.sh`

Clone this repo’s experiment branch into workspace (later):

```bash
cd "/home/foxx/repos/My Projects/espXX/esp32-s3-imu-basics"
git checkout -b experiment/zephyr
mkdir -p zephyr/app
# west manifest or CMake app TBD in forked thread
```

---

## What we keep frozen on `main`

- Android app, backend/Grafana, WiFi wizard — unchanged until Zephyr reaches Z5+.
- `features.md` F8 stays **deferred** until experiment branch proves Z4.

---

## Forked thread starter prompt

Copy into new chat:

> Continue Zephyr experiment from `docs/zephyr-experiment.md`. Hardware: **Waveshare ESP32-S3-LCD-1.47B only** (`/dev/ttyACM0`). Revert via flash backup + `./scripts/build.sh production --upload`. Next: flash `hello_world` on esp32s3_devkitm, then Waveshare 1.47B devicetree overlay.

---

## Open questions for you

1. **Zephyr version:** v3.7 LTS vs main (v4.x has Waveshare 1.28″ board)?
2. **First flash OK?** `hello_world` on generic S3 target (no LCD yet) — backup taken?
3. **Forked thread:** custom `zephyr/app` in-repo vs standalone west workspace?
