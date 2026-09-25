#!/usr/bin/env bash
# Defer esp_wifi_init/start until scan/connect. Zephyr net_if_post_init otherwise
# auto-ups the iface and the ESP32 driver starts the WiFi MAC at boot, which
# shares the tiny sys_heap with btController and blows chunk metadata on BLE
# connect (heap.c:183, thread btController).
set -euo pipefail

ZEPHYR_ROOT="${1:?usage: apply-esp32-wifi-defer-start.sh ZEPHYR_ROOT}"
DRV="$ZEPHYR_ROOT/drivers/wifi/esp32/src/esp_wifi_drv.c"

if [[ ! -f "$DRV" ]]; then
	echo "ERROR: missing $DRV" >&2
	exit 1
fi

# Zephyr 4.4+ already inits WiFi in esp32_wifi_dev_init (mode NULL) and only
# calls esp_wifi_start() from scan/connect — the old 3.7 iface-init start
# race is gone. Keep a marker so desk/CI verify still passes.
if grep -q 'esp32_wifi_dev_init' "$DRV" && \
   grep -q 'esp_wifi_init(&config)' "$DRV" && \
   ! grep -q 'esp32_wifi_radio_ensure' "$DRV"; then
	set +e
	python3 - "$DRV" <<'PY4'
from pathlib import Path
import re
import sys

p = Path(sys.argv[1])
t = p.read_text()
iface = re.search(
    r"static void esp32_wifi_init\(struct net_if \*iface\)\s*\{(.*?)\n\}",
    t,
    re.S,
)
if iface and "esp_wifi_start" in iface.group(1):
    raise SystemExit(2)  # still need full 3.7 patch
marker = "/* esp32_wifi_radio_ensure: not required on Zephyr 4.4+ (start deferred in-tree) */\n"
if marker not in t:
    t = t.replace(
        "static struct esp32_wifi_runtime esp32_data;\n",
        "static struct esp32_wifi_runtime esp32_data;\n" + marker,
        1,
    )
    p.write_text(t)
print("ESP32 WiFi defer-start: skipped (Zephyr 4.4+ in-tree defer)")
PY4
	_rc=$?
	set -e
	if [[ "$_rc" -eq 0 ]]; then
		exit 0
	elif [[ "$_rc" -ne 2 ]]; then
		exit "$_rc"
	fi
fi

if grep -q 'esp_wifi_set_ps(WIFI_PS_NONE)' "$DRV" && grep -q 'scan_time.active.min = 300' "$DRV"; then
	echo "ESP32 WiFi defer-start patch already applied"
else

if grep -q 'esp32_wifi_radio_ensure' "$DRV"; then
	echo "Upgrading existing ESP32 WiFi defer-start patch (PS_NONE + dwell)"
	python3 - "$DRV" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
t = p.read_text()
if "esp_wifi_set_ps(WIFI_PS_NONE)" not in t:
    old = "\tstarted = true;\n\t/* esp_wifi_start() is async"
    new = "\tstarted = true;\n\t(void)esp_wifi_set_ps(WIFI_PS_NONE);\n\t/* esp_wifi_start() is async"
    if old not in t:
        sys.stderr.write("ERROR: cannot insert WIFI_PS_NONE — driver changed\n")
        sys.exit(1)
    t = t.replace(old, new, 1)
t = t.replace("scan_config.scan_time.active.min = 120;", "scan_config.scan_time.active.min = 400;")
t = t.replace("scan_config.scan_time.active.max = 300;", "scan_config.scan_time.active.max = 700;")
t = t.replace("scan_config.scan_time.active.min = 200;", "scan_config.scan_time.active.min = 400;")
t = t.replace("scan_config.scan_time.active.max = 400;", "scan_config.scan_time.active.max = 700;")
t = t.replace("scan_config.scan_time.active.min = 300;", "scan_config.scan_time.active.min = 400;")
t = t.replace("scan_config.scan_time.active.max = 500;", "scan_config.scan_time.active.max = 700;")
p.write_text(t)
print("Upgraded ESP32 WiFi defer-start patch")
PY
else

