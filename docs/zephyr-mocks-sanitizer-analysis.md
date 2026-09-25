# Host sanitizer analysis (2026-09-15)

## Suite (`mocks/sanitize-all.sh` → `/tmp/mocks-san-reports`)

| Tool | Result | Notes |
|------|--------|-------|
| TSan | OK after fixes | Long `--virtual-time --stress-recover` was the finder |
| ASan+UBSan | OK | No heap OOB / UB |
| UBSan alone | OK | |
| LSan | OK | No definite leaks |
| MSan | OK | Flat heap disabled for MSan |
| Valgrind memcheck | OK | 0 definite leaks |
| Helgrind | OK | 0 errors (many suppressed) |

## Races TSan found (desk hang relevant)

### 1. Host shim — lazy `k_mutex` init (host-only)
`K_MUTEX_DEFINE` left mutex NULL; render + recover first-touch raced → unlock-of-unlocked.
**Fix:** static `PTHREAD_MUTEX_INITIALIZER` (matches Zephyr static mutexes).

### 2. Firmware — `g_latest` torn publish (v233)
`imu_pipeline_read_raw` wrote `g_latest` unlocked while `imu_pipeline_snapshot` read under `imu_lock` (recover/cal path unlocks during I2C on purpose).
**Fix:** `read_raw` only fills `*out`; publish `g_latest` under lock in `tick` and at end of `init_hw_and_calibrate`.

### 3. Firmware — backoff fields (v234)
`imu_pipeline_request_recover` read `g_recover_fail_streak` / `g_recover_backoff_until_ms` unlocked vs recover work writes.
**Fix:** check backoff under `imu_lock`.

Post-fix: TSan clean on normal + 15s stress + 100s virtual-time stress (`WARNINGS=0`).

## Desk
Repeated SILENT hangs during the session; recovered via USB. Flashed **handshake v234** with the firmware race fixes. Soft-reset/PSRAM hang may still be separate — watch after this build.

## Re-run
```bash
cd mocks && ./sanitize-all.sh
TSAN_OPTIONS=halt_on_error=1 ./build-tsan/handshake_host \
  --seconds 100 --virtual-time --stress-recover
```
