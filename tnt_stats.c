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
 * tnt_stats.c -- see tnt_stats.h for the design contract. This file is
 * deliberately free of switch_* calls (only libc), so test_stats can link
 * it standalone and exercise filter/extract/normalize/hash edge cases
 * without a running FreeSWITCH or a Tarantool server.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "tnt_stats.h"

/* pseudo-names: '*' prefix guarantees they never collide with real tables */
#define TT_UNKNOWN	"*unknown*"
#define TT_SUBQUERY	"*subquery*"

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static void lower_copy(char *dst, size_t cap, const char *src)
{
	size_t i;

	for (i = 0; i + 1 < cap && src[i]; i++)
		dst[i] = (src[i] >= 'A' && src[i] <= 'Z') ? (char)(src[i] - 'A' + 'a') : src[i];
	dst[i] = '\0';
}

static uint64_t fnv1a64(const char *s, size_t n)
{
	uint64_t h = 1469598103934665603ULL;
	size_t i;

	for (i = 0; i < n; i++) {
		h ^= (uint8_t)s[i];
		h *= 1099511628211ULL;
	}
	return h;
}

static int is_ident_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		   (c >= '0' && c <= '9') || c == '_' || c == '$';
}

/* is the last emitted char a self-delimiting token (no space needed
 * between it and a masked literal '?')? */
static int last_is_delim(const char *buf, size_t n)
{
	return n > 0 && (buf[n - 1] == '(' || buf[n - 1] == ')' ||
					 buf[n - 1] == ',' || buf[n - 1] == ' ');
}

static int word_eq(const char *sql, size_t len, size_t pos, const char *kw)
{
	size_t k = strlen(kw);
	size_t i;

	if (pos + k > len)
		return 0;
	for (i = 0; i < k; i++) {
		char c = sql[pos + i];
		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
		if (c != kw[i])
			return 0;
	}
	/* left word boundary */
	if (pos > 0 && is_ident_char(sql[pos - 1]))
		return 0;
	/* right word boundary */
	if (pos + k < len && is_ident_char(sql[pos + k]))
		return 0;
	return 1;
}

/* skip whitespace (and comments, which the caller never sees inside spans
 * produced by tnt_sql_split anyway) */
static size_t skip_ws(const char *sql, size_t len, size_t p)
{
	while (p < len && (sql[p] == ' ' || sql[p] == '\t' || sql[p] == '\r' ||
					   sql[p] == '\n' || sql[p] == '\f' || sql[p] == '\v'))
		p++;
	return p;
}

/* parse an identifier starting at *pp; returns 1 on success */
static int parse_ident(const char *sql, size_t len, size_t *pp,
					   char *dst, size_t cap)
{
	size_t p = skip_ws(sql, len, *pp);
	size_t n = 0;

	if (p >= len || !(is_ident_char(sql[p]) && !(sql[p] >= '0' && sql[p] <= '9')))
		return 0;
	while (p < len && is_ident_char(sql[p]) && n + 1 < cap)
		dst[n++] = sql[p++];
	/* trailing quoted identifier part ("table"."col") is ignored here */
	dst[n] = '\0';
	*pp = p;
	return n > 0;
}

/* ------------------------------------------------------------------ */
/* SELECT table extraction                                             */
/* ------------------------------------------------------------------ */

/* find the top-level (outside literals/parens) whole-word occurrence of kw
 * within [start,end); returns the offset or (size_t)-1 */
