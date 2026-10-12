/*
 * mod_tarantool for FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2026, Filippov Anatoliy <error.email@mail.ru>
 *
 * Version: MPL 2.0
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 2.0 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * circuit_breaker.c -- shared per-host circuit breaker state machine.
 *
 * Lock-free fast path: in CLOSED state cb_check_and_register() performs a
 * single __atomic_load_n and returns CB_ACTION_ALLOW without touching the
 * mutex. All transitions into/out of OPEN/HALF_OPEN are serialized by the
 * per-breaker mutex; waiters sleep on the cond and are woken by
 * cb_finalize_probe() via pthread_cond_broadcast.
 *
 * Built with -std=gnu99: no <stdatomic.h>, GCC __atomic_* builtins only.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "circuit_breaker.h"

#include <string.h>
#include <time.h>

int64_t cb_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void cb_init(circuit_breaker_t *cb, cb_mode_t mode, int threshold,
			 int64_t timeout_ms)
{
	memset(cb, 0, sizeof(*cb));
	__atomic_store_n(&cb->state, CB_CLOSED, __ATOMIC_RELEASE);
	__atomic_store_n(&cb->failure_count, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&cb->open_until, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&cb->mode, (int)mode, __ATOMIC_RELEASE);
	__atomic_store_n(&cb->threshold,
					 threshold > 0 ? threshold : CB_FAIL_THRESHOLD_DEFAULT,
					 __ATOMIC_RELEASE);
	__atomic_store_n(&cb->timeout_ms,
					 timeout_ms > 0 ? timeout_ms : CB_TIMEOUT_MS_DEFAULT,
					 __ATOMIC_RELEASE);
	__atomic_store_n(&cb->is_probing, 0, __ATOMIC_RELEASE);
	pthread_mutex_init(&cb->mutex, NULL);
	pthread_cond_init(&cb->cond, NULL);
}

void cb_destroy(circuit_breaker_t *cb)
{
	pthread_mutex_destroy(&cb->mutex);
	pthread_cond_destroy(&cb->cond);
}

static cb_action_t block_action(circuit_breaker_t *cb)
{
	__atomic_fetch_add(&cb->metrics_fail_fast, 1, __ATOMIC_RELAXED);
	return __atomic_load_n(&cb->mode, __ATOMIC_ACQUIRE) == CB_MODE_FAST
		? CB_ACTION_BLOCK_FAST : CB_ACTION_BLOCK_WAIT;
}

cb_action_t cb_check_and_register(circuit_breaker_t *cb)
{
	int st;
	int64_t now;

	if (__atomic_load_n(&cb->mode, __ATOMIC_ACQUIRE) == CB_MODE_OFF)
		return CB_ACTION_ALLOW;

	st = __atomic_load_n(&cb->state, __ATOMIC_ACQUIRE);
	now = cb_now_ms();

	if (st == CB_CLOSED)
		return CB_ACTION_ALLOW;

	if (st == CB_OPEN) {
		if (now < __atomic_load_n(&cb->open_until, __ATOMIC_ACQUIRE))
			return block_action(cb);

		/* window expired: race for the single probe */
		pthread_mutex_lock(&cb->mutex);
		if (__atomic_load_n(&cb->state, __ATOMIC_ACQUIRE) == CB_OPEN) {
			__atomic_store_n(&cb->state, CB_HALF_OPEN, __ATOMIC_RELEASE);
			__atomic_store_n(&cb->is_probing, 1, __ATOMIC_RELEASE);
			__atomic_fetch_add(&cb->metrics_probes, 1, __ATOMIC_RELAXED);
			pthread_mutex_unlock(&cb->mutex);
			return CB_ACTION_PROBE;
		}
		pthread_mutex_unlock(&cb->mutex);
	}

	if (__atomic_load_n(&cb->state, __ATOMIC_ACQUIRE) == CB_HALF_OPEN) {
		if (__atomic_load_n(&cb->is_probing, __ATOMIC_ACQUIRE))
			return block_action(cb);

		pthread_mutex_lock(&cb->mutex);
		if (!__atomic_load_n(&cb->is_probing, __ATOMIC_ACQUIRE)) {
			__atomic_store_n(&cb->is_probing, 1, __ATOMIC_RELEASE);
			__atomic_fetch_add(&cb->metrics_probes, 1, __ATOMIC_RELAXED);
			pthread_mutex_unlock(&cb->mutex);
			return CB_ACTION_PROBE;
		}
		pthread_mutex_unlock(&cb->mutex);
	}

	return block_action(cb);
}

int cb_wait_until_open(circuit_breaker_t *cb, int64_t max_wait_ms)
{
	int64_t deadline = cb_now_ms() + (max_wait_ms > 0 ? max_wait_ms : 50);

	for (;;) {
		int st = __atomic_load_n(&cb->state, __ATOMIC_ACQUIRE);
		int64_t now = cb_now_ms();
		int64_t open_until = __atomic_load_n(&cb->open_until, __ATOMIC_ACQUIRE);
		int64_t wait_ms;

		if (st == CB_CLOSED)
			return 0;
		if (st == CB_OPEN && now >= open_until)
			return 0;	/* window expired: caller re-checks (may win the probe) */
		if (now >= deadline)
			return -1;	/* wait budget exhausted while still blocked */

		wait_ms = open_until - now;
		if (wait_ms <= 0)
			wait_ms = 50;	/* HALF_OPEN probing: poll in small steps */
		if (deadline - now < wait_ms)
			wait_ms = deadline - now;
		if (wait_ms <= 0)
			return -1;

		/* sleep on the cond; cb_finalize_probe() broadcasts on completion */
		{
			struct timespec ts;

			clock_gettime(CLOCK_MONOTONIC, &ts);
			ts.tv_sec += wait_ms / 1000;
			ts.tv_nsec += (wait_ms % 1000) * 1000000;
			if (ts.tv_nsec >= 1000000000) {
				ts.tv_sec++;
				ts.tv_nsec -= 1000000000;
			}
			pthread_mutex_lock(&cb->mutex);
			pthread_cond_timedwait(&cb->cond, &cb->mutex, &ts);
			pthread_mutex_unlock(&cb->mutex);
		}
	}
}

