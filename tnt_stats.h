/*
 * mod_tarantool for FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2026, Filippov Anatoliy <error.email@mail.ru>
 *
 * Version: MPL 2.0
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 2.0 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/2.0
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
 * tnt_stats.h -- runtime SQL statistics for the API command family
 * "tarantool sqlstats ..." (pure layer, no switch_* calls, unit-testable
 * standalone; the caller owns locking, e.g. a dedicated stats mutex).
 *
 * Purpose: measure on live traffic how much of the SQL stream consists of
 * repeated templates, to size (a) the module-side translate cache and
 * (b) a possible IPROTO_PREPARE+bind layer, WITHOUT dumping raw SQL to a log
 * (only normalized samples are kept - no PII in the CLI output).
 *
 * Two counting levels per statement:
 *   A. (type x table) totals  - cheap, shows WHERE the traffic goes;
 *   B. normalized templates   - literals ('...', bare numbers) are masked
 *      to '?', keywords lowercased, whitespace collapsed, comments dropped,
 *      then hashed (FNV-1a 64). distinct/top-hit per type and per table are
 *      aggregated from level B, so a high total / low distinct ratio directly
 *      predicts the cache win.
 *
 * Statement (type, table) comes from tnt_stmt_t (filled by tnt_sql_split /
 * tnt_sql_classify). SELECT is analyzed here: the first top-level FROM
 * identifier becomes the table (a view name shows up as-is); a parenthesized
 * FROM reports "*subquery*"; a top-level JOIN/comma after the first table
 * appends the marker " (join)"; anything undeterminable is "*unknown*".
 *
 * Table filter ("tarantool sqlstats tables ..."):
 *   - replace semantics: every "tables <t1> [t2...]" call REPLACES the list;
 *   - names are lowercased, duplicates fold silently, limit 32;
 *   - NULL/empty list = "all" (default); "tables all" restores it;
 *   - the filter is checked BEFORE normalization/hashing, so an off-list
 *     table costs only the name scan, not a hash.
 *
 * reset() clears the counters AND restores the "all" filter, but does not
 * touch the enabled flag (accumulation keeps running if it was on).
 */

#ifndef TNT_STATS_H
#define TNT_STATS_H

#include <stddef.h>
#include <stdint.h>
#include "tnt_sql.h"

/* filter list cap ("tarantool sqlstats tables t1 t2 ...") */
#define TNT_STATS_TABLES_MAX	32
/* level B template cap; ~290 bytes per entry => ~1.2 MB worst case */
#define TNT_STATS_TEMPLATES_MAX	4096
/* level A (type x table) pair cap */
#define TNT_STATS_TT_MAX		128
/* normalized name / sample lengths */
#define TNT_STATS_NAME_MAX		64
#define TNT_STATS_SAMPLE_MAX	200

/* one level-B entry: a normalized SQL template */
typedef struct tnt_stats_templ {
	uint64_t hash;							/* FNV-1a 64 of the normalized text */
	uint64_t count;							/* statements matching this template */
	tnt_stmt_type_t type;					/* statement type of the first sighting */
	char table[TNT_STATS_NAME_MAX + 8];		/* lower-case table name */
	char sample[TNT_STATS_SAMPLE_MAX];		/* normalized sample (NUL-terminated) */
} tnt_stats_templ_t;

/* one level-A entry: (type, table) -> statement count */
typedef struct tnt_stats_tt {
	tnt_stmt_type_t type;
	char table[TNT_STATS_NAME_MAX + 8];
	uint64_t total;
} tnt_stats_tt_t;

typedef struct tnt_stats {
	int enabled;			/* "tarantool sqlstats on|off" */
	int ntables;			/* filter list size; -1 = all tables (default) */
	char tables[TNT_STATS_TABLES_MAX][TNT_STATS_NAME_MAX];

	/* level A */
	int ntt;
	tnt_stats_tt_t tt[TNT_STATS_TT_MAX];
	uint64_t tt_overflow;	/* (type,table) pairs beyond the cap (summed here) */

	/* level B */
	int ntempl;
	tnt_stats_templ_t templ[TNT_STATS_TEMPLATES_MAX];
	uint64_t templ_overflow;/* statements whose template exceeded the cap */

	/* global counters (statements, not SQL strings) */
	uint64_t total_all;		/* every statement seen while enabled */
	uint64_t total_counted;	/* passed the table filter */
	uint64_t filtered_out;	/* rejected by the table filter */
} tnt_stats_t;

/* Zero the state; filter defaults to "all"; enabled = 0. */
void tnt_stats_init(tnt_stats_t *s);

/* Replace the filter list (replace semantics, lower-case, silent dedup).
 * names==NULL or n==0 selects "all". Returns the number of names applied;
 * when more than TNT_STATS_TABLES_MAX names are given, exactly the first
 * TNT_STATS_TABLES_MAX are applied and the excess is ignored. */
int tnt_stats_tables_set(tnt_stats_t *s, const char *const *names, int n);

/* 1 = the statement should be counted. "*"-prefixed pseudo-names always
 * pass the filter (they denote "not determinable", never a real table). */
int tnt_stats_filter_match(const tnt_stats_t *s, const char *table);

/* Clear all counters and restore the "all" filter; keeps `enabled`. */
void tnt_stats_reset(tnt_stats_t *s);

/* Account one batch of statements (as produced by tnt_sql_split on the
 * ORIGINAL sql string; stmts[i].sql must point at it). No-op when disabled.
 * Thread-safety: the caller serializes (e.g. a stats mutex). */
void tnt_stats_record(tnt_stats_t *s, const tnt_stmt_t *stmts, int n);

/* Account a single statement span. Exposed for unit tests. */
void tnt_stats_record_stmt(tnt_stats_t *s, const tnt_stmt_t *stmt);

/* Normalize one statement span into buf (NUL-terminated, <= cap incl NUL).
 * Literals -> '?', bare numbers -> '?', quoted identifiers kept verbatim,
 * comments dropped, keywords lowercased, whitespace collapsed to single
 * spaces. Returns the length written (excluding NUL). Exposed for tests. */
size_t tnt_stats_normalize(const char *sql, size_t start, size_t end,
						   char *buf, size_t cap);

/* Extract the lower-case table name for a statement span (SELECT included:
 * first top-level FROM identifier, "*subquery*" for FROM (, " (join)"
 * appended when a top-level JOIN/comma follows, "*unknown*" when neither
 * tnt_stmt_t.table nor the FROM scan yields a name). Returns buf. Exposed
 * for tests. buf must be at least TNT_STATS_NAME_MAX+8 bytes. */
const char *tnt_stats_extract(const tnt_stmt_t *stmt, const char *sql,
							  char *buf, size_t cap);

/* Build the human-readable report ("tarantool sqlstats show [N] [table]").
 * only_table (NULL/"" = no filter) restricts the per-table block, the
 * per-type summary and the top list to that table. topn clamps to [1,100],
 * 0 = default 20. Writes at most cap bytes incl NUL; returns the length
 * written (excluding NUL). The caller supplies the buffer (e.g. 64 KB). */
size_t tnt_stats_show(const tnt_stats_t *s, int topn, const char *only_table,
					  char *out, size_t cap);

#endif /* TNT_STATS_H */
