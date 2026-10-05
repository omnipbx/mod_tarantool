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
* tnt_sql.c -- statement splitter and classifier
*
*/

#include "tnt_sql.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static int is_ident_start(char c)
{
	return isalpha((unsigned char)c) || c == '_';
}

static int is_ident_char(char c)
{
	return isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.';
}

/* Skip whitespace and comments; returns new position. */
static size_t skip_ws(const char *s, size_t len, size_t pos)
{
	while (pos < len) {
		if (isspace((unsigned char)s[pos])) {
			pos++;
		} else if (s[pos] == '-' && pos + 1 < len && s[pos + 1] == '-') {
			while (pos < len && s[pos] != '\n')
				pos++;
		} else if (s[pos] == '/' && pos + 1 < len && s[pos + 1] == '*') {
			pos += 2;
			while (pos + 1 < len && !(s[pos] == '*' && s[pos + 1] == '/'))
				pos++;
			pos = (pos + 2 < len) ? pos + 2 : len;
		} else {
			break;
		}
	}
	return pos;
}

SWITCH_DECLARE(size_t) tnt_sql_find_keyword(const char *s, size_t len, const char *kw)
{
	size_t kwlen = strlen(kw);
	size_t i;

	for (i = 0; i + kwlen <= len; i++) {
		size_t j;

		/* require a word boundary on the left (position 0 is a boundary) */
		if (i > 0 && is_ident_char(s[i - 1]))
			continue;
		for (j = 0; j < kwlen; j++) {
			if (tolower((unsigned char)s[i + j]) != tolower((unsigned char)kw[j]))
				break;
		}
		if (j == kwlen &&
			(i + kwlen == len || !is_ident_char(s[i + kwlen]))) {
			return i;
		}
	}
	return SIZE_MAX;
}

/* Copy an identifier starting at *pos into out; advances pos past it. */
static void parse_ident(const char *s, size_t len, size_t *pos, char *out, size_t outsz)
{
	size_t p = *pos;
	size_t n = 0;

	while (p < len && isspace((unsigned char)s[p]))
		p++;

	if (p < len && s[p] == '"') {
		/* quoted identifier */
		p++;
		while (p < len && s[p] != '"' && n + 1 < outsz) {
			out[n++] = s[p++];
		}
		if (p < len && s[p] == '"')
			p++;
	} else {
		while (p < len && is_ident_char(s[p]) && n + 1 < outsz) {
			out[n++] = (char)tolower((unsigned char)s[p++]);
		}
	}
	out[n] = '\0';
	*pos = p;
}

/* Skip an optional leading clause in CREATE/DROP TABLE and CREATE INDEX:
	*   IF [NOT] EXISTS
	* Returns the position after the clause, or the original position if absent.
	* tmp is a caller-owned scratch buffer of size tmpsz. */
static size_t skip_if_clause(const char *sql, size_t len, size_t pos,
							 char *tmp, size_t tmpsz)
{
	size_t p = skip_ws(sql, len, pos);
	size_t l = 0;

	while (p + l < len && is_ident_char(sql[p + l]) && l < tmpsz - 1) {
		tmp[l] = (char)tolower((unsigned char)sql[p + l]);
		l++;
	}
	tmp[l] = '\0';
	if (strcmp(tmp, "if"))
		return pos;			/* not an IF clause at all */

	p = skip_ws(sql, len, p + l);

	/* optional NOT (CREATE ... IF NOT EXISTS) */
	l = 0;
	while (p + l < len && is_ident_char(sql[p + l]) && l < tmpsz - 1) {
		tmp[l] = (char)tolower((unsigned char)sql[p + l]);
		l++;
	}
	tmp[l] = '\0';
	if (!strcmp(tmp, "not"))
		p = skip_ws(sql, len, p + l);

	/* then EXISTS */
	l = 0;
	while (p + l < len && is_ident_char(sql[p + l]) && l < tmpsz - 1) {
		tmp[l] = (char)tolower((unsigned char)sql[p + l]);
		l++;
	}
	tmp[l] = '\0';
	if (strcmp(tmp, "exists"))
		return pos;			/* "IF <other>": not a create/drop guard clause */

	return skip_ws(sql, len, p + l);
}

