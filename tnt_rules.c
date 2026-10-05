/*
* mod_tarantool for FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2026, Filippov Anatoliy <error.email@mail.ru>
*
* Version: MPL 2.0
*
* The contents of this file are subject to the Mozilla Public License Version
* 1.1 (the "License"); you may not use this file except in compliance with
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
* tnt_rules.c -- declarative DDL translation
*
*/

#include "tnt_rules.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ---------- small buffer utils ---------- */

static void buf_putc(char *b, size_t cap, size_t *len, char c)
{
	if (*len + 1 < cap) {
		b[*len] = c;
		(*len)++;
	}
}

static void buf_puts(char *b, size_t cap, size_t *len, const char *s)
{
	while (*s) {
		buf_putc(b, cap, len, *s++);
	}
}

static void buf_putn(char *b, size_t cap, size_t *len, const char *s, size_t n)
{
	while (n--) {
		buf_putc(b, cap, len, *s++);
	}
}

/* Case-insensitive whole-word search (word boundaries are non-ident chars). */
static size_t find_word(const char *s, size_t len, const char *kw)
{
	size_t kwlen = strlen(kw);
	size_t i;

	for (i = 0; i + kwlen <= len; i++) {
		size_t j;

		if (i > 0 && (isalnum((unsigned char)s[i - 1]) || s[i - 1] == '_'))
			continue;
		for (j = 0; j < kwlen; j++) {
			if (tolower((unsigned char)s[i + j]) != tolower((unsigned char)kw[j]))
				break;
		}
		if (j == kwlen &&
			(i + kwlen == len ||
			 !(isalnum((unsigned char)s[i + kwlen]) || s[i + kwlen] == '_'))) {
			return i;
		}
	}
	return SIZE_MAX;
}

/* Copy src[0..len) replacing whole words BIGINT with INTEGER. */
static char *fix_bigint(const char *src, size_t len)
{
	char *out = (char *)malloc(len + 64);
	size_t o = 0;
	size_t i = 0;

	if (!out)
		return NULL;

	while (i < len) {
		size_t p = find_word(src + i, len - i, "bigint");

		if (p == SIZE_MAX) {
			buf_putn(out, len + 64, &o, src + i, len - i);
			break;
		}
		buf_putn(out, len + 64, &o, src + i, p);
		buf_puts(out, len + 64, &o, "INTEGER");
		i += p + 6;
	}
	buf_putc(out, len + 64, &o, '\0');
	return out;
}

/* ---------- rule lookup ---------- */

static const tnt_pk_rule_t *find_pk(const tnt_rules_t *r, const char *table)
{
	int i;

	for (i = 0; i < r->npk; i++) {
		if (!strcmp(r->pk[i].table, table))
			return &r->pk[i];
	}
	return NULL;
}

static const tnt_pkadd_rule_t *find_pkadd(const tnt_rules_t *r, const char *table)
{
	int i;

	for (i = 0; i < r->npkadd; i++) {
		if (!strcmp(r->pkadd[i].table, table))
			return &r->pkadd[i];
	}
	return NULL;
}

static void find_fields(const tnt_rules_t *r, const char *table,
						const tnt_addfield_rule_t **list, int *n)
{
	int i;
	int found = 0;

	*list = NULL;
	*n = 0;
	for (i = 0; i < r->nfields; i++) {
		if (!strcmp(r->fields[i].table, table)) {
			if (!found) {
				*list = &r->fields[i];
				found = 1;
			}
			(*n)++;
		}
	}
}

SWITCH_DECLARE(void) tnt_rules_init(tnt_rules_t *r)
{
	memset(r, 0, sizeof(*r));
	/* built-in Tarantool-reserved words; more can be added at runtime
	 * from the <reserved> section of tarantool.conf.xml */
	tnt_rules_add_reserved(r, "uuid");
	tnt_rules_add_reserved(r, "alias");
}

SWITCH_DECLARE(int) tnt_rules_add_pk(tnt_rules_t *r, const char *table, const char *field)
{
	if (r->npk >= TNT_RULES_MAX)
		return -1;
	snprintf(r->pk[r->npk].table, sizeof(r->pk[r->npk].table), "%s", table);
	snprintf(r->pk[r->npk].field, sizeof(r->pk[r->npk].field), "%s", field);
	r->npk++;
	return 0;
}

