#!/usr/bin/env bash
# ESP32 BT+WiFi coexist can leave the controller deaf. Zephyr's default is a
# fatal BT_ASSERT on HCI command timeout (sysworkq oops). Return -ETIMEDOUT
# instead so MT200 scan / conn-param updates fail soft.
set -euo pipefail

ZEPHYR_ROOT="${1:?usage: apply-hci-cmd-timeout-soft.sh ZEPHYR_ROOT}"
HCI_CORE="$ZEPHYR_ROOT/subsys/bluetooth/host/hci_core.c"

if [[ ! -f "$HCI_CORE" ]]; then
	echo "ERROR: missing $HCI_CORE" >&2
	exit 1
fi

if grep -q 'HCI cmd timeout is non-fatal' "$HCI_CORE"; then
	echo "HCI cmd timeout soft-fail already applied"
	exit 0
fi

python3 - "$HCI_CORE" <<'PY'
from pathlib import Path
import sys

p = Path(sys.argv[1])
t = p.read_text()

old1 = """\t\t\t__maybe_unused bool success = process_pending_cmd(HCI_CMD_TIMEOUT);

			BT_ASSERT_MSG(success, "command opcode 0x%04x timeout", opcode);
"""
new1 = """\t\t\tbool success = process_pending_cmd(HCI_CMD_TIMEOUT);

			if (!success) {
				/* HCI cmd timeout is non-fatal on ESP32 BT+WiFi. */
				LOG_ERR("command opcode 0x%04x timeout", opcode);
				cmd(buf)->sync = NULL;
				net_buf_unref(buf);
				return -ETIMEDOUT;
			}
"""
if old1 not in t:
	sys.stderr.write("ERROR: process_pending_cmd assert site not found\n")
	sys.exit(1)
t = t.replace(old1, new1, 1)

old2 = """\terr = k_sem_take(&sync_sem, HCI_CMD_TIMEOUT);
	BT_ASSERT_MSG(err == 0,
		      "Controller unresponsive, command opcode 0x%04x timeout with err %d",
		      opcode, err);
"""
new2 = """\terr = k_sem_take(&sync_sem, HCI_CMD_TIMEOUT);
	if (err != 0) {
		/* HCI cmd timeout is non-fatal on ESP32 BT+WiFi. */
		LOG_ERR("Controller unresponsive, command opcode 0x%04x timeout with err %d",
			opcode, err);
		cmd(buf)->sync = NULL;
		net_buf_unref(buf);
		return -ETIMEDOUT;
	}
"""
if old2 not in t:
	sys.stderr.write("ERROR: k_sem_take HCI timeout assert site not found\n")
	sys.exit(1)
t = t.replace(old2, new2, 1)
p.write_text(t)
print("HCI cmd timeout is now a logged -ETIMEDOUT (not a kernel oops)")
PY
