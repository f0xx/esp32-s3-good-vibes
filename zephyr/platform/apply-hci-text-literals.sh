#!/usr/bin/env bash
# Xtensa IRAM_ATTR functions in hci_esp32.c must keep l32r literal pools in
# the same section. Without -mtext-section-literals those constants stay in
# flash, so a cache-off / MSPI-busy fetch after k_msleep still explodes as
# EXCCAUSE 0 (illegal instruction) — just on the l32r instead of the memw.
set -euo pipefail

ZEPHYR_ROOT="${1:?usage: apply-hci-text-literals.sh ZEPHYR_ROOT}"
CMAKE="$ZEPHYR_ROOT/drivers/bluetooth/hci/CMakeLists.txt"

if grep -q 'mtext-section-literals' "$CMAKE"; then
	echo "hci_esp32.c -mtext-section-literals already applied"
	exit 0
fi

python3 - "$CMAKE" <<'PY'
import sys
from pathlib import Path

path = Path(sys.argv[1])
text = path.read_text()
needle = "zephyr_library_sources_ifdef(CONFIG_BT_ESP32       hci_esp32.c)\n"
if needle not in text:
    raise SystemExit("ERROR: hci_esp32.c source line not found in CMakeLists.txt")
insert = needle + (
    "if(CONFIG_BT_ESP32)\n"
    "  set_source_files_properties(hci_esp32.c PROPERTIES COMPILE_OPTIONS "
    "\"-mtext-section-literals\")\n"
    "endif()\n"
)
path.write_text(text.replace(needle, insert, 1))
print("Applied hci_esp32.c -mtext-section-literals")
PY
