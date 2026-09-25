/*
 * WiFi CDN self-OTA — HTTPS GET channel → manifest → signed fw.bin → inactive slot.
 * Runs only when STA is up, a WiFi profile exists, phone BLE is idle, and BLE OTA is idle.
 * TLS peer verify is off (CDN hosts rotate; keeps cert bundle off the image).
 */

#include "ota_wifi.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "ble_imu_gatt.h"
#include "ble_ota_gatt.h"
#include "fw_version.h"
#include "net_profile_store.h"
#include "network_manager.h"
#include "ota_ab.h"
#include "ota_channel.h"

LOG_MODULE_REGISTER(ota_wifi, LOG_LEVEL_INF);

#if defined(CONFIG_APP_WIFI_OTA) && defined(CONFIG_NET_TCP) && \
	defined(CONFIG_NET_SOCKETS_SOCKOPT_TLS) && defined(CONFIG_DNS_RESOLVER)

#include <zephyr/net/tls_credentials.h>

#define OTA_WIFI_STACK_SIZE 8192
#define OTA_WIFI_PRIORITY   8
#define RECV_BUF_SIZE       1536
#define JSON_BUF_SIZE       4096
#define URL_MAX             256
#define HOST_MAX            64
#define OTA_PATH_MAX        192

#define POLL_FIRST_MS       (90U * 1000U)
#define POLL_OK_MS          (6U * 3600U * 1000U)
#define POLL_FAIL_MS        (15U * 60U * 1000U)
#define POLL_IDLE_MS        (30U * 1000U)

static K_THREAD_STACK_DEFINE(ota_wifi_stack, OTA_WIFI_STACK_SIZE);
static struct k_thread ota_wifi_thread;
static k_tid_t g_tid;
static atomic_t g_busy;
static int64_t g_next_attempt_ms;
static bool g_started;

struct url_parts {
	char host[HOST_MAX];
	char path[OTA_PATH_MAX];
	bool tls;
	uint16_t port;
};

static bool parse_url(const char *url, struct url_parts *out)
{
	const char *p = url;

	memset(out, 0, sizeof(*out));
	if (strncmp(p, "https://", 8) == 0) {
		out->tls = true;
		out->port = 443;
		p += 8;
	} else if (strncmp(p, "http://", 7) == 0) {
		out->tls = false;
		out->port = 80;
		p += 7;
	} else {
		return false;
	}

	const char *slash = strchr(p, '/');
	const char *colon = strchr(p, ':');
	size_t host_len;

	if (colon != NULL && (slash == NULL || colon < slash)) {
		host_len = (size_t)(colon - p);
		out->port = (uint16_t)atoi(colon + 1);
	} else if (slash != NULL) {
		host_len = (size_t)(slash - p);
	} else {
		host_len = strlen(p);
	}
	if (host_len == 0U || host_len >= sizeof(out->host)) {
		return false;
	}
	memcpy(out->host, p, host_len);
	out->host[host_len] = '\0';
	if (slash != NULL) {
		snprintf(out->path, sizeof(out->path), "%s", slash);
	} else {
		snprintf(out->path, sizeof(out->path), "/");
	}
	return true;
}

