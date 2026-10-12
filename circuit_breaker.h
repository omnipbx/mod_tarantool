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
 * circuit_breaker.h -- shared per-host circuit breaker state machine
 * (CLOSED -> OPEN -> HALF_OPEN -> CLOSED/OPEN).
 *
 * This layer is deliberately free of switch_* calls (only POSIX + pthread),
 * so it can be unit-tested standalone (see tests/test_cb.c). The module is
 * built with -std=gnu99, so C11 <stdatomic.h> is NOT available: all shared
 * state uses GCC __atomic_* builtins over plain int/int64_t/uint64_t.
 *
 * Semantics (see README "Circuit breaker"):
 *   - CLOSED:  host is healthy. Every request goes to the network; each
 *              transport failure increments failure_count; at threshold the
 *              breaker opens for cb-timeout ms.
 *   - OPEN:    requests are blocked. mode=wait sleeps until the window ends
 *              (bounded by the query timeout), mode=fast fails immediately.
 *   - HALF_OPEN: after the window expires exactly ONE thread (the "probe
 *              winner") performs a real connect; the rest keep waiting.
 *              A successful probe closes the breaker, a failed one re-opens
 *              it for another window.
 *
 * The breaker is per-host (keyed by addr:port / unix socket path) and shared
 * across all profiles/handles/pools, so a wave of failures from a whole pool
 * counts as ONE open event and the probe race is global (anti-Thundering
 * Herd).
 */

#ifndef TNT_CIRCUIT_BREAKER_H
#define TNT_CIRCUIT_BREAKER_H

#include <pthread.h>
#include <stdint.h>

/* Breaker modes (tarantool.conf.xml <settings> cb-mode). */
typedef enum {
	CB_MODE_OFF = 0,	/* breaker disabled: classic per-conn backoff */
	CB_MODE_WAIT,		/* OPEN: threads sleep until the window ends */
	CB_MODE_FAST		/* OPEN: immediate fail-fast error */
} cb_mode_t;

typedef enum {
	CB_CLOSED = 0,		/* host operational: all traffic allowed */
	CB_OPEN,			/* host down: traffic blocked / delayed */
	CB_HALF_OPEN		/* verification: one thread probes the network */
} cb_state_t;

typedef enum {
	CB_ACTION_ALLOW = 0,	/* route the request to the network */
	CB_ACTION_PROBE,		/* this thread won the race: must probe */
	CB_ACTION_BLOCK_FAST,	/* fail-fast (mode=fast) */
	CB_ACTION_BLOCK_WAIT	/* sleep until the window ends (mode=wait) */
} cb_action_t;

/* Defaults (overridable via <settings>). */
#define CB_FAIL_THRESHOLD_DEFAULT	3
#define CB_TIMEOUT_MS_DEFAULT		5000

typedef struct circuit_breaker {
	/* shared state (all accessed via __atomic_* builtins) */
	int state;				/* cb_state_t */
	int failure_count;		/* consecutive transport failures */
	int64_t open_until;		/* monotonic ms when OPEN expires */
	int mode;				/* cb_mode_t (hot-reloadable) */
	int threshold;			/* failures before OPEN (hot-reloadable) */
	int64_t timeout_ms;		/* OPEN window length (hot-reloadable) */
	int is_probing;			/* 1 = a probe thread is in the network */
	uint64_t metrics_opens;			/* OPEN transitions */
	uint64_t metrics_fail_fast;		/* requests cut without touching the net */
	uint64_t metrics_probes;		/* probe attempts */
	uint64_t metrics_successes;		/* successful requests / probes */
	/* guards state transitions + the cond (only on OPEN/HALF_OPEN paths) */
	pthread_mutex_t mutex;
	pthread_cond_t cond;
} circuit_breaker_t;

/* Monotonic milliseconds (exported for the standalone unit test). */
int64_t cb_now_ms(void);

void cb_init(circuit_breaker_t *cb, cb_mode_t mode, int threshold,
			 int64_t timeout_ms);
void cb_destroy(circuit_breaker_t *cb);

/* Route one request. Never blocks. */
cb_action_t cb_check_and_register(circuit_breaker_t *cb);

/* Sleep while the breaker is OPEN/HALF_OPEN-probing, bounded by max_wait_ms.
 * Returns 0 when the caller may proceed (state CLOSED or the window expired
 * -- the caller must re-check, it may have won the probe race), -1 when the
 * wait budget was exhausted while still blocked. */
int cb_wait_until_open(circuit_breaker_t *cb, int64_t max_wait_ms);

/* Transport failure/success accounting (no-op when mode=off). */
void cb_report_failure(circuit_breaker_t *cb);
void cb_report_success(circuit_breaker_t *cb);

/* Probe outcome: success closes the breaker, failure re-opens it. */
void cb_finalize_probe(circuit_breaker_t *cb, int success);

/* 1 = the host is currently blocked (OPEN window not expired, or a probe is
 * in flight). Side-effect free: never races for the probe. */
int cb_is_blocked(const circuit_breaker_t *cb);

/* Hot-reload: update parameters WITHOUT touching state/counters. */
void cb_configure(circuit_breaker_t *cb, cb_mode_t mode, int threshold,
				  int64_t timeout_ms);
/* CLI reset: back to CLOSED, counters cleared, parameters kept. */
void cb_reset(circuit_breaker_t *cb);

const char *cb_state_name(int state);
const char *cb_mode_name(int mode);

#endif /* TNT_CIRCUIT_BREAKER_H */