static size_t find_word_toplevel(const char *sql, size_t start, size_t end,
								 const char *kw)
{
	size_t p = start;
	int depth = 0;
	enum { S_NORM, S_SQ, S_DQ, S_LC, S_BC } st = S_NORM;

	while (p < end) {
		char c = sql[p];

		switch (st) {
		case S_NORM:
			if (c == '\'')
				st = S_SQ;
			else if (c == '"')
				st = S_DQ;
			else if (c == '-' && p + 1 < end && sql[p + 1] == '-')
				st = S_LC, p++;
			else if (c == '/' && p + 1 < end && sql[p + 1] == '*')
				st = S_BC, p++;
			else if (c == '(')
				depth++;
			else if (c == ')') {
				if (depth > 0)
					depth--;
			} else if (depth == 0 && word_eq(sql, end, p, kw)) {
				return p;
			}
			break;
		case S_SQ:
			if (c == '\'') {
				if (p + 1 < end && sql[p + 1] == '\'')
					p++;
				else
					st = S_NORM;
			}
			break;
		case S_DQ:
			if (c == '"') {
				if (p + 1 < end && sql[p + 1] == '"')
					p++;
				else
					st = S_NORM;
			}
			break;
		case S_LC:
			if (c == '\n')
				st = S_NORM;
			break;
		case S_BC:
			if (c == '*' && p + 1 < end && sql[p + 1] == '/') {
				p++;
				st = S_NORM;
			}
			break;
		}
		p++;
	}
	return (size_t)-1;
}

/* after the first table name: any top-level ',' or JOIN marker until the
 * statement end (depth 0, outside literals) => the query spans >1 source */
static int select_has_join(const char *sql, size_t start, size_t end)
{
	size_t p = start;
	int depth = 0;
	enum { S_NORM, S_SQ, S_DQ, S_LC, S_BC } st = S_NORM;

	while (p < end) {
		char c = sql[p];

		switch (st) {
		case S_NORM:
			if (c == '\'')
				st = S_SQ;
			else if (c == '"')
				st = S_DQ;
			else if (c == '-' && p + 1 < end && sql[p + 1] == '-')
				st = S_LC, p++;
			else if (c == '/' && p + 1 < end && sql[p + 1] == '*')
				st = S_BC, p++;
			else if (c == '(')
				depth++;
			else if (c == ')') {
				if (depth > 0)
					depth--;
			} else if (depth == 0) {
				if (c == ',')
					return 1;
				if (word_eq(sql, end, p, "join"))
					return 1;
			}
			break;
		case S_SQ:
			if (c == '\'') {
				if (p + 1 < end && sql[p + 1] == '\'')
					p++;
				else
					st = S_NORM;
			}
			break;
		case S_DQ:
			if (c == '"') {
				if (p + 1 < end && sql[p + 1] == '"')
					p++;
				else
					st = S_NORM;
			}
			break;
		case S_LC:
			if (c == '\n')
				st = S_NORM;
			break;
		case S_BC:
			if (c == '*' && p + 1 < end && sql[p + 1] == '/') {
				p++;
				st = S_NORM;
			}
			break;
		}
		p++;
	}
	return 0;
}

const char *tnt_stats_extract(const tnt_stmt_t *stmt, const char *sql,
							  char *buf, size_t cap)
{
	char raw[TNT_STATS_NAME_MAX];
	size_t start = stmt->start, end = stmt->end, p;
	size_t from;

	buf[0] = '\0';

	/* DDL/DML: tnt_sql_classify already parsed the table name */
	if (stmt->table[0]) {
		lower_copy(buf, cap, stmt->table);
		return buf;
	}

	if (stmt->type != TNT_STMT_SELECT) {
		/* BEGIN/COMMIT/ROLLBACK/PRAGMA/UNKNOWN and the like */
		snprintf(buf, cap, "%s", TT_UNKNOWN);
		return buf;
	}

	/* SELECT: first top-level FROM */
	from = find_word_toplevel(sql, start, end, "from");
	if (from == (size_t)-1) {
		snprintf(buf, cap, "%s", TT_UNKNOWN);
		return buf;
	}
	p = skip_ws(sql, end, from + 4);
	if (p < end && sql[p] == '(') {
		snprintf(buf, cap, "%s", TT_SUBQUERY);
		return buf;
	}
	if (!parse_ident(sql, end, &p, raw, sizeof(raw))) {
		snprintf(buf, cap, "%s", TT_UNKNOWN);
		return buf;
	}
	lower_copy(buf, cap, raw);
	if (select_has_join(sql, p, end)) {
		size_t n = strlen(buf);
		snprintf(buf + n, cap - n, " (join)");
	}
	return buf;
}

/* ------------------------------------------------------------------ */
/* normalization                                                       */
/* ------------------------------------------------------------------ */