static int connect_host(const struct url_parts *u)
{
	char port_str[8];
	struct zsock_addrinfo hints;
	struct zsock_addrinfo *res = NULL;
	int sock = -1;
	int ret;

	snprintf(port_str, sizeof(port_str), "%u", (unsigned)u->port);
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	ret = zsock_getaddrinfo(u->host, port_str, &hints, &res);
	if (ret != 0 || res == NULL) {
		LOG_WRN("dns %s failed %d", u->host, ret);
		return -EIO;
	}

	if (u->tls) {
		sock = zsock_socket(res->ai_family, SOCK_STREAM, IPPROTO_TLS_1_2);
	} else {
		sock = zsock_socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP);
	}
	if (sock < 0) {
		zsock_freeaddrinfo(res);
		return -errno;
	}

	if (u->tls) {
		const int verify = TLS_PEER_VERIFY_NONE;
		const char *hname = u->host;

		(void)zsock_setsockopt(sock, SOL_TLS, TLS_PEER_VERIFY, &verify, sizeof(verify));
		(void)zsock_setsockopt(sock, SOL_TLS, TLS_HOSTNAME, hname, strlen(hname));
	}

	const int timeout_ms = 20000;
	struct timeval tv = {
		.tv_sec = timeout_ms / 1000,
		.tv_usec = (timeout_ms % 1000) * 1000,
	};

	(void)zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	(void)zsock_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	ret = zsock_connect(sock, res->ai_addr, res->ai_addrlen);
	zsock_freeaddrinfo(res);
	if (ret < 0) {
		LOG_WRN("connect %s:%u failed %d", u->host, u->port, errno);
		zsock_close(sock);
		return -errno;
	}
	return sock;
}

static int http_get_buffer(const char *url, char *body, size_t body_cap, size_t *body_len)
{
	struct url_parts u;
	char req[384];
	char rxbuf[RECV_BUF_SIZE];
	char hdr[2048];
	size_t hdr_len = 0U;
	size_t body_got = 0U;
	bool hdr_done = false;
	int content_len = -1;
	int sock;
	int redirects = 0;
	char cur_url[URL_MAX];

	snprintf(cur_url, sizeof(cur_url), "%s", url);
	*body_len = 0U;

ota_http_redirect:
	if (!parse_url(cur_url, &u)) {
		return -EINVAL;
	}
	sock = connect_host(&u);
	if (sock < 0) {
		return sock;
	}

	snprintf(req, sizeof(req),
		 "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n"
		 "User-Agent: good-vibes-esp/1\r\nAccept: application/octet-stream\r\n\r\n",
		 u.path, u.host);
	if (zsock_send(sock, req, strlen(req), 0) < 0) {
		zsock_close(sock);
		return -EIO;
	}

	hdr_len = 0U;
	body_got = 0U;
	hdr_done = false;
	content_len = -1;

	while (true) {
		const int n = zsock_recv(sock, rxbuf, sizeof(rxbuf), 0);

		if (n <= 0) {
			break;
		}
		if (!hdr_done) {
			if (hdr_len + (size_t)n > sizeof(hdr) - 1U) {
				zsock_close(sock);
				return -ENOMEM;
			}
			memcpy(hdr + hdr_len, rxbuf, (size_t)n);
			hdr_len += (size_t)n;
			hdr[hdr_len] = '\0';
			char *sep = strstr(hdr, "\r\n\r\n");

			if (sep == NULL) {
				continue;
			}
			*sep = '\0';
			const size_t header_bytes = (size_t)(sep - hdr) + 4U;
			const char *cl = strstr(hdr, "Content-Length:");
			if (cl == NULL) {
				cl = strstr(hdr, "content-length:");
			}
			if (cl != NULL) {
				content_len = atoi(cl + 15);
			}
			int status = 0;

			if (sscanf(hdr, "HTTP/%*s %d", &status) != 1) {
				zsock_close(sock);
				return -EPROTO;
			}
			if (status == 301 || status == 302 || status == 307 || status == 308) {
				const char *loc = strstr(hdr, "Location:");
				if (loc == NULL) {
					loc = strstr(hdr, "location:");
				}
				zsock_close(sock);
				if (loc == NULL || redirects++ >= 3) {
					return -EIO;
				}
				loc += 9;
				while (*loc == ' ') {
					loc++;
				}
				const char *end = strstr(loc, "\r\n");
				size_t llen = end != NULL ? (size_t)(end - loc) : strlen(loc);

				if (llen >= sizeof(cur_url)) {
					return -EINVAL;
				}
				memcpy(cur_url, loc, llen);
				cur_url[llen] = '\0';
				goto ota_http_redirect;
			}
			if (status < 200 || status >= 300) {
				LOG_WRN("HTTP %d for %s", status, u.host);
				zsock_close(sock);
				return -EIO;
			}
			hdr_done = true;
			const size_t already = hdr_len - header_bytes;

			if (already > 0U) {
				if (already >= body_cap) {
					zsock_close(sock);
					return -ENOMEM;
				}
				memcpy(body, hdr + header_bytes, already);
				body_got = already;
			}
			continue;
		}
		if (body_got + (size_t)n >= body_cap) {
			zsock_close(sock);
			return -ENOMEM;
		}
		memcpy(body + body_got, rxbuf, (size_t)n);
		body_got += (size_t)n;
		if (content_len >= 0 && (int)body_got >= content_len) {
			break;
		}
	}
	zsock_close(sock);
	body[body_got < body_cap ? body_got : body_cap - 1U] = '\0';
	*body_len = body_got;
	return 0;
}

