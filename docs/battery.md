# Battery measurement

How cell voltage is read on the Waveshare ESP32-S3-LCD-1.47B and shown on the HUD.

## Hardware

| Item | Value |
|------|-------|
| Sense pin | **GPIO1** (ADC1 channel 0) |
| Divider | 200 kΩ (high) / 100 kΩ (low) → ratio **3.0×** |
| VBUS sense | **None** — DC vs battery is inferred from voltage trend |

LiPo connects via the board JST connector. With USB plugged in and no battery, GPIO1 may sit below the “external DC” threshold → HUD shows **`DC ext`**.

## Arduino reference (gold standard)

File: `esp32_s3_imu_basics/battery_monitor.cpp`

1. `analogReadResolution(12)`
2. Average **4×** `analogReadMilliVolts(GPIO1)` (ESP-IDF eFuse curve-fit calibration)
3. `v_cell = (adc_v × 3.0) / bat_offset` with `bat_offset = 0.992857`
4. EMA smoothing (`BAT_VOLTAGE_EMA = 0.25`)
5. Map to % using `BAT_EMPTY_V = 3.00` … `BAT_FULL_V = 4.20`
6. **Trend classifier** over 16 samples: rising → charging/DC, falling → battery, stable high → DC

## Zephyr port

| File | Role |
|------|------|
| `battery_adc_esp32.c` | `adc_oneshot_hal` raw read + IDF `adc_cali` curve-fit |
| `battery_monitor.c` | Same EMA, %, trend, DC/BAT logic as Arduino |
| `board_config.h` | Shared constants |

**Important:** Do not call `adc_set_hw_calibration_code()` during battery reads when using `adc_cali` curve-fit — it inflates raw ~2.3× (HUD showed 9.18 V instead of 4.19 V). The curve-fit cal applies per-channel compensation in software.

Init logs (serial @ 115200):

```
bat_adc: battery ADC cal efuse ver=... ok=...
battery probe raw=... adc=...mV
battery init GPIO1 adc=...mV v=...V pct=...% src=...
```

Telemetry every 10 s:

```
telemetry ... bat=4.05V 82% adc=1320mV src=DC
```

## HUD labels

| Display | Condition |
|---------|-----------|
| `BAT 82%` | Classified as battery (`src=BAT`) |
| `DC 4.05V` | USB/charge path and cell voltage readable (`src=DC`, v ≥ 3.25 V) |
| `DC ext` | USB power but ADC &lt; 3.25 V — empty JST, dead cell, or calibration mismatch |

## Validation

Compare Arduino vs Zephyr on the **same board** with a LiPo on JST:

```bash
# Arduino
cd esp32_s3_imu_basics && PORT=/dev/ttyACM0 ./scripts/build.sh production --upload
# Note serial battery lines

# Zephyr
PORT=/dev/ttyACM0 ../zephyr/scripts/flash-zephyr.sh handshake
# Compare probe raw= and adc= values
```

Healthy USB+battery: expect `adc` roughly **1200–1400 mV** at pin (≈ **3.6–4.2 V** cell after ×3 divider math).

If Zephyr `adc` reads ~3000 mV while Arduino reads ~1300 mV, the Zephyr ADC channel setup or attenuation index needs adjustment — file an issue with both boot logs.
