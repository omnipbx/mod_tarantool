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
 * tnt_sql.h -- lightweight SQL scanner: statement splitter and classifier.
 *
 * The splitter uses a deterministic state machine (quoted strings with ''
 * escaping, line/block comments, balanced parentheses). Semicolons inside
 * string literals or comments are never treated as statement separators.
 *
 */

#ifndef TNT_SQL_H
#define TNT_SQL_H

#include <stddef.h>
#include <switch.h>

typedef enum {
	TNT_STMT_UNKNOWN = 0,
	TNT_STMT_CREATE_TABLE,
	TNT_STMT_CREATE_VIEW,
	TNT_STMT_CREATE_INDEX,
	TNT_STMT_DROP_TABLE,
	TNT_STMT_ALTER_TABLE,
	TNT_STMT_INSERT,
	TNT_STMT_UPDATE,
	TNT_STMT_DELETE,
	TNT_STMT_SELECT,
	TNT_STMT_BEGIN,
	TNT_STMT_COMMIT,
	TNT_STMT_ROLLBACK,
	TNT_STMT_PRAGMA
} tnt_stmt_type_t;

/* A single statement span inside a possibly multi-statement SQL string. */
typedef struct tnt_stmt {
	const char *sql;		/* borrowed full SQL string */
	size_t start;			/* offset of the first significant char */
	size_t end;				/* offset one past the last char (before ';') */
	tnt_stmt_type_t type;
	char table[64];			/* parsed table name for DDL/DML statements */
} tnt_stmt_t;

#define TNT_SQL_MAX_STMTS 32

/* Split a SQL string into statements on top-level ';'.
 * Returns the number of statements found (<= cap). Empty statements are skipped. */
SWITCH_DECLARE(int) tnt_sql_split(const char *sql, tnt_stmt_t *stmts, int cap);

/* Classify a single statement span; fills type and table. */
SWITCH_DECLARE(void) tnt_sql_classify(const char *sql, size_t start, size_t end, tnt_stmt_t *out);

/* Case-insensitive whole-word keyword search within [s, s+len). Returns offset or SIZE_MAX. */
SWITCH_DECLARE(size_t) tnt_sql_find_keyword(const char *s, size_t len, const char *kw);

/* Lexer-level rewrite: quote Tarantool-reserved words used as bare identifiers
 * ('uuid', 'alias') with double quotes so statements survive Tarantool's parser.
 * words/nwords extends the built-in list at runtime (tarantool.conf.xml
 * <reserved> section); words may be NULL when nwords == 0. String literals and
 * already-quoted identifiers are copied verbatim; a reserved word immediately
 * followed by '(' (function call, e.g. uuid()) is left alone.
 * Writes at most dstcap bytes incl. NUL; returns chars written (excluding NUL). */
SWITCH_DECLARE(size_t) tnt_sql_quote_reserved(const char *src, char *dst, size_t dstcap,
											  const char *const *words, int nwords);

/* Lightweight config-time SQL validation (no server round-trip): splits the
 * string on top-level ';' and rejects empty/comment-only input, too many
 * statements, unterminated string literals/comments, unbalanced parentheses
 * and statements with an unrecognized leading keyword. want_select requires
 * exactly one SELECT statement (used by <views>, whose bodies substitute a
 * missing Tarantool VIEW). Returns 0 when valid; -1 otherwise, reason
 * (reason_cap bytes, NUL-terminated) carries a human-readable cause. */
SWITCH_DECLARE(int) tnt_sql_validate(const char *sql, int want_select,
									 char *reason, size_t reason_cap);

/* ---- statement templateize (translate-cache key layer) ---- */

/* Marker byte replacing data literals inside a statement template.
 * Impossible in valid SQL text; never produced by translate/quote. */
#define TNT_SQL_TPL_MARK	'\001'

/* Max number of data literals extracted from one statement; statements
 * with more are not cacheable. */
#define TNT_SQL_VALS_MAX	32
/* Max literal payload length; longer literals make the statement
 * uncacheable. */
#define TNT_SQL_VAL_MAX		512