python3 - "$DRV" <<'PY'
from pathlib import Path
import sys

p = Path(sys.argv[1])
t = p.read_text()
fwd = "static struct esp32_wifi_runtime esp32_data;\n\nstatic int esp32_wifi_radio_ensure(void);\n"
if "static struct esp32_wifi_runtime esp32_data;\n" not in t:
	sys.stderr.write("ERROR: esp32_data marker not found — driver changed\n")
	sys.exit(1)
t = t.replace("static struct esp32_wifi_runtime esp32_data;\n", fwd, 1)
old_init_tail = """\tethernet_init(iface);
	net_if_carrier_off(iface);

	wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();

	esp_err_t ret = esp_wifi_init(&config);

	esp_wifi_internal_reg_rxcb(ESP_IF_WIFI_STA, eth_esp32_rx);

	ret |= esp_wifi_set_mode(ESP32_WIFI_MODE_STA);
	ret |= esp_wifi_start();

	if (ret != ESP_OK) {
		LOG_ERR("Failed to start Wi-Fi driver");
	}
}
"""
new_init_tail = """\tethernet_init(iface);
	net_if_carrier_off(iface);
	net_if_flag_set(iface, NET_IF_NO_AUTO_START);
	LOG_INF("WiFi iface ready (radio deferred until scan/connect)");
}

static int esp32_wifi_radio_ensure(void)
{
	static bool started;
	esp_err_t ret;

	if (started) {
		for (int i = 0; i < 25 && esp32_data.state == ESP32_STA_STOPPED; i++) {
			k_sleep(K_MSEC(20));
		}
		return esp32_data.state == ESP32_STA_STOPPED ? -EAGAIN : 0;
	}

	wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();

	ret = esp_wifi_init(&config);
	if (ret != ESP_OK) {
		LOG_ERR("esp_wifi_init failed (%d)", ret);
		return -EIO;
	}

	esp_wifi_internal_reg_rxcb(ESP_IF_WIFI_STA, eth_esp32_rx);
	ret = esp_wifi_set_mode(ESP32_WIFI_MODE_STA);
	ret |= esp_wifi_start();
	if (ret != ESP_OK) {
		LOG_ERR("Failed to start Wi-Fi driver");
		return -EIO;
	}

	started = true;
	(void)esp_wifi_set_ps(WIFI_PS_NONE);
	/* esp_wifi_start() is async — scan before WIFI_EVENT_STA_START returns
	 * ESP_ERR_WIFI_NOT_STARTED and leaves scan_cb armed (next scans EINPROGRESS). */
	for (int i = 0; i < 50 && esp32_data.state == ESP32_STA_STOPPED; i++) {
		k_sleep(K_MSEC(20));
	}
	if (esp32_data.state == ESP32_STA_STOPPED) {
		LOG_ERR("WiFi STA start timeout");
		return -ETIMEDOUT;
	}
	LOG_INF("WiFi radio started (on demand, state=%u)", esp32_data.state);
	return 0;
}
"""
if old_init_tail not in t:
	sys.stderr.write("ERROR: esp32_wifi_init tail not found — driver changed\n")
	sys.exit(1)
t = t.replace(old_init_tail, new_init_tail, 1)

old_connect = """\tif (data->state == ESP32_STA_CONNECTING || data->state == ESP32_STA_CONNECTED) {
		wifi_mgmt_raise_connect_result_event(esp32_wifi_iface, -1);
		return -EALREADY;
	}

	ret = esp_wifi_get_mode(&mode);
"""
new_connect = """\tif (data->state == ESP32_STA_CONNECTING || data->state == ESP32_STA_CONNECTED) {
		wifi_mgmt_raise_connect_result_event(esp32_wifi_iface, -1);
		return -EALREADY;
	}

	if (esp32_wifi_radio_ensure() != 0) {
		return -EIO;
	}

	ret = esp_wifi_get_mode(&mode);
"""
if old_connect not in t:
	sys.stderr.write("ERROR: esp32_wifi_connect site not found — driver changed\n")
	sys.exit(1)
