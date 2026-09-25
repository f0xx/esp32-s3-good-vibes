#!/usr/bin/env bash
# Run host binary under TSan / ASan+UBSan / Valgrind.
# Headless-safe: forces HOST_NO_SDL=1 so optional SDL never blocks CI-like runs.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
CC="${CC:-clang}"
export PATH="/usr/lib/llvm/22/bin:${PATH}"
export HOST_NO_SDL=1
export TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1:second_deadlock_stack=1}"
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"

SECONDS_ARG=(--seconds "${HOST_SAN_SECONDS:-300}" --log-level inf --nvs-path "$ROOT/build/san_nvs.bin")
STRESS_SECONDS="${HOST_STRESS_SECONDS:-300}"
FAIL=0

run_cfg() {
  local name="$1"; shift
  local build_dir="$ROOT/build-$name"
  echo ""
  echo "======== $name ========"
  rm -rf "$build_dir"
  cmake -S "$ROOT" -B "$build_dir" -DCMAKE_C_COMPILER="$CC" "$@"
  cmake --build "$build_dir" -j"$(nproc)"
  if "$build_dir/handshake_host" "${SECONDS_ARG[@]}"; then
    echo "OK $name"
  else
    echo "FAIL $name (exit $?)"
    FAIL=1
  fi
}

run_cfg tsan -DHOST_TSAN=ON -DHOST_ASAN=OFF -DHOST_UBSAN=OFF -DHOST_FLAT_HEAP=ON
run_cfg asan_ubsan -DHOST_TSAN=OFF -DHOST_ASAN=ON -DHOST_UBSAN=ON -DHOST_FLAT_HEAP=ON -DHOST_FLAT_HEAP_WRAP=OFF

# Plain binary for valgrind (sanitizers + valgrind fight).
run_cfg plain -DHOST_TSAN=OFF -DHOST_ASAN=OFF -DHOST_UBSAN=OFF -DHOST_FLAT_HEAP=ON

if command -v valgrind >/dev/null 2>&1; then
  echo ""
  echo "======== valgrind ========"
  # Custom arena hides leaks from Memcheck — disable flat heap under Valgrind.
  # Process-lifetime wq pthread TLS shows as "possibly lost" — only fail on definite.
  VG_SECONDS=(--seconds "${HOST_VALGRIND_SECONDS:-300}" --log-level inf --no-flat-heap \
    --nvs-path "$ROOT/build/san_nvs_vg.bin")
  if HOST_NO_FLAT_HEAP=1 valgrind --error-exitcode=99 --leak-check=full \
      --show-leak-kinds=definite --errors-for-leak-kinds=definite \
      --track-origins=yes \
      "$ROOT/build-plain/handshake_host" "${VG_SECONDS[@]}"; then
    echo "OK valgrind"
  else
    echo "FAIL valgrind (exit $?)"
    FAIL=1
  fi
else
  echo "SKIP valgrind (not installed)"
fi

# Stress under TSan
echo ""
echo "======== tsan stress-recover ========"
if "$ROOT/build-tsan/handshake_host" --seconds "$STRESS_SECONDS" --stress-recover --log-level wrn \
    --nvs-path "$ROOT/build/san_nvs_stress.bin"; then
  echo "OK tsan-stress"
else
  echo "FAIL tsan-stress"
  FAIL=1
fi

echo ""
if [[ "$FAIL" -eq 0 ]]; then
  echo "All host sanitizer runs passed."
  exit 0
fi
echo "One or more sanitizer runs failed."
exit 1