SWITCH_DECLARE(int) tnt_rules_add_pkadd(tnt_rules_t *r, const char *table, const char *field,
										const char *type, int core_uuid)
{
	if (r->npkadd >= TNT_RULES_MAX)
		return -1;
	snprintf(r->pkadd[r->npkadd].table, sizeof(r->pkadd[r->npkadd].table), "%s", table);
	snprintf(r->pkadd[r->npkadd].field, sizeof(r->pkadd[r->npkadd].field), "%s", field);
	snprintf(r->pkadd[r->npkadd].type, sizeof(r->pkadd[r->npkadd].type), "%s", type ? type : "");
	r->pkadd[r->npkadd].core_uuid = core_uuid;
	r->npkadd++;
	return 0;
}

SWITCH_DECLARE(int) tnt_rules_add_field(tnt_rules_t *r, const char *table, const char *field,
										const char *type, int is_null, const char *def,
										const char *value, int inline_val)
{
	if (r->nfields >= TNT_RULES_MAX)
		return -1;
	snprintf(r->fields[r->nfields].table, sizeof(r->fields[r->nfields].table), "%s", table);
	snprintf(r->fields[r->nfields].field, sizeof(r->fields[r->nfields].field), "%s", field);
	snprintf(r->fields[r->nfields].type, sizeof(r->fields[r->nfields].type), "%s", type ? type : "STRING");
	r->fields[r->nfields].is_null = is_null;
	r->fields[r->nfields].inline_val = inline_val;
	snprintf(r->fields[r->nfields].def, sizeof(r->fields[r->nfields].def), "%s", def ? def : "");
	snprintf(r->fields[r->nfields].value, sizeof(r->fields[r->nfields].value), "%s", value ? value : "");
	r->nfields++;
	return 0;
}

SWITCH_DECLARE(int) tnt_rules_add_reserved(tnt_rules_t *r, const char *word)
{
	size_t wl;
	int i;

	if (!r || !word || !word[0])
		return -1;
	wl = strlen(word);
	if (wl >= TNT_RESERVED_WORD_LEN)
		wl = TNT_RESERVED_WORD_LEN - 1;
	/* de-dup case-insensitively */
	for (i = 0; i < r->nreserved; i++) {
		if (strlen(r->reserved[i]) == wl && !strncasecmp(r->reserved[i], word, wl))
			return 0;
	}
	if (r->nreserved >= TNT_RESERVED_MAX)
		return -1;
	snprintf(r->reserved[r->nreserved], TNT_RESERVED_WORD_LEN, "%s", word);
	r->nreserved++;
	return 0;
}

SWITCH_DECLARE(int) tnt_rules_add_view(tnt_rules_t *r, const char *name, const char *body)
{
	size_t nlen, blen;
	int i;

	if (!r || !name || !name[0] || !body || !body[0])
		return -1;
	blen = strlen(body);
	/* an oversized body is REJECTED, never truncated: -2 tells the caller
	 * to refuse the whole configuration (see the header comment) */
	if (blen >= TNT_VIEW_BODY_LEN)
		return -2;
	nlen = strlen(name);
	if (nlen >= sizeof(r->views[0].name))
		nlen = sizeof(r->views[0].name) - 1;
	/* re-declaration of an existing name replaces the body (reload-friendly) */
	for (i = 0; i < r->nviews; i++) {
		if (strlen(r->views[i].name) == nlen && !strncasecmp(r->views[i].name, name, nlen)) {
			snprintf(r->views[i].name, sizeof(r->views[i].name), "%s", name);
			memcpy(r->views[i].body, body, blen + 1);
			return 0;
		}
	}
	if (r->nviews >= TNT_VIEWS_MAX)
		return -3;
	snprintf(r->views[r->nviews].name, sizeof(r->views[r->nviews].name), "%s", name);
	memcpy(r->views[r->nviews].body, body, blen + 1);
	r->nviews++;
	return 0;
}

/* ---------- CREATE TABLE rewriting ---------- */

/* Find the index of the first ')' matching the '(' at open. */
static size_t find_matching_paren(const char *s, size_t len, size_t open)
{
	size_t depth = 1;
	size_t i = open + 1;
	int in_str = 0;

	for (; i < len; i++) {
		char c = s[i];

		if (in_str) {
			if (c == '\'') {
				if (i + 1 < len && s[i + 1] == '\'') {
					i++;
				} else {
					in_str = 0;
				}
			}
			continue;
		}
		if (c == '\'') {
			in_str = 1;
		} else if (c == '(') {
			depth++;
		} else if (c == ')') {
			depth--;
			if (depth == 0)
				return i;
		}
	}
	return SIZE_MAX;
}

