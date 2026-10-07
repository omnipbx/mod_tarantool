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
* tnt_rules.h -- declarative DDL translation rules.
*
* The FreeSWITCH core and mod_sofia issue SQLite-flavoured DDL that
* Tarantool's SQL dialect cannot execute verbatim. This module translates
* each statement based on static rules from tarantool.conf.xml:
*
*   - <primaryKey table field>    mark an EXISTING column as PRIMARY KEY
*   - <primaryKeyAdd table field> add "field UUID NOT NULL DEFAULT <gen>"
*                                  and make it the PRIMARY KEY
*   - <addField table field type isNull default> add an arbitrary column
*   - <reserved name>             extend the list of words the SQL rewriter
*                                  double-quotes ("uuid", "alias" are built in)
*   - <views><view name CDATA>    declare SELECT bodies that substitute the
*                                  missing Tarantool VIEW: every core SELECT
*                                  with "FROM <name>" is rewritten into
*                                  "FROM (<body>) AS <name>"
*   - BIGINT -> INTEGER           (Tarantool has only INTEGER)
*   - CREATE VIEW                 -> no-op (logged by the caller)
*   - IF NOT EXISTS / IF EXISTS   removed (Tarantool rejects them)
*   - CREATE INDEX                -> IF NOT EXISTS form
*   - benign errors               mapped by tnt_rules_is_benign_error()
*
*/

#ifndef TNT_RULES_H
#define TNT_RULES_H

#include "tnt_sql.h"

#define TNT_RULES_MAX	64

/* Words the SQL rewriter must double-quote when used as bare identifiers
 * (Tarantool SQL reserved words). "uuid"/"alias" are built-in defaults set by
 * tnt_rules_init(); more words come from the <reserved> config section. */
#define TNT_RESERVED_MAX		32
#define TNT_RESERVED_WORD_LEN	32

/* Declarative VIEW bodies (<views> config section): Tarantool has no VIEW,
 * so a core SELECT with "FROM <name>" is rewritten into
 * "FROM (<body>) AS <name>". Bodies live in fixed buffers (no malloc/free)
 * so the whole tnt_rules_t stays a plain value-copyable struct. */
#define TNT_VIEWS_MAX		8
#define TNT_VIEW_BODY_LEN	4096

typedef struct tnt_view_rule {
	char name[64];				/* view name used in "FROM <name>" */
	char body[TNT_VIEW_BODY_LEN];	/* SELECT body (without CREATE VIEW ... AS) */
} tnt_view_rule_t;

typedef struct tnt_pk_rule {
	char table[64];
	char field[64];			/* existing column to make PRIMARY KEY */
} tnt_pk_rule_t;

typedef struct tnt_pkadd_rule {
	char table[64];
	char field[64];			/* column to ADD and make PRIMARY KEY */
	char type[32];			/* SQL type; "" = varchar(36) */
	int core_uuid;			/* -1 auto (string->yes, else no),
							   0 Tarantool-side uuid(), 1 core UUIDv7 */
} tnt_pkadd_rule_t;

typedef struct tnt_addfield_rule {
	char table[64];
	char field[64];
	char type[32];			/* SQL type, passed through as-is */
	int is_null;			/* 1 = allow NULL, 0 = NOT NULL */
	int inline_val;			/* 1 = resolve $${var}/${var} via the resolver
							   at INSERT time, 0 = insert the value literally */
	char def[128];			/* DEFAULT value; empty string = no DEFAULT */
	char value[128];		/* value injected on INSERT */
} tnt_addfield_rule_t;

/* Numeric columns whose values may arrive as QUOTED string literals
 * ('1791322069') from the FreeSWITCH core: channels.created_epoch and
 * calls.call_created_epoch are formatted by switch_core_sqldb.c as '%ld'
 * INSIDE single quotes. Tarantool does not coerce string->number, so such
 * literals are rewritten to CAST('...' AS <type>) on INSERT/UPDATE. The
 * <intColumns> config section declares exactly which (table, field) pairs
 * can show this variation; only those columns are ever touched. */
#define TNT_INTCOL_MAX	64

typedef struct tnt_intcol_rule {
	char table[64];
	char field[64];
	char type[16];			/* canonical CAST target (INTEGER/NUMBER/DOUBLE/DECIMAL) */
} tnt_intcol_rule_t;

/* Recognized numeric type names for <intColumns type="..."> and their
 * CAST(... AS ...) targets (Tarantool SQL). */
typedef struct tnt_intcol_type {
	const char *name;		/* value accepted in the config */
	const char *cast;		/* CAST(... AS <cast>) target */
} tnt_intcol_type_t;

