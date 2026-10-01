# System architecture

End-to-end data flow for the ESP32-S3 IMU sensor platform.

## Components

```mermaid
flowchart LR
  subgraph board [ESP32-S3-LCD-1.47B]
    IMU[QMI8658 IMU]
    TFT[ST7789 172x320]
    BLE[BLE peripheral]
    WIFI[WiFi STA optional]
    IMU --> FW[Zephyr handshake]
    FW --> TFT
    FW --> BLE
    FW --> WIFI
  end

  subgraph phone [Android ESP32S3ImuSim]
    SCAN[BLE scan/connect]
    SCENE[Live scene view]
    VIBRO[Vibration verdicts]
    OFFLOAD[JSONL offload queue]
    SCAN --> SCENE
    SCENE --> VIBRO
    VIBRO --> OFFLOAD
  end

  subgraph cloud [Backend VM]
    API[FastAPI :8080]
    DB[(TimescaleDB)]
    GRAF[Grafana :3000]
    API --> DB
    DB --> GRAF
  end

  BLE <-->|GATT IMU/NET/Config| phone
  OFFLOAD -->|HTTPS POST verdicts| API
```

## Use cases

| Case | Path | Description |
|------|------|-------------|
| **A** | Board → WiFi → (future) | Body sensor with direct network when available |
| **B** | Board → BLE → Phone → Backend | Machine vibration: edge verdict on phone, upload when online |
| **C** | Board → BLE → Phone | Phone-as-transmitter: live IMU scene, optional cloud |

The **handshake** firmware and Android app implement Case B/C today. The backend ingests **verdicts only** (low-rate JSON), not raw IMU streams.

## Firmware tracks

| Track | When to use |
|-------|-------------|
| **Zephyr `handshake`** | Production — IMU, scene, BLE GATT, WiFi, A/B OTA, crash ring |
| **Zephyr `smoke`** | Hardware sanity — LCD, BOOT, BLE advertising only |
| **Arduino `production`** | Frozen reference — battery curve / early UI. Overwrites MCUboot |

See [dual-firmware-probing.md](dual-firmware-probing.md) for switching between tracks.
Cloud / phone / A/B sequence: [zephyr-ota.md](zephyr-ota.md).

## BLE services (handshake)

| Service | UUID prefix | Purpose |
|---------|-------------|---------|
| IMU | `4a6e0001-…` | Live samples, mode, caps, status |
| Config | `4a6e0101-…` | Device settings |
| NET | `4a6e0200-…` | WiFi profile provisioning, HTTP proxy |
| OTA | `4a6e0201-…` | MCUboot signed-slot transfer (CTRL/DATA) |
| Crash | `4a6e0301-…` | Crash-ring drain (OTA OK/NOK rides here) |

Protocol headers: `zephyr/app/common/ble_imu_protocol.h`, Android `ImuProtocol.kt`.
Arduino headers are legacy.

## Storage

| Store | Location | Contents |
|-------|----------|----------|
| NVS (board) | Zephyr `storage` partition | WiFi profiles, device config |
| Offload (phone) | `files/offload/verdicts.jsonl` | Pending verdict uploads |
| Postgres (backend) | Docker volume | Ingested verdict rows |

## Typical deploy sequence

1. Flash **handshake** on the board → [zephyr-build.md](zephyr-build.md)
2. Install **Android APK** → [android-app.md](android-app.md)
3. Start **backend** on a LAN VM → [backend.md](backend.md)
4. Configure Cloud URL + API key in the app
5. Connect BLE, verify scene; verdicts upload when network is up