/* Split body[start..end) into top-level comma-separated chunks.
 * Returns an array of {start,end} pairs; caller frees. */
typedef struct seg { size_t start, end; } seg_t;

static seg_t *split_defs(const char *s, size_t start, size_t end, int *nseg)
{
	seg_t *segs = NULL;
	int n = 0, cap = 0;
	size_t depth = 0;
	size_t part = start;
	size_t i;
	int in_str = 0;

	for (i = start; i <= end; i++) {
		if (i < end) {
			char c = s[i];

			if (in_str) {
				if (c == '\'') {
					if (i + 1 < end && s[i + 1] == '\'')
						i++;
					else
						in_str = 0;
				}
			} else if (c == '\'') {
				in_str = 1;
			} else if (c == '(') {
				depth++;
			} else if (c == ')') {
				if (depth > 0)
					depth--;
			}
		}
		if (i == end || (depth == 0 && !in_str && s[i] == ',')) {
			size_t a = part, b = i;

			/* trim ws */
			while (a < b && isspace((unsigned char)s[a]))
				a++;
			while (b > a && isspace((unsigned char)s[b - 1]))
				b--;
			if (b > a) {
				if (n >= cap) {
					size_t ncap = cap ? cap * 2 : 8;
					seg_t *ns = (seg_t *)realloc(segs, ncap * sizeof(*ns));

					if (!ns)
						goto fail;
					segs = ns;
					cap = (int)ncap;
				}
				segs[n].start = a;
				segs[n].end = b;
				n++;
			}
			part = i + 1;
		}
	}
	*nseg = n;
	return segs;
fail:
	free(segs);
	*nseg = 0;
	return NULL;
}

/* Extract the first identifier (quoted or bare) from def; returns length. */
static size_t def_first_ident(const char *s, size_t start, size_t end, char *out, size_t outsz)
{
	size_t p = start;
	size_t n = 0;

	while (p < end && isspace((unsigned char)s[p]))
		p++;
	if (p >= end)
		return 0;

	if (s[p] == '"') {
		p++;
		while (p < end && s[p] != '"' && n + 1 < outsz)
			out[n++] = s[p++];
	} else {
		while (p < end && (isalnum((unsigned char)s[p]) || s[p] == '_') && n + 1 < outsz) {
			out[n++] = (char)tolower((unsigned char)s[p++]);
		}
	}
	out[n] = '\0';
	return n;
}

/* Does def contain a PRIMARY KEY clause? */
static int def_has_pk(const char *s, size_t start, size_t end)
{
	size_t p = find_word(s + start, end - start, "primary");

	return p != SIZE_MAX &&
		find_word(s + start + p, end - start - p, "key") != SIZE_MAX;
}

