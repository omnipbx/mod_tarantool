/*
 * mod_tarantool -- standalone unit test for composite PRIMARY KEY rules
 * (v1.0.16, promts/mod_tarantool_AddMultiPK.md).
 *
 * Build (from the module directory):
 *   gcc -O2 -Wall -Wextra -Werror -std=gnu99 -Itests/stub -I. \
 *       -o tests/test_pk tests/test_pk.c tnt_rules.c tnt_sql.c
 *
 * Covers (rules-level cases from the spec; parser-level cases E1-E5 /
 * T12-T15 live in mod_tarantool.c and need a full FreeSWITCH tree - they
 * are verified manually on the build VM):
 *   T1  legacy <primaryKey> single field        -> PRIMARY KEY (uuid)
 *   T2  composite <primaryKey> 2 fields         -> PRIMARY KEY (uuid, hostname)
 *   T3  composite <primaryKey> 4 fields         -> all four, declaration order
 *   T4  > TNT_PK_FIELDS_MAX via the API        -> tnt_rules_add_pk() == -1
 *   T5  legacy <primaryKeyAdd> single + type    -> column + PRIMARY KEY
 *   T6  composite pkadd: generated + existing  -> one column appended, PK of two
 *   T7  INSERT without a column list           -> generated value appended,
 *                                                 empty-type field untouched
 *   T8  INSERT with a column list              -> column + value appended
 *   T9  INSERT already listing the generated   -> no duplicate injection
 *   T10 empty-type field covered by <addField> -> column from addField,
 *                                                 value injected from addField
 *   T11 empty-type field NOT covered anywhere  -> PRIMARY KEY clause skipped
 *   T16 type=uuid + core-uuid=false            -> forced core UUIDv7
 */

#include "tnt_rules.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- log capture (stub switch.h calls this) ---- */
static char last_log[2048];
static int last_log_level = -1;

void tnt_test_log_capture(int level, const char *line)
{
	last_log_level = level;
	snprintf(last_log, sizeof(last_log), "%s", line);
}

static int log_contains(const char *needle)
{
	return last_log[0] && strstr(last_log, needle) != NULL;
}

/* ---- helpers ---- */
static int failures;

#define CHECK(cond, name) do {										\
		if (cond) {													\
			printf("ok   %s\n", (name));							\
		} else {													\
			printf("FAIL %s (line %d)\n", (name), __LINE__);		\
			failures++;											\
		}															\
	} while (0)

#define CHECK_STR(hay, needle, name) do {							\
		if ((hay) && strstr((hay), (needle))) {						\
			printf("ok   %s\n", (name));							\
		} else {													\
			printf("FAIL %s (line %d): [%s] lacks [%s]\n",			\
				   (name), __LINE__, (hay) ? (hay) : "(null)", (needle)); \
			failures++;											\
		}															\
	} while (0)

#define CHECK_NO_STR(hay, needle, name) do {						\
		if (!(hay) || !strstr((hay), (needle))) {					\
			printf("ok   %s\n", (name));							\
		} else {													\
			printf("FAIL %s (line %d): [%s] must not contain [%s]\n",\
				   (name), __LINE__, (hay), (needle));				\
			failures++;											\
		}															\
	} while (0)

/* build a tnt_stmt_t for one statement */
static tnt_stmt_t mk_stmt(const char *sql, tnt_stmt_type_t type, const char *table)
{
	tnt_stmt_t st;

	memset(&st, 0, sizeof(st));
	st.sql = sql;
	st.start = 0;
	st.end = strlen(sql);
	st.type = type;
	snprintf(st.table, sizeof(st.table), "%s", table);
	return st;
}

static char *translate(const tnt_rules_t *r, const char *sql,
					   tnt_stmt_type_t type, const char *table,
					   tnt_translate_ctx_t *ctx)
{
	tnt_stmt_t st = mk_stmt(sql, type, table);
	int noop = 0;

	return tnt_rules_translate(r, &st, 3, ctx, &noop);
}

static const char *create_recovery =
	"CREATE TABLE recovery (\n"
	"   runtime_uuid    VARCHAR(255),\n"
	"   technology      VARCHAR(255),\n"
	"   profile_name    VARCHAR(255),\n"
	"   uuid            VARCHAR(255),\n"
	"   hostname        VARCHAR(255),\n"
	"   metadata        VARCHAR(255)\n"
	")";

static const char *create_registrations =
	"CREATE TABLE registrations (\n"
	"   reg_user      VARCHAR(256),\n"
	"   realm     VARCHAR(256),\n"
	"   hostname VARCHAR(256),\n"
	"   metadata VARCHAR(256)\n"
	")";