t = t.replace(old_connect, new_connect, 1)

old_scan = """\tdata->scan_cb = cb;

	wifi_scan_config_t scan_config = { 0 };
"""
new_scan = """\tdata->scan_cb = cb;

	if (esp32_wifi_radio_ensure() != 0) {
		data->scan_cb = NULL;
		return -EIO;
	}

	wifi_scan_config_t scan_config = { 0 };

	scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
	scan_config.show_hidden = true;
	scan_config.scan_time.active.min = 400;
	scan_config.scan_time.active.max = 700;
	scan_config.scan_time.passive = 400;
"""
if old_scan not in t:
	sys.stderr.write("ERROR: esp32_wifi_scan site not found — driver changed\n")
	sys.exit(1)
t = t.replace(old_scan, new_scan, 1)

old_scan_fail = """\tif (ret != ESP_OK) {
		LOG_ERR("Failed to start Wi-Fi scanning");
		return -EAGAIN;
	}
"""
new_scan_fail = """\tif (ret != ESP_OK) {
		LOG_ERR("Failed to start Wi-Fi scanning");
		data->scan_cb = NULL;
		return -EAGAIN;
	}
"""
if old_scan_fail not in t:
	sys.stderr.write("ERROR: scan failure site not found — driver changed\\n")
	sys.exit(1)
t = t.replace(old_scan_fail, new_scan_fail, 1)

if "#include <zephyr/kernel.h>" not in t:
	t = t.replace("#include <zephyr/device.h>",
		      "#include <zephyr/device.h>\n#include <zephyr/kernel.h>", 1)

p.write_text(t)
print("Applied ESP32 WiFi defer-start patch")
PY
fi
fi

# After a scan the WiFi MAC stays up and starves BLE HCI (opcode 0x2005
# LE_SET_RANDOM_ADDRESS timeout → sysworkq ASSERT). Stop the MAC when idle
# and allow radio_ensure() to restart it on the next scan/connect.
python3 - "$DRV" <<'PY'
from pathlib import Path
import sys

p = Path(sys.argv[1])
t = p.read_text()
if "esp32_wifi_radio_release" in t and "WiFi radio restarted" in t:
	print("ESP32 WiFi radio_release already applied")
	sys.exit(0)

old_started = """\tif (started) {
		for (int i = 0; i < 25 && esp32_data.state == ESP32_STA_STOPPED; i++) {
			k_sleep(K_MSEC(20));
		}
		return esp32_data.state == ESP32_STA_STOPPED ? -EAGAIN : 0;
	}
"""
new_started = """\tif (started) {
		esp_err_t r;

		if (esp32_data.state != ESP32_STA_STOPPED) {
			return 0;
		}
		r = esp_wifi_start();
		if (r != ESP_OK) {
			LOG_ERR("esp_wifi_start failed (%d)", r);
			return -EIO;
		}
		for (int i = 0; i < 50 && esp32_data.state == ESP32_STA_STOPPED; i++) {
			k_sleep(K_MSEC(20));
		}
		if (esp32_data.state == ESP32_STA_STOPPED) {
			LOG_ERR("WiFi STA restart timeout");
			return -ETIMEDOUT;
		}
		LOG_INF("WiFi radio restarted (on demand, state=%u)", esp32_data.state);
		return 0;
	}
"""
if old_started not in t:
	sys.stderr.write("ERROR: radio_ensure started-block not found — driver changed\n")
	sys.exit(1)
t = t.replace(old_started, new_started, 1)

