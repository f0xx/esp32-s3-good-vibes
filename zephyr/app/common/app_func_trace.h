/*
 * High-severity enter/leave breadcrumbs for stall hunting.
 * Zephyr: CONFIG_APP_FUNC_TRACE=y (prj_crash). Logs at ERR so they print with default INF.
 * Host mocks: HOST_FUNC_TRACE=1 (env) — runtime, no rebuild required once built with support.
 */
#pragma once

#if defined(CONFIG_HOST_MOCK)

#include "host_debug.h"

bool host_func_trace_enabled(void);

#define APP_ENTER()                                                                                \
	do {                                                                                       \
		if (host_func_trace_enabled()) {                                                   \
			HOST_LOG(HOST_LOG_ERR, "fn", ">> %s", __func__);                         \
		}                                                                                  \
	} while (0)
#define APP_LEAVE()                                                                                \
	do {                                                                                       \
		if (host_func_trace_enabled()) {                                                   \
			HOST_LOG(HOST_LOG_ERR, "fn", "<< %s", __func__);                         \
		}                                                                                  \
	} while (0)

#elif defined(CONFIG_APP_FUNC_TRACE) && CONFIG_APP_FUNC_TRACE

#include <zephyr/logging/log.h>

#define APP_ENTER() LOG_ERR(">> %s", __func__)
#define APP_LEAVE() LOG_ERR("<< %s", __func__)

#else

#define APP_ENTER() do { } while (0)
#define APP_LEAVE() do { } while (0)

#endif