struct dl_ctx {
	struct flash_img_context *flash;
	size_t written;
	size_t expect;
	int err;
};

static int http_get_flash(const char *url, struct flash_img_context *flash, size_t expect)
{
	struct url_parts u;
	char req[384];
	char rxbuf[RECV_BUF_SIZE];
	char hdr[1024];
	size_t hdr_len = 0U;
	bool hdr_done = false;
	int content_len = -1;
	int sock;
	int redirects = 0;
	char cur_url[URL_MAX];
	struct dl_ctx dl = { .flash = flash, .written = 0U, .expect = expect, .err = 0 };

	snprintf(cur_url, sizeof(cur_url), "%s", url);

ota_http_redirect:
	if (!parse_url(cur_url, &u)) {
		return -EINVAL;
	}
	sock = connect_host(&u);
	if (sock < 0) {
		return sock;
	}

	snprintf(req, sizeof(req),
		 "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n"
		 "User-Agent: good-vibes-esp/1\r\nAccept: application/octet-stream\r\n\r\n",
		 u.path, u.host);
	if (zsock_send(sock, req, strlen(req), 0) < 0) {
		zsock_close(sock);
		return -EIO;
	}

	hdr_len = 0U;
	hdr_done = false;
	content_len = -1;
	dl.written = 0U;

	while (true) {
		const int n = zsock_recv(sock, rxbuf, sizeof(rxbuf), 0);

		if (n <= 0) {
			break;
		}
		if (!hdr_done) {
			if (hdr_len + (size_t)n > sizeof(hdr) - 1U) {
				zsock_close(sock);
				return -ENOMEM;
			}
			memcpy(hdr + hdr_len, rxbuf, (size_t)n);
			hdr_len += (size_t)n;
			hdr[hdr_len] = '\0';
			char *sep = strstr(hdr, "\r\n\r\n");

			if (sep == NULL) {
				continue;
			}
			*sep = '\0';
			const size_t header_bytes = (size_t)(sep - hdr) + 4U;
			const char *cl = strstr(hdr, "Content-Length:");
			if (cl == NULL) {
				cl = strstr(hdr, "content-length:");
			}
			if (cl != NULL) {
				content_len = atoi(cl + 15);
			}
			int status = 0;

			if (sscanf(hdr, "HTTP/%*s %d", &status) != 1) {
				zsock_close(sock);
				return -EPROTO;
			}
			if (status == 301 || status == 302 || status == 307 || status == 308) {
				const char *loc = strstr(hdr, "Location:");
				if (loc == NULL) {
					loc = strstr(hdr, "location:");
				}
				zsock_close(sock);
				if (loc == NULL || redirects++ >= 3) {
					return -EIO;
				}
				loc += 9;
				while (*loc == ' ') {
					loc++;
				}
				const char *end = strstr(loc, "\r\n");
				size_t llen = end != NULL ? (size_t)(end - loc) : strlen(loc);

				if (llen >= sizeof(cur_url)) {
					return -EINVAL;
				}
				memcpy(cur_url, loc, llen);
				cur_url[llen] = '\0';
				goto ota_http_redirect;
			}
			if (status < 200 || status >= 300) {
				zsock_close(sock);
				return -EIO;
			}
			hdr_done = true;
			const size_t already = hdr_len - header_bytes;

			if (already > 0U) {
				if (flash_img_buffered_write(flash, (uint8_t *)(hdr + header_bytes),
							     already, false) != 0) {
					zsock_close(sock);
					return -EIO;
				}
				dl.written += already;
			}
			continue;
		}
		if (flash_img_buffered_write(flash, (uint8_t *)rxbuf, (size_t)n, false) != 0) {
			zsock_close(sock);
			return -EIO;
		}
		dl.written += (size_t)n;
		if (content_len >= 0 && (int)dl.written >= content_len) {
			break;
		}
		if (expect > 0U && dl.written >= expect) {
			break;
		}
	}
	zsock_close(sock);
	if (expect > 0U && dl.written != expect) {
		LOG_WRN("fw size mismatch got=%u want=%u", (unsigned)dl.written, (unsigned)expect);
		return -EIO;
	}
	LOG_INF("wifi ota downloaded %u bytes", (unsigned)dl.written);
	return 0;
}