static char *translate_create_table(const tnt_rules_t *r, const tnt_stmt_t *st)
{
	const tnt_pkadd_rule_t *pa = find_pkadd(r, st->table);
	const tnt_pk_rule_t *pk = find_pk(r, st->table);
	const tnt_addfield_rule_t *af = NULL;
	int naf = 0;
	char *fixed;
	size_t flen;
	size_t open, close;
	seg_t *segs;
	int nseg, i;
	char *out;
	size_t cap, o = 0;
	int have_pk = 0;
	int pkadd_defined = 0;

	find_fields(r, st->table, &af, &naf);

	/* whole statement with BIGINT -> INTEGER */
	fixed = fix_bigint(st->sql + st->start, st->end - st->start);
	if (!fixed)
		return NULL;
	flen = strlen(fixed);

	/* locate the column-list parentheses */
	open = SIZE_MAX;
	for (i = 0; i < (int)flen; i++) {
		if (fixed[i] == '(') {
			open = (size_t)i;
			break;
		}
	}
	if (open == SIZE_MAX) {
		/* no parens: nothing to rewrite, drop CREATE TABLE -> passthrough */
		return fixed;
	}
	close = find_matching_paren(fixed, flen, open);
	if (close == SIZE_MAX) {
		return fixed;
	}

	segs = split_defs(fixed, open + 1, close, &nseg);
	if (!segs)
		return fixed;

	for (i = 0; i < nseg; i++) {
		if (def_has_pk(fixed, segs[i].start, segs[i].end))
			have_pk = 1;
	}

	/* build: CREATE TABLE <name> ( <defs>, <added>, PRIMARY KEY (...) ) */
	cap = flen + 512;
	out = (char *)malloc(cap);
	if (!out) {
		free(segs);
		free(fixed);
		return NULL;
	}

	buf_puts(out, cap, &o, "CREATE TABLE ");
	buf_puts(out, cap, &o, st->table);
	buf_puts(out, cap, &o, " (");
	if (nseg > 0) {
		for (i = 0; i < nseg; i++) {
			char fld[64];

			buf_putc(out, cap, &o, ' ');
			buf_putn(out, cap, &o, fixed + segs[i].start, segs[i].end - segs[i].start);
			buf_putc(out, cap, &o, ',');
			/* remember existing field names to avoid duplicating injected ones */
			if (pa && def_first_ident(fixed, segs[i].start, segs[i].end, fld, sizeof(fld)) &&
				!strcmp(fld, pa->field)) {
				pkadd_defined = 1;
			}
		}
	}
	/* every def above ends with ',' — drop the dangling separator when
	 * nothing follows it (no injected PK column, no addField, no PRIMARY
	 * KEY clause), otherwise Tarantool sees "column INTEGER,)" and aborts
	 * the CREATE TABLE with a syntax error. */
	if (nseg > 0 &&
		!((pa && !pkadd_defined) || (af && naf > 0) ||
		  (!have_pk && (pk ? pk->field : (pa ? pa->field : NULL)) != NULL))) {
		if (o > 0 && out[o - 1] == ',')
			out[--o] = '\0';
	}
	/* inject the primary-key UUID column (only when the app did not).
	 * No DEFAULT clause: Tarantool 3.x does NOT evaluate expression
	 * defaults on INSERT ("NOT NULL constraint failed"); the module instead
	 * injects the value into every INSERT for this table (inject_insert_columns).
	 * Column type comes from the rule; "" defaults to varchar(36). */
	if (pa && !pkadd_defined) {
		const char *ptype = pa->type[0] ? pa->type : "varchar(36)";

		buf_puts(out, cap, &o, " ");
		buf_puts(out, cap, &o, pa->field);
		buf_putc(out, cap, &o, ' ');
		buf_puts(out, cap, &o, ptype);
		buf_puts(out, cap, &o, " NOT NULL,");
	}
	/* inject additional columns */
	if (af && naf > 0) {
		for (i = 0; i < naf; i++) {
			buf_puts(out, cap, &o, " ");
			buf_puts(out, cap, &o, af[i].field);
			buf_putc(out, cap, &o, ' ');
			buf_puts(out, cap, &o, af[i].type[0] ? af[i].type : "STRING");
			if (!af[i].is_null)
				buf_puts(out, cap, &o, " NOT NULL");
			/* function-call defaults (uuid(), ...) are not applied by
			 * Tarantool on INSERT — skip the clause for them */
			if (af[i].def[0] && !strchr(af[i].def, '(')) {
				buf_puts(out, cap, &o, " DEFAULT ");
				buf_puts(out, cap, &o, af[i].def);
			}
			buf_putc(out, cap, &o, ',');
		}
	}
	/* PK clause (single-column PK from pkadd or pk rule) */
	if (!have_pk) {
		const char *pkf = pk ? pk->field : (pa ? pa->field : NULL);

		if (pkf) {
			buf_puts(out, cap, &o, " PRIMARY KEY (");
			buf_puts(out, cap, &o, pkf);
			buf_puts(out, cap, &o, ")");
		}
	}
	buf_puts(out, cap, &o, ")");
	buf_putc(out, cap, &o, '\0');

	free(segs);
	free(fixed);
	return out;
}

/* ---------- other statements ---------- */

