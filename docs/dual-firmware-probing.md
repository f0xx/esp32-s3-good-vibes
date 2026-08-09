# Dual-firmware probing — Arduino vs Zephyr

Both firmware tracks stay in this repo so you can flash either image and probe the board from the Android app or nRF Connect.

| Track | Path | BLE name | Mobile handshake |
|-------|------|----------|------------------|
| **Arduino (production)** | `esp32_s3_imu_basics/` | `ESP32S3 IMU sim` | Full IMU + config + net + OTA |
| **Zephyr smoke** | `zephyr/app/smoke/` | `WS147B-Zephyr` | Advertising only (HW sanity) |
| **Zephyr handshake** | `zephyr/app/handshake/` | `ESP32S3 IMU sim` | IMU GATT stub (Z5) — app connects |

Only one image runs at a time. Flash the track you want before scanning.

---

## Revert / restore Arduino

Full 16 MB backup (recommended before first Zephyr flash):

```bash
./backups/restore-arduino-fullflash.sh
# or
./esp32_s3_imu_basics/scripts/build.sh production --upload
```

See `backups/LATEST` for SHA256 of the golden image.

---

## Flash Arduino (bare-metal)

```bash
./esp32_s3_imu_basics/scripts/build.sh production --upload
```

Expected after boot:

- LCD scene / IMU UI (production app)
- WS2812 acrylic via RMT (`rgbLedWrite` on GPIO38)
- BLE advertises **`ESP32S3 IMU sim`** with IMU service UUID `4a6e0001-…`

---

## Flash Zephyr handshake (mobile app parity stub)

West does not tolerate spaces in `BOARD_ROOT`; use the helper script (symlinks under `~/zephyrproject/`):

```bash
chmod +x zephyr/scripts/flash-zephyr.sh
PORT=/dev/ttyACM0 zephyr/scripts/flash-zephyr.sh handshake
```

Manual equivalent (replace `/path/to/esp32-s3-imu-basics` with your clone):

```bash
source ~/zephyrproject/.venv/bin/activate && source ~/.zephyrrc
REPO="/path/to/esp32-s3-imu-basics"
ln -sfn "$REPO/zephyr/app/handshake" ~/zephyrproject/waveshare-handshake
ln -sfn "$REPO/zephyr" ~/zephyrproject/waveshare-board-root
cd ~/zephyrproject/zephyr
west build -p always -b esp32s3_lcd_147b/esp32s3/procpu ~/zephyrproject/waveshare-handshake \
  -- -DBOARD_ROOT=~/zephyrproject/waveshare-board-root
west flash --esp-device /dev/ttyACM0
```

### Boot behaviour (handshake)

1. **WS2812 acrylic test** — red → green → blue (500 ms each), then red dimming 255→128→64→32→0, then off.
2. **LCD corners** — UL red, UR green, LR blue, LL grey (172×320 ST7789, BGR565).
3. **BLE** — connectable, name **`ESP32S3 IMU sim`**, IMU service UUID in advertising.

Press **BOOT** (GPIO0): acrylic R→G→B→off flash + white bar on LCD.

### Android app handshake (handshake firmware)

The app (`ImuProtocol.kt` / `BleImuClient.kt`) expects:

1. Scan filter: device name **or** IMU service UUID
2. Connect → MTU 517 → discover services
3. Enable NOTIFY on `…0006`, write MODE / POLL_MS / TIME, read CAPS
4. Poll DATA on NOTIFY

Zephyr implements all seven IMU characteristics. **QMI8658** on I2C (SDA=48, SCL=47) feeds live samples into DATA batches:

- **COMPUTED** (app default): one v3 row per poll with accel in `fx/fy/fz`, identity rotation matrix
- **RAW**: `[t,ax,ay,az,gx,gy,gz,dm]` rows
- **SCENE**: minimal row with live accel in footer fields

STATUS includes chip temp from IMU (`tc`) and `"fw":"zephyr"`. CAPS = `IMU+TFT+TEMP`.

Serial on boot should log `QMI8658 ready at 0x6B` and a sample line. If IMU init fails, batches stay empty (`n:0`) — check I2C wiring / WHO_AM_I.

**BOOT bar:** full-screen GRAM clear on boot (ST7789 RAM survives MCU reset). BOOT press clears to black, then a solid white bar (row-wise draw). Pink bands were stale `0xFFE0` from older flashes — that value renders magenta on this panel.

---

## Flash Zephyr smoke (hardware-only)

```bash
zephyr/scripts/flash-zephyr.sh smoke
```

- BLE name **`WS147B-Zephyr`** — will **not** match the Android app scan filter.
- Use for LCD/BOOT/WS2812-off checks and nRF Connect visibility.

---

## WS2812 acrylic — control model

The red acrylic edge light is a **single WS2812** (one GRB pixel) on **GPIO38**.

| Layer | Arduino | Zephyr handshake |
|-------|---------|------------------|
| Physical | Digital NRZ ~800 kHz | Same (GPIO bit-bang) |
| Colour order | GRB | GRB |
| “Brightness” | 8-bit R/G/B in protocol frame | Same — not DAC/ADC |
| Timing | RMT peripheral | `esp_rom_delay_us` bit-bang |

There is **no** analog PWM/DAC on the data pin. “PWM/frequency” in product docs refers to **programmable RGB levels inside the WS2812 frame**, not ESP32 LEDC on GPIO38. Future Zephyr work may switch to the `ws2812` SPI/RMT driver; bit-bang matches Arduino behaviour today.

**Do not** enable SPI3 CS on GPIO38 in devicetree — that latched red on early wrong-pin builds.

---

## Protocol source of truth

| Artifact | Path |
|----------|------|
| Arduino BLE headers | `esp32_s3_imu_basics/ble/ble_protocol.h`, `device_caps.h` |
| Zephyr shared header | `zephyr/app/common/ble_imu_protocol.h` |
| Zephyr GATT stub | `zephyr/app/handshake/src/ble_imu_gatt.c` |
| Android | `android/ESP32S3ImuSim/.../ImuProtocol.kt`, `BleImuClient.kt` |

Keep UUIDs and CAP bitmasks in sync when extending the Zephyr port (Z6+ net, OTA, real IMU batches).

---

## Identifying which firmware is running

| Signal | Arduino | Zephyr handshake | Zephyr smoke |
|--------|---------|------------------|--------------|
| BLE name | ESP32S3 IMU sim | ESP32S3 IMU sim | WS147B-Zephyr |
| Boot acrylic | App-driven | R/G/B cycle then off | Off after boot |
| STATUS JSON | Full telemetry | `"fw":"zephyr"` | N/A (no GATT) |
| Serial banner | Arduino / ESP-IDF | `handshake:` log module | `smoke:` log module |

When both Arduino and Zephyr use the same BLE name, only one board should be powered nearby during app testing.