size_t tnt_stats_normalize(const char *sql, size_t start, size_t end,
						   char *buf, size_t cap)
{
	size_t p = start, n = 0;
	int prev_space = 1;		/* trim leading whitespace */
	enum { N_NORM, N_SQ, N_DQ, N_LC, N_BC } st = N_NORM;

	if (cap == 0)
		return 0;

	while (p < end) {
		char c = sql[p];

		switch (st) {
		case N_NORM:
			if (c == '\'') {
				/* emit '?' once, then swallow the whole literal ('' keeps
				 * us inside until the real terminator); no space after
				 * self-delimiting tokens like '(' or ',' */
				if (!last_is_delim(buf, n) && n + 1 < cap)
					buf[n++] = ' ';
				if (n + 1 < cap)
					buf[n++] = '?';
				prev_space = 0;
				st = N_SQ;
			} else if (c == '"') {
				/* quoted identifier: part of the schema shape, keep verbatim */
				if (!prev_space && n + 1 < cap)
					buf[n++] = ' ';
				if (n + 1 < cap)
					buf[n++] = c;
				prev_space = 0;
				st = N_DQ;
			} else if (c == '-' && p + 1 < end && sql[p + 1] == '-') {
				st = N_LC;
				p++;
			} else if (c == '/' && p + 1 < end && sql[p + 1] == '*') {
				st = N_BC;
				p++;
			} else if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
					   c == '\f' || c == '\v') {
				if (!prev_space && n + 1 < cap)
					buf[n++] = ' ';
				prev_space = 1;
			} else if ((c >= '0' && c <= '9') &&
					   !(p > start && is_ident_char(sql[p - 1]))) {
				/* bare number start -> '?'; identifier chars before it mean
				 * it is part of a name (col2) or already-emitted '?\'' */
				if (!last_is_delim(buf, n) && n + 1 < cap)
					buf[n++] = ' ';
				if (n + 1 < cap)
					buf[n++] = '?';
				prev_space = 0;
				while (p + 1 < end && ((sql[p + 1] >= '0' && sql[p + 1] <= '9') ||
									   sql[p + 1] == '.' || sql[p + 1] == 'x' ||
									   sql[p + 1] == 'X' ||
									   (sql[p + 1] >= 'a' && sql[p + 1] <= 'f') ||
									   (sql[p + 1] >= 'A' && sql[p + 1] <= 'F')))
					p++;
			} else {
				if (c >= 'A' && c <= 'Z')
					c = (char)(c - 'A' + 'a');
				if (n + 1 < cap)
					buf[n++] = c;
				prev_space = 0;
			}
			break;
		case N_SQ:
			if (c == '\'') {
				if (p + 1 < end && sql[p + 1] == '\'')
					p++;	/* '' inside the literal */
				else
					st = N_NORM;
			}
			break;
		case N_DQ:
			if (n + 1 < cap)
				buf[n++] = c;
			if (c == '"') {
				if (p + 1 < end && sql[p + 1] == '"') {
					p++;
					if (n + 1 < cap)
						buf[n++] = '"';
				} else {
					st = N_NORM;
				}
			}
			break;
		case N_LC:
			if (c == '\n')
				st = N_NORM;
			break;
		case N_BC:
			if (c == '*' && p + 1 < end && sql[p + 1] == '/') {
				p++;
				st = N_NORM;
			}
			break;
		}
		p++;
	}
	/* trim trailing space */
	while (n > 0 && buf[n - 1] == ' ')
		n--;
	buf[n] = '\0';
	return n;
}

/* ------------------------------------------------------------------ */
/* filter                                                              */
/* ------------------------------------------------------------------ */

int tnt_stats_tables_set(tnt_stats_t *s, const char *const *names, int n)
{
	int i, applied = 0;

	if (!s)
		return 0;
	if (!names || n <= 0) {
		s->ntables = -1;	/* all */
		return 0;
	}
	for (i = 0; i < n && applied < TNT_STATS_TABLES_MAX; i++) {
		int dup = 0, j;

		if (!names[i] || !names[i][0])
			continue;
		lower_copy(s->tables[applied], TNT_STATS_NAME_MAX, names[i]);
		for (j = 0; j < applied; j++) {
			if (!strcmp(s->tables[j], s->tables[applied])) {
				dup = 1;
				break;
			}
		}
		if (!dup)
			applied++;
	}
	s->ntables = applied;
	return applied;
}