SWITCH_DECLARE(void) tnt_sql_classify(const char *sql, size_t start, size_t end, tnt_stmt_t *out)
{
	size_t p = skip_ws(sql, end, start);
	size_t word_len = 0;
	char word[16] = "";
	char w2[16] = "";
	char tmp[64] = "";

	memset(out, 0, sizeof(*out));
	out->sql = sql;
	out->start = start;
	out->end = end;

	while (p + word_len < end && is_ident_start(sql[p + word_len]))
		word_len++;

	if (!word_len)
		return;
	if (word_len >= sizeof(word))
		word_len = sizeof(word) - 1;
	for (size_t i = 0; i < word_len; i++)
		word[i] = (char)tolower((unsigned char)sql[p + i]);
	word[word_len] = '\0';

	if (!strcmp(word, "create")) {
		size_t q = skip_ws(sql, end, p + word_len);
		size_t l2 = 0;

		while (q + l2 < end && is_ident_start(sql[q + l2]))
			l2++;
		if (l2 >= sizeof(w2))
			l2 = sizeof(w2) - 1;
		for (size_t i = 0; i < l2; i++)
			w2[i] = (char)tolower((unsigned char)sql[q + i]);
		w2[l2] = '\0';

		if (!strcmp(w2, "table")) {
			size_t r = q + l2;
			out->type = TNT_STMT_CREATE_TABLE;
			r = skip_if_clause(sql, end, r, tmp, sizeof(tmp));
			parse_ident(sql, end, &r, out->table, sizeof(out->table));
		} else if (!strcmp(w2, "view")) {
			size_t r = q + l2;
			out->type = TNT_STMT_CREATE_VIEW;
			parse_ident(sql, end, &r, out->table, sizeof(out->table));
		} else if (!strcmp(w2, "index") || !strcmp(w2, "unique")) {
			/* CREATE [UNIQUE] INDEX [IF NOT EXISTS] <name> ON <table> */
			size_t r = q + l2;

			out->type = TNT_STMT_CREATE_INDEX;
			r = skip_ws(sql, end, r);
			if (!strcmp(w2, "unique")) {
				/* skip the "index" keyword */
				size_t l3 = 0;
				while (r + l3 < end && is_ident_start(sql[r + l3]))
					l3++;
				r = skip_ws(sql, end, r + l3);
			}
			/* optional IF NOT EXISTS */
			r = skip_if_clause(sql, end, r, tmp, sizeof(tmp));
			/* index name */
			parse_ident(sql, end, &r, tmp, sizeof(tmp));
			/* ON <table> */
			{
				size_t on = tnt_sql_find_keyword(sql + r, end - r, "on");
				if (on != SIZE_MAX) {
					size_t rr = skip_ws(sql, end, r + on + 2);
					parse_ident(sql, end, &rr, out->table, sizeof(out->table));
				}
			}
		}
	} else if (!strcmp(word, "alter")) {
		size_t q = skip_ws(sql, end, p + word_len);
		size_t l2 = 0;
		while (q + l2 < end && is_ident_start(sql[q + l2]))
			l2++;
		if (l2 >= sizeof(w2))
			l2 = sizeof(w2) - 1;
		for (size_t i = 0; i < l2; i++)
			w2[i] = (char)tolower((unsigned char)sql[q + i]);
		w2[l2] = '\0';
		if (!strcmp(w2, "table")) {
			size_t r = q + l2;
			out->type = TNT_STMT_ALTER_TABLE;
			parse_ident(sql, end, &r, out->table, sizeof(out->table));
		}
	} else if (!strcmp(word, "drop")) {
		size_t q = skip_ws(sql, end, p + word_len);
		size_t l2 = 0;
		while (q + l2 < end && is_ident_start(sql[q + l2]))
			l2++;
		if (l2 >= sizeof(w2))
			l2 = sizeof(w2) - 1;
		for (size_t i = 0; i < l2; i++)
			w2[i] = (char)tolower((unsigned char)sql[q + i]);
		w2[l2] = '\0';
		if (!strcmp(w2, "table")) {
			size_t r = q + l2;
			out->type = TNT_STMT_DROP_TABLE;
			r = skip_if_clause(sql, end, r, tmp, sizeof(tmp));
			parse_ident(sql, end, &r, out->table, sizeof(out->table));
		}
	} else if (!strcmp(word, "insert")) {
		size_t q = skip_ws(sql, end, p + word_len);
		size_t l2 = 0;
		while (q + l2 < end && is_ident_start(sql[q + l2]))
			l2++;
		if (l2 >= sizeof(w2))
			l2 = sizeof(w2) - 1;
		for (size_t i = 0; i < l2; i++)
			w2[i] = (char)tolower((unsigned char)sql[q + i]);
		w2[l2] = '\0';
		if (!strcmp(w2, "into")) {
			size_t r = q + l2;
			out->type = TNT_STMT_INSERT;
			parse_ident(sql, end, &r, out->table, sizeof(out->table));
		}
	} else if (!strcmp(word, "update")) {
		size_t q = skip_ws(sql, end, p + word_len);
		out->type = TNT_STMT_UPDATE;
		parse_ident(sql, end, &q, out->table, sizeof(out->table));
	} else if (!strcmp(word, "delete")) {
		size_t q = skip_ws(sql, end, p + word_len);
		size_t l2 = 0;
		while (q + l2 < end && is_ident_start(sql[q + l2]))
			l2++;
		if (l2 >= sizeof(w2))
			l2 = sizeof(w2) - 1;
		for (size_t i = 0; i < l2; i++)
			w2[i] = (char)tolower((unsigned char)sql[q + i]);
		w2[l2] = '\0';
		if (!strcmp(w2, "from")) {
			size_t r = q + l2;
			out->type = TNT_STMT_DELETE;
			parse_ident(sql, end, &r, out->table, sizeof(out->table));
		}
	} else if (!strcmp(word, "select")) {
		out->type = TNT_STMT_SELECT;
	} else if (!strcmp(word, "begin")) {
		out->type = TNT_STMT_BEGIN;
	} else if (!strcmp(word, "commit")) {
		out->type = TNT_STMT_COMMIT;
	} else if (!strcmp(word, "rollback")) {
		out->type = TNT_STMT_ROLLBACK;
	} else if (!strcmp(word, "pragma")) {
		out->type = TNT_STMT_PRAGMA;
	} else {
		out->type = TNT_STMT_UNKNOWN;
	}
}