marker = '\tLOG_INF("WiFi radio started (on demand, state=%u)", esp32_data.state);\n\treturn 0;\n}\n'
release = '''\tLOG_INF("WiFi radio started (on demand, state=%u)", esp32_data.state);
	return 0;
}

int esp32_wifi_radio_release(void)
{
	if (esp32_data.state == ESP32_STA_CONNECTED ||
	    esp32_data.state == ESP32_STA_CONNECTING ||
	    esp32_data.state == ESP32_AP_CONNECTED) {
		return 0;
	}
	if (esp32_data.state == ESP32_STA_STOPPED) {
		return 0;
	}
	(void)esp_wifi_stop();
	LOG_INF("WiFi radio stopped (BLE coexist)");
	return 0;
}
'''
if marker not in t:
	sys.stderr.write("ERROR: radio_ensure tail not found — driver changed\n")
	sys.exit(1)
t = t.replace(marker, release, 1)
p.write_text(t)
print("Added esp32_wifi_radio_release + STA restart")
PY

# World-safe 2.4 GHz country so scan is not stuck on a 1–11 CN/US subset.
python3 - "$DRV" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
t = p.read_text()
if "esp_wifi_set_country" in t:
	print("ESP32 WiFi country already applied")
	sys.exit(0)
old = "\t(void)esp_wifi_set_ps(WIFI_PS_NONE);\n"
new = """\t(void)esp_wifi_set_ps(WIFI_PS_NONE);
	{
		wifi_country_t country = {
			.cc = {'0', '1', '\\0'},
			.schan = 1,
			.nchan = 13,
			.max_tx_power = 20,
			.policy = WIFI_COUNTRY_POLICY_MANUAL,
		};
		(void)esp_wifi_set_country(&country);
	}
"""
if old not in t:
	sys.stderr.write("ERROR: WIFI_PS_NONE site not found for country insert\n")
	sys.exit(1)
t = t.replace(old, new, 1)
p.write_text(t)
print("Added WiFi country 01 ch 1-13")
PY

# Longer dwell, honor Zephyr dwell/scan_type, 20 dBm TX, RF settle after STA up.
# Nearby 2.4 GHz APs were returning 0 because scan ran before RF cal and
# active probes lost the antenna to BLE — passive dwell + settle fixes that.
python3 - "$DRV" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
t = p.read_text()
if "esp_wifi_set_max_tx_power(80)" in t and "WiFi scan start type=" in t:
	print("ESP32 WiFi scan RF tune already applied")
	sys.exit(0)

t = t.replace("scan_config.scan_time.active.min = 300;", "scan_config.scan_time.active.min = 400;")
t = t.replace("scan_config.scan_time.active.max = 500;",
	"scan_config.scan_time.active.max = 700;\n\tscan_config.scan_time.passive = 400;")
if "scan_config.scan_time.passive" not in t:
	t = t.replace("scan_config.scan_time.active.max = 700;",
		"scan_config.scan_time.active.max = 700;\n\tscan_config.scan_time.passive = 400;")

old_params = """\tif (params) {
		/* The enum values are same, so, no conversion needed */
		scan_config.scan_type = params->scan_type;
	}
"""
new_params = """\tif (params) {
		/* The enum values are same, so, no conversion needed */
		scan_config.scan_type = params->scan_type;
		if (params->dwell_time_active > 0) {
			scan_config.scan_time.active.min = params->dwell_time_active;
			scan_config.scan_time.active.max = params->dwell_time_active;
		}
		if (params->dwell_time_passive > 0) {
			scan_config.scan_time.passive = params->dwell_time_passive;
		}
	}
	LOG_INF("WiFi scan start type=%u min=%u max=%u passive=%u",
		(unsigned)scan_config.scan_type,
		scan_config.scan_time.active.min,
		scan_config.scan_time.active.max,
		scan_config.scan_time.passive);
"""
if old_params not in t:
	sys.stderr.write("ERROR: scan params site not found — driver changed\n")
	sys.exit(1)
t = t.replace(old_params, new_params, 1)

