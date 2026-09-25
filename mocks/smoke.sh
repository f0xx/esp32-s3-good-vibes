#!/usr/bin/env bash
# Fast host smoke (no Valgrind). Default for CI optional hook.
# Override duration with HOST_SMOKE_SECONDS (default 12).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
CC="${CC:-clang}"
export PATH="/usr/lib/llvm/22/bin:${PATH}"
export HOST_NO_SDL=1
export HOST_NO_LATENCY="${HOST_NO_LATENCY:-1}"
SEC="${HOST_SMOKE_SECONDS:-12}"
BUILD="$ROOT/build-smoke"

echo "mocks smoke: ${SEC}s (HOST_NO_LATENCY=${HOST_NO_LATENCY})"
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_C_COMPILER="$CC" \
  -DHOST_TSAN=ON -DHOST_ASAN=OFF -DHOST_UBSAN=OFF -DHOST_FLAT_HEAP=ON
cmake --build "$BUILD" -j"$(nproc)"
"$BUILD/handshake_host" --seconds "$SEC" --log-level wrn \
  --nvs-path "$BUILD/smoke_nvs.bin"
echo "mocks smoke OK"