/* CREATE [UNIQUE] INDEX -> ensure IF NOT EXISTS right after INDEX. */
static char *translate_create_index(const tnt_stmt_t *st)
{
	char *fixed = fix_bigint(st->sql + st->start, st->end - st->start);
	size_t flen, idx, p;
	char *out;
	size_t cap, o = 0;

	if (!fixed)
		return NULL;
	flen = strlen(fixed);

	idx = find_word(fixed, flen, "index");
	if (idx == SIZE_MAX) {
		return fixed;
	}
	p = idx + 5;
	/* already has IF NOT EXISTS? */
	if (find_word(fixed + p, flen - p, "if") != SIZE_MAX &&
		find_word(fixed + p, flen - p, "not") != SIZE_MAX) {
		return fixed;
	}

	cap = flen + 32;
	out = (char *)malloc(cap);
	if (!out) {
		free(fixed);
		return NULL;
	}
	buf_putn(out, cap, &o, fixed, p);
	buf_puts(out, cap, &o, " IF NOT EXISTS");
	buf_putn(out, cap, &o, fixed + p, flen - p);
	buf_putc(out, cap, &o, '\0');
	free(fixed);
	return out;
}

/* DROP TABLE -> strip "IF EXISTS". */
static char *translate_drop_table(const tnt_stmt_t *st)
{
	char *fixed = fix_bigint(st->sql + st->start, st->end - st->start);
	size_t flen, ie;

	if (!fixed)
		return NULL;
	flen = strlen(fixed);
	ie = find_word(fixed, flen, "if");
	if (ie == SIZE_MAX)
		return fixed;
	{
		size_t n2 = find_word(fixed + ie, flen - ie, "exists");
		size_t end;
		char *out;
		size_t cap, o = 0;

		if (n2 == SIZE_MAX)
			return fixed;
		end = ie + n2 + 6;
		cap = flen;
		out = (char *)malloc(cap + 1);
		if (!out) {
			free(fixed);
			return NULL;
		}
		/* copy up to IF, then skip to after EXISTS */
		buf_putn(out, cap + 1, &o, fixed, ie);
		while (end < flen && isspace((unsigned char)fixed[end]))
			end++;
		buf_putn(out, cap + 1, &o, fixed + end, flen - end);
		buf_putc(out, cap + 1, &o, '\0');
		free(fixed);
		return out;
	}
}

/* Quote-aware scan: index of the ')' matching the '(' at open, or SIZE_MAX. */
static size_t r_match_paren(const char *sql, size_t len, size_t open)
{
	enum { ST_NORM, ST_SINGLE, ST_DOUBLE } st = ST_NORM;
	size_t depth = 1;
	size_t i;

	for (i = open + 1; i < len; i++) {
		char c = sql[i];

		switch (st) {
		case ST_NORM:
			if (c == '\'')
				st = ST_SINGLE;
			else if (c == '"')
				st = ST_DOUBLE;
			else if (c == '(')
				depth++;
			else if (c == ')' && --depth == 0)
				return i;
			break;
		case ST_SINGLE:
			if (c == '\'') {
				if (i + 1 < len && sql[i + 1] == '\'')
					i++;
				else
					st = ST_NORM;
			}
			break;
		case ST_DOUBLE:
			if (c == '"') {
				if (i + 1 < len && sql[i + 1] == '"')
					i++;
				else
					st = ST_NORM;
			}
			break;
		}
	}
	return SIZE_MAX;
}

/* Does the comma-separated identifier list sql[a..b) contain name? */
static int r_list_has(const char *sql, size_t a, size_t b, const char *name)
{
	size_t nl = strlen(name);
	size_t i = a;

	while (i < b) {
		while (i < b && (isspace((unsigned char)sql[i]) || sql[i] == ','))
			i++;
		if (i >= b)
			break;
		{
			size_t s = i;

			while (i < b && !(isspace((unsigned char)sql[i]) || sql[i] == ','))
				i++;
			if (i > s) {
				size_t n = i - s;

				if (sql[s] == '"' && n >= 2 && sql[s + n - 1] == '"') {
					s++;
					n -= 2;
				}
				if (n == nl && !strncasecmp(sql + s, name, nl))
					return 1;
			}
		}
	}
	return 0;
}

/* ---------- column-type helpers (Tarantool dialect) ---------- */

static int type_is_uuid(const char *t)
{
	return t && t[0] && !strcasecmp(t, "uuid");
}

static int type_is_string(const char *t)
{
	static const char *names[] = { "varchar", "char", "character", "text", "string", NULL };
	int i;

	if (!t || !t[0])
		return 1;			/* empty -> default varchar(36) */
	for (i = 0; names[i]; i++) {
		size_t n = strlen(names[i]);

		if (!strncasecmp(t, names[i], n) &&
			(t[n] == '\0' || t[n] == '(' || t[n] == ' '))
			return 1;
	}
	return 0;
}