SWITCH_DECLARE(int) tnt_sql_split(const char *sql, tnt_stmt_t *stmts, int cap)
{
	size_t len = strlen(sql);
	size_t i = 0;
	int n = 0;

	while (i < len && n < cap) {
		enum { ST_NORMAL, ST_SINGLE, ST_DOUBLE, ST_LINE, ST_BLOCK } st = ST_NORMAL;
		size_t stmt_start = skip_ws(sql, len, i);
		size_t p = stmt_start;
		int depth = 0;
		size_t stmt_end = stmt_start;

		if (stmt_start >= len)
			break;

		while (p < len) {
			char c = sql[p];

			switch (st) {
			case ST_NORMAL:
				if (c == '\'') {
					st = ST_SINGLE;
				} else if (c == '"') {
					st = ST_DOUBLE;
				} else if (c == '-' && p + 1 < len && sql[p + 1] == '-') {
					st = ST_LINE;
					p++;
				} else if (c == '/' && p + 1 < len && sql[p + 1] == '*') {
					st = ST_BLOCK;
					p++;
				} else if (c == '(') {
					depth++;
				} else if (c == ')') {
					if (depth > 0)
						depth--;
				} else if (c == ';' && depth == 0) {
					/* statement boundary: leave the scan loop */
					goto stmt_done;
				}
				break;
			case ST_SINGLE:
				if (c == '\'') {
					if (p + 1 < len && sql[p + 1] == '\'') {
						p++;		/* escaped '' stays inside the literal */
					} else {
						st = ST_NORMAL;
					}
				}
				break;
			case ST_DOUBLE:
				if (c == '"') {
					if (p + 1 < len && sql[p + 1] == '"') {
						p++;
					} else {
						st = ST_NORMAL;
					}
				}
				break;
			case ST_LINE:
				if (c == '\n')
					st = ST_NORMAL;
				break;
			case ST_BLOCK:
				if (c == '*' && p + 1 < len && sql[p + 1] == '/') {
					st = ST_NORMAL;
					p++;
				}
				break;
			}
			p++;
		}
stmt_done:
		/* trim trailing whitespace from the statement */
		stmt_end = p;
		while (stmt_end > stmt_start && isspace((unsigned char)sql[stmt_end - 1]))
			stmt_end--;

		if (stmt_end > stmt_start) {
			tnt_sql_classify(sql, stmt_start, stmt_end, &stmts[n]);
			n++;
		}

		/* advance past the ';' if present */
		if (p < len && sql[p] == ';')
			p++;
		i = p;
	}

	return n;
}