static bool json_find_string(const char *json, const char *key, char *out, size_t out_len)
{
	char pat[48];
	const char *p;

	snprintf(pat, sizeof(pat), "\"%s\"", key);
	p = strstr(json, pat);
	if (p == NULL) {
		return false;
	}
	p = strchr(p + strlen(pat), ':');
	if (p == NULL) {
		return false;
	}
	p++;
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	if (*p != '"') {
		return false;
	}
	p++;
	size_t i = 0U;

	while (*p != '\0' && *p != '"' && i + 1U < out_len) {
		if (*p == '\\' && p[1] != '\0') {
			p++;
		}
		out[i++] = *p++;
	}
	out[i] = '\0';
	return i > 0U;
}

static bool json_find_int_after(const char *json, const char *section, const char *key, int *out)
{
	const char *sec = section != NULL ? strstr(json, section) : json;
	char pat[40];
	const char *p;

	if (sec == NULL) {
		return false;
	}
	snprintf(pat, sizeof(pat), "\"%s\"", key);
	p = strstr(sec, pat);
	if (p == NULL) {
		return false;
	}
	p = strchr(p + strlen(pat), ':');
	if (p == NULL) {
		return false;
	}
	*out = atoi(p + 1);
	return true;
}

static const char *channel_file(void)
{
	const char *ch = ota_channel_get();

	if (strcmp(ch, "staging") == 0) {
		return "staging.json";
	}
	if (strcmp(ch, "dev") == 0) {
		return "dev.json";
	}
	return "stable.json";
}