/* Effective core-uuid flag: an explicit attribute wins; otherwise string
 * typed PK columns default to core-generated UUIDv7 (Tarantool-side uuid()
 * produces a UUID value, which a STRING column rejects). */
static int pkadd_core_uuid(const tnt_pkadd_rule_t *pa)
{
	if (pa->core_uuid >= 0)
		return pa->core_uuid;
	return type_is_string(pa->type) ? 1 : 0;
}

/* Append s as a single-quoted SQL string literal (quotes doubled). */
static void sq_literal(char *out, size_t cap, size_t *o, const char *s)
{
	buf_putc(out, cap, o, '\'');
	while (*s) {
		if (*s == '\'')
			buf_putc(out, cap, o, '\'');
		buf_putc(out, cap, o, *s++);
	}
	buf_putc(out, cap, o, '\'');
}

typedef struct inj_col {
	char field[64];
	char expr[192];
} inj_col_t;

/* INSERT INTO <table> (<cols>) VALUES (...): inject columns the caller did
 * not supply:
 *   - primaryKeyAdd rule: the synthetic PK column, filled with the
 *     configured source (core UUIDv7 literal/CAST, or uuid() for uuid type);
 *   - addField rules with isNull="false" and a value: the configured value
 *     ($${var} is expanded at runtime through ctx->resolver).
 * Multi-row INSERTs and statements that already list the columns are left
 * unchanged (NULL). Returns a malloc'ed rewritten statement or NULL. */
