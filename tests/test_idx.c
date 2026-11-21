/*
 * mod_tarantool -- standalone unit test for tnt_sql_create_index_info()
 * (skip-index-except, v1.0.18).
 *
 * Build (from the module directory):
 *   gcc -O2 -Wall -Wextra -Werror -std=gnu99 -Itests/stub -I. \
 *       -o tests/test_idx tests/test_idx.c tnt_sql.c
 *
 * Covers:
 *   T18.1  plain CREATE INDEX                     -> name, unique=0
 *   T18.2  CREATE UNIQUE INDEX                    -> unique=1
 *   T18.3  IF NOT EXISTS                          -> name parsed
 *   T18.4  quoted identifier + mixed case         -> lowercased name
 *   T18.5  leading whitespace / comments          -> parsed
 *   T18.6  CREATE TABLE / INSERT / missing name / missing ON -> -1
 */

#include "tnt_sql.h"

#include <stdio.h>
#include <string.h>

#define IDX_NAME_MAX 64

static int failures;

#define CHECK(cond, name) do {										\
		if (cond) {													\
			printf("ok   %s\n", (name));							\
		} else {													\
			printf("FAIL %s (line %d)\n", (name), __LINE__);		\
			failures++;											\
		}															\
	} while (0)

static int info(const char *sql, char *name, size_t cap, int *uniq)
{
	return tnt_sql_create_index_info(sql, 0, strlen(sql), name, cap, uniq);
}

static void t18_1_plain(void)
{
	char nm[IDX_NAME_MAX];
	int uq = -1;

	CHECK(info("create index sr_call_id on sip_registrations (call_id)",
			   nm, sizeof(nm), &uq) == 0, "T18.1: parsed");
	CHECK(!strcmp(nm, "sr_call_id"), "T18.1: name");
	CHECK(uq == 0, "T18.1: not unique");
}

static void t18_2_unique(void)
{
	char nm[IDX_NAME_MAX];
	int uq = -1;

	CHECK(info("CREATE UNIQUE INDEX ch_uuid ON channels (uuid)",
			   nm, sizeof(nm), &uq) == 0, "T18.2: parsed");
	CHECK(!strcmp(nm, "ch_uuid"), "T18.2: name");
	CHECK(uq == 1, "T18.2: unique flag");
}

static void t18_3_if_not_exists(void)
{
	char nm[IDX_NAME_MAX];
	int uq = -1;

	CHECK(info("create index if not exists sd_uuid on sip_dialogs (uuid)",
			   nm, sizeof(nm), &uq) == 0, "T18.3: parsed");
	CHECK(!strcmp(nm, "sd_uuid"), "T18.3: name");
	CHECK(uq == 0, "T18.3: not unique");

	CHECK(info("CREATE UNIQUE INDEX IF NOT EXISTS u_x ON t (a)",
			   nm, sizeof(nm), &uq) == 0, "T18.3b: unique+if parsed");
	CHECK(!strcmp(nm, "u_x") && uq == 1, "T18.3b: name+flag");
}

static void t18_4_quoted_case(void)
{
	char nm[IDX_NAME_MAX];
	int uq = -1;

	CHECK(info("create index \"MyIdx\" on t (a)", nm, sizeof(nm), &uq) == 0,
		  "T18.4: quoted parsed");
	CHECK(!strcmp(nm, "myidx"), "T18.4: quoted name lowercased");

	CHECK(info("CrEaTe InDeX Sd_UUID oN t (a)", nm, sizeof(nm), &uq) == 0,
		  "T18.4b: mixed case parsed");
	CHECK(!strcmp(nm, "sd_uuid"), "T18.4b: name lowercased");
}

static void t18_5_ws_comments(void)
{
	char nm[IDX_NAME_MAX];
	int uq = -1;

	CHECK(info("\n\t  create /* c */ index  sd_hostname\n on sip_dialogs (hostname)",
			   nm, sizeof(nm), &uq) == 0, "T18.5: whitespace/comments");
	CHECK(!strcmp(nm, "sd_hostname"), "T18.5: name");

	CHECK(info("-- leading comment\ncreate index ss_multi on sip_subscriptions (a, b, c)",
			   nm, sizeof(nm), &uq) == 0, "T18.5b: line comment");
	CHECK(!strcmp(nm, "ss_multi"), "T18.5b: name");
}

static void t18_6_rejects(void)
{
	char nm[IDX_NAME_MAX];
	int uq = -1;

	CHECK(info("create table t (a int)", nm, sizeof(nm), &uq) == -1,
		  "T18.6: CREATE TABLE rejected");
	CHECK(info("insert into t values (1)", nm, sizeof(nm), &uq) == -1,
		  "T18.6: INSERT rejected");
	CHECK(info("create index", nm, sizeof(nm), &uq) == -1,
		  "T18.6: missing name rejected");
	CHECK(info("create index x", nm, sizeof(nm), &uq) == -1,
		  "T18.6: missing ON rejected");
	CHECK(info("drop index x", nm, sizeof(nm), &uq) == -1,
		  "T18.6: DROP rejected");
}

int main(void)
{
	t18_1_plain();
	t18_2_unique();
	t18_3_if_not_exists();
	t18_4_quoted_case();
	t18_5_ws_comments();
	t18_6_rejects();

	if (failures) {
		printf("\n%d check(s) FAILED\n", failures);
		return 1;
	}
	printf("\nall checks passed\n");
	return 0;
}