static int tnt_sql_is_keyword(const char *s, size_t len);

/* 1 if w[0..len) matches one of the built-in Tarantool-reserved words or an
 * entry of the configurable extra list (words/nwords), case-insensitively.
 * The built-in defaults ("uuid", "alias") are always active; the <reserved>
 * section of tarantool.conf.xml can add more words without a rebuild. */
static int tnt_sql_reserved_match(const char *w, size_t len,
								  const char *const *words, int nwords)
{
	static const char *const base[] = { "uuid", "alias" };
	size_t k;

	for (k = 0; k < sizeof(base) / sizeof(base[0]); k++) {
		size_t bl = strlen(base[k]);

		if (len == bl && !strncasecmp(w, base[k], bl))
			return 1;
	}
	for (k = 0; k < (size_t)nwords; k++) {
		const char *wd = words ? words[k] : NULL;
		size_t wl = wd ? strlen(wd) : 0;

		if (wl == len && !strncasecmp(w, wd, len))
			return 1;
	}
	return 0;
}

/* Tarantool SQL reserves some words; when the core uses them as bare
 * identifiers (e.g. column name 'uuid'), the statement must quote them:
 * uuid -> "uuid", alias -> "alias". Double-quoting an identifier is always
 * safe, whether the word is reserved or not. words/nwords extends the
 * built-in list at runtime (tarantool.conf.xml <reserved> section). */