static void t1_legacy_pk_single(void)
{
	tnt_rules_t r;
	tnt_pk_field_t f[1];
	char *out;

	tnt_rules_init(&r);
	memset(f, 0, sizeof(f));
	snprintf(f[0].field, sizeof(f[0].field), "uuid");
	assert(tnt_rules_add_pk(&r, "channels", f, 1) == 0);

	out = translate(&r, "CREATE TABLE channels (uuid VARCHAR(255), name VARCHAR(255))",
					TNT_STMT_CREATE_TABLE, "channels", NULL);
	CHECK_STR(out, "PRIMARY KEY (uuid)", "T1: single-field PK clause");
	CHECK_NO_STR(out, "uuid VARCHAR(255) NOT NULL", "T1: no column injected for <primaryKey>");
	free(out);
}

static void t2_composite_pk_two(void)
{
	tnt_rules_t r;
	tnt_pk_field_t f[2];
	char *out;

	tnt_rules_init(&r);
	memset(f, 0, sizeof(f));
	snprintf(f[0].field, sizeof(f[0].field), "uuid");
	snprintf(f[1].field, sizeof(f[1].field), "hostname");
	assert(tnt_rules_add_pk(&r, "recovery", f, 2) == 0);

	out = translate(&r, create_recovery, TNT_STMT_CREATE_TABLE, "recovery", NULL);
	CHECK_STR(out, "PRIMARY KEY (uuid, hostname)", "T2: composite PK clause");
	CHECK_NO_STR(out, "NOT NULL,", "T2: no injected columns");
	free(out);
}

static void t3_composite_pk_four(void)
{
	tnt_rules_t r;
	tnt_pk_field_t f[4];
	char *out;

	tnt_rules_init(&r);
	memset(f, 0, sizeof(f));
	snprintf(f[0].field, sizeof(f[0].field), "a");
	snprintf(f[1].field, sizeof(f[1].field), "b");
	snprintf(f[2].field, sizeof(f[2].field), "c");
	snprintf(f[3].field, sizeof(f[3].field), "d");
	assert(tnt_rules_add_pk(&r, "quad", f, 4) == 0);

	out = translate(&r, "CREATE TABLE quad (a VARCHAR(8), b VARCHAR(8), c VARCHAR(8), d VARCHAR(8))",
					TNT_STMT_CREATE_TABLE, "quad", NULL);
	CHECK_STR(out, "PRIMARY KEY (a, b, c, d)", "T3: four-field PK clause");
	free(out);
}

static void t4_api_limit(void)
{
	tnt_rules_t r;
	tnt_pk_field_t f[TNT_PK_FIELDS_MAX + 1];

	tnt_rules_init(&r);
	memset(f, 0, sizeof(f));
	snprintf(f[0].field, sizeof(f[0].field), "a");
	snprintf(f[1].field, sizeof(f[1].field), "b");
	snprintf(f[2].field, sizeof(f[2].field), "c");
	snprintf(f[3].field, sizeof(f[3].field), "d");
	snprintf(f[4].field, sizeof(f[4].field), "e");
	CHECK(tnt_rules_add_pk(&r, "t4", f, TNT_PK_FIELDS_MAX + 1) == -1,
		  "T4: > TNT_PK_FIELDS_MAX rejected by the API");
	CHECK(tnt_rules_add_pk(&r, "t4", f, 0) == -1, "T4: zero fields rejected");
}

static void t5_legacy_pkadd_single(void)
{
	tnt_rules_t r;
	tnt_pk_field_t f[1];
	char *out;

	tnt_rules_init(&r);
	memset(f, 0, sizeof(f));
	/* the parser expands a legacy pkadd param without 'type' to varchar(36) */
	snprintf(f[0].field, sizeof(f[0].field), "calls_uuid");
	snprintf(f[0].type, sizeof(f[0].type), "varchar(36)");
	f[0].core_uuid = 1;
	assert(tnt_rules_add_pkadd(&r, "calls", f, 1) == 0);

	out = translate(&r, "CREATE TABLE calls (uuid VARCHAR(255), name VARCHAR(255))",
					TNT_STMT_CREATE_TABLE, "calls", NULL);
	CHECK_STR(out, "calls_uuid varchar(36) NOT NULL", "T5: generated column appended");
	CHECK_STR(out, "PRIMARY KEY (calls_uuid)", "T5: single pkadd PK clause");
	free(out);
}

