#pragma once

/* Minimal device stub — power_manager.h includes this. */
struct device {
	const char *name;
};

#define DEVICE_DT_GET(node) ((const struct device *)0)
#define DEVICE_DT_GET_OR_NULL(node) ((const struct device *)0)

static inline bool device_is_ready(const struct device *dev)
{
	(void)dev;
	return true;
}