SWITCH_DECLARE(size_t) tnt_sql_quote_reserved(const char *src, char *dst, size_t dstcap,
											  const char *const *words, int nwords)
{
	size_t i = 0, o = 0;
	int prev_word = 0;		/* previous significant token was identifier-shaped */
	int prev_kw = 0;		/* ... and it was a SQL keyword */
	char prev_name[16];		/* text of the previous identifier-shaped token */
	size_t prev_nlen = 0;

	while (src[i] && o + 1 < dstcap) {
		char c = src[i];

		if (c == '\'' || c == '"') {
			/* copy a quoted section verbatim (string literal or an already
			 * quoted identifier) */
			char q = c;

			dst[o++] = src[i++];
			while (src[i] && o + 1 < dstcap) {
				dst[o++] = src[i];
				if (src[i] == q) {
					if (src[i + 1] == q) {	/* doubled quote escape */
						if (o + 1 >= dstcap)
							break;
						dst[o++] = src[i + 1];
						i += 2;
						continue;
					}
					i++;
					break;
				}
				i++;
			}
			prev_word = 0;
			prev_kw = 0;
			prev_nlen = 0;
			continue;
		}

		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_') {
			size_t start = i;
			size_t len;
			int quoted = 0;

			while ((src[i] >= 'A' && src[i] <= 'Z') || (src[i] >= 'a' && src[i] <= 'z') ||
				   (src[i] >= '0' && src[i] <= '9') || src[i] == '_')
				i++;
			len = i - start;
			if (tnt_sql_reserved_match(src + start, len, words, nwords)) {
				if (src[i] == '(') {
					/* 'uuid(' is a function call, not an identifier */
					quoted = 0;
				} else if (prev_kw && len == 3 && !strncasecmp(src + start, "key", 3) &&
						   prev_nlen == 7 &&
						   (!strncasecmp(prev_name, "primary", 7) ||
							!strncasecmp(prev_name, "foreign", 7))) {
					/* 'PRIMARY KEY' / 'FOREIGN KEY' is SQL syntax, not an
					 * identifier position — leave the word alone even when
					 * 'key' was added to the reserved dictionary */
					quoted = 0;
				} else if (!prev_word || prev_kw) {
					/* identifier position: after '( , .', at statement
					 * start, or after a SQL keyword (select/where/...) */
					quoted = 1;
				} else {
					/* preceded by another identifier: this is the TYPE
					 * position of a column definition (e.g.
					 * 'aliases_uuid UUID NOT NULL') — leave alone */
					quoted = 0;
				}
			}
			if (quoted) {
				if (o + len + 3 > dstcap)	/* quotes + NUL */
					goto done;
				dst[o++] = '"';
				memcpy(dst + o, src + start, len);
				o += len;
				dst[o++] = '"';
			} else {
				if (o + len >= dstcap)
					goto done;
				memcpy(dst + o, src + start, len);
				o += len;
			}
			prev_word = 1;
			prev_kw = tnt_sql_is_keyword(src + start, len);
			if (len < sizeof(prev_name)) {
				memcpy(prev_name, src + start, len);
				prev_nlen = len;
			} else {
				prev_nlen = 0;
			}
			continue;
		}

		if (c == '(' || c == ',' || c == '.') {
			/* identifier-list opener: the next word is a fresh identifier */
			prev_word = 0;
			prev_kw = 0;
			prev_nlen = 0;
		}
		dst[o++] = c;
		i++;
	}
done:
	dst[o] = '\0';
	return o;
}

static int tnt_sql_is_keyword(const char *s, size_t len)
{
	static const char *const keywords[] = {
		"add", "alter", "and", "as", "by", "case", "column", "constraint",
		"create", "default", "delete", "desc", "distinct", "drop", "else",
		"end", "exists", "foreign", "from", "group", "having", "in", "index",
		"inner", "insert", "into", "is", "join", "key", "left", "like",
		"limit", "not", "null", "on", "or", "order", "outer", "primary",
		"references", "right", "select", "set", "table", "then", "unique",
		"update", "values", "view", "when", "where", NULL
	};
	int k;

	for (k = 0; keywords[k]; k++) {
		if (len == strlen(keywords[k]) && !strncasecmp(s, keywords[k], len))
			return 1;
	}
	return 0;
}

/* Lexical check of one statement span: unterminated string literals /
	* comments and unbalanced parentheses. Returns 0 when the span is sound;
	* reason (rcap bytes) carries the first problem found. */
