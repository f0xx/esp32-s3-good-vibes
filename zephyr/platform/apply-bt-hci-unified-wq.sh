#!/usr/bin/env bash
# Move Zephyr HCI TX + conn tx-complete work off sysworkq onto the BT RX workqueue.
set -euo pipefail

ZEPHYR_ROOT="${1:?usage: apply-bt-hci-unified-wq.sh ZEPHYR_ROOT}"

HCI_CORE_C="$ZEPHYR_ROOT/subsys/bluetooth/host/hci_core.c"
HCI_CORE_H="$ZEPHYR_ROOT/subsys/bluetooth/host/hci_core.h"
CONN_C="$ZEPHYR_ROOT/subsys/bluetooth/host/conn.c"
L2CAP_C="$ZEPHYR_ROOT/subsys/bluetooth/host/l2cap.c"

marker='bt_hci_wq_submit(struct k_work *work)'
if grep -q "$marker" "$HCI_CORE_H" && \
   grep -q 'bt_hci_wq_submit(&conn->tx_complete_work)' "$HCI_CORE_C" && \
   grep -q 'bt_hci_wq_submit(&chan->rx_work)' "$L2CAP_C"; then
	echo "BT HCI unified workqueue patch already applied"
	exit 0
fi

python3 - "$HCI_CORE_H" "$HCI_CORE_C" "$CONN_C" "$L2CAP_C" <<'PY'
import sys
from pathlib import Path

hci_h, hci_c, conn_c, l2cap_c = map(Path, sys.argv[1:5])

h = hci_h.read_text()
if "bt_hci_wq_submit" not in h:
    h = h.replace(
        "void bt_tx_irq_raise(void);\n",
        "void bt_tx_irq_raise(void);\n"
        "int bt_hci_wq_submit(struct k_work *work);\n"
        "k_tid_t bt_hci_wq_thread_get(void);\n",
    )
    hci_h.write_text(h)

c = hci_c.read_text()
if "bt_hci_wq_submit" not in c:
    insert = '''
#if defined(CONFIG_BT_RECV_WORKQ_BT)
int bt_hci_wq_submit(struct k_work *work)
{
\treturn k_work_submit_to_queue(&bt_workq, work);
}

k_tid_t bt_hci_wq_thread_get(void)
{
\treturn &bt_workq.thread;
}
#else
int bt_hci_wq_submit(struct k_work *work)
{
\treturn k_work_submit(work);
}

k_tid_t bt_hci_wq_thread_get(void)
{
\treturn &k_sys_work_q.thread;
}
#endif

'''
    c = c.replace(
        "#endif /* CONFIG_BT_RECV_WORKQ_BT */\n\nstatic void init_work",
        "#endif /* CONFIG_BT_RECV_WORKQ_BT */\n" + insert + "static void init_work",
    )
    c = c.replace(
        "if (k_current_get() == &k_sys_work_q.thread) {",
        "if (k_current_get() == bt_hci_wq_thread_get()) {",
        1,
    )
    c = c.replace(
        "k_work_submit(&conn->tx_complete_work);",
        "(void)bt_hci_wq_submit(&conn->tx_complete_work);",
    )
    c = c.replace(
        "\t/* We now TX everything from the syswq */\n\treturn &k_sys_work_q.thread;",
        "\treturn bt_hci_wq_thread_get();",
    )
    c = c.replace(
        "\tk_work_submit(&tx_work);",
        "\t(void)bt_hci_wq_submit(&tx_work);",
    )
    hci_c.write_text(c)

n = conn_c.read_text()
if "bt_hci_wq_thread_get" not in n:
    n = n.replace(
        "\t__ASSERT_NO_MSG(k_current_get() ==\n"
        "\t\t\tk_work_queue_thread_get(&k_sys_work_q));",
        "\t__ASSERT_NO_MSG(k_current_get() == bt_hci_wq_thread_get());",
    )
    n = n.replace(
        "\tif (IS_ENABLED(CONFIG_BT_RECV_WORKQ_SYS) ||\n"
        "\t    k_current_get() == k_work_queue_thread_get(&k_sys_work_q)) {",
        "\tif (k_current_get() == bt_hci_wq_thread_get()) {",
    )
    n = n.replace(
        "\t\terr = k_work_submit(&conn->tx_complete_work);",
        "\t\terr = bt_hci_wq_submit(&conn->tx_complete_work);",
    )
    n = n.replace(
        "\t    k_current_get() == k_work_queue_thread_get(&k_sys_work_q)) {",
        "\t    k_current_get() == bt_hci_wq_thread_get()) {",
    )
    conn_c.write_text(n)

l = l2cap_c.read_text()
if "bt_hci_wq_submit(&chan->rx_work)" not in l:
    l = l.replace(
        "\tk_work_submit(&chan->rx_work);",
        "\t(void)bt_hci_wq_submit(&chan->rx_work);",
    )
    l2cap_c.write_text(l)

print("Applied BT HCI unified workqueue patch")
PY
