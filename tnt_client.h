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
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 * Filippov Anatoliy <error.email@mail.ru>
 *
 * tnt_client.h -- minimal IPROTO binary-protocol client (no libtarantool,
 * no Lua). Implements unix-socket/TCP connect, chap-sha1 authentication,
 * IPROTO_EXECUTE and IPROTO_PING, bounded send/recv timeouts, pre-read
 * broken-connection detection, and multi-host failover with exponential
 * backoff.
 *
 * This layer is deliberately free of switch_* calls (only POSIX + malloc),
 * so it can be unit-tested standalone. Error reporting: functions return
 * 0 on success, 1 on a protocol/SQL error (details in the result), -1 on a
 * transport failure; tnt_conn_last_error()/tnt_result_error() carry a
 * human-readable message.
 *
 */

#ifndef TNT_CLIENT_H
#define TNT_CLIENT_H

#include <stddef.h>
#include <stdint.h>
#include <switch.h>
#include "circuit_breaker.h"
#include "tnt_sql.h"

/* Per-profile failover list cap (<hosts><host> in tarantool.conf.xml).
 * Bounds tnt_profile_cfg_t.hosts[], tnt_session_t.dead_until_host[] and
 * backoff_host[]. Hosts beyond this limit are skipped with a warning,
 * never fatal. */
#define TNT_HOST_MAX	8
#define TNT_ADDR_MAX	256

/* Per-handle connection pool cap (<param name="pool-size"/> in a
 * <profile>). Each handle keeps up to this many independent sessions
 * (each with its own failover state and prepared-statement cache) and
 * distributes statements round-robin. 1 = the classic one-conn-per-handle
 * behaviour. */
#define TNT_POOL_MAX	8

typedef enum {
	TNT_ROLE_ANY = 0,
	TNT_ROLE_MASTER,
	TNT_ROLE_REPLICA
} tnt_role_t;

/* Static host description, parsed from tarantool.conf.xml. */
typedef struct tnt_host_cfg {
	char addr[TNT_ADDR_MAX];	/* host name / IP or unix socket path ('/') */
	uint16_t port;				/* TCP port (ignored for unix sockets) */
	int use_unix;				/* addr is a unix socket path */
	tnt_role_t role;
	char version[16];			/* requested server generation, e.g. "3.2" */
	char tls_profile[64];		/* TLS profile name (TLS deferred) */
	int prepare;				/* 1 = try IPROTO_PREPARE on this host;
								 * resolved at parse time (host <param>
								 * overrides the profile default) */
} tnt_host_cfg_t;

/* A profile is an independent connection pool keyed by DSN name
 * (tarantool://<name>). Contains an ordered failover host list. */
typedef struct tnt_profile_cfg {
	char name[64];
	char username[64];
	char password[128];
	tnt_host_cfg_t hosts[TNT_HOST_MAX];
	int nhosts;
	uint32_t connect_timeout_ms;	/* total connect+auth budget per attempt */
	uint32_t query_timeout_ms;		/* per request send/recv timeout */
	uint32_t ping_idle_s;			/* idle seconds before issuing PING */
	uint32_t ping_timeout_s;		/* PING wait before declaring suspect */
	uint32_t backoff_max_s;			/* cap for exponential backoff */
	int skip_index;					/* 1 = suppress CREATE INDEX (indexes managed manually) */
	int full_scan;					/* 1 = run SET SESSION sql_full_scan=true (2.11/3.x)
									   or sql_seq_scan=true (older 2.x) on every connect */
	int prepare;					/* default for hosts without their own
									 * <param name="prepare"/> */
	int pool_size;					/* connections per handle (1..TNT_POOL_MAX);
									 * 1 = classic one-conn-per-handle */
	char init_sql[512];				/* extra SQL run on every new connection; "" = none */
} tnt_profile_cfg_t;

/* Decoded result of one IPROTO_EXECUTE. All strings are NUL-terminated
 * copies owned by the result; NULL database values become "". */
typedef struct tnt_result {
	char **col_name;			/* [ncols] */
	char **col_type;			/* [ncols] */
	char ***rows;				/* [nrows][ncols] */
	int ncols;
	int nrows;
	int affected_rows;			/* from SQL_INFO row_count (-1 if absent) */
	char error[512];			/* protocol/SQL error message, if any */
	uint32_t stmt_id;			/* IPROTO_STMT_ID from a PREPARE response */
	int has_stmt_id;			/* 1 = stmt_id was present in the response */
} tnt_result_t;