typedef struct tnt_sql_val {
	char text[TNT_SQL_VAL_MAX];	/* literal payload (''-unescaped for strings) */
	int is_string;				/* 1 = single-quoted literal, 0 = bare number */
} tnt_sql_val_t;

/* Replace data literals in the statement span with TNT_SQL_TPL_MARK bytes
 * and collect them (in order) into vals. The template is lowercased and
 * whitespace-collapsed, so its hash is stable across calls of the same
 * statement shape with different literals. Quoted identifiers ("..."),
 * comments and everything else are copied verbatim. Returns 0 on success,
 * -1 when the statement is not cacheable (payload overflow, too many
 * literals, marker byte in the source). */
SWITCH_DECLARE(int) tnt_sql_templateize(const char *sql, size_t start, size_t end,
										char *tpl, size_t tplcap,
										tnt_sql_val_t *vals, int maxvals, int *nvals);

/* Rebuild a concrete statement from a template: every TNT_SQL_TPL_MARK
 * byte is replaced by the next value (strings are re-quoted with ''
 * escaping, numbers copied verbatim). Returns 0 on success, -1 when the
 * marker count does not match nvals or the output does not fit. */
SWITCH_DECLARE(int) tnt_sql_rebuild(const char *tpl, const tnt_sql_val_t *vals,
									int nvals, char *out, size_t cap);

/* Convert a template into a Tarantool prepared-statement text: every
 * TNT_SQL_TPL_MARK byte becomes a positional '?' parameter. Returns 0 on
 * success, -1 when the output does not fit (cap includes the NUL). */
SWITCH_DECLARE(int) tnt_sql_tpl_qmarks(const char *tpl, char *out, size_t cap);

/* Detect data literals in the SELECT list of a statement template, i.e.
 * marker bytes before the first FROM keyword. Tarantool SQL cannot type
 * anonymous '?' parameters in the SELECT list (PREPARE fails with
 * "Supplied key type of part N ..."), so such literals must stay constants
 * in the prepared text. Returns 1 when the template is a SELECT and carries
 * at least one marker before FROM (or anywhere, when there is no FROM),
 * 0 otherwise. Used once at cache-store time to tag the entry. */
SWITCH_DECLARE(int) tnt_sql_has_select_markers(const char *tpl, size_t len);

/* Materialize the SELECT-list literals of a statement template into
 * concrete quoted literals and remove their values from the front of vals
 * (the array is compacted in place, *nvals reduced). The result is a
 * "partially materialized" template: constants in the SELECT list, marker
 * bytes everywhere else - exactly what Tarantool's PREPARE accepts. When
 * the template has no markers before FROM, out is a verbatim copy and
 * nothing is removed. Returns 0 on success, -1 on marker/vals mismatch or
 * output overflow (vals is left untouched on failure). */
SWITCH_DECLARE(int) tnt_sql_materialize_select(const char *tpl, tnt_sql_val_t *vals,
											   int *nvals, char *out, size_t cap);

/* Inject the SEQSCAN keyword before every table name in FROM/JOIN clauses
 * (including subqueries inside DML: DELETE ... IN (SELECT ... FROM t1
 * JOIN t2 ...)). Tarantool forbids full scans unless the session variable
 * sql_full_scan is set - which the node01 fork does NOT know ("Session
 * setting sql_full_scan doesn't exist") - so the only reliable way to allow
 * a scan is the explicit FROM SEQSCAN t form.
 *
 * Rules: only statements that start with SELECT or are inside parentheses
 * (subqueries) are touched, so "DELETE FROM t" stays untouched while its
 * "IN (SELECT ... FROM t2 ...)" subquery gets SEQSCAN; "FROM (SELECT ...)"
 * is not injected (the subquery has its own FROM); an already-present
 * SEQSCAN is not duplicated; string literals, quoted identifiers and
 * comments are skipped. Returns 0 on success, -1 when the output does not
 * fit (cap includes the NUL). */
SWITCH_DECLARE(int) tnt_sql_inject_seqscan(const char *sql, char *out, size_t cap);

#endif /* TNT_SQL_H */
