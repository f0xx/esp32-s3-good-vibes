#!/usr/bin/env bash
# Zephyr 4.4 ESP32-S3: esp_psram_smh_init() → sys_heap_init() on the PSRAM
# SMH window hangs if called before z_prep_c, and still faults in main() when
# deferred to POST_KERNEL (coredump shows PC near _ext_ram_heap_start).
# Chip map + .ext_ram.{bss,noinit} still work via esp_init_psram(); skip SMH
# registration. SPIRAM runtime allocs fall back (heap_caps / k_malloc).
set -euo pipefail

ZEPHYR_ROOT="${1:?usage: apply-esp32-psram-smh-defer.sh ZEPHYR_ROOT}"
SOC="$ZEPHYR_ROOT/soc/espressif/esp32s3/soc.c"

if [[ ! -f "$SOC" ]]; then
	echo "ERROR: missing $SOC" >&2
	exit 1
fi

if grep -q 'SMH skipped: sys_heap_init on PSRAM' "$SOC"; then
	echo "ESP32 PSRAM SMH skip patch already applied"
	exit 0
fi

if ! grep -q 'esp_psram_smh_init' "$SOC"; then
	echo "ESP32 PSRAM SMH skip: no smh_init — skip"
	exit 0
fi

python3 - "$SOC" <<'PY'
from pathlib import Path
import sys

p = Path(sys.argv[1])
t = p.read_text()

# Undo a prior POST_KERNEL defer if present.
if "esp_psram_smh_init_deferred" in t:
    import re
    t = re.sub(
        r"\n#if CONFIG_ESP_SPIRAM\nstatic int esp_psram_smh_init_deferred\(void\).*?SYS_INIT\(esp_psram_smh_init_deferred, POST_KERNEL, 0\);\n#endif\n",
        "\n",
        t,
        count=1,
        flags=re.S,
    )
    t = t.replace("#include <zephyr/init.h>\n", "")

old = """#if CONFIG_ESP_SPIRAM
\tesp_init_psram();

\tint err = esp_psram_smh_init();

\tif (err) {
\t\tprintk("Failed to initialize PSRAM shared multi heap (%d)\\n", err);
\t}
#endif
"""

new = """#if CONFIG_ESP_SPIRAM
\tesp_init_psram();
\t/* SMH skipped: sys_heap_init on PSRAM hangs/faults on this S3 + Zephyr 4.4. */
#endif
"""

# Also match already-deferred form (init only, no smh call)
old2 = """#if CONFIG_ESP_SPIRAM
\tesp_init_psram();
\t/* SMH deferred: sys_heap_init on PSRAM before z_prep_c hangs on S3. */
#endif
"""

if old in t:
    t = t.replace(old, new, 1)
elif old2 in t:
    t = t.replace(old2, new, 1)
elif "SMH skipped: sys_heap_init on PSRAM" in t:
    print("ESP32 PSRAM SMH skip patch already applied")
    raise SystemExit(0)
else:
    sys.stderr.write("ERROR: esp_psram_smh_init / defer block not found — soc.c changed\\n")
    sys.exit(1)

p.write_text(t)
print("Applied ESP32 PSRAM SMH skip (keep esp_init_psram only)")
PY