void cb_report_failure(circuit_breaker_t *cb)
{
	int fails;

	if (__atomic_load_n(&cb->mode, __ATOMIC_ACQUIRE) == CB_MODE_OFF)
		return;
	if (__atomic_load_n(&cb->state, __ATOMIC_ACQUIRE) != CB_CLOSED)
		return;

	fails = __atomic_fetch_add(&cb->failure_count, 1, __ATOMIC_RELAXED) + 1;
	if (fails >= __atomic_load_n(&cb->threshold, __ATOMIC_ACQUIRE)) {
		pthread_mutex_lock(&cb->mutex);
		if (__atomic_load_n(&cb->state, __ATOMIC_ACQUIRE) == CB_CLOSED) {
			__atomic_store_n(&cb->state, CB_OPEN, __ATOMIC_RELEASE);
			__atomic_store_n(&cb->open_until,
							 cb_now_ms() + __atomic_load_n(&cb->timeout_ms, __ATOMIC_ACQUIRE),
							 __ATOMIC_RELEASE);
			__atomic_fetch_add(&cb->metrics_opens, 1, __ATOMIC_RELAXED);
		}
		pthread_mutex_unlock(&cb->mutex);
	}
}

void cb_report_success(circuit_breaker_t *cb)
{
	if (__atomic_load_n(&cb->mode, __ATOMIC_ACQUIRE) == CB_MODE_OFF)
		return;
	if (__atomic_load_n(&cb->state, __ATOMIC_ACQUIRE) == CB_CLOSED) {
		__atomic_store_n(&cb->failure_count, 0, __ATOMIC_RELEASE);
		__atomic_fetch_add(&cb->metrics_successes, 1, __ATOMIC_RELAXED);
	}
}

void cb_finalize_probe(circuit_breaker_t *cb, int success)
{
	pthread_mutex_lock(&cb->mutex);
	__atomic_store_n(&cb->is_probing, 0, __ATOMIC_RELEASE);
	if (success) {
		__atomic_store_n(&cb->state, CB_CLOSED, __ATOMIC_RELEASE);
		__atomic_store_n(&cb->failure_count, 0, __ATOMIC_RELEASE);
		__atomic_store_n(&cb->open_until, 0, __ATOMIC_RELEASE);
		__atomic_fetch_add(&cb->metrics_successes, 1, __ATOMIC_RELAXED);
	} else {
		__atomic_store_n(&cb->state, CB_OPEN, __ATOMIC_RELEASE);
		__atomic_store_n(&cb->open_until,
						 cb_now_ms() + __atomic_load_n(&cb->timeout_ms, __ATOMIC_ACQUIRE),
						 __ATOMIC_RELEASE);
		__atomic_fetch_add(&cb->metrics_opens, 1, __ATOMIC_RELAXED);
	}
	pthread_cond_broadcast(&cb->cond);
	pthread_mutex_unlock(&cb->mutex);
}

int cb_is_blocked(const circuit_breaker_t *cb)
{
	int st;

	if (__atomic_load_n(&cb->mode, __ATOMIC_ACQUIRE) == CB_MODE_OFF)
		return 0;
	st = __atomic_load_n(&cb->state, __ATOMIC_ACQUIRE);
	if (st == CB_OPEN) {
		if (cb_now_ms() < __atomic_load_n(&cb->open_until, __ATOMIC_ACQUIRE))
			return 1;
		return 0;	/* expired: will race for the probe in try_host */
	}
	if (st == CB_HALF_OPEN)
		return __atomic_load_n(&cb->is_probing, __ATOMIC_ACQUIRE) ? 1 : 0;
	return 0;
}

void cb_configure(circuit_breaker_t *cb, cb_mode_t mode, int threshold,
				  int64_t timeout_ms)
{
	__atomic_store_n(&cb->mode, (int)mode, __ATOMIC_RELEASE);
	__atomic_store_n(&cb->threshold,
					 threshold > 0 ? threshold : CB_FAIL_THRESHOLD_DEFAULT,
					 __ATOMIC_RELEASE);
	__atomic_store_n(&cb->timeout_ms,
					 timeout_ms > 0 ? timeout_ms : CB_TIMEOUT_MS_DEFAULT,
					 __ATOMIC_RELEASE);
}

void cb_reset(circuit_breaker_t *cb)
{
	pthread_mutex_lock(&cb->mutex);
	__atomic_store_n(&cb->state, CB_CLOSED, __ATOMIC_RELEASE);
	__atomic_store_n(&cb->failure_count, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&cb->open_until, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&cb->is_probing, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&cb->metrics_opens, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&cb->metrics_fail_fast, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&cb->metrics_probes, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&cb->metrics_successes, 0, __ATOMIC_RELAXED);
	pthread_cond_broadcast(&cb->cond);
	pthread_mutex_unlock(&cb->mutex);
}

const char *cb_state_name(int state)
{
	switch (state) {
	case CB_CLOSED:
		return "closed";
	case CB_OPEN:
		return "open";
	case CB_HALF_OPEN:
		return "half-open";
	}
	return "?";
}

const char *cb_mode_name(int mode)
{
	switch (mode) {
	case CB_MODE_OFF:
		return "off";
	case CB_MODE_WAIT:
		return "wait";
	case CB_MODE_FAST:
		return "fast";
	}
	return "?";
}