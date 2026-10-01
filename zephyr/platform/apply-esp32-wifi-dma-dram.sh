#!/usr/bin/env bash
# WiFi MAC DMA buffers must live in internal DRAM.
#
# Two holes on ESP32-S3 Zephyr:
# 1) heap_caps_* used to ignore MALLOC_CAP_DMA and route into PSRAM.
# 2) wifi adapter malloc_internal_wrapper → wifi_malloc → wrapped k_malloc,
#    which still sends ≥ ESP_HEAP_MIN_EXTRAM_THRESHOLD (1 KiB) to PSRAM.
#    Static RX buffers are ~1.6 KiB → DMA-deaf scan → 0 APs even when
#    heap_caps is patched (WiFi OSI never calls heap_caps for _malloc_internal).
#
# Arduino/IDF use heap_caps_malloc(DMA|INTERNAL) for that path.
set -euo pipefail

WEST_ROOT="${1:?usage: apply-esp32-wifi-dma-dram.sh WEST_WORKSPACE}"
HEAP="$WEST_ROOT/modules/hal/espressif/zephyr/port/heap/heap_caps_zephyr.c"

if [[ ! -f "$HEAP" ]]; then
	echo "ERROR: missing $HEAP" >&2
	exit 1
fi

# --- heap_caps: honour DMA/INTERNAL -----------------------------------------
if grep -q 'heap_caps_internal_dram' "$HEAP"; then
	echo "ESP32 WiFi DMA-in-DRAM heap patch already applied"
else
	python3 - "$HEAP" <<'PY'
from pathlib import Path
import sys

p = Path(sys.argv[1])
t = p.read_text()

helper = """
static void *heap_caps_internal_dram(size_t size)
{
	extern void *__real_k_malloc(size_t sz) __attribute__((weak));

	/* App: --wrap=k_malloc, __real_k_malloc is the DRAM sys_heap.
	 * McUboot: no wrap, weak symbol is NULL — k_malloc is already DRAM.
	 */
	if (__real_k_malloc != NULL) {
		return __real_k_malloc(size);
	}
	return k_malloc(size);
}

"""

if "heap_caps_internal_dram" not in t:
    marker = "static void *heap_caps_malloc_base("
    if marker not in t:
        sys.stderr.write("ERROR: heap_caps_malloc_base not found — HAL changed\\n")
        sys.exit(1)
    t = t.replace(marker, helper + marker, 1)

import re
malloc_pat = re.compile(
    r"static void \*heap_caps_malloc_base\( size_t size, uint32_t caps\)\n\{.*?\n\}",
    re.S,
)
malloc_new = """static void *heap_caps_malloc_base( size_t size, uint32_t caps)
{
    void *ptr;

    /* WiFi DMA buffers stay in DRAM — wrapped k_malloc() puts ≥1 KiB in PSRAM.
     * MALLOC_CAP_EXEC is optional in HAL 4.x (CONFIG_HEAP_HAS_EXEC_HEAP). */
    if (caps & (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL
#ifdef MALLOC_CAP_EXEC
		| MALLOC_CAP_EXEC
#endif
		)) {
        ptr = heap_caps_internal_dram(size);
    } else {
        ptr = k_malloc(size);
    }

    if (!ptr && size > 0) {
        heap_caps_alloc_failed(size, caps, __func__);
    }

    return ptr;
}"""
t2, n = malloc_pat.subn(malloc_new, t, count=1)
if n != 1:
    sys.stderr.write("ERROR: could not replace heap_caps_malloc_base\\n")
    sys.exit(1)
t = t2

idx = t.rfind("static void *heap_caps_calloc_base")
if idx < 0:
    sys.stderr.write("ERROR: heap_caps_calloc_base not found — HAL changed\\n")
    sys.exit(1)
rest = t[idx:]
old_ret = "    return k_calloc(n, size);\n}"
ret_at = rest.find(old_ret)
if ret_at < 0:
    sys.stderr.write("ERROR: heap_caps_calloc_base return site not found\\n")
    sys.exit(1)
rest = rest[:ret_at] + """    if (caps & (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL
#ifdef MALLOC_CAP_EXEC
		| MALLOC_CAP_EXEC
#endif
		)) {
        void *p = heap_caps_internal_dram(size_bytes);
        if (p != NULL) {
            (void)memset(p, 0, size_bytes);
        }
        return p;
    }

    return k_calloc(n, size);
}""" + rest[ret_at + len(old_ret):]
t = t[:idx] + rest

p.write_text(t)
print("Applied ESP32 WiFi DMA-in-DRAM heap patch")
PY
fi

# --- malloc_internal must not use wrapped k_malloc (PSRAM / DMA-deaf) -------
# Zephyr 3.7: zephyr/esp32s3/src/wifi/esp_wifi_adapter.c malloc_internal_wrapper
# Zephyr 4.x: components/esp_coex/esp32s3/esp_coex_adapter.c
#             esp_coex_common_malloc_internal_wrapper → k_malloc
ADAPTER_V37="$WEST_ROOT/modules/hal/espressif/zephyr/esp32s3/src/wifi/esp_wifi_adapter.c"
ADAPTER_V44="$WEST_ROOT/modules/hal/espressif/components/esp_coex/esp32s3/esp_coex_adapter.c"
if [[ -f "$ADAPTER_V44" ]]; then
	ADAPTER="$ADAPTER_V44"
