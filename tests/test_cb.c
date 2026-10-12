/*
 * mod_tarantool -- standalone unit test for the shared circuit breaker.
 *
 * Build (from the module directory):
 *   gcc -O2 -Wall -Wextra -Werror -std=gnu99 -D_GNU_SOURCE \
 *       -I.. -o test_cb test_cb.c ../circuit_breaker.c -lpthread
 *
 * Covers:
 *   - mode=off: breaker never blocks, failures do not change state;
 *   - mode=fast: OPEN returns CB_ACTION_BLOCK_FAST;
 *   - threshold: 3 consecutive failures open the breaker exactly once;
 *   - success resets the failure counter;
 *   - failed probe re-opens the breaker for a fresh window;
 *   - high-contention race: 16 threads fired simultaneously from a barrier
 *     yield EXACTLY ONE probe winner, 15 waiters, final state CLOSED.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "circuit_breaker.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#define THREAD_COUNT 16

static pthread_barrier_t startup_barrier;
static circuit_breaker_t test_cb;
static int probe_winners;
static int wait_losers;

static void *concurrent_worker(void *arg)
{
	cb_action_t action;

	(void)arg;
	pthread_barrier_wait(&startup_barrier);

	action = cb_check_and_register(&test_cb);
	if (action == CB_ACTION_PROBE) {
		__atomic_fetch_add(&probe_winners, 1, __ATOMIC_RELAXED);
		usleep(50000);	/* emulate network probing lag */
		cb_finalize_probe(&test_cb, 1);	/* revive the node */
	} else if (action == CB_ACTION_BLOCK_WAIT) {
		__atomic_fetch_add(&wait_losers, 1, __ATOMIC_RELAXED);
		cb_wait_until_open(&test_cb, 5000);
	}
	return NULL;
}

static void test_off(void)
{
	circuit_breaker_t cb;

	cb_init(&cb, CB_MODE_OFF, 3, 1000);
	__atomic_store_n(&cb.state, CB_OPEN, __ATOMIC_RELEASE);
	__atomic_store_n(&cb.open_until, cb_now_ms() + 5000, __ATOMIC_RELEASE);
	assert(cb_check_and_register(&cb) == CB_ACTION_ALLOW);
	cb_report_failure(&cb);
	assert(__atomic_load_n(&cb.state, __ATOMIC_ACQUIRE) == CB_OPEN);
	cb_destroy(&cb);
	printf("[OFF] mode=off never blocks, state untouched: ok\n");
}

static void test_fast(void)
{
	circuit_breaker_t cb;

	cb_init(&cb, CB_MODE_FAST, 3, 1000);
	__atomic_store_n(&cb.state, CB_OPEN, __ATOMIC_RELEASE);
	__atomic_store_n(&cb.open_until, cb_now_ms() + 5000, __ATOMIC_RELEASE);
	assert(cb_check_and_register(&cb) == CB_ACTION_BLOCK_FAST);
	cb_destroy(&cb);
	printf("[FAST] OPEN -> BLOCK_FAST: ok\n");
}

static void test_threshold(void)
{
	circuit_breaker_t cb;

	cb_init(&cb, CB_MODE_WAIT, 3, 1000);
	assert(cb_check_and_register(&cb) == CB_ACTION_ALLOW);
	cb_report_failure(&cb);
	cb_report_failure(&cb);
	assert(__atomic_load_n(&cb.state, __ATOMIC_ACQUIRE) == CB_CLOSED);
	/* success resets the counter while still CLOSED */
	cb_report_success(&cb);
	assert(__atomic_load_n(&cb.failure_count, __ATOMIC_ACQUIRE) == 0);
	cb_report_failure(&cb);
	cb_report_failure(&cb);
	cb_report_failure(&cb);
	assert(__atomic_load_n(&cb.state, __ATOMIC_ACQUIRE) == CB_OPEN);
	assert(__atomic_load_n(&cb.metrics_opens, __ATOMIC_RELAXED) == 1);
	cb_destroy(&cb);
	printf("[THRESHOLD] 3 failures -> OPEN once, success resets: ok\n");
}

static void test_probe_fail(void)
{
	circuit_breaker_t cb;

	cb_init(&cb, CB_MODE_WAIT, 3, 1000);
	__atomic_store_n(&cb.state, CB_OPEN, __ATOMIC_RELEASE);
	__atomic_store_n(&cb.open_until, cb_now_ms() - 10, __ATOMIC_RELEASE);
	assert(cb_check_and_register(&cb) == CB_ACTION_PROBE);
	cb_finalize_probe(&cb, 0);
	assert(__atomic_load_n(&cb.state, __ATOMIC_ACQUIRE) == CB_OPEN);
	assert(__atomic_load_n(&cb.open_until, __ATOMIC_ACQUIRE) > cb_now_ms());
	assert(cb_check_and_register(&cb) == CB_ACTION_BLOCK_WAIT);
	cb_destroy(&cb);
	printf("[PROBE_FAIL] failed probe re-opens for a fresh window: ok\n");
}

static void test_race(void)
{
	pthread_t threads[THREAD_COUNT];
	int i;

	cb_init(&test_cb, CB_MODE_WAIT, 3, 2000);
	/* force an already-expired OPEN state */
	__atomic_store_n(&test_cb.state, CB_OPEN, __ATOMIC_RELEASE);
	__atomic_store_n(&test_cb.open_until, cb_now_ms() - 10, __ATOMIC_RELEASE);
	__atomic_store_n(&probe_winners, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&wait_losers, 0, __ATOMIC_RELAXED);

	pthread_barrier_init(&startup_barrier, NULL, THREAD_COUNT);
	for (i = 0; i < THREAD_COUNT; i++)
		pthread_create(&threads[i], NULL, concurrent_worker, NULL);
	for (i = 0; i < THREAD_COUNT; i++)
		pthread_join(threads[i], NULL);
	pthread_barrier_destroy(&startup_barrier);

	printf("[RACE] probe winners=%d (expected 1), wait losers=%d (expected %d), "
		   "final state=%s (expected closed)\n",
		   __atomic_load_n(&probe_winners, __ATOMIC_RELAXED),
		   __atomic_load_n(&wait_losers, __ATOMIC_RELAXED), THREAD_COUNT - 1,
		   cb_state_name(__atomic_load_n(&test_cb.state, __ATOMIC_ACQUIRE)));
	assert(__atomic_load_n(&probe_winners, __ATOMIC_RELAXED) == 1);
	assert(__atomic_load_n(&wait_losers, __ATOMIC_RELAXED) == THREAD_COUNT - 1);
	assert(__atomic_load_n(&test_cb.state, __ATOMIC_ACQUIRE) == CB_CLOSED);
	cb_destroy(&test_cb);
}

int main(void)
{
	printf("[RUNNING] circuit breaker unit tests...\n");
	test_off();
	test_fast();
	test_threshold();
	test_probe_fail();
	test_race();
	printf("[SUCCESS] all circuit breaker tests passed\n");
	return 0;
}