static void t6_pkadd_composite(void)
{
	tnt_rules_t r;
	tnt_pk_field_t f[2];
	char *out;

	tnt_rules_init(&r);
	memset(f, 0, sizeof(f));
	snprintf(f[0].field, sizeof(f[0].field), "registrations_uuid");
	snprintf(f[0].type, sizeof(f[0].type), "varchar(36)");
	f[0].core_uuid = 1;
	snprintf(f[1].field, sizeof(f[1].field), "hostname");	/* exists in DDL */
	assert(tnt_rules_add_pkadd(&r, "registrations", f, 2) == 0);

	out = translate(&r, create_registrations, TNT_STMT_CREATE_TABLE, "registrations", NULL);
	CHECK_STR(out, "registrations_uuid varchar(36) NOT NULL", "T6: generated column appended");
	CHECK_STR(out, "PRIMARY KEY (registrations_uuid, hostname)", "T6: composite PK clause");
	/* the existing hostname column must NOT be re-appended after the defs */
	CHECK_NO_STR(out, "hostname varchar", "T6: existing column not duplicated");
	free(out);
}

static void t7_insert_no_column_list(void)
{
	tnt_rules_t r;
	tnt_pk_field_t f[2];
	tnt_translate_ctx_t ctx;
	char *out;

	tnt_rules_init(&r);
	memset(f, 0, sizeof(f));
	snprintf(f[0].field, sizeof(f[0].field), "registrations_uuid");
	snprintf(f[0].type, sizeof(f[0].type), "varchar(36)");
	f[0].core_uuid = 1;
	snprintf(f[1].field, sizeof(f[1].field), "hostname");
	assert(tnt_rules_add_pkadd(&r, "registrations", f, 2) == 0);

	memset(&ctx, 0, sizeof(ctx));
	ctx.core_uuid = "0192a0c0-test-uuidv7";
	out = translate(&r, "insert into registrations values ('u','r','h','m')",
					TNT_STMT_INSERT, "registrations", &ctx);
	CHECK_STR(out, "'0192a0c0-test-uuidv7'", "T7: generated value appended");
	/* hostname has an empty type -> NOT injected by the pkadd path */
	CHECK(out && strchr(out, ',') != NULL, "T7: statement rewritten");
	CHECK_NO_STR(out, "'h', 'h'", "T7: no duplicate hostname injection");
	free(out);
}

static void t8_insert_with_column_list(void)
{
	tnt_rules_t r;
	tnt_pk_field_t f[1];
	tnt_translate_ctx_t ctx;
	char *out;

	tnt_rules_init(&r);
	memset(f, 0, sizeof(f));
	snprintf(f[0].field, sizeof(f[0].field), "calls_uuid");
	snprintf(f[0].type, sizeof(f[0].type), "varchar(36)");
	f[0].core_uuid = 1;
	assert(tnt_rules_add_pkadd(&r, "calls", f, 1) == 0);

	memset(&ctx, 0, sizeof(ctx));
	ctx.core_uuid = "core-uuid-v7";
	out = translate(&r, "insert into calls (uuid, name) values ('u', 'n')",
					TNT_STMT_INSERT, "calls", &ctx);
	CHECK_STR(out, "calls_uuid", "T8: column appended to the list");
	CHECK_STR(out, "'core-uuid-v7'", "T8: value appended");
	free(out);
}

static void t9_insert_already_listed(void)
{
	tnt_rules_t r;
	tnt_pk_field_t f[1];
	tnt_translate_ctx_t ctx;
	char *out;
	const char *sql = "insert into calls (uuid, name, calls_uuid) values ('u', 'n', 'mine')";

	tnt_rules_init(&r);
	memset(f, 0, sizeof(f));
	snprintf(f[0].field, sizeof(f[0].field), "calls_uuid");
	snprintf(f[0].type, sizeof(f[0].type), "varchar(36)");
	f[0].core_uuid = 1;
	assert(tnt_rules_add_pkadd(&r, "calls", f, 1) == 0);

	memset(&ctx, 0, sizeof(ctx));
	ctx.core_uuid = "core-uuid-v7";
	out = translate(&r, sql, TNT_STMT_INSERT, "calls", &ctx);
	CHECK_NO_STR(out, "'core-uuid-v7'", "T9: no duplicate injection");
	free(out);
}

static const char *test_var_value = "host-A";

static const char *test_resolver(const char *name, void *ud)
{
	(void)ud;
	if (!strcmp(name, "other_hostname"))
		return test_var_value;
	return NULL;
}