int tnt_stats_filter_match(const tnt_stats_t *s, const char *table)
{
	int i;

	if (!s || !table || !table[0])
		return 0;
	if (s->ntables < 0)
		return 1;			/* all */
	if (table[0] == '*')
		return 1;			/* pseudo-names always pass */
	/* the filter list is stored lower-cased; the incoming name may be in
	 * any case (defensive: extract() lower-cases, but stay safe) */
	for (i = 0; i < s->ntables; i++)
		if (!strcasecmp(s->tables[i], table))
			return 1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* accounting                                                          */
/* ------------------------------------------------------------------ */

void tnt_stats_init(tnt_stats_t *s)
{
	if (!s)
		return;
	memset(s, 0, sizeof(*s));
	s->ntables = -1;		/* all */
	s->enabled = 0;
}

void tnt_stats_reset(tnt_stats_t *s)
{
	int ntables;

	if (!s)
		return;
	ntables = s->ntables;	/* reset() also restores the default filter */
	memset(s->tt, 0, sizeof(s->tt));
	memset(s->templ, 0, sizeof(s->templ));
	s->ntt = 0;
	s->ntempl = 0;
	s->tt_overflow = 0;
	s->templ_overflow = 0;
	s->total_all = 0;
	s->total_counted = 0;
	s->filtered_out = 0;
	(void)ntables;
	s->ntables = -1;		/* all */
}

void tnt_stats_record_stmt(tnt_stats_t *s, const tnt_stmt_t *stmt)
{
	char tbl[TNT_STATS_NAME_MAX + 8];
	char norm[1024];
	const char *sql = stmt->sql;
	size_t nlen;
	uint64_t hash;
	int i, found;

	if (!s || !s->enabled || !stmt)
		return;

	tnt_stats_extract(stmt, sql, tbl, sizeof(tbl));

	s->total_all++;

	if (!tnt_stats_filter_match(s, tbl)) {
		s->filtered_out++;
		return;
	}
	s->total_counted++;

	/* level A: (type, table) -> total */
	found = 0;
	for (i = 0; i < s->ntt; i++) {
		if (s->tt[i].type == stmt->type && !strcmp(s->tt[i].table, tbl)) {
			s->tt[i].total++;
			found = 1;
			break;
		}
	}
	if (!found) {
		if (s->ntt < TNT_STATS_TT_MAX) {
			s->tt[s->ntt].type = stmt->type;
			snprintf(s->tt[s->ntt].table, sizeof(s->tt[s->ntt].table), "%s", tbl);
			s->tt[s->ntt].total = 1;
			s->ntt++;
		} else {
			s->tt_overflow++;
		}
	}

	/* level B: normalized template */
	nlen = tnt_stats_normalize(sql, stmt->start, stmt->end, norm, sizeof(norm));
	if (nlen == 0)
		snprintf(norm, sizeof(norm), "%s", TT_UNKNOWN);
	hash = fnv1a64(norm, strlen(norm));

	found = 0;
	for (i = 0; i < s->ntempl; i++) {
		if (s->templ[i].hash == hash) {
			s->templ[i].count++;
			found = 1;
			break;
		}
	}
	if (!found) {
		if (s->ntempl < TNT_STATS_TEMPLATES_MAX) {
			s->templ[s->ntempl].hash = hash;
			s->templ[s->ntempl].count = 1;
			s->templ[s->ntempl].type = stmt->type;
			snprintf(s->templ[s->ntempl].table, sizeof(s->templ[s->ntempl].table),
					 "%s", tbl);
			snprintf(s->templ[s->ntempl].sample, sizeof(s->templ[s->ntempl].sample),
					 "%s", norm);
			s->ntempl++;
		} else {
			s->templ_overflow++;
		}
	}
}

void tnt_stats_record(tnt_stats_t *s, const tnt_stmt_t *stmts, int n)
{
	int i;

	if (!s || !s->enabled || !stmts || n <= 0)
		return;
	for (i = 0; i < n; i++)
		tnt_stats_record_stmt(s, &stmts[i]);
}

/* ------------------------------------------------------------------ */
/* report                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
	const char *name;
	tnt_stmt_type_t types[5];
	int ntypes;
} stats_group_t;

static const stats_group_t *type_group(tnt_stmt_type_t t)
{
	static const stats_group_t groups[] = {
		{ "SELECT", { TNT_STMT_SELECT }, 1 },
		{ "INSERT", { TNT_STMT_INSERT }, 1 },
		{ "UPDATE", { TNT_STMT_UPDATE }, 1 },
		{ "DELETE", { TNT_STMT_DELETE }, 1 },
		{ "DDL",    { TNT_STMT_CREATE_TABLE, TNT_STMT_CREATE_VIEW,
					  TNT_STMT_CREATE_INDEX, TNT_STMT_DROP_TABLE,
					  TNT_STMT_ALTER_TABLE }, 5 },
		{ "other",  { TNT_STMT_UNKNOWN, TNT_STMT_BEGIN, TNT_STMT_COMMIT,
					  TNT_STMT_ROLLBACK, TNT_STMT_PRAGMA }, 5 },
	};
	int i, j;

	for (i = 0; i < (int)(sizeof(groups) / sizeof(groups[0])); i++)
		for (j = 0; j < groups[i].ntypes; j++)
			if (groups[i].types[j] == t)
				return &groups[i];
	return &groups[5];
}

/* type belongs to group g? */
static int in_group(const stats_group_t *g, tnt_stmt_type_t t)
{
	int j;

	for (j = 0; j < g->ntypes; j++)
		if (g->types[j] == t)
			return 1;
	return 0;
}

static int templ_by_count_desc(const void *a, const void *b)
{
	const tnt_stats_templ_t *x = (const tnt_stats_templ_t *)a;
	const tnt_stats_templ_t *y = (const tnt_stats_templ_t *)b;

	if (x->count < y->count)
		return 1;
	if (x->count > y->count)
		return -1;
	return 0;
}

size_t tnt_stats_show(const tnt_stats_t *s, int topn, const char *only_table,
					  char *out, size_t cap)
{
	size_t off = 0;
	int g, i;
	uint64_t g_total[6], g_distinct[6], g_top[6];
	const stats_group_t *groups = NULL;
	tnt_stats_templ_t *top;
	int ntop = 0;
	char flt[512];

#define EMIT(...) do { \
		if (off < cap) { \
			int _w = snprintf(out + off, cap - off, __VA_ARGS__); \
			if (_w > 0) off += (_w > (int)(cap - off)) ? (int)(cap - off) : _w; \
		} \
	} while (0)

	if (!s || !out || cap == 0)
		return 0;
	if (topn <= 0)
		topn = 20;
	if (topn > 100)
		topn = 100;
	{
		/* local copy of the group descriptors for aggregation (type_group()
		 * below reuses the same table for the per-pair block) */
		static const stats_group_t lg[] = {
			{ "SELECT", { TNT_STMT_SELECT }, 1 },
			{ "INSERT", { TNT_STMT_INSERT }, 1 },
			{ "UPDATE", { TNT_STMT_UPDATE }, 1 },
			{ "DELETE", { TNT_STMT_DELETE }, 1 },
			{ "DDL",    { TNT_STMT_CREATE_TABLE, TNT_STMT_CREATE_VIEW,
						  TNT_STMT_CREATE_INDEX, TNT_STMT_DROP_TABLE,
						  TNT_STMT_ALTER_TABLE }, 5 },
			{ "other",  { TNT_STMT_UNKNOWN, TNT_STMT_BEGIN, TNT_STMT_COMMIT,
						  TNT_STMT_ROLLBACK, TNT_STMT_PRAGMA }, 5 },
		};
		groups = lg;
	}

	memset(g_total, 0, sizeof(g_total));
	memset(g_distinct, 0, sizeof(g_distinct));
	memset(g_top, 0, sizeof(g_top));

	/* header */
	if (s->ntables < 0)
		snprintf(flt, sizeof(flt), "%s", "all");
	else {
		size_t n = 0;
		int k;

		flt[0] = '\0';
		for (k = 0; k < s->ntables; k++) {
			int _w = snprintf(flt + n, sizeof(flt) - n, "%s%s", k ? ", " : "",
							  s->tables[k]);
			if (_w > 0 && (size_t)_w < sizeof(flt) - n)
				n += (size_t)_w;
			else
				break;
		}
	}
	EMIT("sqlstats: state=%s filter: %s\n", s->enabled ? "on" : "off", flt);
	EMIT("statements: total=%llu counted=%llu filtered-out=%llu\n",
		 (unsigned long long)s->total_all,
		 (unsigned long long)s->total_counted,
		 (unsigned long long)s->filtered_out);

	/* aggregate per group from level A (totals) and level B (distinct/top) */
	for (g = 0; g < 6; g++) {
		for (i = 0; i < s->ntt; i++) {
			if (!in_group(&groups[g], s->tt[i].type))
				continue;
			if (only_table && only_table[0] &&
				strcmp(only_table, s->tt[i].table))
				continue;
			g_total[g] += s->tt[i].total;
		}
		for (i = 0; i < s->ntempl; i++) {
			if (!in_group(&groups[g], s->templ[i].type))
				continue;
			if (only_table && only_table[0] &&
				strcmp(only_table, s->templ[i].table))
				continue;
			g_distinct[g]++;
			if (s->templ[i].count > g_top[g])
				g_top[g] = s->templ[i].count;
		}
	}
	EMIT("type      total      distinct   top-hit\n");
	for (g = 0; g < 6; g++) {
		if (g_total[g] == 0 && g_distinct[g] == 0)
			continue;
		EMIT("%-9s %10llu %10llu %10llu\n", groups[g].name,
			 (unsigned long long)g_total[g],
			 (unsigned long long)g_distinct[g],
			 (unsigned long long)g_top[g]);
	}

	/* per (type, table) block */
	EMIT("by (type, table):\n");
	for (i = 0; i < s->ntt; i++) {
		uint64_t dist = 0;

		if (only_table && only_table[0] && strcmp(only_table, s->tt[i].table))
			continue;
		/* distinct for this pair from level B */
		{
			int j;

			for (j = 0; j < s->ntempl; j++) {
				if (s->templ[j].type == s->tt[i].type &&
					!strcmp(s->templ[j].table, s->tt[i].table))
					dist++;
			}
		}
		EMIT("  %-7s %-28s total=%-10llu distinct=%llu\n",
			 type_group(s->tt[i].type)->name, s->tt[i].table,
			 (unsigned long long)s->tt[i].total,
			 (unsigned long long)dist);
	}

	/* top templates */
	top = (tnt_stats_templ_t *)malloc(sizeof(*top) * (size_t)s->ntempl);
	if (top) {
		int j;

		for (i = 0; i < s->ntempl; i++) {
			if (only_table && only_table[0] &&
				strcmp(only_table, s->templ[i].table))
				continue;
			top[ntop++] = s->templ[i];
		}
		qsort(top, (size_t)ntop, sizeof(*top), templ_by_count_desc);
		EMIT("top templates (limit %d):\n", topn);
		for (j = 0; j < ntop && j < topn; j++)
			EMIT("  %d. count=%-10llu [%s] %s\n      %s\n", j + 1,
				 (unsigned long long)top[j].count,
				 type_group(top[j].type)->name, top[j].table,
				 top[j].sample);
		free(top);
	} else {
		EMIT("top templates: out of memory\n");
	}

	EMIT("counters: templates=%d/%d (overflow %llu) "
		 "type-table=%d/%d (overflow %llu)\n",
		 s->ntempl, TNT_STATS_TEMPLATES_MAX,
		 (unsigned long long)s->templ_overflow,
		 s->ntt, TNT_STATS_TT_MAX, (unsigned long long)s->tt_overflow);

#undef EMIT
	return off;
}
