#include "clock_sync.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(clock_ntp, LOG_LEVEL_INF);

#define NTP_BOOT_DELAY_MS     (5U * 60U * 1000U)
#define NTP_PERIODIC_MS       (5ULL * 24ULL * 60ULL * 60ULL * 1000ULL)
#define NTP_RETRY_MS          (6ULL * 60ULL * 60ULL * 1000ULL)
#define NTP_SERVER_IPV4       "129.6.15.28"
#define NTP_UNIX_OFFSET_SEC   2208988800ULL

static int64_t g_boot_deadline;
static int64_t g_periodic_deadline;
static atomic_t g_wifi_pending;
static bool g_periodic_armed;

static int ntp_query_once(void);

static void arm_periodic(void)
{
	g_periodic_armed = true;
	g_periodic_deadline = k_uptime_get() + (int64_t)NTP_PERIODIC_MS;
	LOG_INF("NTP periodic armed (5 days)");
}

#if IS_ENABLED(CONFIG_NET_SOCKETS)

static int ntp_query_once(void)
{
	struct sockaddr_in addr = { 0 };
	uint8_t req[48];
	uint8_t reply[48];
	int sock;
	int ret;

	memset(req, 0, sizeof(req));
	req[0] = 0x1b; /* LI=0 VN=3 Mode=3 (client) */

	sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		LOG_WRN("NTP socket failed (%d)", sock);
		return sock;
	}

	struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };

	(void)zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	(void)zsock_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	addr.sin_family = AF_INET;
	addr.sin_port = htons(123);
	if (zsock_inet_pton(AF_INET, NTP_SERVER_IPV4, &addr.sin_addr) != 1) {
		zsock_close(sock);
		return -EINVAL;
	}

	ret = zsock_sendto(sock, req, sizeof(req), 0, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0) {
		LOG_WRN("NTP send failed (%d)", ret);
		zsock_close(sock);
		return ret;
	}

	ret = zsock_recvfrom(sock, reply, sizeof(reply), 0, NULL, NULL);
	zsock_close(sock);
	if (ret < 48) {
		LOG_WRN("NTP recv failed (%d)", ret);
		return ret >= 0 ? -EIO : ret;
	}

	const uint32_t ntp_sec = ((uint32_t)reply[40] << 24) | ((uint32_t)reply[41] << 16) |
				 ((uint32_t)reply[42] << 8) | (uint32_t)reply[43];

	if (ntp_sec <= NTP_UNIX_OFFSET_SEC) {
		LOG_WRN("NTP invalid sec=%u", ntp_sec);
		return -EINVAL;
	}

	const int64_t unix_ms = ((int64_t)ntp_sec - (int64_t)NTP_UNIX_OFFSET_SEC) * 1000LL;

	if (!clock_sync_set_from_ntp(unix_ms)) {
		return -EINVAL;
	}

	LOG_INF("NTP synced unix_ms=%lld", (long long)unix_ms);
	return 0;
}

#else /* !CONFIG_NET_SOCKETS */

static int ntp_query_once(void)
{
	LOG_DBG("NTP disabled (CONFIG_NET_SOCKETS=n)");
	return -ENOTSUP;
}

#endif /* CONFIG_NET_SOCKETS */

void clock_sync_ntp_init(void)
{
	g_boot_deadline = k_uptime_get() + (int64_t)NTP_BOOT_DELAY_MS;
	g_periodic_deadline = 0;
	atomic_set(&g_wifi_pending, 0);
	g_periodic_armed = false;
	LOG_INF("NTP boot query scheduled in %u min (main poll)", NTP_BOOT_DELAY_MS / 60000U);
}

void clock_sync_ntp_on_wifi_connected(void)
{
	if (g_periodic_armed) {
		return;
	}

	atomic_set(&g_wifi_pending, 1);
}

void clock_sync_ntp_reset_schedule(void)
{
	if (!g_periodic_armed) {
		return;
	}

	arm_periodic();
}

void clock_sync_ntp_poll(void)
{
	if (atomic_cas(&g_wifi_pending, 1, 0)) {
		LOG_INF("NTP WiFi-up attempt");
		if (ntp_query_once() == 0) {
			g_boot_deadline = 0;
			arm_periodic();
		}
		return;
	}

	if (g_boot_deadline > 0 && k_uptime_get() >= g_boot_deadline) {
		g_boot_deadline = 0;
		LOG_INF("NTP boot-delay query (+5 min)");
		if (ntp_query_once() == 0) {
			arm_periodic();
		}
		return;
	}

	if (g_periodic_deadline > 0 && k_uptime_get() >= g_periodic_deadline) {
		g_periodic_deadline = 0;
		LOG_INF("NTP periodic drift check");
		if (ntp_query_once() == 0) {
			arm_periodic();
		} else {
			g_periodic_deadline = k_uptime_get() + (int64_t)NTP_RETRY_MS;
		}
	}
}