static char *inject_insert_columns(const tnt_rules_t *r, const tnt_stmt_t *st,
								   const tnt_translate_ctx_t *ctx)
{
	const tnt_pkadd_rule_t *pa = find_pkadd(r, st->table);
	const tnt_addfield_rule_t *af = NULL;
	int naf = 0;
	inj_col_t inj[TNT_RULES_MAX];
	int ninj = 0;
	size_t len = st->end - st->start;
	const char *sql = st->sql + st->start;
	size_t cl, cr, vl, vr, i;
	char *out;
	size_t cap, o = 0;

	find_fields(r, st->table, &af, &naf);
	if (!pa && naf == 0)
		return NULL;

	/* column-list opening paren after the table name */
	cl = SIZE_MAX;
	for (i = 0; i < len; i++) {
		if (sql[i] == '(') {
			cl = i;
			break;
		}
	}
	if (cl == SIZE_MAX)
		return NULL;
	cr = r_match_paren(sql, len, cl);
	if (cr == SIZE_MAX)
		return NULL;

	/* VALUES keyword and the first value tuple */
	vl = find_word(sql + cr + 1, len - cr - 1, "values");
	if (vl == SIZE_MAX)
		return NULL;
	vl += cr + 1 + 6;		/* skip past the "values" keyword itself */
	while (vl < len && isspace((unsigned char)sql[vl]))
		vl++;
	if (vl >= len || sql[vl] != '(')
		return NULL;
	vr = r_match_paren(sql, len, vl);
	if (vr == SIZE_MAX)
		return NULL;
	/* multi-row INSERT (…), (…) is not rewritten (the core never sends it) */
	for (i = vr + 1; i < len; i++) {
		if (isspace((unsigned char)sql[i]))
			continue;
		if (sql[i] == ',') {
			while (i + 1 < len && isspace((unsigned char)sql[i + 1]))
				i++;
			if (i + 1 < len && sql[i + 1] == '(')
				return NULL;
		}
		break;
	}

	/* primaryKeyAdd column */
	if (pa && !r_list_has(sql, cl + 1, cr, pa->field)) {
		int cu = pkadd_core_uuid(pa);
		int isu = type_is_uuid(pa->type[0] ? pa->type : "varchar(36)");

		/* probe on 3.8: uuid() yields a native UUID value, which a STRING
		 * column rejects ("type mismatch"); forcing core-uuid=false on a
		 * string PK is a misconfiguration -> warn and switch to core UUIDv7 */
		if (!cu && !isu) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "mod_tarantool: %s.%s is string-typed but "
							  "core-uuid=false; uuid() is not accepted for "
							  "string keys, forcing core UUIDv7\n",
							  pa->table, pa->field);
			cu = 1;
		}
		snprintf(inj[ninj].field, sizeof(inj[ninj].field), "%s", pa->field);
		if (cu && ctx && ctx->core_uuid) {
			if (isu)
				snprintf(inj[ninj].expr, sizeof(inj[ninj].expr),
						 "CAST('%s' AS UUID)", ctx->core_uuid);
			else
				snprintf(inj[ninj].expr, sizeof(inj[ninj].expr),
						 "'%s'", ctx->core_uuid);
		} else {
			if (cu) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
								  "mod_tarantool: core-uuid requested for %s.%s but "
								  "no generator is available, falling back to uuid()\n",
								  pa->table, pa->field);
			}
			snprintf(inj[ninj].expr, sizeof(inj[ninj].expr), "uuid()");
		}
		ninj++;
	}

	/* addField columns */
	for (i = 0; i < (size_t)naf; i++) {
		const tnt_addfield_rule_t *f = &af[i];
		const char *val = f->value;
		size_t vlen;

		if (f->is_null || !val[0])
			continue;
		if (r_list_has(sql, cl + 1, cr, f->field))
			continue;
		vlen = strlen(val);
		/* resolve $${global} / ${var} ONLY when in-line="true"; without the
		 * attribute the value is inserted literally, as-is. An unresolved
		 * variable is inserted literally too (with a warning) so a NOT NULL
		 * column never silently disappears from the INSERT. */
		if (f->inline_val && vlen > 2 && val[0] == '$' && val[vlen - 1] == '}' &&
			(val[1] == '{' || (val[1] == '$' && val[2] == '{'))) {
			char nm[64];
			const char *rv;
			size_t off = (val[1] == '{') ? 2 : 3;

			if (vlen - off - 1 >= sizeof(nm))
				continue;
			memcpy(nm, val + off, vlen - off - 1);
			nm[vlen - off - 1] = '\0';
			rv = (ctx && ctx->resolver) ? ctx->resolver(nm, ctx->ud) : NULL;
			if (!rv) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
								  "mod_tarantool: %s.%s value %s unresolved, "
								  "inserting literally\n", f->table, f->field, val);
			} else {
				val = rv;
			}
		}
		snprintf(inj[ninj].field, sizeof(inj[ninj].field), "%s", f->field);
		{
			size_t eo = 0;

			if (type_is_uuid(f->type)) {
				buf_puts(inj[ninj].expr, sizeof(inj[ninj].expr), &eo, "CAST(");
				sq_literal(inj[ninj].expr, sizeof(inj[ninj].expr), &eo, val);
				buf_puts(inj[ninj].expr, sizeof(inj[ninj].expr), &eo, " AS UUID)");
			} else {
				sq_literal(inj[ninj].expr, sizeof(inj[ninj].expr), &eo, val);
			}
		}
		ninj++;
	}

	if (ninj == 0)
		return NULL;

	/* rebuild: cols-head + injected fields + ") VALUES (" +
	 * original tuple values + injected exprs + ")" */
	cap = len + 256 * (size_t)ninj;
	out = (char *)malloc(cap);
	if (!out)
		return NULL;
	buf_putn(out, cap, &o, sql, cr);
	for (i = 0; i < (size_t)ninj; i++) {
		buf_puts(out, cap, &o, ", ");
		buf_puts(out, cap, &o, inj[i].field);
	}
	buf_putn(out, cap, &o, sql + cr, vl + 1 - cr);
	{
		int orig_empty = 1;
		size_t j;

		for (j = vl + 1; j < vr; j++) {
			if (!isspace((unsigned char)sql[j])) {
				orig_empty = 0;
				break;
			}
		}
		/* original values (without the closing ')') */
		if (!orig_empty)
			buf_putn(out, cap, &o, sql + vl + 1, vr - (vl + 1));
		/* injected expressions go LAST so column order matches */
		for (i = 0; i < (size_t)ninj; i++) {
			buf_puts(out, cap, &o, ", ");
			buf_puts(out, cap, &o, inj[i].expr);
		}
	}
	buf_putc(out, cap, &o, ')');
	buf_putc(out, cap, &o, '\0');
	return out;
}

/* SELECT with "FROM <view>": rewrite into "FROM (<body>) AS <view>" so the
 * statement survives on Tarantool, which has no VIEW. Only the FIRST "FROM"
 * clause is inspected; a view referenced elsewhere (JOIN, nested SELECT) is
 * not expanded. Returns a malloc'ed statement or NULL when nothing matched. */
