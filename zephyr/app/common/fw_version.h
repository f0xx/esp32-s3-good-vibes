/*
 * Single firmware version for logs, crash ring, BLE STATUS, OTA manifest,
 * and MCUboot imgtool. Desk/USB builds bump FW_VERSION_CODE here.
 * Cloud builder stamps 00.0001.0000.00001+ (separate from the mobile app
 * allocator) via FW_OTA_VERSION_* in ci-west-build.sh.
 */
#ifndef IMU_FW_VERSION_H
#define IMU_FW_VERSION_H

#ifndef FW_VERSION_CODE
#define FW_VERSION_CODE 322
#endif
#ifndef FW_VERSION_NAME
#define FW_VERSION_NAME "handshake v322"
#endif

#endif /* IMU_FW_VERSION_H */
