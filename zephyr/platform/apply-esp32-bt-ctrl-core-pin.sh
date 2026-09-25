#!/usr/bin/env bash
# Give ESP32_BT_CTLR_PINNED_TO_CORE a prompt so prj.conf can pin the BLE
# controller off core 0 (WiFi task). Upstream HAL leaves it prompt-less
# (default 0 only).
set -euo pipefail

ZEPHYR_ROOT="${1:?usage: apply-esp32-bt-ctrl-core-pin.sh ZEPHYR_ROOT}"
# Argument is usually the Zephyr tree; HAL lives beside it under west.
HAL_KCONFIG=""
for cand in \
	"$ZEPHYR_ROOT/../modules/hal/espressif/zephyr/Kconfig" \
	"$ZEPHYR_ROOT/modules/hal/espressif/zephyr/Kconfig" \
	"${ZEPHYR_BASE:-}/../modules/hal/espressif/zephyr/Kconfig"; do
	if [[ -f "$cand" ]]; then
		HAL_KCONFIG="$(cd "$(dirname "$cand")" && pwd)/$(basename "$cand")"
		break
	fi
done

if [[ -z "$HAL_KCONFIG" || ! -f "$HAL_KCONFIG" ]]; then
	echo "ERROR: hal/espressif zephyr/Kconfig not found relative to $ZEPHYR_ROOT" >&2
	exit 1
fi

if grep -q 'BT controller CPU core (0 or 1)' "$HAL_KCONFIG"; then
	echo "ESP32 BT controller core pin: prompt already applied"
	exit 0
fi

python3 - "$HAL_KCONFIG" <<'PY'
from pathlib import Path
import sys

p = Path(sys.argv[1])
t = p.read_text()
old = """config ESP32_BT_CTLR_PINNED_TO_CORE
\tint
\tdefault 0
"""
new = """config ESP32_BT_CTLR_PINNED_TO_CORE
\tint "BT controller CPU core (0 or 1)"
\trange 0 1
\tdefault 0
\thelp
\t  Pin the ESP Bluetooth controller task. Prefer a different core than
\t  CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_* for WiFi+BLE coexistence.
"""
if old not in t:
    sys.stderr.write(f"ERROR: ESP32_BT_CTLR_PINNED_TO_CORE block not found in {p}\\n")
    sys.exit(1)
p.write_text(t.replace(old, new, 1))
print(f"Applied ESP32 BT controller core pin prompt -> {p}")
PY