typedef struct tnt_rules {
	tnt_pk_rule_t pk[TNT_RULES_MAX];
	int npk;
	tnt_pkadd_rule_t pkadd[TNT_RULES_MAX];
	int npkadd;
	tnt_addfield_rule_t fields[TNT_RULES_MAX];
	int nfields;
	char reserved[TNT_RESERVED_MAX][TNT_RESERVED_WORD_LEN];
	int nreserved;			/* "uuid" and "alias" pre-loaded by tnt_rules_init */
	tnt_view_rule_t views[TNT_VIEWS_MAX];
	int nviews;
	tnt_intcol_rule_t intcols[TNT_INTCOL_MAX];
	int nintcols;
	char uuid_default[64];		/* e.g. "uuid7()" (3.x) or "uuid()" (2.10) */
} tnt_rules_t;

/* Translation context supplied by the caller (mod_tarantool.c) so that
 * tnt_rules.c stays free of FreeSWITCH runtime APIs. */
typedef const char *(*tnt_var_resolver_t)(const char *name, void *ud);

typedef struct tnt_translate_ctx {
	const char *core_uuid;		/* pre-generated UUIDv7 string, or NULL */
	tnt_var_resolver_t resolver;	/* runtime $${var} lookup, or NULL */
	void *ud;
} tnt_translate_ctx_t;

SWITCH_DECLARE(void) tnt_rules_init(tnt_rules_t *r);
SWITCH_DECLARE(int) tnt_rules_add_pk(tnt_rules_t *r, const char *table, const char *field);
SWITCH_DECLARE(int) tnt_rules_add_pkadd(tnt_rules_t *r, const char *table, const char *field,
										const char *type, int core_uuid);
SWITCH_DECLARE(int) tnt_rules_add_field(tnt_rules_t *r, const char *table, const char *field,
										const char *type, int is_null, const char *def,
										const char *value, int inline_val);
/* Add a word to the double-quote dictionary (<reserved> section). Duplicates
 * (case-insensitive) are ignored; returns 0 on success, -1 when full/invalid. */
SWITCH_DECLARE(int) tnt_rules_add_reserved(tnt_rules_t *r, const char *word);

/* Add a VIEW body (<views> section). A re-declaration of an existing name
 * replaces the body (reload-friendly). Return codes:
 *   0  - added (or an existing name replaced);
 *  -1  - empty name or body;
 *  -2  - body does not fit TNT_VIEW_BODY_LEN: this is a HARD config error —
 *        the caller must reject the whole configuration (module load aborted
 *        / reload keeps the previously applied config);
 *  -3  - TNT_VIEWS_MAX reached: the extra view is ignored (non-fatal).
 * No truncation happens: an oversized body is rejected, never cut. */
SWITCH_DECLARE(int) tnt_rules_add_view(tnt_rules_t *r, const char *name, const char *body);

/* Add an <intColumns> entry. type must be a recognized numeric type (see
 * tnt_intcol_cast_for_type); duplicates (table+field) are ignored.
 * Returns 0 on success, -1 when invalid (empty names/type) or the
 * TNT_INTCOL_MAX limit is reached. */
SWITCH_DECLARE(int) tnt_rules_add_intcol(tnt_rules_t *r, const char *table,
										 const char *field, const char *type);

/* Canonical CAST target ("INTEGER", "NUMBER", "DOUBLE" or "DECIMAL") for a
 * recognized numeric type name ("BIGINT", "REAL", ...) used by <intColumns>;
 * NULL when the type is unknown. Used for config-time validation. */
SWITCH_DECLARE(const char *) tnt_intcol_cast_for_type(const char *type);

/* Translate a single statement. Returns a malloc'ed translated SQL string
 * (NUL-terminated, containing only this statement) or NULL when:
 *   - the statement is a no-op (CREATE VIEW), *is_noop is set to 1, or
 *   - out of memory.
 * server_major selects the UUID default generation (2 or 3); ctx provides
 * the core-generated UUIDv7 and the runtime variable resolver for INSERT
 * injections (may be NULL). */
SWITCH_DECLARE(char *) tnt_rules_translate(const tnt_rules_t *r, const tnt_stmt_t *st,
										   int server_major, const tnt_translate_ctx_t *ctx,
										   int *is_noop);

/* 1 if the server error should be treated as SUCCESS, 0 otherwise.
 * cleanup_ddl is 1 when the failing batch contains only DROP or CREATE
 * INDEX statements: only then is "Space X does not exist" benign
 * (idempotent cleanup). For data statements (SELECT/DELETE/INSERT/UPDATE)
 * that error MUST propagate so the core's reactive schema creation
 * (switch_cache_db_test_reactive) sees the missing table. */
SWITCH_DECLARE(int) tnt_rules_is_benign_error(const char *err, int cleanup_ddl);

#endif /* TNT_RULES_H */
