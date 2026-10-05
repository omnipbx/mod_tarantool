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

#endif /* TNT_SQL_H */
