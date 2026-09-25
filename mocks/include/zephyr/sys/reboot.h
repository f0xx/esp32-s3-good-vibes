#pragma once

#define SYS_REBOOT_COLD 0
#define SYS_REBOOT_WARM 1

void sys_reboot(int type);
