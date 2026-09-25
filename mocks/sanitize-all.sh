#!/usr/bin/env bash
# Exhaustive host sanitizer sweep. Reports always under /tmp (no spaces in paths).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
REP=/tmp/mocks-san-reports
CC="${CC:-clang}"
export PATH="/usr/lib/llvm/22/bin:${PATH}"
export HOST_NO_SDL=1
mkdir -p "$REP"
rm -f "$REP"/*

# Long soak by default — intermittent hangs on desk often need minutes, not seconds.
SECONDS_N="${HOST_SAN_SECONDS:-300}"
STRESS_N="${HOST_STRESS_SECONDS:-300}"
VG_N="${HOST_VALGRIND_SECONDS:-300}"
FAIL=0

run_cfg() {
  local name="$1"; shift
  local build_dir="$ROOT/build-$name"
  local log="$REP/${name}.log"
  echo ""
  echo "======== $name ========"
  {
    echo "### configure $name $*"
    rm -rf "$build_dir"
    cmake -S "$ROOT" -B "$build_dir" -DCMAKE_C_COMPILER="$CC" "$@"
    cmake --build "$build_dir" -j"$(nproc)"
  } >"$log" 2>&1 || { echo "FAIL $name (build)"; FAIL=1; return 0; }

  local bin="$build_dir/handshake_host"
  local nvs="$REP/${name}_nvs.bin"
  rm -f "$nvs"

  set +e
  HOST_NO_SDL=1 "$bin" --seconds "$SECONDS_N" --log-level inf --nvs-path "$nvs" \
    >>"$log" 2>&1
  local rc=$?
  set -e
  if [[ $rc -eq 0 ]]; then
    echo "OK $name (rc=0)"
  else
    echo "FAIL $name (run rc=$rc) see $log"
    FAIL=1
  fi
}

echo "Reports -> $REP"
echo "run seconds: san=$SECONDS_N stress=$STRESS_N valgrind=$VG_N"
echo "clang: $(clang --version | head -1)"

# Keep OPTIONS free of paths with spaces; dump to stdout/stderr into .log files.
export TSAN_OPTIONS="halt_on_error=0:second_deadlock_stack=1:history_size=7"
run_cfg tsan -DHOST_TSAN=ON -DHOST_ASAN=OFF -DHOST_UBSAN=OFF -DHOST_FLAT_HEAP=ON

echo ""
echo "======== tsan-stress ========"
if [[ ! -x "$ROOT/build-tsan/handshake_host" ]]; then
  echo "FAIL tsan-stress (no binary — tsan build failed)"
  FAIL=1
else
  set +e
  {
    export TSAN_OPTIONS="halt_on_error=0:second_deadlock_stack=1:history_size=7"
    "$ROOT/build-tsan/handshake_host" --seconds "$STRESS_N" --stress-recover --log-level wrn \
      --nvs-path "$REP/tsan_stress_nvs.bin"
    echo "stress_exit=$?"
  } >"$REP/tsan-stress.log" 2>&1
  set -e
  if grep -q 'WARNING: ThreadSanitizer' "$REP/tsan.log" "$REP/tsan-stress.log" 2>/dev/null; then
    echo "TSan races found"
    FAIL=1
  elif ! grep -q 'stress_exit=0' "$REP/tsan-stress.log"; then
    echo "FAIL tsan-stress (run)"; FAIL=1
  else
    echo "OK tsan-stress (no WARNING)"
  fi
fi

export ASAN_OPTIONS="detect_leaks=1:halt_on_error=0"
export UBSAN_OPTIONS="halt_on_error=0:print_stacktrace=1"
run_cfg asan_ubsan -DHOST_TSAN=OFF -DHOST_ASAN=ON -DHOST_UBSAN=ON \
  -DHOST_FLAT_HEAP=ON -DHOST_FLAT_HEAP_WRAP=OFF

unset ASAN_OPTIONS || true
export UBSAN_OPTIONS="halt_on_error=0:print_stacktrace=1"
run_cfg ubsan -DHOST_TSAN=OFF -DHOST_ASAN=OFF -DHOST_UBSAN=ON \
  -DHOST_FLAT_HEAP=ON -DHOST_FLAT_HEAP_WRAP=ON

echo ""
echo "======== lsan ========"
set +e
{
  rm -rf "$ROOT/build-lsan"
  cmake -S "$ROOT" -B "$ROOT/build-lsan" -DCMAKE_C_COMPILER="$CC" \
    -DHOST_TSAN=OFF -DHOST_ASAN=OFF -DHOST_UBSAN=OFF \
    -DHOST_FLAT_HEAP=ON -DHOST_FLAT_HEAP_WRAP=OFF \
    -DCMAKE_C_FLAGS="-fsanitize=leak -fno-omit-frame-pointer" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=leak"
  cmake --build "$ROOT/build-lsan" -j"$(nproc)"
  export LSAN_OPTIONS="halt_on_error=0"
  HOST_NO_SDL=1 "$ROOT/build-lsan/handshake_host" --seconds "$SECONDS_N" --log-level inf \
    --nvs-path "$REP/lsan_nvs.bin"
  echo "lsan_exit=$?"
} >"$REP/lsan.log" 2>&1
set -e
if grep -qE 'ERROR: LeakSanitizer|detected memory leaks' "$REP/lsan.log"; then
  echo "FAIL lsan (leaks)"; FAIL=1
elif grep -q 'lsan_exit=0' "$REP/lsan.log"; then
  echo "OK lsan"
else
  echo "FAIL lsan"; FAIL=1
fi

echo ""
echo "======== msan ========"
set +e
{
  rm -rf "$ROOT/build-msan"
  cmake -S "$ROOT" -B "$ROOT/build-msan" -DCMAKE_C_COMPILER="$CC" \
    -DHOST_TSAN=OFF -DHOST_ASAN=OFF -DHOST_UBSAN=OFF \
    -DHOST_FLAT_HEAP=OFF -DHOST_FLAT_HEAP_WRAP=OFF \
    -DCMAKE_C_FLAGS="-fsanitize=memory -fsanitize-memory-track-origins=2 -fno-omit-frame-pointer -fPIE" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=memory -pie"
  cmake --build "$ROOT/build-msan" -j"$(nproc)"
  export MSAN_OPTIONS="halt_on_error=0"
  HOST_NO_SDL=1 HOST_NO_FLAT_HEAP=1 "$ROOT/build-msan/handshake_host" \
    --seconds "$SECONDS_N" --log-level inf --no-flat-heap --nvs-path "$REP/msan_nvs.bin"
  echo "msan_exit=$?"
} >"$REP/msan.log" 2>&1
set -e
if grep -qE 'WARNING: MemorySanitizer|use-of-uninitialized' "$REP/msan.log"; then
  echo "FAIL msan (uninit)"; FAIL=1
elif grep -q 'msan_exit=0' "$REP/msan.log"; then
  echo "OK msan"
else
  echo "FAIL msan"; FAIL=1
fi

unset TSAN_OPTIONS ASAN_OPTIONS UBSAN_OPTIONS LSAN_OPTIONS MSAN_OPTIONS || true
run_cfg plain -DHOST_TSAN=OFF -DHOST_ASAN=OFF -DHOST_UBSAN=OFF -DHOST_FLAT_HEAP=ON

if command -v valgrind >/dev/null 2>&1; then
  echo ""
  echo "======== valgrind ========"
  set +e
  {
    HOST_NO_FLAT_HEAP=1 HOST_NO_SDL=1 valgrind --error-exitcode=99 --leak-check=full \
      --show-leak-kinds=definite --errors-for-leak-kinds=definite \
      --track-origins=yes --log-file="$REP/valgrind.txt" \
      "$ROOT/build-plain/handshake_host" --seconds "$VG_N" --log-level inf --no-flat-heap \
      --nvs-path "$REP/vg_nvs.bin"
    echo "vg_exit=$?"
  } >"$REP/valgrind.log" 2>&1
  set -e
  if grep -q 'vg_exit=0' "$REP/valgrind.log"; then echo "OK valgrind"; else echo "FAIL valgrind"; FAIL=1; fi

  echo ""
  echo "======== helgrind ========"
  set +e
  {
    HOST_NO_FLAT_HEAP=1 HOST_NO_SDL=1 valgrind --tool=helgrind --error-exitcode=99 \
      --log-file="$REP/helgrind.txt" \
      "$ROOT/build-plain/handshake_host" --seconds "$STRESS_N" --log-level wrn --no-flat-heap \
      --stress-recover --nvs-path "$REP/hg_nvs.bin"
    echo "hg_exit=$?"
  } >"$REP/helgrind.log" 2>&1
  set -e
  if grep -q 'hg_exit=0' "$REP/helgrind.log"; then echo "OK helgrind"; else echo "FAIL helgrind"; FAIL=1; fi
else
  echo "SKIP valgrind/helgrind"
fi

echo ""
echo "======== DIGEST ========"
{
  echo "## Digest $(date -Iseconds)"
  for f in "$REP"/*; do
    [[ -f "$f" ]] || continue
    echo
    echo "### $(basename "$f")"
    grep -nE 'WARNING:|ERROR:|SUMMARY:|data race|heap-use|use-of-uninitialized|UndefinedBehavior|LEAK SUMMARY|definitely lost|Possible data race|Conflict|ThreadSanitizer|AddressSanitizer|MemorySanitizer|IMU never|BLE script|helgrind' "$f" 2>/dev/null | head -60 || true
  done
} | tee "$REP/DIGEST.txt"

echo ""
echo "Digest: $REP/DIGEST.txt"
if [[ "$FAIL" -eq 0 ]]; then
  echo "All sanitizer runs clean."
  exit 0
fi
echo "Problems found."
exit 1
