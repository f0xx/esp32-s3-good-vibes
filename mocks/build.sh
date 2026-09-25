#!/usr/bin/env bash
# Build + run host IMU pipeline (TSan by default).
# SDL is optional: linked if pkg-config finds sdl2; runtime auto-skips headless.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="${ROOT}/build"
CC="${CC:-clang}"

mkdir -p "$BUILD"
cmake -S "$ROOT" -B "$BUILD" \
  -DCMAKE_C_COMPILER="$CC" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DHOST_TSAN=ON \
  -DHOST_ASAN=OFF \
  -DHOST_UBSAN=OFF \
  -DHOST_ENABLE_SDL=ON
cmake --build "$BUILD" -j"$(nproc)"

echo "--- run handshake_host $* ---"
export TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1:second_deadlock_stack=1}"
"$BUILD/handshake_host" "$@"
