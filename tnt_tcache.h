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
 * tnt_tcache.h -- dynamic statement translate cache (pure layer, no
 * switch_* calls; the caller owns locking, e.g. a dedicated cache mutex,
 * and owns the key hashing).
 *
 * Rationale (measured with "tarantool sqlstats" on live traffic): the SQL
 * stream is ~99% repeated templates (69 distinct on 8613 statements), so
 * the per-exec pipeline split -> translate -> quote is redundant work. The
 * cache is self-populating: the FIRST occurrence of a statement pays the
 * full translation, every later one gets the final (translated + quoted)
 * string. Invalidation is epoch-driven: on a rules reload the caller
 * bumps its epoch and invalidates the cache; a hit returns a COPY, so a
 * concurrent invalidate/reset can never hand out freed memory.
 *
 * Statement results that depend on the per-call translate context (an
 * injected literal core UUID, a $${var} resolved through ctx->resolver)
 * MUST NOT be stored - the caller checks tnt_translate_ctx_t.ctx_dep.
 */

#ifndef TNT_TCACHE_H
#define TNT_TCACHE_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "tnt_sql.h"

/* max entries; live traffic measured 69 distinct templates, so this is
 * ~7x headroom */
#define TNT_TCACHE_MAX		512
/* statements longer than this are not cached (worst-case memory bound:
 * 512 * 8 KB = 4 MB) */
#define TNT_TCACHE_SQL_MAX	8192

typedef struct tnt_tcache_entry {
	uint64_t hash;			/* FNV-1a 64 of the raw statement span */
	char *sql;				/* final translated+quoted text, owned */
	int noop;				/* statement translates to no-op (skip) */
	int literal;			/* SELECT list carries data literals (markers
							 * before FROM) that must be materialized into
							 * constants for PREPARE; computed once at store */
	tnt_stmt_type_t type;	/* statement type of the first sighting */
	uint64_t hits;			/* cache hits on this entry */
	time_t created;			/* wall clock at store time */
} tnt_tcache_entry_t;

typedef struct tnt_tcache {
	int enabled;			/* caller-managed switch (param + API) */
	int limit;				/* <= TNT_TCACHE_MAX */
	int n;
	tnt_tcache_entry_t e[TNT_TCACHE_MAX];
	uint64_t hits;
	uint64_t misses;
} tnt_tcache_t;

/* Zero the state; enabled = 0; limit = TNT_TCACHE_MAX. */
void tnt_tcache_init(tnt_tcache_t *t, int limit);

/* Drop every entry (frees the strings); counters are kept.
 * The caller holds the cache lock. */
void tnt_tcache_invalidate(tnt_tcache_t *t);

/* invalidate + reset hit/miss counters. */
void tnt_tcache_reset(tnt_tcache_t *t);

/* Lookup: returns a borrowed pointer to the cached template (valid only
 * while the caller holds the cache lock) or NULL on a miss; *noop and
 * *literal receive the entry flags on a hit (both set to 0 on a miss).
 * Increments hits/misses. The caller holds the cache lock; the caller
 * rebuilds the concrete statement under the lock with tnt_sql_rebuild(). */
const char *tnt_tcache_fetch(tnt_tcache_t *t, uint64_t hash, int *noop,
							 int *literal);

/* Store the final SQL text (copied). Replaces an entry with the same hash;
 * otherwise takes a free slot or evicts the least-hits entry. No-op when
 * the text exceeds TNT_TCACHE_SQL_MAX. The caller holds the cache lock. */
void tnt_tcache_store(tnt_tcache_t *t, uint64_t hash, const char *sql,
					  int noop, tnt_stmt_type_t type, int literal);

/* Build the human-readable report. topn clamps to [1,100], 0 = 20;
 * full != 0 prints the complete SQL text of the top entries instead of a
 * 120-byte sample. Writes at most cap bytes incl NUL; returns the length
 * written. The caller holds the cache lock (or accepts a snapshot read). */
size_t tnt_tcache_show(const tnt_tcache_t *t, int topn, int full,
					   char *out, size_t cap);

#endif /* TNT_TCACHE_H */