/* ---- prepared statements (per-connection: stmt_id is conn-bound) ---- */

/* Per-connection prepared-statement cache capacity. Live traffic shows
 * ~69 distinct templates with the top 20 covering ~90% of executions, so
 * 128 entries with FIFO eviction are ample. */
#define TNT_PREP_MAX	128

typedef struct tnt_prep_entry {
	uint64_t hash;				/* FNV-1a 64 of the TRANSLATED template */
	uint32_t stmt_id;			/* server-side prepared statement id */
} tnt_prep_entry_t;

/* ---- low level: one connection ---- */

typedef struct tnt_conn {
	int fd;
	int use_unix;
	char addr[TNT_ADDR_MAX];
	uint16_t port;
	char user[64];
	char pass[128];
	uint32_t rw_timeout_ms;
	int64_t last_used_mono;		/* CLOCK_MONOTONIC ms */
	uint8_t *rsp;				/* growable response buffer */
	size_t rsp_cap;
	size_t rsp_len;
	char error[512];
	uint64_t sync;
	uint8_t salt[64];			/* server salt from the greeting (for AUTH) */
	uint8_t salt_len;			/* 20 (chap-sha1) or 32 (chap-sha256) */
	tnt_prep_entry_t prep[TNT_PREP_MAX];	/* prepared stmt_id cache */
	int nprep;					/* used prep[] slots (FIFO eviction) */
	int prepare_broken;			/* 1 = server rejected PREPARE: never retry */
	int prep_fail_run;			/* consecutive PREPARE failures */
	int prepare_allowed;		/* host config flag (runtime override via
								 * 'tarantool prepare on/off' until reconnect) */
	int server_major;			/* REAL server version from the greeting */
	int server_minor;			/*   (0 when the greeting was not parseable) */
	int prepare_supported;		/* 1 = greeting says >= 2.11 (box.prepare
								 * exists); 0 = do not even try */
} tnt_conn_t;

SWITCH_DECLARE(void) tnt_conn_init(tnt_conn_t *c, const tnt_host_cfg_t *h,
								   const char *user, const char *pass,
								   uint32_t rw_timeout_ms);
SWITCH_DECLARE(int) tnt_conn_connect(tnt_conn_t *c, uint32_t connect_timeout_ms);
SWITCH_DECLARE(void) tnt_conn_close(tnt_conn_t *c);
SWITCH_DECLARE(int) tnt_conn_is_open(const tnt_conn_t *c);

/* 1 = server closed / has pending data (broken), 0 = healthy, -1 = error */
SWITCH_DECLARE(int) tnt_conn_precheck(tnt_conn_t *c);

/* Parse "Tarantool X.Y[.Z] ..." from the greeting's first line. Returns 1
 * on success (major and minor are set), 0 when no version was extracted. */
SWITCH_DECLARE(int) tnt_parse_server_version(const char *line,
											 int *major, int *minor);

/* 0 = ok, 1 = SQL/protocol error (res->error), -1 = transport error */
SWITCH_DECLARE(int) tnt_conn_execute(tnt_conn_t *c, const char *sql, tnt_result_t *res);
/* 0 = pong, -1 = transport error */
SWITCH_DECLARE(int) tnt_conn_ping(tnt_conn_t *c);

/* ---- prepared statements (IPROTO_PREPARE + EXECUTE with STMT_ID) ---- */

/* PREPARE an SQL text with '?' positional parameters. 0 = ok (*stmt_id
 * set, response also carries bind metadata), 1 = SQL error (c->error),
 * -1 = transport error. */
SWITCH_DECLARE(int) tnt_conn_prepare(tnt_conn_t *c, const char *sql_q,
									 uint32_t *stmt_id);

/* EXECUTE a prepared statement with a pre-encoded msgpack bind array
 * (see tnt_bind_encode). bind may be NULL when the statement has no
 * parameters. Return codes match tnt_conn_execute. */
SWITCH_DECLARE(int) tnt_conn_exec_stmt(tnt_conn_t *c, uint32_t stmt_id,
									   const uint8_t *bind, size_t bind_len,
									   tnt_result_t *res);

/* Encode tnt_sql_val_t literals into the IPROTO_SQL_BIND msgpack array.
 * Strings become MP_STR, integers MP_INT/MP_UINT, floats MP_DOUBLE.
 * buf is caller-allocated (worst case ~517 * nvals bytes). 0 = ok,
 * -1 = does not fit. */
SWITCH_DECLARE(int) tnt_bind_encode(const tnt_sql_val_t *vals, int nvals,
									uint8_t *buf, size_t cap, size_t *blen);