static char *expand_view_select(const tnt_rules_t *r, const tnt_stmt_t *st)
{
	size_t len = st->end - st->start;
	const char *sql = st->sql + st->start;
	size_t fi = tnt_sql_find_keyword(sql, len, "from");
	size_t pos;
	int vi;

	if (fi == SIZE_MAX || r->nviews == 0)
		return NULL;
	pos = fi + 4;
	while (pos < len && isspace((unsigned char)sql[pos]))
		pos++;
	for (vi = 0; vi < r->nviews; vi++) {
		size_t vlen = strlen(r->views[vi].name);
		char after;
		size_t need;
		char *out;
		size_t o = 0;

		if (vlen == 0 || pos + vlen > len || strncasecmp(sql + pos, r->views[vi].name, vlen))
			continue;
		after = (pos + vlen < len) ? sql[pos + vlen] : ' ';
		if (!(isspace((unsigned char)after) || after == ',' || after == ';'))
			continue;

		need = len + strlen(r->views[vi].body) + vlen + 16;
		out = (char *)malloc(need);
		if (!out)
			return NULL;
		buf_putn(out, need, &o, sql, pos);			/* up to and incl. "from " */
		buf_puts(out, need, &o, "(");
		buf_puts(out, need, &o, r->views[vi].body);
		buf_puts(out, need, &o, ") as ");
		buf_puts(out, need, &o, r->views[vi].name);
		buf_putn(out, need, &o, sql + pos + vlen, len - (pos + vlen));
		buf_putc(out, need, &o, '\0');
		return out;
	}
	return NULL;
}

SWITCH_DECLARE(char *) tnt_rules_translate(const tnt_rules_t *r, const tnt_stmt_t *st,
										   int server_major, const tnt_translate_ctx_t *ctx,
										   int *is_noop)
{
	*is_noop = 0;

	switch (st->type) {
	case TNT_STMT_CREATE_VIEW:
		*is_noop = 1;
		return NULL;
	case TNT_STMT_CREATE_TABLE:
		return translate_create_table(r, st);
	case TNT_STMT_CREATE_INDEX:
		return translate_create_index(st);
	case TNT_STMT_DROP_TABLE:
		return translate_drop_table(st);
	case TNT_STMT_INSERT:
		{
			char *inj = inject_insert_columns(r, st, ctx);

			if (inj)
				return inj;
		}
		/* FALLTHROUGH: no injection needed, pass through */
	case TNT_STMT_SELECT:
		{
			char *exp = expand_view_select(r, st);

			if (exp)
				return exp;
		}
		/* FALLTHROUGH: plain select without a declared view */
	case TNT_STMT_ALTER_TABLE:
	default:
		/* passthrough (ALTER ADD COLUMN, DML) with BIGINT -> INTEGER */
		return fix_bigint(st->sql + st->start, st->end - st->start);
	}

	(void)server_major;
}

/* Case-insensitive substring search. */
static const char *ci_strstr(const char *hay, const char *needle)
{
	size_t nl = strlen(needle);
	size_t hl = strlen(hay);

	if (nl > hl)
		return NULL;
	for (size_t i = 0; i + nl <= hl; i++) {
		size_t j;

		for (j = 0; j < nl; j++) {
			if (tolower((unsigned char)hay[i + j]) != tolower((unsigned char)needle[j]))
				break;
		}
		if (j == nl)
			return hay + i;
	}
	return NULL;
}

SWITCH_DECLARE(int) tnt_rules_is_benign_error(const char *err, int cleanup_ddl)
{
	if (!err)
		return 0;
	/* idempotent DDL races / lost updates the core treats as success */
	if (ci_strstr(err, "already exists") ||
		ci_strstr(err, "duplicate key")) {
		return 1;
	}
	/* "Space X does not exist": benign ONLY for idempotent cleanup DDL
	 * (DROP or CREATE INDEX). For data statements it MUST propagate:
	 * switch_cache_db_test_reactive() uses a failing SELECT as the signal
	 * to (re)create the table; swallowing the error here leaves the core
	 * running WITHOUT any schema (every DML silently no-ops). */
	if (cleanup_ddl && ci_strstr(err, "does not exist"))
		return 1;
	return 0;
}
