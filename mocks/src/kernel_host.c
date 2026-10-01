#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include "host_debug.h"

LOG_MODULE_REGISTER(kernel_host, LOG_LEVEL_INF);

/* --- time --- */

static pthread_mutex_t g_time_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_virtual_time;
static int64_t g_virt_ms;
static int64_t g_mono_origin;
static bool g_mono_anchored;

static int64_t steady_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

static void anchor_mono_locked(void)
{
	if (!g_mono_anchored) {
		g_mono_origin = steady_ms();
		g_mono_anchored = true;
	}
}

void host_time_set_virtual(bool on)
{
	pthread_mutex_lock(&g_time_lock);
	g_virtual_time = on;
	if (on) {
		g_virt_ms = 0;
	} else {
		anchor_mono_locked();
	}
	pthread_mutex_unlock(&g_time_lock);
	HOST_LOG(HOST_LOG_DBG, "kernel_host", "virtual_time=%d", on ? 1 : 0);
}

void host_time_advance_ms(int64_t ms)
{
	pthread_mutex_lock(&g_time_lock);
	g_virt_ms += ms;
	pthread_mutex_unlock(&g_time_lock);
}

int64_t k_uptime_get(void)
{
	int64_t now;

	pthread_mutex_lock(&g_time_lock);
	if (g_virtual_time) {
		now = g_virt_ms;
	} else {
		anchor_mono_locked();
		now = steady_ms() - g_mono_origin;
	}
	pthread_mutex_unlock(&g_time_lock);
	return now;
}

uint32_t k_uptime_get_32(void)
{
	return (uint32_t)k_uptime_get();
}

void k_msleep(int32_t ms)
{
	if (ms <= 0) {
		return;
	}
	HOST_LOG(HOST_LOG_TRACE, "kernel_host", "k_msleep(%d)", ms);
	pthread_mutex_lock(&g_time_lock);
	if (g_virtual_time) {
		g_virt_ms += ms;
		pthread_mutex_unlock(&g_time_lock);
		sched_yield();
		return;
	}
	pthread_mutex_unlock(&g_time_lock);
	{
		struct timespec ts = {
			.tv_sec = ms / 1000,
			.tv_nsec = (long)(ms % 1000) * 1000000L,
		};
		nanosleep(&ts, NULL);
	}
}

void k_usleep(int32_t us)
{
	if (us <= 0) {
		return;
	}
	pthread_mutex_lock(&g_time_lock);
	if (g_virtual_time) {
		g_virt_ms += (us + 999) / 1000;
		pthread_mutex_unlock(&g_time_lock);
		sched_yield();
		return;
	}
	pthread_mutex_unlock(&g_time_lock);
	usleep((useconds_t)us);
}

void k_busy_wait(uint32_t usec)
{
	k_usleep((int32_t)usec);
}

void sys_reboot(int type)
{
	HOST_LOG(HOST_LOG_ERR, "kernel_host", "sys_reboot(%d) — host exit", type);
	_exit(42);
}

/* --- mutex --- */

void k_mutex_init(struct k_mutex *m)
{
	if (m->ready) {
		return;
	}
	pthread_mutex_init(&m->lock, NULL);
	m->ready = true;
	HOST_LOG(HOST_LOG_TRACE, "kernel_host", "k_mutex_init %p", (void *)m);
}

int k_mutex_lock(struct k_mutex *m, k_timeout_t timeout)
{
	int rc;

	if (!m->ready) {
		k_mutex_init(m);
	}

	HOST_LOG(HOST_LOG_TRACE, "kernel_host", "k_mutex_lock %p timeout=%lld", (void *)m,
		 (long long)timeout);

	if (timeout == K_FOREVER || timeout < 0) {
		rc = pthread_mutex_lock(&m->lock) == 0 ? 0 : -EIO;
	} else if (timeout == K_NO_WAIT) {
		rc = pthread_mutex_trylock(&m->lock) == 0 ? 0 : -EBUSY;
	} else {
		struct timespec abs;

		clock_gettime(CLOCK_REALTIME, &abs);
		abs.tv_sec += timeout / 1000;
		abs.tv_nsec += (long)(timeout % 1000) * 1000000L;
		if (abs.tv_nsec >= 1000000000L) {
			abs.tv_sec++;
			abs.tv_nsec -= 1000000000L;
		}
		rc = pthread_mutex_timedlock(&m->lock, &abs) == 0 ? 0 : -EAGAIN;
	}
	if (rc != 0) {
		HOST_LOG(HOST_LOG_DBG, "kernel_host", "k_mutex_lock %p -> %d", (void *)m, rc);
	}
	return rc;
}

int k_mutex_unlock(struct k_mutex *m)
{
	if (!m->ready) {
		HOST_LOG(HOST_LOG_ERR, "kernel_host", "k_mutex_unlock before init %p", (void *)m);
		return -EIO;
	}
	HOST_LOG(HOST_LOG_TRACE, "kernel_host", "k_mutex_unlock %p", (void *)m);
	return pthread_mutex_unlock(&m->lock) == 0 ? 0 : -EIO;
}

/* --- workqueue --- */

struct host_wq {
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t cv;
	bool running;
	struct k_work_delayable *head;
	unsigned run_count;
};

static struct host_wq *wq_from(struct k_work_q *queue)
{
	return (struct host_wq *)queue->impl;
}