/* prep[] cache operations (single-threaded per connection: the caller
 * serializes handle usage). */
void tnt_conn_prep_reset(tnt_conn_t *c);
/* 1 = hit (*stmt_id set), 0 = miss */
int tnt_conn_prep_lookup(const tnt_conn_t *c, uint64_t hash, uint32_t *stmt_id);
/* Insert (FIFO eviction when full); replaces an existing hash. */
void tnt_conn_prep_insert(tnt_conn_t *c, uint64_t hash, uint32_t stmt_id);
/* Remove one entry (schema/protocol errors on EXECUTE). */
void tnt_conn_prep_drop(tnt_conn_t *c, uint64_t hash);

SWITCH_DECLARE(const char *) tnt_conn_last_error(const tnt_conn_t *c);

/* ---- results ---- */

SWITCH_DECLARE(int) tnt_result_init(tnt_result_t *res, int ncols, int nrows);
SWITCH_DECLARE(void) tnt_result_free(tnt_result_t *res);

/* ---- session: one active host + failover/backoff ---- */

typedef struct tnt_session {
	const tnt_profile_cfg_t *cfg;	/* borrowed; owned by the module */
	char user[64];
	char pass[128];
	int cur;						/* index of the currently used host */
	tnt_conn_t conn;
	int64_t dead_until_host[TNT_HOST_MAX];	/* ms; per-host retry barrier */
	uint32_t backoff_host[TNT_HOST_MAX];	/* ms; per-host current backoff */
	int all_dead;
	int broken;						/* 1 = connection lost, waiting for recovery */
} tnt_session_t;

SWITCH_DECLARE(void) tnt_session_init(tnt_session_t *s, const tnt_profile_cfg_t *cfg);
/* Connect to the first healthy host; -1 if all are down. */
SWITCH_DECLARE(int) tnt_session_open(tnt_session_t *s);
SWITCH_DECLARE(void) tnt_session_close(tnt_session_t *s);

/* Execute with failover. retryable must be true only for read-only
 * statements (writes are never retried after a transport error to avoid
 * duplicates). 0 = ok, 1 = SQL error, -1 = transport error (all hosts dead). */
SWITCH_DECLARE(int) tnt_session_execute(tnt_session_t *s, const char *sql,
										int retryable, tnt_result_t *res);
/* Execute a prepared statement on the CURRENT connection only - no
 * failover (stmt_id is conn-bound). 0 = ok, 1 = SQL error, -1 = transport
 * failure (the caller falls back to tnt_session_execute, which restores
 * the session; prep[] is cleared by tnt_conn_init on every reconnect). */
SWITCH_DECLARE(int) tnt_session_exec_prepared(tnt_session_t *s, uint32_t stmt_id,
											  const uint8_t *bind, size_t bind_len,
											  tnt_result_t *res);
SWITCH_DECLARE(int) tnt_session_ping(tnt_session_t *s);

/* Fire a "mod_tarantool" custom event (subclass "mod_tarantool",
 * "action" header, optional "profile"/"detail"). */
SWITCH_DECLARE(void) tnt_event_emit(const char *action, const char *profile, const char *detail);

/* ---- shared circuit breaker registry (per-host, keyed by addr:port) ---- */

/* Global per-host breaker cap. Hosts beyond this limit share no breaker
 * (they fall back to the classic per-conn backoff). */
#define TNT_CB_MAX	32

/* Find (or lazily create) the breaker for a host. Never returns NULL while
 * the registry has free slots; the breaker inherits the current global
 * mode/threshold/timeout. */
SWITCH_DECLARE(circuit_breaker_t *) tnt_cb_get(const char *addr, uint16_t port,
											   int use_unix);
/* 1 = cb-mode != off (breaker gates connect/open paths). */
SWITCH_DECLARE(int) tnt_cb_enabled(void);
/* Hot-reload: update the global parameters AND every existing breaker
 * WITHOUT touching their state/counters. */
SWITCH_DECLARE(void) tnt_cb_configure(cb_mode_t mode, int threshold,
									  int64_t timeout_ms);
/* CLI reset: every breaker back to CLOSED, counters cleared. */
SWITCH_DECLARE(void) tnt_cb_reset_all(void);
/* Render the whole registry into buf (NUL-terminated). Returns bytes
 * written (may be truncated at buflen-1). */
SWITCH_DECLARE(int) tnt_cb_show(char *buf, size_t buflen);

#endif /* TNT_CLIENT_H */
