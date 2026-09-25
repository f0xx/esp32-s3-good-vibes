#pragma once

/*
 * Host debug logger for mocks/handshake_host.
 * Style inspired by copy_recovery/clib_core/debug.h:
 *   [lvl ms tid:core] <mod func file:line> message
 *
 * Severity filter is runtime (host_debug_set_level / --log-level).
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Severity (lower = more important). Matches Zephyr-ish names + TRACE/FUNC. */
enum host_log_level {
	HOST_LOG_ALWAYS = 0,
	HOST_LOG_ERR = 1,
	HOST_LOG_WRN = 2,
	HOST_LOG_INF = 3,
	HOST_LOG_DBG = 4,
	HOST_LOG_TRACE = 5,
	HOST_LOG_FUNC = 6,
	HOST_LOG_ALL = 10,
};

#ifndef HOST_LOG_DEFAULT_LEVEL
#define HOST_LOG_DEFAULT_LEVEL HOST_LOG_INF
#endif

#ifdef __PRETTY_FUNCTION__
#define HOST_FUNCTION __PRETTY_FUNCTION__
#else
#define HOST_FUNCTION __func__
#endif

void host_debug_init(void);
void host_debug_set_level(int level);
int host_debug_get_level(void);
bool host_func_trace_enabled(void);
void host_func_trace_set(bool on);
/** Parse "err|wrn|inf|dbg|trace|func|all" or decimal; returns -1 on failure. */
int host_debug_parse_level(const char *s);
/** Optional short name for current pthread (shown instead of raw tid). */
void host_debug_set_thread_name(const char *name);
const char *host_debug_thread_name(void);

uint64_t host_debug_uptime_ms(void);
unsigned long host_debug_tid(void);
int host_debug_core_id(void);

void host_debug_vlog(int level, const char *mod, const char *file, const char *func, int line,
		     const char *fmt, va_list ap);
void host_debug_log(int level, const char *mod, const char *file, const char *func, int line,
		    const char *fmt, ...) __attribute__((format(printf, 6, 7)));

#define HOST_LOG(level, mod, fmt, ...)                                                             \
	do {                                                                                       \
		if ((level) <= host_debug_get_level()) {                                           \
			host_debug_log((level), (mod), __FILE__, HOST_FUNCTION, __LINE__, (fmt)     \
				        __VA_OPT__(,) __VA_ARGS__);                                             \
		}                                                                                  \
	} while (0)

/* copy_recovery-style aliases */
#define DP_ALWAYS HOST_LOG_ALWAYS
#define DP_WARNINGS HOST_LOG_WRN
#define DP_VERBOSE HOST_LOG_INF
#define DP_FUNCTION HOST_LOG_FUNC
#define DP_TRACE HOST_LOG_TRACE
#define DP_FUNC HOST_LOG_FUNC
#define DP_ALL HOST_LOG_ALL

#define P_DBG(level, fmt, ...) HOST_LOG((level), "app", (fmt) __VA_OPT__(,) __VA_ARGS__)
#define LOG_API_ENTER() HOST_LOG(HOST_LOG_FUNC, "app", "enter")
#define LOG_API_LEAVE() HOST_LOG(HOST_LOG_FUNC, "app", "leave")

#ifdef __cplusplus
}
#endif
