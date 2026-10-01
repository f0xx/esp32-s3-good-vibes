#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "imu_sample.h"

/** Scripted phone BLE peripheral side for host sim. */
void ble_host_init(void);
void ble_host_set_connected(bool on);
bool ble_host_connected(void);
void ble_host_set_notify_enabled(bool on);
bool ble_host_notify_enabled(void);

/** Push one IMU sample into the NOTIFY FIFO (drop-oldest if full). */
void ble_host_enqueue_sample(const struct imu_sample *s, uint32_t t_ms);
/** Drain up to `max` samples (oldest first). Returns count. */
size_t ble_host_drain_notify(struct imu_sample *out, size_t max);
uint32_t ble_host_notify_drops(void);
uint32_t ble_host_notify_queued(void);

/** Drive scripted connect → NOTIFY enable after `connect_after_ms`. */
void ble_host_script_tick(uint32_t now_ms);
void ble_host_script_arm(uint32_t connect_after_ms, uint32_t notify_after_ms);
