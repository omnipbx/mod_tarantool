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
* mod_tarantool.c -- Tarantool Core DB driver module
*
* Implements the switch_database_interface so that FreeSWITCH's Core DB
* (channels/calls/registrations and everything routed through
* switch_core_db_*) can run on Tarantool's SQL over the IPROTO binary
* protocol, without ODBC/libtarantool.
*
* Features (v1):
*   - profiles: independent connection pools keyed by DSN (tarantool://<name>)
*   - unix-socket / TCP, chap-sha1 auth, bounded timeouts
*   - multi-host failover with exponential backoff; SELECT-only retry
*   - declarative DDL translation (primaryKey / primaryKeyAdd / addField)
*   - batch statement execution for sofia-style "stmt1;stmt2" SQL strings
*   - API: tarantool status|list|stats|ping|sql|translate|reload|drop|version|help
*
*/

#include <switch.h>

#include <errno.h>
#include <inttypes.h>
#include <unistd.h>            /* getentropy() */

#include <private/switch_uuidv7_pvt.h>

#include "tnt_client.h"
#include "tnt_rules.h"
#include "tnt_sql.h"

switch_loadable_module_interface_t *MODULE_INTERFACE;

SWITCH_MODULE_LOAD_FUNCTION(mod_tarantool_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_tarantool_shutdown);
SWITCH_MODULE_DEFINITION(mod_tarantool, mod_tarantool_load, mod_tarantool_shutdown, NULL);

/* Maximum number of <profile> entries in tarantool.conf.xml.
 * Bounds glob->profiles[] in tnt_global_t (~3 KB per profile). The array
 * lives on the heap: config_load() parses into a malloc'ed snapshot and
 * swaps it into glob under the mutex, so no large struct is ever placed
 * on the stack. Extra profiles are skipped with a warning, never fatal. */
#define TNT_PROFILES_MAX	32
#define TNT_BATCH_INIT		4096
/* Cap for the diagnostic handle registry (tarantool conns). This is NOT
 * a connection limit — real connections are bounded by the FreeSWITCH
 * switch_cache_db pool. Only affects tracking visibility in 'conns'. */
#define TNT_HANDLES_MAX		256
/* Maximum number of <tls><profile> entries in tarantool.conf.xml.
 * TLS client profiles are a v1.1 placeholder (server-side TLS is
 * Enterprise-only on Tarantool CE); parsed and stored for forward
 * compatibility. Exceeding the cap is logged, never fatal. */
#define TNT_PROFILES_TLS_MAX	16

/* <maintenance>: periodic cleanup SQL. One shared interval for all entries,
 * each entry carries its own profile. Bodies are rejected (never truncated)
 * when larger than TNT_MAINT_SQL_LEN; more than TNT_MAINT_MAX entries are
 * ignored with a warning. */
#define TNT_MAINT_MAX		16
#define TNT_MAINT_SQL_LEN	4096
#define TNT_MAINT_MIN_INTERVAL	60
#define TNT_MAINT_MAX_INTERVAL	3600

/* Module version, reported by the 'tarantool version' API command.
 * Bump on every feature release (X.Y.Z). */
#define TNT_VERSION	"1.0.3"

typedef struct tnt_maint_sql {
	char profile[64];			/* <sql profile="..."> */
	char sql[TNT_MAINT_SQL_LEN];	/* one or more ';'-separated statements */
} tnt_maint_sql_t;

/* Per-handle connection context (one TCP/unix connection per handle). */
typedef struct tnt_handle {
	char profile_name[64];
	tnt_profile_cfg_t profile;	/* deep copy at handle_new time */
	tnt_rules_t rules;			/* deep copy at handle_new time */
	switch_mutex_t *lock;		/* serializes SQL calls vs the reload rules swap */
	tnt_session_t session;
	switch_bool_t auto_commit;
	int affected_rows;
	uint64_t n_queries;			/* total statements sent through this handle */
	int64_t created_mono;		/* CLOCK_MONOTONIC ms at handle_new */
	int caller_set;				/* first callsite that used this handle */
	char caller_file[64];
	char caller_func[64];
	int caller_line;
} tnt_handle_t;

/* One <tls><profile> entry (v1.1 placeholder; see TNT_PROFILES_TLS_MAX). */
typedef struct tnt_tls_profile {
	char name[64];
	char ca_file[256];
	char cert_file[256];
	char key_file[256];
	int verify;					/* 1 = verify the server certificate */
	int verify_host;			/* 1 = also verify the hostname */
	char min_version[16];		/* e.g. "TLSv1.2" */
} tnt_tls_profile_t;

/* ------------------------------------------------------------------ */
/* global configuration snapshot                                       */
/* ------------------------------------------------------------------ */

typedef struct tnt_global {
	switch_mutex_t *mutex;
	switch_memory_pool_t *pool;	/* module pool (load-time), used for handle locks */
	tnt_profile_cfg_t profiles[TNT_PROFILES_MAX];
	int nprofiles;
	tnt_tls_profile_t tls_profiles[TNT_PROFILES_TLS_MAX];
	int ntls_profiles;
	tnt_rules_t rules;
	tnt_maint_sql_t maint_sql[TNT_MAINT_MAX];
	int nmaint_sql;
	int maint_interval;			/* seconds; 0 = disabled */
	int maint_enabled;			/* effective: enable attr AND >=1 valid <sql> */
	uint32_t default_action_add;
	char uuid_default_32[64];
	char uuid_default_210[64];
	uint32_t ping_idle_ms;
	uint32_t ping_timeout_ms;
	uint32_t reconnect_ms;
	int debug;					/* tarantool debug on/off */
	char debug_profile[64];		/* optional per-profile filter */
	tnt_handle_t *handles[TNT_HANDLES_MAX];
	int nhandles;
} tnt_global_t;

static tnt_global_t *glob = NULL;

/* Set by a successful reload so the <maintenance> thread re-reads the config
 * immediately (enable/interval/<sql> hot-applied, latency <= 250 ms). */
static int maint_wakeup = 0;

/* Set while parsing when a <views> body exceeds TNT_VIEW_BODY_LEN. Such a
 * configuration must be REJECTED as a whole: on load the module refuses to
 * start (SWITCH_STATUS_TERM), on reload the previously applied config stays. */
static int tnt_cfg_view_fatal = 0;

/* ------------------------------------------------------------------ */
/* config parsing                                                      */
/* ------------------------------------------------------------------ */

static int parse_bool_attr(const char *v, int def)
{
	if (!v)
		return def;
	if (!strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcmp(v, "1"))
		return 1;
	if (!strcasecmp(v, "false") || !strcasecmp(v, "no") || !strcmp(v, "0"))
		return 0;
	return def;
}

static uint32_t parse_ms(const char *v, uint32_t def)
{
	if (!v)
		return def;
	return (uint32_t)atol(v);
}

/* parse a host block (either inline profile params or a <host> child) */
static void parse_host_params(switch_xml_t xml, tnt_host_cfg_t *h,
							  uint32_t *query_timeout, char *user, size_t user_len,
							  char *pass, size_t pass_len)
{
	switch_xml_t p;

	for (p = switch_xml_child(xml, "param"); p; p = p->next) {
		const char *name = switch_xml_attr(p, "name");
		const char *value = switch_xml_attr(p, "value");

		if (!name || !value)
			continue;
		if (!strcasecmp(name, "unix-socket")) {
			snprintf(h->addr, sizeof(h->addr), "%s", value);
			h->use_unix = 1;
		} else if (!strcasecmp(name, "host")) {
			snprintf(h->addr, sizeof(h->addr), "%s", value);
			h->use_unix = 0;
		} else if (!strcasecmp(name, "port")) {
			h->port = (uint16_t)atoi(value);
		} else if (!strcasecmp(name, "user")) {
			snprintf(user, user_len, "%s", value);
		} else if (!strcasecmp(name, "password")) {
			snprintf(pass, pass_len, "%s", value);
		} else if (!strcasecmp(name, "query-timeout")) {
			if (query_timeout)
				*query_timeout = parse_ms(value, 3000);
		} else if (!strcasecmp(name, "tls-profile")) {
			/* v1.1 placeholder: parsed/stored but NOT used yet — the
			 * connection is still plain TCP/unix. See TNT_PROFILES_TLS_MAX. */
			snprintf(h->tls_profile, sizeof(h->tls_profile), "%s", value);
		} else if (!strcasecmp(name, "full-scan") || !strcasecmp(name, "init-sql")) {
			/* These are PROFILE-level parameters: they apply to every host
			 * of the profile (incl. failover reconnects) and must be placed
			 * on <profile>, not inside <host>. Accept-and-warn so a host-
			 * level typo does not silently vanish. */
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "mod_tarantool: \"%s\" is a profile-level parameter; "
							  "move it to <profile %s> so it applies to all hosts\n",
							  name, h->addr[0] ? h->addr : "(inline)");
		}
	}
}

