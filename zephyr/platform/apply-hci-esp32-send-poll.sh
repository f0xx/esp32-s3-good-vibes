#!/usr/bin/env bash
# Upstream hci_esp32.c TX waits on k_sem_take(..., K_MSEC(2000)). On ESP32-S3
# that path can run with arch_is_in_isr()!=0 (VHCI / btController nested), and
# Zephyr asserts:
#   ASSERTION FAIL [...] @ kernel/sem.c:136
#   Current thread: btController
# Poll esp_vhci_host_check_send_available() instead (same approach as our
# deferred-VHCI overlay) so TX never blocks on a semaphore.
set -euo pipefail

ZEPHYR_ROOT="${1:?usage: apply-hci-esp32-send-poll.sh ZEPHYR_ROOT}"
HCI="$ZEPHYR_ROOT/drivers/bluetooth/hci/hci_esp32.c"

if [[ ! -f "$HCI" ]]; then
	echo "ERROR: missing $HCI" >&2
	exit 1
fi

if grep -q 'HCI TX polls VHCI ready' "$HCI"; then
	echo "HCI ESP32 send-poll already applied"
	exit 0
fi

python3 - "$HCI" <<'PY'
from pathlib import Path
import sys

p = Path(sys.argv[1])
t = p.read_text()

old = """\tif (k_sem_take(&hci_send_sem, HCI_BT_ESP32_TIMEOUT) != 0) {
\t\tLOG_ERR("Send packet timeout error");
\t\terr = -ETIMEDOUT;
\t} else {
\t\tif (!esp_vhci_host_check_send_available()) {
\t\t\tLOG_WRN("VHCI not available, sending anyway");
\t\t}
\t\tesp_vhci_host_send_packet(buf->data, buf->len);
\t}
"""

new = """\t/* HCI TX polls VHCI ready — never k_sem_take with timeout (btController/ISR). */
\t{
\t\tint waited_ms = 0;
\t\tconst int timeout_ms = 2000;

\t\twhile (!esp_vhci_host_check_send_available()) {
\t\t\tif (k_is_in_isr()) {
\t\t\t\tLOG_ERR("Send packet not ready in ISR");
\t\t\t\terr = -EBUSY;
\t\t\t\tbreak;
\t\t\t}
\t\t\tk_msleep(1);
\t\t\tif (++waited_ms >= timeout_ms) {
\t\t\t\tLOG_ERR("Send packet timeout error");
\t\t\t\terr = -ETIMEDOUT;
\t\t\t\tbreak;
\t\t\t}
\t\t}
\t\tif (!err) {
\t\t\tesp_vhci_host_send_packet(buf->data, buf->len);
\t\t}
\t}
"""

if old not in t:
	sys.stderr.write("ERROR: hci_esp32.c send k_sem_take site not found\n")
	sys.exit(1)

t = t.replace(old, new, 1)

if '#include <zephyr/kernel.h>' not in t:
	t = t.replace(
		'#include <zephyr/drivers/bluetooth.h>\n',
		'#include <zephyr/drivers/bluetooth.h>\n#include <zephyr/kernel.h>\n',
		1,
	)

p.write_text(t)
print("HCI ESP32 TX now polls VHCI (no blocking k_sem_take)")
PY