elif [[ -f "$ADAPTER_V37" ]]; then
	ADAPTER="$ADAPTER_V37"
else
	echo "ERROR: missing WiFi/coex malloc_internal adapter under $WEST_ROOT/modules/hal/espressif" >&2
	exit 1
fi

adapter_malloc_internal_uses_dram() {
	awk '
		/malloc_internal_wrapper|esp_coex_common_malloc_internal_wrapper/ { in_fn = 1 }
		in_fn && /__real_k_malloc/ { found = 1 }
		in_fn && /^}/ {
			if (found) { ok = 1; exit }
			in_fn = 0
			found = 0
		}
		END { exit !ok }
	' "$ADAPTER"
}

if adapter_malloc_internal_uses_dram; then
	echo "ESP32 WiFi malloc_internal→DRAM adapter patch already applied ($ADAPTER)"
	exit 0
fi

python3 - "$ADAPTER" <<'PY'
from pathlib import Path
import re
import sys

p = Path(sys.argv[1])
t = p.read_text()

new_coex = """void * IRAM_ATTR esp_coex_common_malloc_internal_wrapper(size_t size)
{
	/* Must be DRAM: wrapped k_malloc puts >=1KiB in PSRAM and the MAC cannot
	 * DMA from SPIRAM -> SCAN_DONE with 0 APs. */
	extern void *__real_k_malloc(size_t sz) __attribute__((weak));

	if (__real_k_malloc != NULL) {
		return __real_k_malloc(size);
	}
	return k_malloc(size);
}"""

coex_pat = re.compile(
    r"void \* IRAM_ATTR esp_coex_common_malloc_internal_wrapper\(size_t size\)\n\{\n\s*return k_malloc\(size\);\n\}"
)
t2, n = coex_pat.subn(new_coex, t, count=1)
if n == 1:
    p.write_text(t2)
    print("Applied ESP32 WiFi malloc_internal->DRAM (esp_coex_common_malloc_internal_wrapper)")
    raise SystemExit(0)

old_m = """static void *IRAM_ATTR malloc_internal_wrapper(size_t size)
{
	return wifi_malloc(size);
}"""
new_m = """static void *IRAM_ATTR malloc_internal_wrapper(size_t size)
{
	/* Must be DRAM: wifi_malloc->wrapped k_malloc puts >=1KiB in PSRAM and
	 * the MAC cannot DMA from SPIRAM -> SCAN_DONE with 0 APs. */
	extern void *__real_k_malloc(size_t sz) __attribute__((weak));

	if (__real_k_malloc != NULL) {
		return __real_k_malloc(size);
	}
	return k_malloc(size);
}"""

old_c = """static void *IRAM_ATTR calloc_internal_wrapper(size_t n, size_t size)
{
	return wifi_calloc(n, size);
}"""
new_c = """static void *IRAM_ATTR calloc_internal_wrapper(size_t n, size_t size)
{
	extern void *__real_k_malloc(size_t sz) __attribute__((weak));
	size_t bytes;
	void *p;

	if (__builtin_mul_overflow(n, size, &bytes)) {
		return NULL;
	}
	p = (__real_k_malloc != NULL) ? __real_k_malloc(bytes) : k_malloc(bytes);
	if (p != NULL) {
		(void)memset(p, 0, bytes);
	}
	return p;
}"""

old_z = """static void *IRAM_ATTR zalloc_internal_wrapper(size_t size)
{
	return wifi_calloc(1, size);
}"""
new_z = """static void *IRAM_ATTR zalloc_internal_wrapper(size_t size)
{
	return calloc_internal_wrapper(1, size);
}"""

if old_m not in t:
    sys.stderr.write("ERROR: malloc_internal site not found — adapter changed\n")
    sys.exit(1)
if old_c not in t:
    sys.stderr.write("ERROR: calloc_internal_wrapper site not found — adapter changed\n")
    sys.exit(1)
if old_z not in t:
    sys.stderr.write("ERROR: zalloc_internal_wrapper site not found — adapter changed\n")
    sys.exit(1)

t = t.replace(old_m, new_m, 1).replace(old_c, new_c, 1).replace(old_z, new_z, 1)
if "#include <string.h>" not in t:
    t = t.replace("#include <zephyr/kernel.h>", "#include <zephyr/kernel.h>\n#include <string.h>", 1)

p.write_text(t)
print("Applied ESP32 WiFi malloc_internal->DRAM adapter patch (3.7 path)")
PY
if ! adapter_malloc_internal_uses_dram; then
	echo "ERROR: malloc_internal still not using __real_k_malloc ($ADAPTER)" >&2
	exit 1
fi
echo "Verified: WiFi malloc_internal uses DRAM (__real_k_malloc)"