static void parse_profile(switch_xml_t xml, tnt_profile_cfg_t *p)
{
	const char *name = switch_xml_attr(xml, "name");
	const char *version = switch_xml_attr(xml, "version");
	const char *skip_index = switch_xml_attr(xml, "skip-index");
	switch_xml_t xml_hosts, xml_host, param;

	memset(p, 0, sizeof(*p));
	if (name)
		snprintf(p->name, sizeof(p->name), "%s", name);
	p->connect_timeout_ms = 5000;
	p->query_timeout_ms = 3000;
	p->ping_idle_s = 30;
	p->ping_timeout_s = 3;
	p->backoff_max_s = 30;
	p->skip_index = (skip_index && !strcasecmp(skip_index, "true")) ? 1 : 0;
	/* FreeSWITCH core queries are full table scans by default and Tarantool
	 * forbids them unless enabled per session — see task 27. On by default. */
	p->full_scan = 1;

	/* profile-level params (also feed defaults for host-level ones) */
	for (param = switch_xml_child(xml, "param"); param; param = param->next) {
		const char *pn = switch_xml_attr(param, "name");
		const char *pv = switch_xml_attr(param, "value");

		if (!pn || !pv)
			continue;
		if (!strcasecmp(pn, "connect-timeout"))
			p->connect_timeout_ms = parse_ms(pv, p->connect_timeout_ms);
		else if (!strcasecmp(pn, "query-timeout"))
			p->query_timeout_ms = parse_ms(pv, p->query_timeout_ms);
		else if (!strcasecmp(pn, "ping-idle"))
			p->ping_idle_s = parse_ms(pv, p->ping_idle_s) / 1000;
		else if (!strcasecmp(pn, "ping-timeout"))
			p->ping_timeout_s = parse_ms(pv, p->ping_timeout_s) / 1000;
		else if (!strcasecmp(pn, "backoff-max"))
			p->backoff_max_s = parse_ms(pv, p->backoff_max_s) / 1000;
		else if (!strcasecmp(pn, "full-scan"))
			p->full_scan = parse_bool_attr(pv, 1);
		else if (!strcasecmp(pn, "init-sql"))
			snprintf(p->init_sql, sizeof(p->init_sql), "%s", pv);
	}

	/* inline form: params directly on <profile> */
	xml_hosts = switch_xml_child(xml, "hosts");
	if (xml_hosts) {
		/* multi-host form: version is per-<host>; the profile-level
		 * version attribute is ignored for connection purposes */
		int hskip = 0;

		for (xml_host = switch_xml_child(xml_hosts, "host"); xml_host; xml_host = xml_host->next) {
			tnt_host_cfg_t *h;
			const char *hv;
			char huser[64] = "";
			char hpass[128] = "";

			if (p->nhosts >= TNT_HOST_MAX) {
				hskip++;
				continue;
			}
			h = &p->hosts[p->nhosts];
			hv = switch_xml_attr(xml_host, "version");
			memset(h, 0, sizeof(*h));
			h->port = 3301;
			if (hv)
				snprintf(h->version, sizeof(h->version), "%s", hv);
			parse_host_params(xml_host, h, &p->query_timeout_ms, huser, sizeof(huser), hpass, sizeof(hpass));
			if (huser[0])
				snprintf(p->username, sizeof(p->username), "%s", huser);
			if (hpass[0])
				snprintf(p->password, sizeof(p->password), "%s", hpass);
			p->nhosts++;
		}
		if (hskip) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "mod_tarantool: profile '%s': %d host(s) skipped, max is %d\n",
							  p->name[0] ? p->name : "(unnamed)", hskip, TNT_HOST_MAX);
		}
	} else {
		/* inline form */
		tnt_host_cfg_t *h = &p->hosts[0];
		const char *hn = switch_xml_attr(xml, "name");

		(void)hn;
		memset(h, 0, sizeof(*h));
		h->port = 3301;
		if (version)
			snprintf(h->version, sizeof(h->version), "%s", version);
		parse_host_params(xml, h, &p->query_timeout_ms, p->username, sizeof(p->username),
						  p->password, sizeof(p->password));
		if (h->addr[0])
			p->nhosts = 1;
	}
}

/* Runtime $${name} resolver for addField injections (FreeSWITCH globals). */
static const char *tnt_resolve_global(const char *name, void *ud)
{
	(void)ud;
	return switch_core_get_variable(name);
}

/* UUIDv7 generation.
 *
 * The core's uuidv7_new() is NOT exported from libfreeswitch.so (it lacks
 * SWITCH_DECLARE / visibility attribute and modules are built with
 * -fvisibility=hidden), so calling uuidv7_new_string() from a module ends
 * in "undefined symbol: uuidv7_new" at dlopen time. The header's low-level
 * primitives (uuidv7_generate / uuidv7_to_string) are static inline and
 * compile into the module, so we drive them directly with getentropy(). */
static int tnt_uuidv7(char *out /* 36 chars + NUL */)
{
	uint8_t uuid[16];
	uint8_t rnd[10];
	int8_t st;

	if (getentropy(rnd, sizeof(rnd)) != 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "mod_tarantool: getentropy failed: %s\n", strerror(errno));
		return -1;
	}
	st = uuidv7_generate(uuid, switch_time_now() / 1000, rnd, NULL);
	if (st < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "mod_tarantool: uuidv7_generate failed: %d\n", (int)st);
		return -1;
	}
	uuidv7_to_string(uuid, out);
	return 0;
}

/* Warn about attributes the module does not understand. config typos are
 * otherwise silent: an unknown attr is simply never read. */
static void tnt_warn_unknown_attrs(switch_xml_t param, const char *const *allowed,
								   const char *what)
{
	char **ap;

	if (!param || !param->attr)
		return;
	for (ap = param->attr; ap[0]; ap += 2) {
		int i, known = 0;

		for (i = 0; allowed[i]; i++) {
			if (!strcasecmp(ap[0], allowed[i])) {
				known = 1;
				break;
			}
		}
		if (!known) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "mod_tarantool: %s: unknown attribute \"%s\" ignored\n",
							  what, ap[0]);
		}
	}
}

/* Build a pointer list into r->reserved (caller must keep r alive and
 * stable — no concurrent reload — for the returned pointers to stay valid). */
static int tnt_reserved_ptr_list(const tnt_rules_t *r, const char *out[], int max)
{
	int i, n = 0;

	if (!r)
		return 0;
	for (i = 0; i < r->nreserved && n < max; i++) {
		if (r->reserved[i][0])
			out[n++] = r->reserved[i];
	}
	return n;
}

/* Copy the reserved words into a caller-owned snapshot buffer so the list
 * survives releasing glob->mutex (e.g. during a concurrent reload). */
static int tnt_reserved_snapshot(const tnt_rules_t *r, char (*out)[TNT_RESERVED_WORD_LEN],
								 int max)
{
	int i, n = 0;

	if (!r)
		return 0;
	for (i = 0; i < r->nreserved && n < max; i++) {
		if (r->reserved[i][0]) {
			snprintf(out[n], TNT_RESERVED_WORD_LEN, "%s", r->reserved[i]);
			n++;
		}
	}
	return n;
}

static void parse_rule_tables(switch_xml_t xml, tnt_global_t *g)
{
	switch_xml_t sec, param;

	for (sec = switch_xml_child(xml, "reserved"); sec; sec = sec->next) {
		for (param = switch_xml_child(sec, "param"); param; param = param->next) {
			const char *nm = switch_xml_attr(param, "name");
			static const char *const a[] = { "name", NULL };

			tnt_warn_unknown_attrs(param, a, "reserved");
			if (nm && tnt_rules_add_reserved(&g->rules, nm) != 0) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
								  "mod_tarantool: <reserved> word '%s' ignored "
								  "(empty or limit %d reached)\n",
								  nm, TNT_RESERVED_MAX);
			}
		}
	}

	/* <intColumns>: numeric columns that may arrive as QUOTED string literals
	 * ('1791322069') from the FreeSWITCH core. The type attribute is checked
	 * at load/reload time — an unknown type is reported to the console and
	 * the entry is skipped. */
	for (sec = switch_xml_child(xml, "intColumns"); sec; sec = sec->next) {
		for (param = switch_xml_child(sec, "param"); param; param = param->next) {
			const char *tb = switch_xml_attr(param, "table");
			const char *fd = switch_xml_attr(param, "field");
			const char *ty = switch_xml_attr(param, "type");
			static const char *const a[] = { "table", "field", "type", NULL };

			tnt_warn_unknown_attrs(param, a, "intColumns");
			if (!ty || !tnt_intcol_cast_for_type(ty)) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
								  "mod_tarantool: <intColumns> %s.%s: unknown type '%s' "
								  "(INTEGER/INT/BIGINT/SMALLINT/TINYINT/MEDIUMINT/NUMBER/"
								  "DOUBLE/REAL/FLOAT/DECIMAL/NUMERIC), entry ignored\n",
								  tb ? tb : "?", fd ? fd : "?", ty ? ty : "(missing)");
				continue;
			}
			if (tnt_rules_add_intcol(&g->rules, tb, fd, ty) != 0) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
								  "mod_tarantool: <intColumns> %s.%s ignored "
								  "(empty names or limit %d reached)\n",
								  tb ? tb : "?", fd ? fd : "?", TNT_INTCOL_MAX);
			}
		}
	}

	/* <views><view name="..."><![CDATA[ SELECT body ]]></view></views>:
		* Tarantool has no VIEW, so every core SELECT with "FROM <name>" is
		* rewritten into "FROM (<body>) AS <name>" (see expand_view_select). */
	for (sec = switch_xml_child(xml, "views"); sec; sec = sec->next) {
		switch_xml_t view;

		for (view = switch_xml_child(sec, "view"); view; view = view->next) {
			const char *nm = switch_xml_attr(view, "name");
			const char *body = switch_xml_txt(view);
			size_t blen = body ? strlen(body) : 0;
			char vreason[128];
			int rc;
			static const char *const a[] = { "name", NULL };

			tnt_warn_unknown_attrs(view, a, "views/view");
			/* a view body substitutes a whole VIEW: it must be ONE valid
			 * SELECT — reject garbage / composite SQL with a console warning */
			if (nm && body && tnt_sql_validate(body, 1, vreason, sizeof(vreason))) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
								  "mod_tarantool: <views> '%s': invalid SQL, "
								  "view NOT applied: %s\n",
								  nm, vreason);
				continue;
			}
			rc = (nm && body) ? tnt_rules_add_view(&g->rules, nm, body) : -1;
			if (rc == 0)
				continue;
			if (rc == -2) {
				/* hard error: refuse the whole configuration */
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT,
								  "mod_tarantool: <views> '%s': body %u bytes exceeds limit %d "
								  "-- configuration rejected\n",
								  nm ? nm : "?", (unsigned)blen, TNT_VIEW_BODY_LEN);
				tnt_cfg_view_fatal = 1;
			} else if (rc == -3) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
								  "mod_tarantool: <views> '%s' ignored: view limit %d reached\n",
								  nm ? nm : "?", TNT_VIEWS_MAX);
			} else {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
								  "mod_tarantool: <views> entry ignored (empty name or body)\n");
			}
		}
	}

	for (sec = switch_xml_child(xml, "primaryKey"); sec; sec = sec->next) {
		for (param = switch_xml_child(sec, "param"); param; param = param->next) {
			const char *t = switch_xml_attr(param, "table");
			const char *f = switch_xml_attr(param, "field");
			static const char *const a[] = { "table", "field", NULL };

			tnt_warn_unknown_attrs(param, a, "primaryKey");
			if (t && f)
				tnt_rules_add_pk(&g->rules, t, f);
		}
	}
	for (sec = switch_xml_child(xml, "primaryKeyAdd"); sec; sec = sec->next) {
		for (param = switch_xml_child(sec, "param"); param; param = param->next) {
			const char *t = switch_xml_attr(param, "table");
			const char *f = switch_xml_attr(param, "field");
			const char *ty = switch_xml_attr(param, "type");
			const char *cu = switch_xml_attr(param, "core-uuid");
			static const char *const a[] = { "table", "field", "type",
				"core-uuid", NULL };

			tnt_warn_unknown_attrs(param, a, "primaryKeyAdd");
			if (t && f)
				tnt_rules_add_pkadd(&g->rules, t, f, ty ? ty : "",
									cu ? parse_bool_attr(cu, 0) : -1);
		}
	}
	for (sec = switch_xml_child(xml, "addField"); sec; sec = sec->next) {
		for (param = switch_xml_child(sec, "param"); param; param = param->next) {
			const char *t = switch_xml_attr(param, "table");
			const char *f = switch_xml_attr(param, "field");
			const char *ty = switch_xml_attr(param, "type");
			const char *nl = switch_xml_attr(param, "isNull");
			const char *df = switch_xml_attr(param, "default");
			const char *vl = switch_xml_attr(param, "value");
			const char *il = switch_xml_attr(param, "in-line");
			static const char *const a[] = { "table", "field", "type",
				"isNull", "default", "value", "in-line", NULL };

			tnt_warn_unknown_attrs(param, a, "addField");
			if (t && f)
				tnt_rules_add_field(&g->rules, t, f, ty ? ty : "STRING",
									parse_bool_attr(nl, 1), df ? df : "",
									vl ? vl : "", parse_bool_attr(il, 0));
		}
	}
}

