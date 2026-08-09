# Zephyr hardware port — Waveshare ESP32-S3-LCD-1.47B

Out-of-tree board: `esp32s3_lcd_147b/esp32s3/procpu`  
DTS: `zephyr/boards/waveshare/esp32s3_lcd_147b/esp32s3_lcd_147b_esp32s3_procpu.dts`

## Driver stack summary

| Subsystem | Hardware | Zephyr Kconfig / DTS | Implementation |
|-----------|----------|----------------------|----------------|
| SoC | ESP32-S3 PROCPU | `espressif,esp32s3` | HAL from `modules/hal/espressif` |
| Clock / CPU | PLL, APB | `CONFIG_CLOCK_CONTROL` | `power_manager.c` → `clock_control_configure()` |
| Console | USB CDC | `zephyr,console = &usb_serial` | `usb_serial` on ESP32-S3 |
| SPI | SPI2 | `&spi2` | `spi_esp32` — MOSI45, SCLK40, CS42 |
| Display DBI | ST7789 | `sitronix,st7789v` | `mipi_dbi_spi.c` + panel init in DTS |
| Framebuffer | PSRAM | `CONFIG_ESP_SPIRAM` | `panel_fb.c` — 172×320 RGB565 in `.ext_ram.bss` |
| Backlight | LEDC ch0 | `pwm-leds` / `&ledc0` | `panel_backlight.c` |
| I2C | I2C0 | `&i2c0` SDA48/SCL47 | `i2c_esp32` |
| IMU | QMI8658 | — (no DT binding) | `qmi8658.c` — register-level driver |
| ADC | SAR ADC1 | `&adc1` | `adc_esp32` + `battery_adc_esp32.c` curve-fit |
| GPIO keys | BOOT | `gpio-keys` button0 | `boot_button.c` |
| BT controller | ESP32 BT | `zephyr,bt-hci = &esp32_bt_hci` | HCI shim → Zephyr Bluetooth host |
| BT GATT | — | `CONFIG_BT_PERIPHERAL` | `ble_imu_gatt.c`, `ble_net_gatt.c`, … |
| WiFi | ESP32 MAC | `&wifi` | `CONFIG_WIFI` + `esp32` WiFi driver, STA + DHCP |
| NVS | flash partition | `storage_partition` | `CONFIG_SETTINGS_NVS`, `net_profile_store.c` |
| Temp | on-die | `&coretemp` | `chip_temp.c` |
| Watchdog | WDT0 | `&wdt0` | enabled in board DTS |
| OTA | MCUboot slots | slot0/slot1 | `ble_ota_gatt.c`, `CONFIG_MCUBOOT_IMG_MANAGER` |

## MCU / memory

| Item | Detail |
|------|--------|
| SoC | ESP32-S3 (Xtensa LX7, PROCPU target) |
| Flash | 16 MB external (2 MB image header in current Zephyr map) |
| PSRAM | 8 MB octal @ 40 MHz (`CONFIG_SPIRAM_MODE_OCT`) |
| Console | USB CDC (`zephyr,console = &usb_serial`) @ 115200 |
| Bootloader | MCUboot partition @ 64 KB |

## CPU and power (`power_manager.c`)

| Mode | CPU target | APB | IMU rate | Render | Trigger |
|------|------------|-----|----------|--------|---------|
| Screen ON | **240 MHz** | 80 MHz | 10 Hz | 30 Hz target (~21 Hz measured) | default |
| Screen OFF | **160 MHz** | 80 MHz | 100 Hz | off | BOOT short tap |

Implementation:

- **Driver:** `espressif_xtensa_lx7` clock control (`clock_control_configure`, ESP32 CPU PLL)
- CPU changes only on screen toggle (no periodic PLL retries — avoids BLE/reboot issues)
- Telemetry logs target vs actual CPU every 10 s

## Display — ST7789 172×320

