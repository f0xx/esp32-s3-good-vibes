# Android app — ESP32S3ImuSim

Phone-as-transmitter: connects to the board over BLE, shows live IMU scene, uploads verdicts to the backend.

Path: `android/ESP32S3ImuSim/`

## Requirements

| Tool | Version |
|------|---------|
| JDK | 21 (OpenJDK) |
| Android SDK | API 34 build-tools |
| Gradle | wrapper included |

## Build debug APK

```bash
cd android/ESP32S3ImuSim

export ANDROID_HOME="$HOME/Android/Sdk"
export JAVA_HOME="/usr/lib/jvm/openjdk-21"   # adjust for your system
export PATH="$JAVA_HOME/bin:$ANDROID_HOME/platform-tools:$PATH"

./gradlew assembleDebug
```

Install:

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Or use the helper script (hardcoded paths — edit if needed):

```bash
./rebuild.sh
```

## Configure on device

1. Open **ESP32S3ImuSim**
2. Scan → connect to **ESP32S3 IMU sim** (Zephyr) or matching name
3. **Cloud** settings (optional):
   - URL: `http://<backend-ip>:8080`
   - API key: same as backend `IMU_API_KEY`
   - Device / group IDs for ingest

## BLE protocol

Custom GATT services matching firmware:

- IMU notify + status JSON
- NET (HTTP proxy over BLE)
- Config / OTA (when enabled)

See `app/src/main/java/com/esp32s3/imusim/ImuProtocol.kt` and `BleImuClient.kt`.

## Dual firmware

The app probes Zephyr vs Arduino capabilities. See [dual-firmware-probing.md](dual-firmware-probing.md).

## Release build

```bash
./gradlew assembleRelease
# Sign with your keystore — configure signingConfigs in app/build.gradle.kts
```

Min SDK **26**, target SDK **34**, package `com.esp32s3.imusim`.