static int sql_span_ok(const char *sql, size_t start, size_t end,
					   char *reason, size_t rcap)
{
	enum { ST_NORMAL, ST_SINGLE, ST_DOUBLE, ST_LINE, ST_BLOCK } st = ST_NORMAL;
	size_t p = start;
	int depth = 0;

	while (p < end) {
		char c = sql[p];

		switch (st) {
		case ST_NORMAL:
			if (c == '\'') {
				st = ST_SINGLE;
			} else if (c == '"') {
				st = ST_DOUBLE;
			} else if (c == '-' && p + 1 < end && sql[p + 1] == '-') {
				st = ST_LINE;
				p++;
			} else if (c == '/' && p + 1 < end && sql[p + 1] == '*') {
				st = ST_BLOCK;
				p++;
			} else if (c == '(') {
				depth++;
			} else if (c == ')') {
				if (depth > 0)
					depth--;
				else {
					snprintf(reason, rcap, "unmatched ')'");
					return -1;
				}
			}
			break;
		case ST_SINGLE:
			if (c == '\'') {
				if (p + 1 < end && sql[p + 1] == '\'')
					p++;		/* escaped '' stays inside the literal */
				else
					st = ST_NORMAL;
			}
			break;
		case ST_DOUBLE:
			if (c == '"') {
				if (p + 1 < end && sql[p + 1] == '"')
					p++;
				else
					st = ST_NORMAL;
			}
			break;
		case ST_LINE:
			if (c == '\n')
				st = ST_NORMAL;
			break;
		case ST_BLOCK:
			if (c == '*' && p + 1 < end && sql[p + 1] == '/') {
				st = ST_NORMAL;
				p++;
			}
			break;
		}
		p++;
	}
	if (st == ST_SINGLE || st == ST_DOUBLE) {
		snprintf(reason, rcap, "unterminated string literal");
		return -1;
	}
	if (st == ST_BLOCK) {
		snprintf(reason, rcap, "unterminated block comment");
		return -1;
	}
	if (depth != 0) {
		snprintf(reason, rcap, "unbalanced parentheses");
		return -1;
	}
	return 0;
}

/* Lightweight config-time SQL validation (no server round-trip):
	* - empty / comment-only input fails;
	* - the string is split on top-level ';' and each statement is checked for
	*   unterminated string literals/comments, unbalanced parentheses and an
	*   unrecognized leading keyword;
	* - want_select requires exactly one SELECT (the <views> bodies substitute
	*   a missing Tarantool VIEW and must be pure single SELECTs).
	* Returns 0 when valid; -1 otherwise, reason (reason_cap bytes,
	* NUL-terminated) carries a human-readable cause. */
SWITCH_DECLARE(int) tnt_sql_validate(const char *sql, int want_select,
									 char *reason, size_t reason_cap)
{
	tnt_stmt_t stmts[TNT_SQL_MAX_STMTS];
	char msg[192];
	const char *q;
	int n, i;

	if (reason && reason_cap > 0)
		reason[0] = '\0';

	if (!sql) {
		snprintf(msg, sizeof(msg), "empty SQL");
	} else {
		for (q = sql; *q && isspace((unsigned char)*q); q++)
			;
		if (!*q) {
			snprintf(msg, sizeof(msg), "empty SQL");
		} else {
			n = tnt_sql_split(sql, stmts, TNT_SQL_MAX_STMTS);
			if (n == 0) {
				snprintf(msg, sizeof(msg), "no SQL statements found");
			} else if (n == TNT_SQL_MAX_STMTS) {
				snprintf(msg, sizeof(msg), "too many statements (>%d)", TNT_SQL_MAX_STMTS);
			} else if (want_select && n != 1) {
				snprintf(msg, sizeof(msg), "expected exactly one statement, got %d", n);
			} else {
				for (i = 0; i < n; i++) {
					char tmp[160];

					if (sql_span_ok(sql, stmts[i].start, stmts[i].end,
									tmp, sizeof(tmp))) {
						snprintf(msg, sizeof(msg), "statement %d: %s", i + 1, tmp);
						break;
					}
					if (stmts[i].type == TNT_STMT_UNKNOWN) {
						snprintf(msg, sizeof(msg),
								 "statement %d: unrecognized SQL", i + 1);
						break;
					}
					if (want_select && stmts[i].type != TNT_STMT_SELECT) {
						snprintf(msg, sizeof(msg),
								 "statement %d: expected SELECT", i + 1);
						break;
					}
				}
				if (i == n)
					return 0;	/* all statements valid */
			}
		}
	}
	if (reason && reason_cap > 0)
		snprintf(reason, reason_cap, "%s", msg);
	return -1;
}