static void *wq_thread(void *arg)
{
	struct host_wq *wq = arg;

	host_debug_set_thread_name("imu_wq");
	HOST_LOG(HOST_LOG_INF, "kernel_host", "workqueue thread started");

	pthread_mutex_lock(&wq->lock);
	while (wq->running) {
		int64_t now = k_uptime_get();
		struct k_work_delayable *ready = NULL;
		struct k_work_delayable **pp = &wq->head;
		int64_t next_due = -1;

		while (*pp) {
			struct k_work_delayable *d = *pp;

			if (d->due_ms <= now) {
				*pp = (struct k_work_delayable *)d->work.next;
				d->work.next = NULL;
				d->pending = false;
				ready = d;
				break;
			}
			if (next_due < 0 || d->due_ms < next_due) {
				next_due = d->due_ms;
			}
			pp = (struct k_work_delayable **)&d->work.next;
		}

		if (ready) {
			k_work_handler_t fn = ready->work.handler;

			wq->run_count++;
			HOST_LOG(HOST_LOG_DBG, "kernel_host",
				 "wq run #%u handler=%p due=%lld now=%lld", wq->run_count,
				 (void *)fn, (long long)ready->due_ms, (long long)now);
			pthread_mutex_unlock(&wq->lock);
			if (fn) {
				fn(&ready->work);
			}
			pthread_mutex_lock(&wq->lock);
			continue;
		}

		if (next_due < 0) {
			HOST_LOG(HOST_LOG_TRACE, "kernel_host", "wq idle wait");
			pthread_cond_wait(&wq->cv, &wq->lock);
		} else {
			struct timespec abs;
			int64_t wait_ms = next_due - k_uptime_get();

			if (wait_ms < 1) {
				wait_ms = 1;
			}
			HOST_LOG(HOST_LOG_TRACE, "kernel_host", "wq timedwait %lldms",
				 (long long)wait_ms);
			clock_gettime(CLOCK_REALTIME, &abs);
			abs.tv_sec += wait_ms / 1000;
			abs.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
			if (abs.tv_nsec >= 1000000000L) {
				abs.tv_sec++;
				abs.tv_nsec -= 1000000000L;
			}
			pthread_cond_timedwait(&wq->cv, &wq->lock, &abs);
		}
	}
	pthread_mutex_unlock(&wq->lock);
	HOST_LOG(HOST_LOG_INF, "kernel_host", "workqueue thread exit");
	return NULL;
}

void k_work_init(struct k_work *work, k_work_handler_t handler)
{
	work->handler = handler;
	work->next = NULL;
}

void k_work_init_delayable(struct k_work_delayable *dwork, k_work_handler_t handler)
{
	k_work_init(&dwork->work, handler);
	dwork->due_ms = 0;
	dwork->pending = false;
	HOST_LOG(HOST_LOG_DBG, "kernel_host", "k_work_init_delayable %p handler=%p", (void *)dwork,
		 (void *)handler);
}

void k_work_queue_start(struct k_work_q *queue, void *stack, size_t stack_size, int prio,
			void *cfg)
{
	(void)stack;
	(void)stack_size;
	(void)cfg;

	struct host_wq *wq = calloc(1, sizeof(*wq));

	pthread_mutex_init(&wq->lock, NULL);
	pthread_cond_init(&wq->cv, NULL);
	wq->running = true;
	queue->impl = wq;
	HOST_LOG(HOST_LOG_INF, "kernel_host", "k_work_queue_start prio=%d stack=%zu", prio,
		 stack_size);
	pthread_create(&wq->thread, NULL, wq_thread, wq);
}

static int schedule_locked(struct host_wq *wq, struct k_work_delayable *dwork, k_timeout_t delay)
{
	int64_t now = k_uptime_get();

	if (dwork->pending) {
		struct k_work_delayable **pp = &wq->head;

		while (*pp) {
			if (*pp == dwork) {
				*pp = (struct k_work_delayable *)dwork->work.next;
				break;
			}
			pp = (struct k_work_delayable **)&(*pp)->work.next;
		}
		HOST_LOG(HOST_LOG_DBG, "kernel_host", "reschedule dwork=%p", (void *)dwork);
	}

	dwork->due_ms = now + (delay < 0 ? 0 : delay);
	dwork->pending = true;
	dwork->work.next = (struct k_work *)wq->head;
	wq->head = dwork;
	HOST_LOG(HOST_LOG_DBG, "kernel_host", "schedule dwork=%p delay=%lld due=%lld",
		 (void *)dwork, (long long)delay, (long long)dwork->due_ms);
	pthread_cond_signal(&wq->cv);
	return 0;
}

int k_work_schedule_for_queue(struct k_work_q *queue, struct k_work_delayable *dwork,
			      k_timeout_t delay)
{
	struct host_wq *wq = wq_from(queue);

	if (wq == NULL) {
		HOST_LOG(HOST_LOG_ERR, "kernel_host", "schedule on null wq");
		return -EINVAL;
	}
	pthread_mutex_lock(&wq->lock);
	int rc = schedule_locked(wq, dwork, delay);
	pthread_mutex_unlock(&wq->lock);
	return rc;
}

int k_work_reschedule_for_queue(struct k_work_q *queue, struct k_work_delayable *dwork,
				k_timeout_t delay)
{
	return k_work_schedule_for_queue(queue, dwork, delay);
}