| Signal | GPIO | Zephyr stack |
|--------|------|--------------|
| MOSI | 45 | SPI2 |
| SCLK | 40 | SPI2 |
| CS | 42 | SPI2 |
| DC | 41 | MIPI DBI |
| RST | 39 | panel reset |
| BL | 46 | PWM LEDC (`panel_backlight.c`) |

| Layer | Component |
|-------|-----------|
| Controller | `sitronix,st7789v` (Zephyr display driver) |
| Transport | MIPI DBI SPI (`mipi_dbi_spi.c`) |
| Framebuffer | 172×320 RGB565 in PSRAM (`panel_fb.c`, 80-row SPI strips) |
| Font/scene | `panel_draw.c` + GLCD font, `scene_live.c` |

Backlight default **40%** (vendor max recommendation 50%).

## IMU — QMI8658

| Signal | GPIO | Bus |
|--------|------|-----|
| SDA | 48 | I2C0 |
| SCL | 47 | I2C0 |
| INT1 | 13 | (optional) |
| INT2 | 12 | (optional) |

| Layer | File |
|-------|------|
| I2C driver | Zephyr `i2c_esp32` |
| Chip driver | `zephyr/app/handshake/src/qmi8658.c` |
| Pipeline | `imu_pipeline.c` — cal, attitude, walk distance |
| Scene | `scene_live.c`, `scene_zoom.c` @ render thread 30 Hz |

## Bluetooth

| Item | Value |
|------|-------|
| Controller | ESP32-S3 integrated BT (HCI via `esp32_bt_adapter`) |
| Stack | Zephyr **Bluetooth Host** (`CONFIG_BT`) |
| Role | Peripheral |
| Device name | `ESP32S3 IMU sim` |
| MTU | 517 |
| Services | IMU notify (`ble_imu_gatt.c`), NET proxy (`ble_net_gatt.c`), config (`ble_config_gatt.c`), OTA (`ble_ota_gatt.c`) |

Coexistence with WiFi enabled (`CONFIG_ESP32_WIFI_*` + `CONFIG_BT`).

## WiFi

| Item | Value |
|------|-------|
| Chip | ESP32-S3 integrated |
| Stack | Zephyr networking + ESP32 WiFi driver |
| Mode | STA, DHCPv4 |
| Start | Lazy — `network_manager.c` after BLE init |
| Buffers | Minimal (4–8 packets; AMPDU disabled) |

WiFi profiles stored in NVS (`net_profile_store.c`); Android app can provision via BLE NET GATT.

## Battery ADC

| Item | Value |
|------|-------|
| Pin | **GPIO1** = ADC1 channel 0 |
| Divider | 200 kΩ / 100 kΩ → **×3.0** |
| Calibration | eFuse curve-fit (same algorithm as Arduino `analogReadMilliVolts`) |
| Code | `battery_adc_esp32.c` + `battery_monitor.c` |
| Reference | `esp32_s3_imu_basics/battery_monitor.cpp` |

Constants (`board_config.h`): full 4.20 V, empty 3.00 V, offset 0.992857.  
DC vs BAT: trend-based (no VBUS GPIO) — same as Arduino.

## Other peripherals

| Function | GPIO | Driver |
|----------|------|--------|
| BOOT button | 0 | `boot_button.c` (GPIO keys) |
| WS2812 RGB | 38 | bit-bang `ws2812_gpio38.c` |
| Chip temp | on-die | `CONFIG_ESP32_TEMP` / `chip_temp.c` |
| NVS/settings | flash | `CONFIG_SETTINGS_NVS`, `device_config.c` |
| TF card | 14–16, 18, 21 | DTS present, not used in handshake app |

## Apps

| App | Purpose |
|-----|---------|
| `handshake` | Full product firmware |
| `smoke` | BLE-only smoke test (`WS147B-Zephyr` name) |

## Partition map (Zephyr)

| Partition | Size |
|-----------|------|
| mcuboot | 64 KB |
| slot0 / slot1 | 1 MB each |
| storage (NVS) | 24 KB |

See `esp32s3_lcd_147b_esp32s3_procpu.dts` `chosen` and `flash0/partitions`.