/* 1 if a profile with this name exists in the freshly parsed snapshot. */
static int tnt_maint_profile_exists(const tnt_global_t *g, const char *name)
{
	int i;

	for (i = 0; i < g->nprofiles; i++) {
		if (!strcasecmp(g->profiles[i].name, name))
			return 1;
	}
	return 0;
}

/* <maintenance interval="120" enable="true"><sql profile="local">...</sql></maintenance>.
	* Effective status: enable attribute (default FALSE) AND interval > 0 AND at
	* least one <sql> whose profile exists in this very configuration. Entries
	* with an unknown profile / empty / oversized body are dropped with WARNING. */
static void parse_maintenance(switch_xml_t cfg, tnt_global_t *g)
{
	switch_xml_t sec, sql;
	const char *en, *iv;
	int want_enable, interval = 0;
	int valid = 0, skipped = 0;
	static const char *const sa[] = { "interval", "enable", NULL };
	static const char *const qa[] = { "profile", NULL };

	g->nmaint_sql = 0;
	g->maint_interval = 0;
	g->maint_enabled = 0;

	if (!(sec = switch_xml_child(cfg, "maintenance")))
		return;
	tnt_warn_unknown_attrs(sec, sa, "maintenance");

	en = switch_xml_attr(sec, "enable");
	want_enable = en ? parse_bool_attr(en, 0) : 0;
	iv = switch_xml_attr(sec, "interval");
	if (iv) {
		interval = atoi(iv);
		if (interval != 0 && interval < TNT_MAINT_MIN_INTERVAL) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "mod_tarantool: <maintenance> interval %d below minimum %d, clamped\n",
							  interval, TNT_MAINT_MIN_INTERVAL);
			interval = TNT_MAINT_MIN_INTERVAL;
		} else if (interval > TNT_MAINT_MAX_INTERVAL) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "mod_tarantool: <maintenance> interval %d above maximum %d, clamped\n",
							  interval, TNT_MAINT_MAX_INTERVAL);
			interval = TNT_MAINT_MAX_INTERVAL;
		}
	}
	g->maint_interval = interval;

	if (!want_enable || interval == 0)
		return;				/* explicitly disabled */

	for (sql = switch_xml_child(sec, "sql"); sql; sql = sql->next) {
		const char *pr = switch_xml_attr(sql, "profile");
		const char *body = switch_xml_txt(sql);
		size_t blen = body ? strlen(body) : 0;

		tnt_warn_unknown_attrs(sql, qa, "maintenance/sql");
		if (!pr || !pr[0]) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "mod_tarantool: <maintenance> <sql> without profile ignored\n");
			skipped++;
			continue;
		}
		if (!tnt_maint_profile_exists(g, pr)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "mod_tarantool: <maintenance> <sql> references unknown profile '%s' ignored\n",
							  pr);
			skipped++;
			continue;
		}
		if (!body || !body[0]) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "mod_tarantool: <maintenance> <sql> (profile '%s') has empty body ignored\n",
							  pr);
			skipped++;
			continue;
		}
		if (blen >= TNT_MAINT_SQL_LEN) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "mod_tarantool: <maintenance> <sql> (profile '%s') body %u bytes exceeds %d ignored\n",
							  pr, (unsigned)blen, TNT_MAINT_SQL_LEN);
			skipped++;
			continue;
		}
		{
			char mreason[128];

			if (tnt_sql_validate(body, 0, mreason, sizeof(mreason))) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
								  "mod_tarantool: <maintenance> <sql> (profile '%s'): "
								  "invalid SQL, entry NOT applied: %s\n",
								  pr, mreason);
				skipped++;
				continue;
			}
		}
		if (g->nmaint_sql >= TNT_MAINT_MAX) {
			skipped++;
			continue;
		}
		snprintf(g->maint_sql[g->nmaint_sql].profile,
				 sizeof(g->maint_sql[g->nmaint_sql].profile), "%s", pr);
		memcpy(g->maint_sql[g->nmaint_sql].sql, body, blen + 1);
		g->nmaint_sql++;
		valid = 1;
	}

	if (skipped) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "mod_tarantool: <maintenance> dropped %d sql entry/entries "
						  "(limit %d, unknown profile, empty, oversized or invalid SQL body)\n",
						  skipped, TNT_MAINT_MAX);
	}
	g->maint_enabled = valid;
	if (g->maint_enabled) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
						  "mod_tarantool: <maintenance> enabled: interval=%ds, %d sql entr%s\n",
						  g->maint_interval, g->nmaint_sql,
						  g->nmaint_sql == 1 ? "y" : "ies");
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "mod_tarantool: <maintenance> NOT active: enable='%s', interval=%d, valid sql=%d\n",
						  want_enable ? "true" : "false", g->maint_interval, valid);
	}
}

static switch_status_t parse_config(switch_xml_t cfg, tnt_global_t *g)
{
	switch_xml_t settings, profiles, p, param;
	const char *defact = NULL;

	memset(g, 0, sizeof(*g));
	tnt_rules_init(&g->rules);
	g->default_action_add = 1;
	g->uuid_default_32[0] = '\0';
	g->uuid_default_210[0] = '\0';
	g->ping_idle_ms = 30000;
	g->ping_timeout_ms = 3000;
	g->reconnect_ms = 5000;

	settings = switch_xml_child(cfg, "settings");
	if (settings) {
		for (param = switch_xml_child(settings, "param"); param; param = param->next) {
			const char *name = switch_xml_attr(param, "name");
			const char *value = switch_xml_attr(param, "value");

			if (!name || !value)
				continue;
			if (!strcasecmp(name, "default-action")) {
				defact = value;
			} else if (!strncasecmp(name, "primaryKeyAddDefault-", 21)) {
				const char *ver = name + 21;

				if (!strncmp(ver, "3.", 2))
					snprintf(g->uuid_default_32, sizeof(g->uuid_default_32), "%s", value);
				else if (!strncmp(ver, "2.", 2))
					snprintf(g->uuid_default_210, sizeof(g->uuid_default_210), "%s", value);
			} else if (!strcasecmp(name, "ping-idle")) {
				g->ping_idle_ms = parse_ms(value, g->ping_idle_ms);
			} else if (!strcasecmp(name, "ping-timeout")) {
				g->ping_timeout_ms = parse_ms(value, g->ping_timeout_ms);
			} else if (!strcasecmp(name, "reconnect-interval")) {
				g->reconnect_ms = parse_ms(value, g->reconnect_ms);
			} else if (!strcasecmp(name, "debug")) {
				g->debug = parse_bool_attr(value, 0);
			}
		}
	}
	if (defact) {
		if (!strcasecmp(defact, "none"))
			g->default_action_add = 0;
	}

	profiles = switch_xml_child(cfg, "profiles");
	if (profiles) {
		int skipped = 0;

		for (p = switch_xml_child(profiles, "profile"); p; p = p->next) {
			if (g->nprofiles >= TNT_PROFILES_MAX) {
				skipped++;
				continue;
			}
			parse_profile(p, &g->profiles[g->nprofiles]);
			if (g->profiles[g->nprofiles].nhosts > 0)
				g->nprofiles++;
		}
		if (skipped) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "mod_tarantool: %d profile(s) skipped, max is %d\n",
							  skipped, TNT_PROFILES_MAX);
		}
	}

	/* <tls><profile> entries (v1.1 placeholder, parsed for forward compat) */
	{
		switch_xml_t tls = switch_xml_child(cfg, "tls");

		if (tls) {
			int skipped = 0;
			switch_xml_t tp;

			for (tp = switch_xml_child(tls, "profile"); tp; tp = tp->next) {
				tnt_tls_profile_t *t;
				switch_xml_t tparam;
				const char *tname = switch_xml_attr(tp, "name");

				if (g->ntls_profiles >= TNT_PROFILES_TLS_MAX) {
					skipped++;
					continue;
				}
				t = &g->tls_profiles[g->ntls_profiles];
				memset(t, 0, sizeof(*t));
				if (tname)
					snprintf(t->name, sizeof(t->name), "%s", tname);
				for (tparam = switch_xml_child(tp, "param"); tparam; tparam = tparam->next) {
					const char *pn = switch_xml_attr(tparam, "name");
					const char *pv = switch_xml_attr(tparam, "value");

					if (!pn || !pv)
						continue;
					if (!strcasecmp(pn, "ca-file"))
						snprintf(t->ca_file, sizeof(t->ca_file), "%s", pv);
					else if (!strcasecmp(pn, "cert-file"))
						snprintf(t->cert_file, sizeof(t->cert_file), "%s", pv);
					else if (!strcasecmp(pn, "key-file"))
						snprintf(t->key_file, sizeof(t->key_file), "%s", pv);
					else if (!strcasecmp(pn, "verify"))
						t->verify = parse_bool_attr(pv, 1);
					else if (!strcasecmp(pn, "verify-host"))
						t->verify_host = parse_bool_attr(pv, 1);
					else if (!strcasecmp(pn, "min-version"))
						snprintf(t->min_version, sizeof(t->min_version), "%s", pv);
				}
				g->ntls_profiles++;
			}
			if (skipped) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
								  "mod_tarantool: %d tls profile(s) skipped, max is %d\n",
								  skipped, TNT_PROFILES_TLS_MAX);
			}
		}
	}

	parse_rule_tables(cfg, g);
	parse_maintenance(cfg, g);

	/* a <views> body larger than TNT_VIEW_BODY_LEN poisons the whole
	 * configuration: refuse it so load aborts / reload keeps the old one */
	if (tnt_cfg_view_fatal)
		return SWITCH_STATUS_FALSE;

	return SWITCH_STATUS_SUCCESS;
}

