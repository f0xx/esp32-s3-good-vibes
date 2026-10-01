#define _GNU_SOURCE
#include "host_debug.h"

#include <ctype.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/syscall.h>
#endif

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_level = HOST_LOG_DEFAULT_LEVEL;
static bool g_func_trace;
static struct timespec g_t0;
static bool g_t0_ok;
static pthread_key_t g_name_key;
static bool g_name_key_ok;

static const char *level_tag(int level)
{
	switch (level) {
	case HOST_LOG_ALWAYS:
		return "ALW";
	case HOST_LOG_ERR:
		return "ERR";
	case HOST_LOG_WRN:
		return "WRN";
	case HOST_LOG_INF:
		return "INF";
	case HOST_LOG_DBG:
		return "DBG";
	case HOST_LOG_TRACE:
		return "TRC";
	case HOST_LOG_FUNC:
		return "FN ";
	default:
		return "???";
	}
}

static const char *basename_of(const char *path)
{
	const char *slash = strrchr(path, '/');

	return slash ? slash + 1 : path;
}

void host_debug_init(void)
{
	if (!g_t0_ok) {
		g_t0_ok = (clock_gettime(CLOCK_MONOTONIC, &g_t0) == 0);
	}
	if (!g_name_key_ok) {
		if (pthread_key_create(&g_name_key, NULL) == 0) {
			g_name_key_ok = true;
		}
	}
	{
		const char *ft = getenv("HOST_FUNC_TRACE");

		if (ft && ft[0] == '1') {
			g_func_trace = true;
		}
	}
}

void host_debug_set_level(int level)
{
	if (level < HOST_LOG_ALWAYS) {
		level = HOST_LOG_ALWAYS;
	}
	if (level > HOST_LOG_ALL) {
		level = HOST_LOG_ALL;
	}
	g_level = level;
}

int host_debug_get_level(void)
{
	return g_level;
}

bool host_func_trace_enabled(void)
{
	return g_func_trace;
}

void host_func_trace_set(bool on)
{
	g_func_trace = on;
}

int host_debug_parse_level(const char *s)
{
	char *end = NULL;
	long n;

	if (s == NULL || *s == '\0') {
		return -1;
	}
	if (!strcasecmp(s, "always") || !strcasecmp(s, "alw")) {
		return HOST_LOG_ALWAYS;
	}
	if (!strcasecmp(s, "err") || !strcasecmp(s, "error")) {
		return HOST_LOG_ERR;
	}
	if (!strcasecmp(s, "wrn") || !strcasecmp(s, "warn") || !strcasecmp(s, "warning")) {
		return HOST_LOG_WRN;
	}
	if (!strcasecmp(s, "inf") || !strcasecmp(s, "info")) {
		return HOST_LOG_INF;
	}
	if (!strcasecmp(s, "dbg") || !strcasecmp(s, "debug")) {
		return HOST_LOG_DBG;
	}
	if (!strcasecmp(s, "trace") || !strcasecmp(s, "trc")) {
		return HOST_LOG_TRACE;
	}
	if (!strcasecmp(s, "func") || !strcasecmp(s, "fn")) {
		return HOST_LOG_FUNC;
	}
	if (!strcasecmp(s, "all")) {
		return HOST_LOG_ALL;
	}
	n = strtol(s, &end, 10);
	if (end != s && *end == '\0') {
		return (int)n;
	}
	return -1;
}

void host_debug_set_thread_name(const char *name)
{
	host_debug_init();
	if (!g_name_key_ok) {
		return;
	}
	pthread_setspecific(g_name_key, (void *)name);
#ifdef __linux__
	if (name) {
		pthread_setname_np(pthread_self(), name);
	}
#endif
}

const char *host_debug_thread_name(void)
{
	if (!g_name_key_ok) {
		return NULL;
	}
	return (const char *)pthread_getspecific(g_name_key);
}

uint64_t host_debug_uptime_ms(void)
{
	struct timespec ts;
	int64_t ms;

	host_debug_init();
	if (!g_t0_ok || clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0;
	}
	/* Signed math — nsec borrow must not underflow uint64. */
	ms = (int64_t)(ts.tv_sec - g_t0.tv_sec) * 1000 +
	     ((int64_t)ts.tv_nsec - (int64_t)g_t0.tv_nsec) / 1000000;
	return ms < 0 ? 0ull : (uint64_t)ms;
}

unsigned long host_debug_tid(void)
{
#ifdef __linux__
	return (unsigned long)syscall(SYS_gettid);
#else
	return (unsigned long)pthread_self();
#endif
}

int host_debug_core_id(void)
{
#ifdef __linux__
	int cpu = sched_getcpu();

	return cpu < 0 ? -1 : cpu;
#else
	return -1;
#endif
}

void host_debug_vlog(int level, const char *mod, const char *file, const char *func, int line,
		     const char *fmt, va_list ap)
{
	const char *tname;
	char prefix[256];
	int n;

	if (level > g_level) {
		return;
	}

	host_debug_init();
	tname = host_debug_thread_name();

	if (tname && tname[0]) {
		n = snprintf(prefix, sizeof(prefix),
			     "[%s %6llu tid=%lu core=%d name=%s] <%s %s %s:%d> ", level_tag(level),
			     (unsigned long long)host_debug_uptime_ms(), host_debug_tid(),
			     host_debug_core_id(), tname, mod ? mod : "?", func ? func : "?",
			     basename_of(file ? file : "?"), line);
	} else {
		n = snprintf(prefix, sizeof(prefix),
			     "[%s %6llu tid=%lu core=%d] <%s %s %s:%d> ", level_tag(level),
			     (unsigned long long)host_debug_uptime_ms(), host_debug_tid(),
			     host_debug_core_id(), mod ? mod : "?", func ? func : "?",
			     basename_of(file ? file : "?"), line);
	}
	(void)n;

	pthread_mutex_lock(&g_log_lock);
	fputs(prefix, stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	fflush(stderr);
	pthread_mutex_unlock(&g_log_lock);
}

void host_debug_log(int level, const char *mod, const char *file, const char *func, int line,
		    const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	host_debug_vlog(level, mod, file, func, line, fmt, ap);
	va_end(ap);
}
