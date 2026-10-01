/*
 * Preferred CDN OTA channel (phone-synced) + last upgrade source for soft crash.
 */

#include "ota_channel.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

LOG_MODULE_REGISTER(ota_ch, LOG_LEVEL_INF);

#define SETTINGS_PREF   "ota/ch"
#define SETTINGS_SOURCE "ota/src"

static char g_pref[OTA_CHANNEL_MAX + 1] = "stable";
static char g_source[OTA_CHANNEL_MAX + 1];
static bool g_loaded;

static const char *normalize(const char *in, char *out, size_t out_len)
{
	if (out == NULL || out_len < 2U) {
		return "stable";
	}
	out[0] = '\0';
	if (in == NULL || in[0] == '\0') {
		snprintf(out, out_len, "%s", "stable");
		return out;
	}

	char tmp[OTA_CHANNEL_MAX + 1];
	size_t n = 0U;

	while (in[n] != '\0' && n < OTA_CHANNEL_MAX) {
		char c = in[n];

		if (c >= 'A' && c <= 'Z') {
			c = (char)(c - 'A' + 'a');
		}
		tmp[n++] = c;
	}
	tmp[n] = '\0';

	if (strcmp(tmp, "imu") == 0 || strcmp(tmp, "prod") == 0 || strcmp(tmp, "stable") == 0) {
		snprintf(out, out_len, "%s", "stable");
	} else if (strcmp(tmp, "staging") == 0 || strcmp(tmp, "dev") == 0) {
		snprintf(out, out_len, "%s", tmp);
	} else {
		snprintf(out, out_len, "%s", "stable");
	}
	return out;
}

static int settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *leaf = name;
	char buf[OTA_CHANNEL_MAX + 1];

	if (len == 0U || len > OTA_CHANNEL_MAX) {
		return -EINVAL;
	}
	if (read_cb(cb_arg, buf, len) != (ssize_t)len) {
		return -EIO;
	}
	buf[len] = '\0';

	if (settings_name_steq(name, "ch", &leaf) && leaf == NULL) {
		normalize(buf, g_pref, sizeof(g_pref));
		g_loaded = true;
		return 0;
	}
	if (settings_name_steq(name, "src", &leaf) && leaf == NULL) {
		normalize(buf, g_source, sizeof(g_source));
		return 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(ota_ch, "ota", NULL, settings_set, NULL, NULL);

void ota_channel_init(void)
{
	(void)settings_load_subtree("ota");
	if (!g_loaded) {
		normalize("stable", g_pref, sizeof(g_pref));
	}
	LOG_INF("ota channel pref=%s source=%s", g_pref,
		g_source[0] != '\0' ? g_source : "(none)");
}

const char *ota_channel_get(void)
{
	return g_pref[0] != '\0' ? g_pref : "stable";
}

bool ota_channel_set(const char *channel)
{
	char norm[OTA_CHANNEL_MAX + 1];

	normalize(channel, norm, sizeof(norm));
	if (strcmp(g_pref, norm) == 0) {
		return true;
	}
	snprintf(g_pref, sizeof(g_pref), "%s", norm);
	if (settings_save_one(SETTINGS_PREF, g_pref, strlen(g_pref)) != 0) {
		LOG_WRN("ota channel save failed");
		return false;
	}
	LOG_INF("ota channel set %s", g_pref);
	return true;
}

const char *ota_channel_source_get(void)
{
	if (g_source[0] != '\0') {
		return g_source;
	}
	return ota_channel_get();
}

void ota_channel_note_source(const char *channel)
{
	char norm[OTA_CHANNEL_MAX + 1];

	normalize(channel != NULL ? channel : ota_channel_get(), norm, sizeof(norm));
	snprintf(g_source, sizeof(g_source), "%s", norm);
	(void)settings_save_one(SETTINGS_SOURCE, g_source, strlen(g_source));
	LOG_INF("ota upgrade source=%s", g_source);
}

void ota_channel_format_outcome(char *dst, size_t dst_len, const char *outcome)
{
	const char *ch = ota_channel_source_get();
	const char *oc = (outcome != NULL && outcome[0] != '\0') ? outcome : "ok";

	/* Keep under crash-ring ota_outcome[16]: "ok:stable" / "rb:staging" / "bad:dev". */
	if (strcmp(oc, "transition_ok") == 0) {
		oc = "ok";
	} else if (strcmp(oc, "reboot") == 0) {
		oc = "rb";
	} else if (strcmp(oc, "target_non_operable") == 0) {
		oc = "bad";
	}
	snprintf(dst, dst_len, "%s:%s", oc, ch);
}
