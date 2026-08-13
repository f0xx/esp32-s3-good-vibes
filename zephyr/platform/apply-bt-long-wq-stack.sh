#!/usr/bin/env bash
# Bump BT_LONG_WQ_STACK_SIZE default — upstream Zephyr issue #92224: default
# 1300 (BT_GATT_CACHING=y case) overflows on connect-time deferred HCI work,
# corrupting adjacent memory. Symbol is hidden (no prompt), so it can't be
# overridden from prj.conf; patch the Kconfig default directly.
#
# Bumped again 1300 -> 2048 -> 3072: this WQ runs deferred GATT/HCI work,
# which for this app can include our own config-service write callbacks
# (vibro reference start/stop, offload ACK). 2048 was still marginal enough
# to contribute to sporadic double-exception/WDT resets; the big stack
# buffers that used to run on this path were since moved to static storage
# (see vibro_capture.c / vibro_band_rms.c), but keep extra headroom here too.
set -euo pipefail

ZEPHYR_ROOT="${1:?usage: apply-bt-long-wq-stack.sh ZEPHYR_ROOT}"
KCONFIG="$ZEPHYR_ROOT/subsys/bluetooth/host/Kconfig"

if grep -q 'default 3072 if BT_GATT_CACHING' "$KCONFIG"; then
	echo "BT_LONG_WQ_STACK_SIZE patch already applied"
	exit 0
fi

python3 - "$KCONFIG" <<'PY'
import sys
from pathlib import Path

path = Path(sys.argv[1])
text = path.read_text()
old_needles = ("\tdefault 1300 if BT_GATT_CACHING\n", "\tdefault 2048 if BT_GATT_CACHING\n")
found = next((n for n in old_needles if n in text), None)
if found is None:
    raise SystemExit("ERROR: BT_LONG_WQ_STACK_SIZE default line not found")
text = text.replace(found, "\tdefault 3072 if BT_GATT_CACHING\n")
path.write_text(text)
print("Applied BT_LONG_WQ_STACK_SIZE patch (-> 3072)")
PY
