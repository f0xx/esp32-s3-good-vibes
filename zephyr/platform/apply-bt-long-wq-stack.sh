#!/usr/bin/env bash
# Bump BT_LONG_WQ_STACK_SIZE default — upstream Zephyr issue #92224: default
# 1300 (BT_GATT_CACHING=y case) overflows on connect-time deferred HCI work,
# corrupting adjacent memory. Symbol is hidden (no prompt), so it can't be
# overridden from prj.conf; patch the Kconfig default directly.
set -euo pipefail

ZEPHYR_ROOT="${1:?usage: apply-bt-long-wq-stack.sh ZEPHYR_ROOT}"
KCONFIG="$ZEPHYR_ROOT/subsys/bluetooth/host/Kconfig"

if grep -q 'default 2048 if BT_GATT_CACHING' "$KCONFIG"; then
	echo "BT_LONG_WQ_STACK_SIZE patch already applied"
	exit 0
fi

python3 - "$KCONFIG" <<'PY'
import sys
from pathlib import Path

path = Path(sys.argv[1])
text = path.read_text()
needle = "\tdefault 1300 if BT_GATT_CACHING\n"
if needle not in text:
    raise SystemExit("ERROR: BT_LONG_WQ_STACK_SIZE default line not found")
text = text.replace(needle, "\tdefault 2048 if BT_GATT_CACHING\n")
path.write_text(text)
print("Applied BT_LONG_WQ_STACK_SIZE patch (1300 -> 2048)")
PY
