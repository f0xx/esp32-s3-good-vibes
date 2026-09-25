#pragma once

#include <stdio.h>
#include <stdint.h>

#include "host_debug.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Map Zephyr log levels onto host_debug severities. */
enum {
	LOG_LEVEL_NONE = 0,
	LOG_LEVEL_ERR = HOST_LOG_ERR,
	LOG_LEVEL_WRN = HOST_LOG_WRN,
	LOG_LEVEL_INF = HOST_LOG_INF,
	LOG_LEVEL_DBG = HOST_LOG_DBG,
};

#define LOG_MODULE_REGISTER(name, level)                                                           \
	static const char *const __log_module_name __attribute__((unused)) = #name;                \
	static const int __log_module_level_unused __attribute__((unused)) = (level)

#define LOG_ERR(...)                                                                               \
	HOST_LOG(HOST_LOG_ERR, __log_module_name, __VA_ARGS__)
#define LOG_WRN(...)                                                                               \
	HOST_LOG(HOST_LOG_WRN, __log_module_name, __VA_ARGS__)
#define LOG_INF(...)                                                                               \
	HOST_LOG(HOST_LOG_INF, __log_module_name, __VA_ARGS__)
#define LOG_DBG(...)                                                                               \
	HOST_LOG(HOST_LOG_DBG, __log_module_name, __VA_ARGS__)

#ifdef __cplusplus
}
#endif