static void t10_empty_type_via_addfield(void)
{
	tnt_rules_t r;
	tnt_pk_field_t f[2];
	tnt_translate_ctx_t ctx;
	char *out;

	tnt_rules_init(&r);
	memset(f, 0, sizeof(f));
	snprintf(f[0].field, sizeof(f[0].field), "registrations_uuid");
	snprintf(f[0].type, sizeof(f[0].type), "varchar(36)");
	f[0].core_uuid = 1;
	snprintf(f[1].field, sizeof(f[1].field), "other_hostname");	/* via <addField> */
	assert(tnt_rules_add_pkadd(&r, "registrations", f, 2) == 0);
	assert(tnt_rules_add_field(&r, "registrations", "other_hostname",
							   "varchar(36)", 0, "", "$${other_hostname}", 1) == 0);

	/* CREATE: column comes from addField, PK is composite */
	out = translate(&r, create_registrations, TNT_STMT_CREATE_TABLE, "registrations", NULL);
	CHECK_STR(out, "other_hostname varchar(36) NOT NULL", "T10: addField column");
	CHECK_STR(out, "PRIMARY KEY (registrations_uuid, other_hostname)", "T10: composite PK");
	free(out);

	/* INSERT: generated uuid + resolved $${other_hostname} */
	memset(&ctx, 0, sizeof(ctx));
	ctx.core_uuid = "core-uuid-v7";
	ctx.resolver = test_resolver;
	out = translate(&r, "insert into registrations values ('u','r','h','m')",
					TNT_STMT_INSERT, "registrations", &ctx);
	CHECK_STR(out, "'core-uuid-v7'", "T10: generated value");
	CHECK_STR(out, "'host-A'", "T10: addField value resolved");
	CHECK(ctx.ctx_dep == 1, "T10: ctx_dep set (per-call value)");
	free(out);
}

static void t11_uncovered_empty_type(void)
{
	tnt_rules_t r;
	tnt_pk_field_t f[2];
	char *out;

	tnt_rules_init(&r);
	memset(f, 0, sizeof(f));
	snprintf(f[0].field, sizeof(f[0].field), "registrations_uuid");
	snprintf(f[0].type, sizeof(f[0].type), "varchar(36)");
	f[0].core_uuid = 1;
	snprintf(f[1].field, sizeof(f[1].field), "nonexistent");	/* neither DDL nor addField */
	assert(tnt_rules_add_pkadd(&r, "registrations", f, 2) == 0);

	last_log[0] = '\0';
	out = translate(&r, create_registrations, TNT_STMT_CREATE_TABLE, "registrations", NULL);
	CHECK_NO_STR(out, "PRIMARY KEY", "T11: PK clause skipped");
	CHECK(log_contains("nonexistent"), "T11: CRIT diagnostic logged");
	CHECK(last_log_level == SWITCH_LOG_CRIT, "T11: level is CRIT");
	/* the generated column is still appended (its own coverage is fine) */
	CHECK_STR(out, "registrations_uuid varchar(36) NOT NULL", "T11: generated column kept");
	free(out);
}

static void t16_uuid_type_forced_core(void)
{
	tnt_rules_t r;
	tnt_pk_field_t f[1];
	tnt_translate_ctx_t ctx;
	char *out;

	tnt_rules_init(&r);
	memset(f, 0, sizeof(f));
	snprintf(f[0].field, sizeof(f[0].field), "reg_uuid");
	snprintf(f[0].type, sizeof(f[0].type), "varchar(36)");
	f[0].core_uuid = 0;		/* misconfig: uuid() for a string key */
	assert(tnt_rules_add_pkadd(&r, "t16", f, 1) == 0);

	memset(&ctx, 0, sizeof(ctx));
	ctx.core_uuid = "0192-core-v7";
	last_log[0] = '\0';
	out = translate(&r, "insert into t16 values ('x')", TNT_STMT_INSERT, "t16", &ctx);
	CHECK_STR(out, "'0192-core-v7'", "T16: core UUIDv7 forced");
	CHECK_NO_STR(out, "uuid()", "T16: no uuid() for a string key");
	free(out);
}

int main(void)
{
	t1_legacy_pk_single();
	t2_composite_pk_two();
	t3_composite_pk_four();
	t4_api_limit();
	t5_legacy_pkadd_single();
	t6_pkadd_composite();
	t7_insert_no_column_list();
	t8_insert_with_column_list();
	t9_insert_already_listed();
	t10_empty_type_via_addfield();
	t11_uncovered_empty_type();
	t16_uuid_type_forced_core();

	if (failures) {
		printf("\n%d check(s) FAILED\n", failures);
		return 1;
	}
	printf("\nall checks passed\n");
	return 0;
}