/* Open the module config DIRECTLY from its own file, bypassing
 * switch_xml_open_cfg()/the global MAIN_XML_ROOT tree entirely.
 *
 * Why: switch_xml_open_cfg() returns nodes that live INSIDE FreeSWITCH's
 * global config tree. Freeing the located <configuration> node with
 * switch_xml_free(cfg) corrupts MAIN_XML_ROOT, and any later XML access
 * (this module's reload, other modules, reloadxml) dies in libc memcmp
 * (see promts/FS_XML_CONFIG_OWNERSHIP_GUIDE.md). Parsing our own file
 * yields a fully INDEPENDENT tree: no shared state, immune to other
 * modules' bugs, and switch_xml_free() on it is always safe. The returned
 * tree's root IS the <configuration> element. */
static switch_xml_t tnt_cfg_open(void)
{
	switch_xml_t xml;
	char path[1024];

	if (zstr(SWITCH_GLOBAL_dirs.conf_dir)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "mod_tarantool: config directory not initialized\n");
		return NULL;
	}
	snprintf(path, sizeof(path), "%s%sautoload_configs%starantool.conf.xml",
			 SWITCH_GLOBAL_dirs.conf_dir, SWITCH_PATH_SEPARATOR, SWITCH_PATH_SEPARATOR);

	if (!(xml = switch_xml_parse_file(path))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "mod_tarantool: cannot open %s\n", path);
		return NULL;
	}
	/* A parse error does NOT yield NULL here: switch_xml_parse_str() returns
	 * a partially built tree whose root carries the message from
	 * switch_xml_err(). Reading sections off such a tree walks garbage, so
	 * reject it explicitly. */
	if (!zstr(switch_xml_error(xml))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "mod_tarantool: invalid config XML %s: %s\n", path, switch_xml_error(xml));
		switch_xml_free(xml);
		return NULL;
	}
	return xml;
}

/* Pick the UUID default for a host version branch; defined near
 * tnt_handle_new, forward-declared for use by the config_load hot-reload. */
static void tnt_rules_set_uuid_default(tnt_rules_t *r, const tnt_profile_cfg_t *p);

/* re-read configuration into a fresh snapshot (used by load and reload).
 * All heavy structures (profiles, TLS profiles, rules incl. view bodies)
 * live on the heap inside *ng; the swap below copies them straight into
 * glob while ng is still alive, so NO large struct is ever placed on the
 * stack (module load happens on threads with limited stack sizes). */
static switch_status_t config_load(void)
{
	switch_xml_t cfg = NULL;
	tnt_global_t *ng;
	int hi;

	tnt_cfg_view_fatal = 0;		/* re-arm for every parse attempt */
	if (!(cfg = tnt_cfg_open()))
		return SWITCH_STATUS_FALSE;

	ng = (tnt_global_t *)malloc(sizeof(*ng));
	if (!ng) {
		switch_xml_free(cfg);
		return SWITCH_STATUS_FALSE;
	}
	if (parse_config(cfg, ng) != SWITCH_STATUS_SUCCESS) {
		free(ng);
		switch_xml_free(cfg);
		return SWITCH_STATUS_FALSE;
	}
	switch_xml_free(cfg);

	/* atomically swap the global snapshot */
	switch_mutex_lock(glob->mutex);
	memcpy(glob->profiles, ng->profiles, sizeof(glob->profiles));
	glob->nprofiles = ng->nprofiles;
	memcpy(glob->tls_profiles, ng->tls_profiles, sizeof(glob->tls_profiles));
	glob->ntls_profiles = ng->ntls_profiles;
	glob->rules = ng->rules;
	glob->ping_idle_ms = ng->ping_idle_ms;
	glob->ping_timeout_ms = ng->ping_timeout_ms;
	glob->reconnect_ms = ng->reconnect_ms;
	glob->debug = ng->debug;
	snprintf(glob->uuid_default_32, sizeof(glob->uuid_default_32), "%s",
			 ng->uuid_default_32[0] ? ng->uuid_default_32 : "uuid7()");
	snprintf(glob->uuid_default_210, sizeof(glob->uuid_default_210), "%s",
			 ng->uuid_default_210[0] ? ng->uuid_default_210 : "uuid()");

	/* hot-reload: push the new rules snapshot into every ACTIVE connection.
	 * All rule sections (reserved/views/primaryKey/intColumns/...) live in a
	 * single tnt_rules_t, so one struct copy applies them wholesale; each
	 * handle refreshes under its own lock, therefore an in-flight SQL call
	 * (which holds that lock for its whole batch) is never torn by the swap.
	 * New connections get the fresh rules at handle_new anyway. */
	for (hi = 0; hi < glob->nhandles; hi++) {
		tnt_handle_t *hh = glob->handles[hi];

		if (!hh)
			continue;
		if (hh->lock)
			switch_mutex_lock(hh->lock);
		hh->rules = glob->rules;
		tnt_rules_set_uuid_default(&hh->rules, &hh->profile);
		if (hh->lock)
			switch_mutex_unlock(hh->lock);
	}
	switch_mutex_unlock(glob->mutex);
	free(ng);

	/* wake the <maintenance> thread so enable/interval/<sql> changes take
	 * effect right away (the loop re-reads the snapshot on the next pass) */
	maint_wakeup = 1;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
					  "mod_tarantool: loaded %d profile(s), %d pk, %d pkadd, %d field rules, "
					  "rules hot-applied to %d active connection(s)\n",
					  glob->nprofiles, glob->rules.npk, glob->rules.npkadd, glob->rules.nfields,
					  glob->nhandles);
	return SWITCH_STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* database interface callbacks                                        */
/* ------------------------------------------------------------------ */

static tnt_profile_cfg_t *find_profile_locked(const char *name)
{
	int i;

	for (i = 0; i < glob->nprofiles; i++) {
		if (!strcasecmp(glob->profiles[i].name, name))
			return &glob->profiles[i];
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* debug tracing + active-handle registry                              */
/* ------------------------------------------------------------------ */

static int tnt_debug_active(const char *profile_name)
{
	if (!glob || !glob->debug)
		return 0;
	if (glob->debug_profile[0] && strcasecmp(glob->debug_profile, profile_name))
		return 0;
	return 1;
}

static void tnt_debug_log(const char *profile_name, const char *kind,
						  const char *file, const char *func, int line, const char *sql)
{
	size_t n;

	if (!sql)
		sql = "";
	n = strlen(sql);
	if (n > 1024) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
						  "mod_tarantool [DEBUG]: profile=%s %s %s:%d %s\n  sql: %.*s... (%u bytes total)\n",
						  profile_name, kind, file ? file : "?", line, func ? func : "?",
						  (int)1024, sql, (unsigned)n);
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
						  "mod_tarantool [DEBUG]: profile=%s %s %s:%d %s\n  sql: %s\n",
						  profile_name, kind, file ? file : "?", line, func ? func : "?", sql);
	}
}

static void tnt_handle_register(tnt_handle_t *h)
{
	if (!glob)
		return;
	switch_mutex_lock(glob->mutex);
	if (glob->nhandles < TNT_HANDLES_MAX) {
		glob->handles[glob->nhandles++] = h;
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "mod_tarantool: handle registry full (%d), connection '%s' not tracked\n",
						  TNT_HANDLES_MAX, h->profile_name);
	}
	switch_mutex_unlock(glob->mutex);
}

static void tnt_handle_unregister(tnt_handle_t *h)
{
	int i;

	if (!glob)
		return;
	switch_mutex_lock(glob->mutex);
	for (i = 0; i < glob->nhandles; i++) {
		if (glob->handles[i] == h) {
			glob->handles[i] = glob->handles[glob->nhandles - 1];
			glob->nhandles--;
			break;
		}
	}
	switch_mutex_unlock(glob->mutex);
}

/* Pick the UUID default for the active host's version branch (3.x -> uuid7(),
 * 2.10 -> uuid()). Called at handle_new and on every hot-reload rules swap. */
static void tnt_rules_set_uuid_default(tnt_rules_t *r, const tnt_profile_cfg_t *p)
{
	const char *ver = p->hosts[0].version;

	if (ver && !strncmp(ver, "3.", 2))
		snprintf(r->uuid_default, sizeof(r->uuid_default), "%s",
				 glob->uuid_default_32[0] ? glob->uuid_default_32 : "uuid7()");
	else
		snprintf(r->uuid_default, sizeof(r->uuid_default), "%s",
				 glob->uuid_default_210[0] ? glob->uuid_default_210 : "uuid()");
}

