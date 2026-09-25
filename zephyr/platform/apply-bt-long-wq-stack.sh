#!/usr/bin/env bash
# Force BT_LONG_WQ_STACK_SIZE — upstream Zephyr issue #92224 + local desk data:
# default chain is NO_OPTIMIZATIONS→4096, BT_ECC→1400, BT_GATT_CACHING→1300.
# With BT_ECC=y the 1400 default wins and thread_analyzer shows 98% use
# (unused 28 / 1408) overnight — overflow there smashes adjacent stacks
# (render Invalid SP / LoadProhibited@0). Symbol is prompt-less so prj.conf
# cannot override; patch Kconfig defaults directly.
set -euo pipefail

ZEPHYR_ROOT="${1:?usage: apply-bt-long-wq-stack.sh ZEPHYR_ROOT}"
KCONFIG="$ZEPHYR_ROOT/subsys/bluetooth/host/Kconfig"
WANT=3072

if grep -qE "default ${WANT} if BT_ECC" "$KCONFIG" && \
   grep -qE "default ${WANT} if BT_GATT_CACHING" "$KCONFIG"; then
	echo "BT_LONG_WQ_STACK_SIZE patch already applied (-> ${WANT})"
	exit 0
fi

python3 - "$KCONFIG" "$WANT" <<'PY'
import re
import sys
from pathlib import Path

path = Path(sys.argv[1])
want = sys.argv[2]
text = path.read_text()
# Replace the whole default block inside BT_LONG_WQ_STACK_SIZE.
pat = re.compile(
    r"(config BT_LONG_WQ_STACK_SIZE\n"
    r"\tint \"Long workqueue stack size\.\"\n)"
    r"(?:\tdefault [^\n]+\n)+",
    re.M,
)
repl = (
    rf"\1"
    rf"\tdefault {want} if NO_OPTIMIZATIONS\n"
    rf"\tdefault {want} if BT_ECC\n"
    rf"\tdefault {want} if BT_GATT_CACHING\n"
    rf"\tdefault {want}\n"
)
new, n = pat.subn(repl, text, count=1)
if n != 1:
    raise SystemExit("ERROR: BT_LONG_WQ_STACK_SIZE default block not found")
path.write_text(new)
print(f"Applied BT_LONG_WQ_STACK_SIZE patch (-> {want}, including BT_ECC)")
PY