old_started = """\tLOG_INF("WiFi radio started (on demand, state=%u)", esp32_data.state);
	return 0;
"""
new_started = """\tLOG_INF("WiFi radio started (on demand, state=%u)", esp32_data.state);
	(void)esp_wifi_set_ps(WIFI_PS_NONE);
	(void)esp_wifi_set_max_tx_power(80);
	k_sleep(K_MSEC(600));
	LOG_INF("WiFi RF settle done (tx=80)");
	return 0;
"""
if old_started not in t:
	sys.stderr.write("ERROR: radio started site not found for RF settle\n")
	sys.exit(1)
t = t.replace(old_started, new_started, 1)

old_restart = """\t\tLOG_INF("WiFi radio restarted (on demand, state=%u)", esp32_data.state);
		return 0;
"""
new_restart = """\t\tLOG_INF("WiFi radio restarted (on demand, state=%u)", esp32_data.state);
		(void)esp_wifi_set_ps(WIFI_PS_NONE);
		(void)esp_wifi_set_max_tx_power(80);
		k_sleep(K_MSEC(600));
		LOG_INF("WiFi RF settle done (tx=80 restart)");
		return 0;
"""
if old_restart not in t:
	sys.stderr.write("ERROR: radio restarted site not found for RF settle\n")
	sys.exit(1)
t = t.replace(old_restart, new_restart, 1)

p.write_text(t)
print("Added WiFi RF settle, 20 dBm TX, dwell/passive scan logging")
PY

# Arduino/Adafruit WiFi.scanNetworks uses IDF default scan config (NULL) and
# reports SCAN_DONE number. Custom dwell + MANUAL country 01 was completing
# with 0 APs in an area where Arduino found several 2.4 GHz networks.
python3 - "$DRV" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
t = p.read_text()
if "WiFi scan start (IDF default config)" in t and "WiFi SCAN_DONE status=" in t:
	print("ESP32 WiFi Arduino-like scan already applied")
	sys.exit(0)

old_start = """\tret = esp_wifi_set_mode(ESP32_WIFI_MODE_STA);
	ret |= esp_wifi_scan_start(&scan_config, false);
"""
new_start = """\tret = esp_wifi_set_mode(ESP32_WIFI_MODE_STA);
	if (params && params->scan_type == WIFI_SCAN_TYPE_PASSIVE) {
		LOG_INF("WiFi scan start (passive)");
		ret |= esp_wifi_scan_start(&scan_config, false);
	} else {
		/* NULL = IDF defaults, same as Arduino WiFi.scanNetworks(). */
		LOG_INF("WiFi scan start (IDF default config)");
		ret |= esp_wifi_scan_start(NULL, false);
	}
"""
if old_start not in t:
	sys.stderr.write("ERROR: esp_wifi_scan_start site not found\\n")
	sys.exit(1)
t = t.replace(old_start, new_start, 1)

old_done = """static void scan_done_handler(void)
{
	uint16_t aps = 0;
	wifi_ap_record_t *ap_list_buffer;
	struct wifi_scan_result res = { 0 };

	esp_wifi_scan_get_ap_num(&aps);
	if (!aps) {
		LOG_INF("No Wi-Fi AP found");
		goto out;
	}
"""
new_done = """static void scan_done_handler(void *event_data)
{
	uint16_t aps = 0;
	wifi_ap_record_t *ap_list_buffer;
	struct wifi_scan_result res = { 0 };
	wifi_event_sta_scan_done_t *done = event_data;

	if (done != NULL) {
		LOG_INF("WiFi SCAN_DONE status=%u number=%u scan_id=%u",
			(unsigned)done->status, (unsigned)done->number,
			(unsigned)done->scan_id);
	}
	esp_wifi_scan_get_ap_num(&aps);
	LOG_INF("esp_wifi_scan_get_ap_num=%u", aps);
	if (!aps) {
		LOG_INF("No Wi-Fi AP found");
		goto out;
	}
"""
if old_done not in t:
	sys.stderr.write("ERROR: scan_done_handler site not found\\n")
	sys.exit(1)