static switch_status_t tnt_handle_new(switch_cache_db_database_interface_options_t opts,
									  switch_database_interface_handle_t **dih)
{
	tnt_handle_t *h;
	const char *pname = opts.connection_string;
	tnt_profile_cfg_t *prof;

	if (!dih)
		return SWITCH_STATUS_FALSE;
	*dih = NULL;
	if (zstr(pname))
		pname = "local";

	h = (tnt_handle_t *)calloc(1, sizeof(*h));
	if (!h)
		return SWITCH_STATUS_FALSE;

	snprintf(h->profile_name, sizeof(h->profile_name), "%s", pname);
	h->auto_commit = SWITCH_TRUE;

	switch_mutex_lock(glob->mutex);
	prof = find_profile_locked(pname);
	if (!prof) {
		switch_mutex_unlock(glob->mutex);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT,
						  "mod_tarantool: profile '%s' not found in config\n", pname);
		free(h);
		return SWITCH_STATUS_FALSE;
	}
	h->profile = *prof;
	h->rules = glob->rules;
	/* pick the UUID default per the active host's version branch */
	tnt_rules_set_uuid_default(&h->rules, &h->profile);
	/* propagate timeouts */
	if (!h->profile.ping_idle_s)
		h->profile.ping_idle_s = glob->ping_idle_ms / 1000;
	if (!h->profile.ping_timeout_s)
		h->profile.ping_timeout_s = glob->ping_timeout_ms / 1000;
	switch_mutex_unlock(glob->mutex);

	tnt_session_init(&h->session, &h->profile);
	if (tnt_session_open(&h->session) < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "mod_tarantool: cannot connect to profile '%s' (%s)\n",
						  pname, tnt_conn_last_error(&h->session.conn));
		free(h);
		return SWITCH_STATUS_FALSE;
	}
	h->created_mono = switch_micro_time_now() / 1000;

	/* per-handle lock: serializes SQL calls against the reload rules swap
	 * (hot-reload refreshes h->rules under this lock). */
	if (glob && glob->pool &&
		switch_mutex_init(&h->lock, SWITCH_MUTEX_NESTED, glob->pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "mod_tarantool: cannot init handle mutex for profile '%s'\n", pname);
		tnt_session_close(&h->session);
		free(h);
		return SWITCH_STATUS_FALSE;
	}

	*dih = (switch_database_interface_handle_t *)calloc(1, sizeof(**dih));
	if (!*dih) {
		if (h->lock)
			switch_mutex_destroy(h->lock);
		tnt_session_close(&h->session);
		free(h);
		return SWITCH_STATUS_FALSE;
	}
	(*dih)->handle = h;
	tnt_handle_register(h);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
					  "mod_tarantool: new connection to profile '%s'\n", pname);
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t tnt_handle_destroy(switch_database_interface_handle_t **dih)
{
	tnt_handle_t *h;

	if (!dih || !*dih)
		return SWITCH_STATUS_FALSE;
	h = (tnt_handle_t *)(*dih)->handle;
	if (h) {
		tnt_handle_unregister(h);
		tnt_session_close(&h->session);
		if (h->lock)
			switch_mutex_destroy(h->lock);
		free(h);
	}
	free(*dih);
	*dih = NULL;
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t tnt_handle_flush(switch_database_interface_handle_t *dih)
{
	tnt_handle_t *h;

	if (!dih || !dih->handle)
		return SWITCH_STATUS_FALSE;
	h = (tnt_handle_t *)dih->handle;
	/* nothing to flush on a synchronous protocol */
	(void)h;
	return SWITCH_STATUS_SUCCESS;
}

/* True if every statement in the batch is idempotent cleanup DDL
 * (DROP * / CREATE INDEX *). Only such batches may swallow the
 * "Space ... does not exist" error: for data statements (SELECT/DELETE/
 * INSERT/UPDATE) that error MUST reach the core so its reactive schema
 * creation (switch_cache_db_test_reactive) creates the missing table. */
static int tnt_batch_is_cleanup_ddl(const tnt_stmt_t *stmts, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		if (stmts[i].type != TNT_STMT_DROP_TABLE &&
			stmts[i].type != TNT_STMT_CREATE_INDEX) {
			return 0;
		}
	}
	return n > 0;
}

/* Execute a list of already-split statements, each via its own
 * IPROTO_EXECUTE. Tarantool SQL does NOT support multiple statements in a
 * single execute, yet mod_sofia REQUIRES batch support (it probes with
 * "stmt1;stmt2" at startup and refuses to start when the driver rejects
 * the batch), so the driver must run the statements sequentially itself.
 *
 * When callback is non-NULL, every row of every SELECT is delivered to it
 * right after the statement that produced it; otherwise *res keeps only the
 * LAST statement's result (exec_detailed/exec_string contract). */
static switch_status_t tnt_exec_statements(tnt_handle_t *h,
										   const tnt_stmt_t *stmts, int n,
										   switch_core_db_callback_func_t callback, void *pdata,
										   tnt_result_t *res, char **err)
{
	int i, r;
	int any_ran = 0;
	int cleanup_ddl;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	memset(res, 0, sizeof(*res));
	cleanup_ddl = tnt_batch_is_cleanup_ddl(stmts, n);

	for (i = 0; i < n; i++) {
		char *out;
		int noop = 0;
		int rc;
		int stmt_retryable;
		tnt_translate_ctx_t ctx;
		char uuidbuf[40];

		stmt_retryable = (stmts[i].type == TNT_STMT_SELECT || stmts[i].type == TNT_STMT_BEGIN) ? 1 : 0;

		/* skip-index=true: never run CREATE INDEX — indexes are managed
		 * manually (e.g. composite ones for JOINs). */
		if (h->profile.skip_index && stmts[i].type == TNT_STMT_CREATE_INDEX) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
							  "mod_tarantool: skipping CREATE INDEX (skip-index=true): [%.*s]\n",
							  (int)(stmts[i].end - stmts[i].start), stmts[i].sql + stmts[i].start);
			continue;
		}

		/* per-statement translation context: fresh core UUIDv7 + global
		 * variable resolver for INSERT injections */
		memset(&ctx, 0, sizeof(ctx));
		if (tnt_uuidv7(uuidbuf) == 0)
			ctx.core_uuid = uuidbuf;
		ctx.resolver = tnt_resolve_global;
		out = tnt_rules_translate(&h->rules, &stmts[i],
								  !strncmp(h->profile.hosts[0].version, "3.", 2) ? 3 : 2,
								  &ctx, &noop);
		if (noop) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
							  "mod_tarantool: skipping statement (no-op): [%.*s]\n",
							  (int)(stmts[i].end - stmts[i].start), stmts[i].sql + stmts[i].start);
			continue;
		}
		if (!out) {
			if (err)
				*err = strdup("mod_tarantool: out of memory translating statement");
			status = SWITCH_STATUS_FALSE;
			goto done;
		}

		/* Quote Tarantool-reserved identifiers ('uuid', 'alias') in this
		 * statement before it hits the server. Worst case is a doubling of
		 * the size, so allocate strlen*2 + slack. */
		{
			size_t ncap = strlen(out) * 2 + 64;
			const char *words[TNT_RESERVED_MAX];
			int nwords = tnt_reserved_ptr_list(&h->rules, words, TNT_RESERVED_MAX);
			char *nb = (char *)malloc(ncap);

			if (!nb) {
				free(out);
				if (err && *err == NULL)
					*err = strdup("mod_tarantool: out of memory quoting statement");
				status = SWITCH_STATUS_FALSE;
				goto done;
			}
			tnt_sql_quote_reserved(out, nb, ncap, words, nwords);
			free(out);
			out = nb;
		}

		h->n_queries++;
		if (tnt_debug_active(h->profile_name))
			tnt_debug_log(h->profile_name, "out", h->caller_file, h->caller_func, h->caller_line, out);

		/* drop the previous statement's result; *res keeps the LAST one */
		tnt_result_free(res);
		rc = tnt_session_execute(&h->session, out, stmt_retryable, res);
		if (rc == 1) {
			/* deterministic SQL error: report unless benign */
			if (cleanup_ddl && tnt_rules_is_benign_error(res->error, 1)) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
								  "mod_tarantool: benign error treated as success: [%s]\n", res->error);
				free(out);
				any_ran = 1;
				continue;
			}
			/* surface the rejected SQL under 'tarantool debug on' — the core
			 * (test_reactive) often hides the CREATE failure, leaving a
			 * silent init loop */
			if (tnt_debug_active(h->profile_name)) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
								  "mod_tarantool: SQL rejected [%s]: %s\n", out, res->error);
			}
			if (err && *err == NULL)
				*err = strdup(res->error[0] ? res->error : "tarantool SQL error");
			free(out);
			status = SWITCH_STATUS_FALSE;
			goto done;
		} else if (rc < 0) {
			if (err && *err == NULL)
				*err = strdup(tnt_conn_last_error(&h->session.conn));
			free(out);
			status = SWITCH_STATUS_FALSE;
			goto done;
		} else {
			h->affected_rows = res->affected_rows >= 0 ? res->affected_rows : 0;
			if (callback && res->ncols > 0) {
				for (r = 0; r < res->nrows; r++) {
					if (callback(pdata, res->ncols, res->rows[r], res->col_name))
						break;
				}
			}
		}
		any_ran = 1;
		free(out);
	}

	if (!any_ran)
		status = SWITCH_STATUS_SUCCESS;

done:
	return status;
}

/* Execute a full SQL string (which may contain several statements).
 * Splits it, runs every statement with its own IPROTO_EXECUTE (Tarantool
 * has no multi-statement execute), and fills *res with the LAST result. */
static switch_status_t tnt_exec_sql(tnt_handle_t *h, const char *sql,
									tnt_result_t *res, char **err)
{
	tnt_stmt_t stmts[TNT_SQL_MAX_STMTS];
	int n;
	switch_status_t st;

	memset(res, 0, sizeof(*res));

	if (zstr(sql))
		return SWITCH_STATUS_SUCCESS;

	if (tnt_debug_active(h->profile_name))
		tnt_debug_log(h->profile_name, "in", h->caller_file, h->caller_func, h->caller_line, sql);

	n = tnt_sql_split(sql, stmts, TNT_SQL_MAX_STMTS);
	st = tnt_exec_statements(h, stmts, n, NULL, NULL, res, err);
	return st;
}

static switch_status_t tnt_exec_detailed(const char *file, const char *func, int line,
										 switch_database_interface_handle_t *dih,
										 const char *sql, char **err)
{
	tnt_handle_t *h;
	tnt_result_t res;
	switch_status_t st;

	if (!dih || !dih->handle)
		return SWITCH_STATUS_FALSE;
	h = (tnt_handle_t *)dih->handle;

	/* serialize SQL calls vs the reload rules swap (hot-reload) */
	if (h->lock)
		switch_mutex_lock(h->lock);

	/* remember the first callsite that uses this connection */
	if (!h->caller_set) {
		snprintf(h->caller_file, sizeof(h->caller_file), "%s", file ? file : "?");
		snprintf(h->caller_func, sizeof(h->caller_func), "%s", func ? func : "?");
		h->caller_line = line;
		h->caller_set = 1;
	}

	st = tnt_exec_sql(h, sql, &res, err);
	tnt_result_free(&res);
	if (h->lock)
		switch_mutex_unlock(h->lock);
	return st;
}

