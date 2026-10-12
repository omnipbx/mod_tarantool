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
 * tnt_tcache.c -- see tnt_tcache.h for the design contract. Pure libc
 * implementation; concurrency (one cache mutex around every call into this
 * layer) and key hashing live in mod_tarantool.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "tnt_tcache.h"

void tnt_tcache_init(tnt_tcache_t *t, int limit)
{
	if (!t)
		return;
	memset(t, 0, sizeof(*t));
	t->limit = (limit > 0 && limit <= TNT_TCACHE_MAX) ? limit : TNT_TCACHE_MAX;
	t->enabled = 0;
}

void tnt_tcache_invalidate(tnt_tcache_t *t)
{
	int i;

	if (!t)
		return;
	for (i = 0; i < t->n; i++) {
		free(t->e[i].sql);
		t->e[i].sql = NULL;
	}
	t->n = 0;
}

void tnt_tcache_reset(tnt_tcache_t *t)
{
	if (!t)
		return;
	tnt_tcache_invalidate(t);
	t->hits = 0;
	t->misses = 0;
}

const char *tnt_tcache_fetch(tnt_tcache_t *t, uint64_t hash, int *noop,
							 int *literal)
{
	int i;

	if (!t || !t->enabled)
		return NULL;
	for (i = 0; i < t->n; i++) {
		if (t->e[i].hash == hash) {
			t->e[i].hits++;
			t->hits++;
			if (noop)
				*noop = t->e[i].noop;
			if (literal)
				*literal = t->e[i].literal;
			/* borrowed pointer: valid while the caller holds the cache
			 * lock; the concrete statement is rebuilt under the lock */
			return t->e[i].sql;
		}
	}
	t->misses++;
	if (noop)
		*noop = 0;
	if (literal)
		*literal = 0;
	return NULL;
}

void tnt_tcache_store(tnt_tcache_t *t, uint64_t hash, const char *sql,
					  int noop, tnt_stmt_type_t type, int literal)
{
	int i, victim = 0;

	if (!t || !t->enabled || !sql)
		return;
	if (strlen(sql) > TNT_TCACHE_SQL_MAX)
		return;

	/* same hash -> replace in place (rules changed between identical texts
	 * is impossible within one epoch, but keep the store idempotent) */
	for (i = 0; i < t->n; i++) {
		if (t->e[i].hash == hash) {
			free(t->e[i].sql);
			t->e[i].sql = strdup(sql);
			t->e[i].noop = noop;
			t->e[i].literal = literal;
			t->e[i].type = type;
			t->e[i].hits = 0;
			t->e[i].created = time(NULL);
			return;
		}
	}

	if (t->n < t->limit) {
		i = t->n++;
	} else {
		/* evict the least-hits entry (hot templates survive, one-off
		 * dialplan probes do not) */
		uint64_t min = (uint64_t)-1;
		int k;

		for (k = 0; k < t->n; k++) {
			if (t->e[k].hits < min) {
				min = t->e[k].hits;
				victim = k;
			}
		}
		i = victim;
		free(t->e[i].sql);
	}

	memset(&t->e[i], 0, sizeof(t->e[i]));
	t->e[i].hash = hash;
	t->e[i].sql = strdup(sql);
	t->e[i].noop = noop;
	t->e[i].literal = literal;
	t->e[i].type = type;
	t->e[i].hits = 0;
	t->e[i].created = time(NULL);
}

size_t tnt_tcache_show(const tnt_tcache_t *t, int topn, int full,
					   char *out, size_t cap)
{
	tnt_tcache_entry_t *top;
	int ntop = 0, i, j;
	uint64_t total_hits = 0;
	size_t off = 0;
	unsigned long long req = (unsigned long long)(t ? (t->hits + t->misses) : 0);
	double rate = req ? (100.0 * (double)(t ? t->hits : 0) / (double)req) : 0.0;

#define EMIT(...) do { \
		if (off < cap) { \
			int _w = snprintf(out + off, cap - off, __VA_ARGS__); \
			if (_w > 0) off += (_w > (int)(cap - off)) ? (int)(cap - off) : _w; \
		} \
	} while (0)

	if (!t || !out || cap == 0)
		return 0;
	if (topn <= 0)
		topn = 20;
	/* cap by cache limit: there cannot be more entries than it holds */
	if (topn > t->limit)
		topn = t->limit;

	EMIT("cache: %s (limit %d, used %d)\n",
		 t->enabled ? "enabled" : "disabled", t->limit, t->n);
	EMIT("hit-rate: %llu/%llu = %.2f%%\n",
		 (unsigned long long)t->hits, req, rate);
	EMIT("misses since reset: %llu\n", (unsigned long long)t->misses);

	if (t->n == 0) {
		EMIT("cache is empty\n");
		return off;
	}

	top = (tnt_tcache_entry_t *)malloc(sizeof(*top) * (size_t)t->n);
	if (!top) {
		EMIT("top entries: out of memory\n");
		return off;
	}
	for (i = 0; i < t->n; i++) {
		total_hits += t->e[i].hits;
		top[ntop++] = t->e[i];
	}
	/* sort by hits desc (insertion sort: n <= 512, sorted output only) */
	for (i = 1; i < ntop; i++) {
		tnt_tcache_entry_t key = top[i];

		for (j = i - 1; j >= 0 && top[j].hits < key.hits; j--)
			top[j + 1] = top[j];
		top[j + 1] = key;
	}

	EMIT("total hits over entries: %llu\n", (unsigned long long)total_hits);
	EMIT("top entries (by hits, limit %d):\n", topn);
	for (i = 0; i < ntop && i < topn; i++) {
		char tbuf[64];
		char *disp;
		struct tm tmv;
		time_t age = time(NULL) - top[i].created;
		size_t slen = strlen(top[i].sql), k;

		tbuf[0] = '\0';
		if (localtime_r(&top[i].created, &tmv))
			strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tmv);
		/* templates carry marker bytes: print them as '?' */
		disp = (char *)malloc(slen + 2);
		if (!disp) {
			EMIT("  %d. hits=%-8llu (out of memory)\n", i + 1,
				 (unsigned long long)top[i].hits);
			continue;
		}
		for (k = 0; k < slen; k++)
			disp[k] = (top[i].sql[k] == TNT_SQL_TPL_MARK) ? '?' : top[i].sql[k];
		disp[k] = '\0';
		EMIT("  %d. hits=%-8llu len=%-6zu age=%llds %s\n",
			 i + 1, (unsigned long long)top[i].hits, slen,
			 (long long)(age < 0 ? 0 : age), tbuf);
		if (full) {
			EMIT("      %s\n", disp);
		} else {
			char sample[121];

			snprintf(sample, sizeof(sample), "%.*s", (int)sizeof(sample) - 1, disp);
			EMIT("      %s%s\n", sample, slen > sizeof(sample) - 1 ? "..." : "");
		}
		free(disp);
	}
	if (ntop > topn)
		EMIT("  ... %d more entries not shown (use 'tarantool cache show %d' for more)\n",
			 ntop - topn, ntop);
	free(top);

#undef EMIT
	return off;
}