static int fetch_channel_and_upgrade(void)
{
	static const char *const hosts[] = {
		"cdn.f0xx.org",
		"cdn0.f0xx.org",
		"cdn1.f0xx.org",
		"cdn2.f0xx.org",
	};
	char url[URL_MAX];
	char json[JSON_BUF_SIZE];
	size_t jlen = 0U;
	char manifest_url[URL_MAX];
	char fw_url[URL_MAX];
	int fw_code = 0;
	int fw_size = 0;
	int rc = -ENOENT;
	const char *chfile = channel_file();

	for (size_t i = 0; i < ARRAY_SIZE(hosts); i++) {
		snprintf(url, sizeof(url), "https://%s/good_vibes/v0/ota/channel/%s", hosts[i],
			 chfile);
		LOG_INF("wifi ota GET %s", url);
		rc = http_get_buffer(url, json, sizeof(json), &jlen);
		if (rc == 0 && jlen > 16U) {
			break;
		}
	}
	if (rc != 0) {
		return rc;
	}
	if (!json_find_string(json, "manifestUrl", manifest_url, sizeof(manifest_url))) {
		LOG_WRN("channel json missing manifestUrl");
		return -EINVAL;
	}

	rc = http_get_buffer(manifest_url, json, sizeof(json), &jlen);
	if (rc != 0) {
		/* try mirrors from channel body if present */
		return rc;
	}
	if (!json_find_int_after(json, "\"fw\"", "versionCode", &fw_code) || fw_code <= 0) {
		LOG_WRN("manifest missing fw.versionCode");
		return -EINVAL;
	}
	if (fw_code <= (int)FW_VERSION_CODE) {
		LOG_INF("wifi ota up to date fwc=%d live=%u ch=%s", fw_code,
			(unsigned)FW_VERSION_CODE, ota_channel_get());
		return 0;
	}
	{
		const char *fw_sec = strstr(json, "\"fw\"");

		if (fw_sec == NULL ||
		    !json_find_string(fw_sec, "url", fw_url, sizeof(fw_url))) {
			LOG_WRN("manifest missing fw.url");
			return -EINVAL;
		}
	}
	(void)json_find_int_after(json, "\"fw\"", "size", &fw_size);

	LOG_INF("wifi ota offer fwc=%d (live=%u) size=%d ch=%s", fw_code, (unsigned)FW_VERSION_CODE,
		fw_size, ota_channel_get());

	struct flash_img_context flash;

	if (flash_img_init(&flash) != 0) {
		return -EIO;
	}
	ota_channel_note_source(ota_channel_get());
	rc = http_get_flash(fw_url, &flash, fw_size > 0 ? (size_t)fw_size : 0U);
	if (rc != 0) {
		return rc;
	}
	return ota_ab_finish_and_reboot(&flash);
}

static bool ready_to_check(void)
{
	if (!network_manager_link_up()) {
		return false;
	}
	if (net_profile_store_count() == 0U) {
		return false;
	}
	if (ble_ota_ui_active() || ota_wifi_busy()) {
		return false;
	}
	/* Prefer no phone link for large HTTPS download (radio coexistence). */
	if (!ble_imu_disconnected_settled(5000U)) {
		return false;
	}
	return true;
}

static void ota_wifi_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	g_next_attempt_ms = k_uptime_get() + POLL_FIRST_MS;

	while (true) {
		const int64_t now = k_uptime_get();

		if (now < g_next_attempt_ms || !ready_to_check()) {
			k_msleep(POLL_IDLE_MS);
			continue;
		}

		atomic_set(&g_busy, 1);
		LOG_INF("wifi ota check channel=%s", ota_channel_get());
		const int rc = fetch_channel_and_upgrade();

		atomic_set(&g_busy, 0);
		if (rc == 0) {
			g_next_attempt_ms = k_uptime_get() + POLL_OK_MS;
		} else {
			LOG_WRN("wifi ota check failed %d", rc);
			g_next_attempt_ms = k_uptime_get() + POLL_FAIL_MS;
		}
		k_msleep(1000);
	}
}

void ota_wifi_init(void)
{
	if (g_started) {
		return;
	}
	g_started = true;
	g_tid = k_thread_create(&ota_wifi_thread, ota_wifi_stack,
				K_THREAD_STACK_SIZEOF(ota_wifi_stack), ota_wifi_thread_fn, NULL,
				NULL, NULL, OTA_WIFI_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(g_tid, "ota_wifi");
	LOG_INF("wifi ota thread started");
}

void ota_wifi_poll(void)
{
	/* work runs on dedicated thread */
}

bool ota_wifi_busy(void)
{
	return atomic_get(&g_busy) != 0;
}

#else /* !CONFIG_APP_WIFI_OTA or missing TCP/TLS/DNS */

void ota_wifi_init(void)
{
	LOG_INF("wifi ota disabled (DRAM: enable CONFIG_APP_WIFI_OTA later)");
}

void ota_wifi_poll(void)
{
}

bool ota_wifi_busy(void)
{
	return false;
}

#endif
