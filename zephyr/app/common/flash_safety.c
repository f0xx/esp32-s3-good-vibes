#include "flash_safety.h"

#include <zephyr/toolchain.h>

__weak bool app_flash_erase_safe(void)
{
	return true;
}