t = t.replace(old_done, new_done, 1)

old_call = """\tcase WIFI_EVENT_SCAN_DONE:
		scan_done_handler();
		break;
"""
new_call = """\tcase WIFI_EVENT_SCAN_DONE:
		scan_done_handler(event_data);
		break;
"""
if old_call not in t:
	sys.stderr.write("ERROR: SCAN_DONE call site not found\\n")
	sys.exit(1)
t = t.replace(old_call, new_call, 1)

t = t.replace(".policy = WIFI_COUNTRY_POLICY_MANUAL,",
	".policy = WIFI_COUNTRY_POLICY_AUTO,")

p.write_text(t)
print("Added Arduino-like IDF default scan + SCAN_DONE logging")
PY

# Trees that already got RF settle without PS_NONE on first start still scan 0 APs
# (modem sleep leaves the radio deaf until a later restart path). Upgrade in place.
python3 - "$DRV" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
t = p.read_text()
old = """\tLOG_INF("WiFi radio started (on demand, state=%u)", esp32_data.state);
	(void)esp_wifi_set_max_tx_power(80);
	k_sleep(K_MSEC(350));
	LOG_INF("WiFi RF settle done (tx=80)");
	return 0;
"""
new = """\tLOG_INF("WiFi radio started (on demand, state=%u)", esp32_data.state);
	(void)esp_wifi_set_ps(WIFI_PS_NONE);
	(void)esp_wifi_set_max_tx_power(80);
	k_sleep(K_MSEC(600));
	LOG_INF("WiFi RF settle done (tx=80)");
	return 0;
"""
old_r = """\t\tLOG_INF("WiFi radio restarted (on demand, state=%u)", esp32_data.state);
		(void)esp_wifi_set_ps(WIFI_PS_NONE);
		(void)esp_wifi_set_max_tx_power(80);
		k_sleep(K_MSEC(350));
		LOG_INF("WiFi RF settle done (tx=80 restart)");
		return 0;
"""
new_r = """\t\tLOG_INF("WiFi radio restarted (on demand, state=%u)", esp32_data.state);
		(void)esp_wifi_set_ps(WIFI_PS_NONE);
		(void)esp_wifi_set_max_tx_power(80);
		k_sleep(K_MSEC(600));
		LOG_INF("WiFi RF settle done (tx=80 restart)");
		return 0;
"""
changed = False
if old in t:
	t = t.replace(old, new, 1)
	changed = True
	print("Upgraded first WiFi start: PS_NONE + 600ms settle")
elif "WiFi RF settle done (tx=80)" in t and "esp_wifi_set_ps(WIFI_PS_NONE)" in t:
	# Already has PS_NONE somewhere — still stretch settle if stuck at 350ms first-start.
	old350 = """\t(void)esp_wifi_set_ps(WIFI_PS_NONE);
	(void)esp_wifi_set_max_tx_power(80);
	k_sleep(K_MSEC(350));
	LOG_INF("WiFi RF settle done (tx=80)");
"""
	new600 = """\t(void)esp_wifi_set_ps(WIFI_PS_NONE);
	(void)esp_wifi_set_max_tx_power(80);
	k_sleep(K_MSEC(600));
	LOG_INF("WiFi RF settle done (tx=80)");
"""
	if old350 in t:
		t = t.replace(old350, new600, 1)
		changed = True
		print("Stretched first WiFi RF settle to 600ms")
	else:
		print("WiFi PS_NONE first-start already present")
else:
	print("WARN: first-start RF settle site not found for PS_NONE upgrade")
if old_r in t:
	t = t.replace(old_r, new_r, 1)
	changed = True
	print("Stretched WiFi restart settle to 600ms")
if changed:
	p.write_text(t)
PY