static switch_status_t tnt_exec_string(switch_database_interface_handle_t *dih,
									   const char *sql, char *resbuf, size_t len, char **err)
{
	tnt_handle_t *h;
	tnt_result_t res;
	switch_status_t st;

	if (!dih || !dih->handle)
		return SWITCH_STATUS_FALSE;
	h = (tnt_handle_t *)dih->handle;

	/* serialize SQL calls vs the reload rules swap (hot-reload) */
	if (h->lock)
		switch_mutex_lock(h->lock);

	/* exec_string has no file/func/line; label the caller explicitly */
	if (!h->caller_set) {
		snprintf(h->caller_file, sizeof(h->caller_file), "%s", "exec_string");
		snprintf(h->caller_func, sizeof(h->caller_func), "%s", "exec_string");
		h->caller_line = 0;
		h->caller_set = 1;
	}

	st = tnt_exec_sql(h, sql, &res, err);
	if (st == SWITCH_STATUS_SUCCESS && res.nrows > 0 && res.ncols > 0 && resbuf && len > 0) {
		snprintf(resbuf, len, "%s", res.rows[0][0] ? res.rows[0][0] : "");
	}
	tnt_result_free(&res);
	if (h->lock)
		switch_mutex_unlock(h->lock);
	return st;
}

static switch_status_t tnt_set_auto_commit(switch_database_interface_handle_t *dih, switch_bool_t on)
{
	tnt_handle_t *h;

	if (!dih || !dih->handle)
		return SWITCH_STATUS_FALSE;
	h = (tnt_handle_t *)dih->handle;
	h->auto_commit = on;
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t tnt_commit(switch_database_interface_handle_t *dih)
{
	tnt_handle_t *h;

	(void)dih;
	if (!dih || !dih->handle)
		return SWITCH_STATUS_FALSE;
	h = (tnt_handle_t *)dih->handle;
	h->auto_commit = SWITCH_TRUE;
	/* Tarantool is always in autocommit mode for single statements */
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t tnt_rollback(switch_database_interface_handle_t *dih)
{
	tnt_handle_t *h;

	if (!dih || !dih->handle)
		return SWITCH_STATUS_FALSE;
	h = (tnt_handle_t *)dih->handle;
	h->auto_commit = SWITCH_TRUE;
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t tnt_affected_rows(switch_database_interface_handle_t *dih, int *affected_rows)
{
	tnt_handle_t *h;

	if (!dih || !dih->handle || !affected_rows)
		return SWITCH_STATUS_FALSE;
	h = (tnt_handle_t *)dih->handle;
	*affected_rows = h->affected_rows;
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t tnt_callback_exec_detailed(const char *file, const char *func, int line,
												  switch_database_interface_handle_t *dih,
												  const char *sql,
												  switch_core_db_callback_func_t callback,
												  void *pdata, char **err)
{
	tnt_handle_t *h;
	tnt_result_t res;
	tnt_stmt_t stmts[TNT_SQL_MAX_STMTS];
	int n;
	switch_status_t st;

	if (!dih || !dih->handle)
		return SWITCH_STATUS_FALSE;
	h = (tnt_handle_t *)dih->handle;
	switch_assert(callback != NULL);

	/* serialize SQL calls vs the reload rules swap (hot-reload) */
	if (h->lock)
		switch_mutex_lock(h->lock);

	/* remember the first callsite that uses this connection */
	if (!h->caller_set) {
		snprintf(h->caller_file, sizeof(h->caller_file), "%s", file ? file : "?");
		snprintf(h->caller_func, sizeof(h->caller_func), "%s", func ? func : "?");
		h->caller_line = line;
		h->caller_set = 1;
	}

	if (tnt_debug_active(h->profile_name))
		tnt_debug_log(h->profile_name, "in", h->caller_file, h->caller_func, h->caller_line, sql);

	/* batch = several ';'-separated statements: deliver every row of every
	 * SELECT to the callback (tnt_exec_statements does it inline) */
	n = tnt_sql_split(sql, stmts, TNT_SQL_MAX_STMTS);
	st = tnt_exec_statements(h, stmts, n, callback, pdata, &res, err);
	tnt_result_free(&res);
	if (h->lock)
		switch_mutex_unlock(h->lock);
	return st;
}

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

#define SWITCH_DATABASE_API_SYNTAX "tarantool [<command> [args]]\n"

static void api_print_profile(switch_stream_handle_t *stream, const tnt_profile_cfg_t *p, int detail)
{
	int i;

	stream->write_function(stream, "Profile: %s\n", p->name);
	stream->write_function(stream, "  hosts: %d, user: %s, skip-index: %s, full-scan: %s\n",
						   p->nhosts, p->username, p->skip_index ? "true" : "false",
						   p->full_scan ? "true" : "false");
	if (p->init_sql[0])
		stream->write_function(stream, "  init-sql: %s\n", p->init_sql);
	for (i = 0; i < p->nhosts; i++) {
		const tnt_host_cfg_t *h = &p->hosts[i];

		stream->write_function(stream, "  host[%d]: %s%s%s:%u v%s\n", i,
							   h->use_unix ? "unix:" : "", h->addr,
							   h->use_unix ? "" : ":",
							   h->port, h->version[0] ? h->version : "?");
	}
	(void)detail;
}

/* Format a duration (seconds) as days:hours:minutes:seconds with units
	* appearing progressively: "1d 02:03:04", "02:03:04", "03:04", "4s". */
static void tnt_fmt_age(int64_t sec, char *buf, size_t cap)
{
	int64_t d, h, m, s;

	if (sec < 0)
		sec = 0;
	d = sec / 86400;
	sec %= 86400;
	h = sec / 3600;
	sec %= 3600;
	m = sec / 60;
	s = sec % 60;

	if (d > 0)
		snprintf(buf, cap, "%" PRId64 "d %02" PRId64 ":%02" PRId64 ":%02" PRId64, d, h, m, s);
	else if (h > 0)
		snprintf(buf, cap, "%02" PRId64 ":%02" PRId64 ":%02" PRId64, h, m, s);
	else if (m > 0)
		snprintf(buf, cap, "%02" PRId64 ":%02" PRId64, m, s);
	else
		snprintf(buf, cap, "%" PRId64 "s", s);
}

/* Join argv[start..argc-1] into a single malloc'ed SQL string, separating
	* tokens with single spaces (the API parser splits the command line on
	* whitespace, so a query with spaces arrives as several argv entries).
	* Strips one pair of matching surrounding double quotes if present (users
	* commonly quote the query). Returns NULL if there are no tokens. */
static char *api_join_query(char **argv, int start, int argc)
{
	char *out;
	char *w;
	int i;
	size_t n = 0;

	if (start >= argc)
		return NULL;
	for (i = start; i < argc; i++)
		n += strlen(argv[i]) + 1;
	out = (char *)malloc(n + 1);
	if (!out)
		return NULL;
	w = out;
	for (i = start; i < argc; i++) {
		if (i > start)
			*w++ = ' ';
		w += sprintf(w, "%s", argv[i]);
	}
	*w = '\0';
	n = strlen(out);
	if (n >= 2 && out[0] == '"' && out[n - 1] == '"') {
		memmove(out, out + 1, n - 2);
		out[n - 2] = '\0';
	}
	return out;
}

SWITCH_STANDARD_API(api_tarantool)
{
	char *mycopy = NULL;
	char *argv[16] = { 0 };
	int argc = 0;
	char *p;
	const char *sub = "";
	const char *args = cmd;

	(void)session;

	if (!zstr(args)) {
		mycopy = strdup(args);
		p = strtok(mycopy, " \t\r\n");
		while (p && argc < 15) {
			argv[argc++] = p;
			p = strtok(NULL, " \t\r\n");
		}
	}
	if (argc > 0)
		sub = argv[0];

	if (!strcasecmp(sub, "help") || !strcasecmp(sub, "")) {
		stream->write_function(stream, "Usage: %s\n", SWITCH_DATABASE_API_SYNTAX);
		stream->write_function(stream, "  status            - connection/profile status\n");
		stream->write_function(stream, "  list              - list configured profiles\n");
		stream->write_function(stream, "  stats             - counters\n");
		stream->write_function(stream, "  debug on|off [profile] - enable/disable SQL tracing\n");
		stream->write_function(stream, "  conns [profile]   - list active connections (and their callers)\n");
		stream->write_function(stream, "  ping <profile>    - ping the pool (first reachable host)\n");
		stream->write_function(stream, "  check <profile>   - ping every host in the profile\n");
		stream->write_function(stream, "  sql <profile> <q> - run a raw query\n");
		stream->write_function(stream, "  translate <q>     - show translated SQL\n");
		stream->write_function(stream, "  reload            - reload tarantool.conf.xml\n");
		stream->write_function(stream, "  drop              - release resources (no-op in v1)\n");
		stream->write_function(stream, "  version           - module version\n");
	} else if (!strcasecmp(sub, "list")) {
		int i;

		switch_mutex_lock(glob->mutex);
		for (i = 0; i < glob->nprofiles; i++)
			api_print_profile(stream, &glob->profiles[i], 0);
		switch_mutex_unlock(glob->mutex);
	} else if (!strcasecmp(sub, "status")) {
		switch_mutex_lock(glob->mutex);
		if (glob->nprofiles == 0) {
			stream->write_function(stream, "no profiles configured\n");
		} else {
			int i;

			for (i = 0; i < glob->nprofiles; i++)
				api_print_profile(stream, &glob->profiles[i], 1);
		}
		switch_mutex_unlock(glob->mutex);
	} else if (!strcasecmp(sub, "stats")) {
		stream->write_function(stream, "mod_tarantool stats: profiles=%d\n", glob->nprofiles);
	} else if (!strcasecmp(sub, "reload")) {
		switch_status_t st = config_load();

		if (st == SWITCH_STATUS_SUCCESS) {
			stream->write_function(stream, "reload ok\n");
			tnt_event_emit("config-reload", "", "ok");
		} else if (tnt_cfg_view_fatal) {
			/* oversized <views> body: keep the previously applied config
			 * (the CRIT detail was already printed while parsing) */
			stream->write_function(stream,
								   "reload failed: <views> body exceeds %d bytes; "
								   "old config kept, fix tarantool.conf.xml and reload again\n",
								   TNT_VIEW_BODY_LEN);
			tnt_event_emit("config-reload", "", "view-too-large");
		} else {
			stream->write_function(stream, "reload failed (see console log)\n");
			tnt_event_emit("config-reload", "", "failed");
		}
	} else if (!strcasecmp(sub, "version")) {
		stream->write_function(stream, "mod_tarantool %s\n", TNT_VERSION);
	} else if (!strcasecmp(sub, "drop")) {
		stream->write_function(stream, "drop: no-op in v1\n");
	} else if (!strcasecmp(sub, "debug")) {
		const char *what = argc > 1 ? argv[1] : NULL;
		const char *pname = argc > 2 ? argv[2] : NULL;

		if (!what || (strcasecmp(what, "on") && strcasecmp(what, "off"))) {
			stream->write_function(stream, "usage: tarantool debug on|off [profile]\n");
			goto out;
		}
		switch_mutex_lock(glob->mutex);
		glob->debug = !strcasecmp(what, "on") ? 1 : 0;
		if (pname)
			snprintf(glob->debug_profile, sizeof(glob->debug_profile), "%s", pname);
		else
			glob->debug_profile[0] = '\0';
		switch_mutex_unlock(glob->mutex);
		stream->write_function(stream, "debug %s%s%s\n", what,
							   pname ? " (profile: " : "", pname ? pname : "");
		if (pname)
			stream->write_function(stream, ")\n");
		tnt_event_emit(!strcasecmp(what, "on") ? "debug-on" : "debug-off",
					   pname ? pname : "", "");
	} else if (!strcasecmp(sub, "conns")) {
		const char *pname = argc > 1 ? argv[1] : NULL;
		int i, shown = 0, up = 0;

		switch_mutex_lock(glob->mutex);
		for (i = 0; i < glob->nhandles; i++) {
			tnt_handle_t *h = glob->handles[i];
			const tnt_host_cfg_t *ch;
			int64_t idle_ms, now_ms;

			if (!h)
				continue;
			if (pname && strcasecmp(pname, h->profile_name))
				continue;
			ch = &h->profile.hosts[h->session.cur >= 0 && h->session.cur < h->profile.nhosts ?
								  h->session.cur : 0];
			now_ms = switch_micro_time_now() / 1000;
			idle_ms = now_ms - h->session.conn.last_used_mono;
			if (idle_ms < 0)
				idle_ms = 0;

			stream->write_function(stream,
								   "[%d] profile=%s fd=%d %s%s%s:%u v%s cur=%d queries=%" PRIu64 "\n",
								   shown, h->profile_name, h->session.conn.fd,
								   ch->use_unix ? "unix:" : "", ch->addr,
								   ch->use_unix ? "" : ":", ch->port,
								   ch->version[0] ? ch->version : "?",
								   h->session.cur, h->n_queries);
			{
				char agebuf[32];
				int64_t age_s = h->created_mono ? (now_ms - h->created_mono) / 1000 : 0;

				tnt_fmt_age(age_s, agebuf, sizeof(agebuf));
				stream->write_function(stream,
									   "    caller=%s:%d %s idle=%" PRId64 "ms auto_commit=%s last_affected=%d age=%s\n",
									   h->caller_file[0] ? h->caller_file : "?",
									   h->caller_line,
									   h->caller_func[0] ? h->caller_func : "?",
									   idle_ms,
									   h->auto_commit ? "true" : "false",
									   h->affected_rows,
									   agebuf);
			}
			if (h->session.conn.fd >= 0)
				up++;
			shown++;
		}
		switch_mutex_unlock(glob->mutex);

		if (!shown)
			stream->write_function(stream, "no active connections%s%s\n",
								   pname ? " for profile '" : "", pname ? pname : "");
		else
			stream->write_function(stream, "%d connection(s), %d up\n", shown, up);
	} else if (!strcasecmp(sub, "ping")) {
		const char *pname = argc > 1 ? argv[1] : "local";
		tnt_profile_cfg_t prof;
		tnt_session_t s;
		int rc;

		switch_mutex_lock(glob->mutex);
		{
			tnt_profile_cfg_t *fp = find_profile_locked(pname);

			if (!fp) {
				switch_mutex_unlock(glob->mutex);
				stream->write_function(stream, "profile '%s' not found\n", pname);
				goto out;
			}
			prof = *fp;
		}
		switch_mutex_unlock(glob->mutex);

		tnt_session_init(&s, &prof);
		rc = tnt_session_ping(&s);
		stream->write_function(stream, "%s: %s\n", pname, rc == 0 ? "UP" : "DOWN");
		tnt_session_close(&s);
	} else if (!strcasecmp(sub, "check")) {
		const char *pname = argc > 1 ? argv[1] : "local";
		tnt_profile_cfg_t prof;
		int i, up = 0, total = 0;

		switch_mutex_lock(glob->mutex);
		{
			tnt_profile_cfg_t *fp = find_profile_locked(pname);

			if (!fp) {
				switch_mutex_unlock(glob->mutex);
				stream->write_function(stream, "profile '%s' not found\n", pname);
				goto out;
			}
			prof = *fp;
		}
		switch_mutex_unlock(glob->mutex);

		for (i = 0; i < prof.nhosts; i++) {
			tnt_profile_cfg_t one = prof;
			tnt_session_t s;
			const tnt_host_cfg_t *h = &prof.hosts[i];
			int rc;

			one.nhosts = 1;
			one.hosts[0] = prof.hosts[i];
			tnt_session_init(&s, &one);
			rc = tnt_session_ping(&s);
			stream->write_function(stream, "  host[%d] %s%s%s:%u v%s: %s",
								   i,
								   h->use_unix ? "unix:" : "", h->addr,
								   h->use_unix ? "" : ":",
								   h->port, h->version[0] ? h->version : "?",
								   rc == 0 ? "UP" : "DOWN");
			if (rc != 0 && s.conn.error[0])
				stream->write_function(stream, " (%s)", s.conn.error);
			stream->write_function(stream, "\n");
			if (rc == 0)
				up++;
			total++;
			tnt_session_close(&s);
		}
		stream->write_function(stream, "%s: %d/%d hosts up\n", pname, up, total);
	} else if (!strcasecmp(sub, "sql")) {
		const char *pname = argc > 1 ? argv[1] : "local";
		char *q = NULL;
		char wbuf[TNT_RESERVED_MAX][TNT_RESERVED_WORD_LEN];
		const char *words[TNT_RESERVED_MAX];
		int nwords = 0, wi;
		tnt_profile_cfg_t prof;
		tnt_session_t s;
		tnt_result_t res;
		int rc, r;

		if (!(q = api_join_query(argv, 2, argc))) {
			stream->write_function(stream, "usage: tarantool sql <profile> <query>\n");
			goto out;
		}
		switch_mutex_lock(glob->mutex);
		{
			tnt_profile_cfg_t *fp = find_profile_locked(pname);

			if (!fp) {
				switch_mutex_unlock(glob->mutex);
				free(q);
				stream->write_function(stream, "profile '%s' not found\n", pname);
				goto out;
			}
			prof = *fp;
			/* copy the reserved dictionary under the lock so the pointers
			 * stay valid even if another thread reloads the config */
			nwords = tnt_reserved_snapshot(&glob->rules, wbuf, TNT_RESERVED_MAX);
		}
		switch_mutex_unlock(glob->mutex);
		for (wi = 0; wi < nwords; wi++)
			words[wi] = wbuf[wi];

		tnt_session_init(&s, &prof);
		memset(&res, 0, sizeof(res));
		/* quote Tarantool-reserved identifiers so manual queries work too */
		{
			size_t ncap = strlen(q) * 2 + 64;
			char *nq = (char *)malloc(ncap);

			if (nq) {
				tnt_sql_quote_reserved(q, nq, ncap, words, nwords);
				free(q);
				q = nq;
			}
		}
		rc = tnt_session_execute(&s, q, 1, &res);
		if (rc != 0) {
			stream->write_function(stream, "error: %s\n", res.error[0] ? res.error : "unknown");
		} else {
			for (r = 0; r < res.nrows; r++) {
				int c;

				for (c = 0; c < res.ncols; c++) {
					stream->write_function(stream, "%s%s", c ? "|" : "",
										   res.rows[r][c] ? res.rows[r][c] : "");
				}
				stream->write_function(stream, "\n");
			}
		}
		free(q);
		tnt_result_free(&res);
		tnt_session_close(&s);
	} else if (!strcasecmp(sub, "translate")) {
		char *q = NULL;
		tnt_stmt_t stmts[TNT_SQL_MAX_STMTS];
		tnt_rules_t *rules;		/* heap copy: rules carry view bodies and
								   must NOT sit on the API thread's stack */
		int n, i;

		if (!(q = api_join_query(argv, 1, argc))) {
			stream->write_function(stream, "usage: tarantool translate <query>\n");
			goto out;
		}
		switch_mutex_lock(glob->mutex);
		rules = (tnt_rules_t *)malloc(sizeof(*rules));
		if (rules) {
			*rules = glob->rules;
			snprintf(rules->uuid_default, sizeof(rules->uuid_default), "%s", glob->uuid_default_32);
		}
		switch_mutex_unlock(glob->mutex);
		if (!rules) {
			stream->write_function(stream, "out of memory\n");
			free(q);
			goto out;
		}

		n = tnt_sql_split(q, stmts, TNT_SQL_MAX_STMTS);
		for (i = 0; i < n; i++) {
			char *out;
			int noop = 0;
			tnt_translate_ctx_t ctx;
			char uuidbuf[40];

			memset(&ctx, 0, sizeof(ctx));
			if (tnt_uuidv7(uuidbuf) == 0)
				ctx.core_uuid = uuidbuf;
			ctx.resolver = tnt_resolve_global;
			out = tnt_rules_translate(rules, &stmts[i], 3, &ctx, &noop);
			if (noop)
				stream->write_function(stream, "[%d] (no-op)\n", i);
			else if (out) {
				stream->write_function(stream, "[%d] %s;\n", i, out);
				free(out);
			} else {
				stream->write_function(stream, "[%d] (error)\n", i);
			}
		}
		free(rules);
		free(q);
	} else {
		stream->write_function(stream, "Unknown command '%s'\n", sub);
		stream->write_function(stream, "Usage: %s\n", SWITCH_DATABASE_API_SYNTAX);
	}

out:
	switch_safe_free(mycopy);
	return SWITCH_STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* <maintenance> background thread                                    */
/* ------------------------------------------------------------------ */

static switch_thread_t *maint_thread = NULL;
static int maint_thread_running = 0;

/* Replace every "${now}" in src with the given epoch value. Never overflows
 * dstcap; the result is always NUL-terminated. */
static void tnt_maint_replace_now(const char *src, char *dst, size_t dstcap,
								  time_t now)
{
	static const char tok[] = "${now}";
	const size_t tl = sizeof(tok) - 1;
	size_t i = 0, o = 0, n = strlen(src);
	char num[32];

	snprintf(num, sizeof(num), "%ld", (long) now);
	while (i < n && o + 1 < dstcap) {
		if (n - i >= tl && !strncmp(src + i, tok, tl)) {
			size_t nl = strlen(num);

			if (o + nl + 1 > dstcap)
				break;
			memcpy(dst + o, num, nl);
			o += nl;
			i += tl;
		} else {
			dst[o++] = src[i++];
		}
	}
	dst[o] = '\0';
}

/* Run one <sql> body (already ${now}-expanded): split on ';' and execute
 * each statement through the normal pipeline (translate + quote_reserved).
 * A failing statement is logged but does not stop the remaining ones. */
static void tnt_maint_execute_sql(tnt_session_t *s, const tnt_rules_t *rules,
								  const char *profile, const char *sql)
{
	tnt_stmt_t stmts[TNT_SQL_MAX_STMTS];
	tnt_translate_ctx_t ctx;
	const char *words[TNT_RESERVED_MAX];
	int nw = tnt_reserved_ptr_list(rules, words, TNT_RESERVED_MAX);
	int n = tnt_sql_split(sql, stmts, TNT_SQL_MAX_STMTS);
	int i;

	memset(&ctx, 0, sizeof(ctx));
	for (i = 0; i < n; i++) {
		char *tr, *final;
		size_t ncap;
		tnt_result_t res;
		int rc, noop = 0;

		tr = tnt_rules_translate(rules, &stmts[i], 3, &ctx, &noop);
		if (noop || !tr)
			continue;
		ncap = strlen(tr) * 2 + 64;
		final = (char *)malloc(ncap);
		if (!final) {
			free(tr);
			continue;
		}
		tnt_sql_quote_reserved(tr, final, ncap, words, nw);
		free(tr);

		memset(&res, 0, sizeof(res));
		rc = tnt_session_execute(s, final, 1, &res);
		if (rc != 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "mod_tarantool: <maintenance> profile=%s statement failed: %s\n",
							  profile, res.error[0] ? res.error : "unknown error");
		}
		tnt_result_free(&res);
		free(final);
	}
}

/* One maintenance round: snapshot config, verify mod_sofia, group the sql
 * entries by profile and run each group in a single session. All heavy data
 * lives on the heap (stack-safety rule from the load fix). */
static void tnt_maint_iteration(void)
{
	tnt_maint_sql_t *snap;
	tnt_rules_t *rr;
	time_t now;
	int n, i, j;

	/* only while mod_sofia is loaded: its schema tables may not exist yet */
	if (switch_loadable_module_exists("mod_sofia") != SWITCH_STATUS_SUCCESS)
		return;

	snap = (tnt_maint_sql_t *)malloc(sizeof(tnt_maint_sql_t) * TNT_MAINT_MAX);
	rr = (tnt_rules_t *)malloc(sizeof(*rr));
	if (!snap || !rr) {
		free(snap);
		free(rr);
		return;
	}

	switch_mutex_lock(glob->mutex);
	n = glob->nmaint_sql;
	for (i = 0; i < n && i < TNT_MAINT_MAX; i++)
		snap[i] = glob->maint_sql[i];
	*rr = glob->rules;
	switch_mutex_unlock(glob->mutex);

	now = switch_epoch_time_now(NULL);

	for (i = 0; i < n; i++) {
		tnt_profile_cfg_t prof;
		tnt_session_t s;
		int found = 0, seen = 0;

		/* already processed in this round? */
		for (j = 0; j < i; j++) {
			if (!strcasecmp(snap[j].profile, snap[i].profile)) {
				seen = 1;
				break;
			}
		}
		if (seen)
			continue;

		switch_mutex_lock(glob->mutex);
		if (tnt_maint_profile_exists(glob, snap[i].profile)) {
			prof = *find_profile_locked(snap[i].profile);
			found = 1;
		}
		switch_mutex_unlock(glob->mutex);
		if (!found) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "mod_tarantool: <maintenance> profile '%s' gone after reload, skipped\n",
							  snap[i].profile);
			continue;
		}

		tnt_session_init(&s, &prof);
		if (tnt_session_open(&s) != 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "mod_tarantool: <maintenance> profile '%s' unreachable, skipped\n",
							  snap[i].profile);
			tnt_session_close(&s);
			continue;
		}
		for (j = i; j < n; j++) {
			char expanded[TNT_MAINT_SQL_LEN + 32];

			if (strcasecmp(snap[j].profile, snap[i].profile))
				continue;
			tnt_maint_replace_now(snap[j].sql, expanded, sizeof(expanded), now);
			tnt_maint_execute_sql(&s, rr, snap[j].profile, expanded);
		}
		tnt_session_close(&s);
	}

	free(snap);
	free(rr);
}

static void *SWITCH_THREAD_FUNC tnt_maint_run(switch_thread_t *thread, void *obj)
{
	(void) thread;
	(void) obj;

	while (maint_thread_running) {
		int interval;

		switch_mutex_lock(glob->mutex);
		interval = glob->maint_enabled ? glob->maint_interval : 0;
		switch_mutex_unlock(glob->mutex);

		if (interval > 0)
			tnt_maint_iteration();

		/* sleep in small steps so shutdown can interrupt quickly; a reload
		 * (maint_wakeup) aborts the rest of the sleep and re-reads the
		 * config immediately (enable/interval/<sql> hot-applied) */
		{
			int ms = interval > 0 ? interval * 1000 : 2000;

			while (ms > 0 && maint_thread_running) {
				switch_sleep(250000);	/* microseconds */
				ms -= 250;
				if (maint_wakeup) {
					maint_wakeup = 0;
					ms = 0;
				}
			}
		}
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* module lifecycle                                                    */
/* ------------------------------------------------------------------ */

SWITCH_MODULE_LOAD_FUNCTION(mod_tarantool_load)
{
	switch_database_interface_t *database_interface;
	switch_api_interface_t *api_interface;
	switch_status_t cfg_st = SWITCH_STATUS_SUCCESS;

	glob = (tnt_global_t *)malloc(sizeof(*glob));
	if (!glob)
		return SWITCH_STATUS_TERM;
	memset(glob, 0, sizeof(*glob));

	glob->pool = pool;
	if (switch_mutex_init(&glob->mutex, SWITCH_MUTEX_NESTED, pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "mod_tarantool: cannot init mutex\n");
		free(glob);
		glob = NULL;
		return SWITCH_STATUS_TERM;
	}

	cfg_st = config_load();
	if (cfg_st != SWITCH_STATUS_SUCCESS) {
		if (tnt_cfg_view_fatal) {
			/* a <views> body exceeds TNT_VIEW_BODY_LEN: refuse to start
			 * (the CRIT detail was already printed while parsing) */
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT,
							  "mod_tarantool: module load aborted: <views> body exceeds %d bytes; "
							  "fix tarantool.conf.xml and retry\n", TNT_VIEW_BODY_LEN);
			switch_mutex_destroy(glob->mutex);
			free(glob);
			glob = NULL;
			return SWITCH_STATUS_TERM;
		}
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT,
						  "mod_tarantool: cannot load tarantool.conf.xml\n");
		/* continue with empty config: still usable via API */
	}

	/* <maintenance>: the background thread is ALWAYS started (regardless of
	 * the current enable flag) so that "tarantool reload" can turn the
	 * section on/off at runtime without reloading the module. The loop reads
	 * the effective flags on every iteration and simply idles while disabled.
	 * NULL attr => APR default stack; the thread keeps all heavy snapshots on
	 * the heap, so its stack usage stays tiny. */
	if (!maint_thread) {
		maint_thread_running = 1;
		switch_thread_create(&maint_thread, NULL, tnt_maint_run, NULL, pool);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
						  "mod_tarantool: maintenance thread started (%s at load)\n",
						  glob->maint_enabled ? "enabled" : "disabled");
	}

	*module_interface = switch_loadable_module_create_module_interface(pool, modname);
	MODULE_INTERFACE = *module_interface;

	database_interface = (switch_database_interface_t *)
		switch_loadable_module_create_interface(*module_interface, SWITCH_DATABASE_INTERFACE);
	database_interface->flags = 0;
	database_interface->interface_name = "tarantool";
	database_interface->prefixes = (char **)calloc(2, sizeof(char *));
	database_interface->prefixes[0] = strdup("tarantool");
	database_interface->handle_new = tnt_handle_new;
	database_interface->handle_destroy = tnt_handle_destroy;
	database_interface->flush = tnt_handle_flush;
	database_interface->exec_detailed = tnt_exec_detailed;
	database_interface->exec_string = tnt_exec_string;
	database_interface->sql_set_auto_commit_attr = tnt_set_auto_commit;
	database_interface->commit = tnt_commit;
	database_interface->rollback = tnt_rollback;
	database_interface->callback_exec_detailed = tnt_callback_exec_detailed;
	database_interface->affected_rows = tnt_affected_rows;

	api_interface = (switch_api_interface_t *)
		switch_loadable_module_create_interface(*module_interface, SWITCH_API_INTERFACE);
	api_interface->interface_name = "tarantool";
	api_interface->desc = "Tarantool Core DB driver control";
	api_interface->function = api_tarantool;
	api_interface->syntax = SWITCH_DATABASE_API_SYNTAX;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_tarantool loaded\n");
	tnt_event_emit("module-load", "",
				   cfg_st == SWITCH_STATUS_SUCCESS ? "ok" : "config-load-failed");
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_tarantool_shutdown)
{
	switch_status_t thr_ret = SWITCH_STATUS_SUCCESS;

	tnt_event_emit("module-unload", "", "ok");

	/* stop the <maintenance> thread first, it touches glob */
	maint_thread_running = 0;
	if (maint_thread) {
		switch_thread_join(&thr_ret, maint_thread);
		maint_thread = NULL;
	}

	if (glob) {
		switch_mutex_destroy(glob->mutex);
		free(glob);
		glob = NULL;
	}
	return SWITCH_STATUS_UNLOAD;
}
