#pragma once

/*
 * Host stand-in for <zephyr/kernel.h> — enough for imu_pipeline / imu_cal.
 * Backed by pthreads so ThreadSanitizer can see real races.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>

#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int64_t k_timeout_t;

#define K_NO_WAIT       ((k_timeout_t)0)
#define K_FOREVER       ((k_timeout_t)-1)
#define K_MSEC(ms)      ((k_timeout_t)(ms))
#define K_SECONDS(s)    ((k_timeout_t)((s) * 1000))

struct k_mutex {
	pthread_mutex_t lock;
	bool ready;
};

struct k_work;
typedef void (*k_work_handler_t)(struct k_work *work);

struct k_work {
	k_work_handler_t handler;
	struct k_work *next;
};

struct k_work_delayable {
	struct k_work work;
	int64_t due_ms;
	bool pending;
};

struct k_work_q {
	void *impl; /* host workqueue */
};

#define K_THREAD_STACK_DEFINE(name, size) \
	uint8_t name[(size)] __attribute__((unused))

#define K_THREAD_STACK_SIZEOF(name) (sizeof(name))

/* Match Zephyr: statically ready — no lazy first-touch init races. */
#define K_MUTEX_DEFINE(name) \
	struct k_mutex name = { .lock = PTHREAD_MUTEX_INITIALIZER, .ready = true }


#ifndef ARG_UNUSED
#define ARG_UNUSED(x) (void)(x)
#endif

void k_mutex_init(struct k_mutex *m);
int k_mutex_lock(struct k_mutex *m, k_timeout_t timeout);
int k_mutex_unlock(struct k_mutex *m);

void k_work_init(struct k_work *work, k_work_handler_t handler);
void k_work_init_delayable(struct k_work_delayable *dwork, k_work_handler_t handler);
int k_work_schedule_for_queue(struct k_work_q *queue, struct k_work_delayable *dwork,
			      k_timeout_t delay);
int k_work_reschedule_for_queue(struct k_work_q *queue, struct k_work_delayable *dwork,
				k_timeout_t delay);
void k_work_queue_start(struct k_work_q *queue, void *stack, size_t stack_size, int prio,
			void *cfg);

int64_t k_uptime_get(void);
uint32_t k_uptime_get_32(void);
void k_msleep(int32_t ms);
void k_usleep(int32_t us);
void k_busy_wait(uint32_t usec);

void *k_malloc(size_t size);
void k_free(void *ptr);

/** Optional: advance virtual time without sleeping (tests). */
void host_time_set_virtual(bool on);
void host_time_advance_ms(int64_t ms);

#ifdef __cplusplus
}
#endif
