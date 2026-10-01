# Host mocks (native debug)

Build real `imu_pipeline` (+ cal / attitude / walk) on Linux with pthread stand-ins
for Zephyr so races show up under **ThreadSanitizer** — no desk board, no Renode.

## Quick start

```bash
cd mocks
./build.sh --seconds 8
./build.sh --seconds 6 --stress-recover
./build.sh --seconds 4 --log-level dbg
./build.sh --seconds 3 --log-level trace   # very chatty (mutex/sleep/wq)
./build.sh --seconds 5 --no-sdl            # force fb-only even with DISPLAY
HOST_NO_SDL=1 ./build.sh --seconds 4       # same, env form (CI-friendly)
```

Full sanitizer sweep (TSan, ASan+UBSan, plain + Valgrind if installed).
Default run duration is **300s** (override with `HOST_SAN_SECONDS` /
`HOST_STRESS_SECONDS` / `HOST_VALGRIND_SECONDS`):

```bash
./sanitize.sh
./sanitize-all.sh   # full matrix incl. MSan / Helgrind → /tmp/mocks-san-reports
```

Stub delays (QMI first-probe WHO misses, BLE/WiFi/MT200, render flush ~73ms) are
calibrated from the desk. The render thread holds the same ext-bus mutex the
firmware uses (`renderer_ext_bus_*`); IMU reads try-lock and skip while a flush
is in progress, and `renderer_busy()` is true for that window.
Set `HOST_NO_LATENCY=1` to skip the sleeps.

CI optional hook (default off): `RUN_MOCKS_SMOKE=1` on Cast runner runs
`mocks/smoke.sh` inside `imu-zephyr-ci` (Ubuntu/glibc; clang+valgrind in the
image). Not Alpine/musl.

Long-form docs (parameters, memory model, examples):

- [`docs/zephyr-mocks.md`](../docs/zephyr-mocks.md)
- [`docs/zephyr-mocks.pdf`](../docs/zephyr-mocks.pdf) — regenerate with `python3 docs/render-zephyr-mocks.py`

Binary: `mocks/build/handshake_host` (Clang + `-fsanitize=thread` by default).

## Host logger (`host_debug.h`)

Style inspired by `copy_recovery/clib_core/debug.h`:

```
[INF    123 tid=12345 core=2 name=main] <imu_pipe recover_work_fn imu_pipeline.c:324> IMU recover OK
```

| Field | Source |
|-------|--------|
| severity | `ERR/WRN/INF/DBG/TRC/FN` — filter with `--log-level` |
| ms | monotonic ms since `host_debug_init` |
| tid | Linux `gettid()` |
| core | `sched_getcpu()` (host “core id”) |
| name | optional `host_debug_set_thread_name` (`main` / `imu_wq` / `render`) |
| mod / func / file:line | module + `__func__` / `__FILE__` / `__LINE__` |

Zephyr `LOG_ERR/WRN/INF/DBG` map through this logger (with file/line). Host code can also use `HOST_LOG`, `P_DBG`, `LOG_API_ENTER/LEAVE`.

Levels: `err|wrn|inf|dbg|trace|func|all` or `0..10`.

## What runs

| Real firmware unit | Host |
|--------------------|------|
| `imu_pipeline.c` | compiled as-is |
| `imu_cal.c`, `attitude.c`, `walk_distance.c` | compiled as-is |
| `qmi8658.c` (I2C) | `src/qmi8658_host.c` — still + noise |
| `k_mutex` / `k_work_q` / `k_msleep` / uptime | `src/kernel_host.c` (pthread) |
| power / floor / vibro / zoom / stall WDT | `src/stubs_host.c` |
| panel RGB565 | `src/host_fb.c` (+ optional SDL window) |
| BLE NOTIFY path | `src/ble_host.c` — scripted connect→NOTIFY + FIFO |
| NVS / crash ring | `src/host_nvs.c` — file-backed (`--nvs-path`) |
| logger | `src/host_debug.c` |

`main_host` runs recover-on-wq + main poll + render snapshot @ ~30 Hz + BLE drain (desk freeze shape).

## Optional SDL (RGB565)

- **Build:** `pkg-config sdl2` → link with `-DHOST_HAS_SDL2`. Missing SDL → fb-only, no error.
- **Runtime auto-skip (headless-safe):** no `DISPLAY`/`WAYLAND_DISPLAY`, or `HOST_NO_SDL=1`, or `--no-sdl`. Soft-fails if `SDL_Init` fails.
- Safe for future CI: sanitize/build scripts set `HOST_NO_SDL=1`; other sim steps never depend on a window.

## Why not QEMU first?

`qemu-system-xtensa` is available, but ESP32-S3 + Zephyr BLE + PSRAM under QEMU
won’t reproduce desk soft-reset hangs well. Host + TSan is for **app races**.

## Later (not wired yet)

- CI job calling `./sanitize.sh` (keep SDL auto-skip; do not require Xvfb)

## Sanitizer notes

`./sanitize.sh` runs TSan, ASan+UBSan, plain, Valgrind (if installed), then TSan `--stress-recover`.

Valgrind treats process-lifetime workqueue pthread TLS as “possibly lost”; the script only fails on **definite** leaks (`--errors-for-leak-kinds=definite`).
