# mod_tarantool

**🌐 [English](README.md) | [Русский](README_ru.md)**

A direct Tarantool database driver for FreeSWITCH implementing the
`switch_database_interface_t` interface. It allows using Tarantool as the
**Core DB** (the `channels`, `calls`, `registrations` tables and everything
that goes through `switch_core_db_*` / `switch_cache_db_*`) instead of the
built-in SQLite or PostgreSQL.

Communication with the server goes directly over the binary **IPROTO**
protocol (`IPROTO_EXECUTE` + SQL), **without** ODBC, libpq or the
`libtarantool` client library. MessagePack encoding/decoding is done by a
minimal self-contained implementation (`mp_decode.c`, ~500 lines), so the
module has no external dependencies other than the standard libc and
FreeSWITCH itself.

---

## Features (v1.0.11)

- **Core DB registration**: the full set of 10 `switch_database_interface_t`
  callbacks (`handle_new`/`destroy`, `exec_detailed`, `exec_string`,
  `sql_set_auto_commit_attr`, `commit`, `rollback`, `callback_exec_detailed`,
  `affected_rows`, `flush`).
- **Connection profiles**: independent pools, keyed by the DSN
  `tarantool://<profile_name>`.
- **Transport**: unix-socket or TCP (`host:port`), unix-socket takes priority.
- **Authentication**: `chap-sha1` (with a stub for `chap-sha256`);
  SHA1/SHA256/base64 implemented from scratch, no OpenSSL.
- **Tarantool server versions**: `2.10+` and `>=3.2` (branches); protocol
  differences are taken into account (e.g. `IPROTO_METADATA = 0x32` in 3.x
  versus `0x52` in older versions).
- **Reliability**:
  - bounded timeouts (`connect-timeout`, `query-timeout`);
  - broken-connection detection: `SO_KEEPALIVE`, pre-read `recv(MSG_PEEK)`,
    `IPROTO_PING` for idle connections;
  - multi-host failover (first live host, switch only on network failures)
    with exponential backoff;
  - retry after a disconnect only for **SELECT** (read-only); transactions
    stick to a single host.
- **DDL translator**: FreeSWITCH/core and mod_sofia emit SQLite-like DDL that
  the Tarantool dialect does not understand "as is". The module translates it
  according to declarative rules from the config (see below).
- **Batch queries**: SQL of the form `stmt1;stmt2;...` (typical for mod_sofia)
  is split by a correct splitter (string literals `''`, comments, parens) and
  executed sequentially.
- **Batch transactionality**: DML batches are executed atomically via IPROTO
  streams — separate `IPROTO_EXECUTE` packets sharing a non-zero
  `IPROTO_STREAM_ID` (the server binds them to a single fiber, so the
  `BEGIN; stmt1; …; COMMIT;` transaction becomes end-to-end; on a disconnect
  the server rolls it back itself, on a timeout the module sends an explicit
  `ROLLBACK` in the same stream). DDL batches stay outside the stream — DDL in
  Tarantool is not transactional. Server requirement: Tarantool ≥2.11
  (present in 3.x). See "Protocol features taken into account".
- **Multithreading**: one connection per (thread × callsite); every
  `handle_new` creates a deep copy of the profile+rules, so a config reload
  does not tear down active connections.
- **Connection pool** (`pool-size`, v1.0.5): each handle keeps up to 8
  independent connections (round-robin, per-conn mutex instead of global
  serialization, transactions pinned to one connection, immutable rules
  snapshot for parallel translation). `pool-size=1` is the classic behavior.
- **Shared circuit breaker** (`cb-mode`, v1.0.6): a per-host state machine
  `CLOSED → OPEN → HALF_OPEN` in a global registry (key `addr:port`), shared
  by all profiles/pools. Modes `off` (classic backoff), `wait` (requests sleep
  until the window ends — safe for `switch_cache_db_test_reactive`) and `fast`
  (immediate failure). Failure threshold, OPEN window, metrics and the
  `tarantool cb show|reset` CLI.
- **SELECT literal materialization** (`literal`, v1.0.7): Tarantool SQL cannot
  type anonymous `'?'` in a SELECT list (`PREPARE` fails with "Supplied key
  type of part N …"). Cache entries that have literals in the SELECT block are
  flagged `literal` (computed once at insert time); before `IPROTO_PREPARE`
  such literals are materialized into constants and their values are removed
  from the bind array. The cache key stays stable (markers everywhere) — no
  `cache` bloat.
- **SEQSCAN injection** (`full-scan`, v1.0.8): the Tarantool 3.8 fork does not
  know the `sql_full_scan` session variable (`Session setting sql_full_scan
  doesn't exist`), so `SET SESSION` silently does nothing and full scans stay
  forbidden. With `full-scan="true"` on a profile the module inserts `SEQSCAN`
  before table names in `FROM`/`JOIN` (including DML subqueries:
  `DELETE ... IN (SELECT ... FROM t1 JOIN t2 ...)`) on all execution paths
  (core, `<maintenance>`, `tarantool sql`) and in `tarantool translate`.
  `DELETE FROM t` without a subquery is left alone; an already-present
  `SEQSCAN` is not duplicated.
- **API command validation** (`translate`/`sql`, v1.0.9): before processing,
  the query goes through variable resolution `${now}`/`${epoch}`/`${hostname}`/
  `${switchname}`/`${strftime(<fmt>)}` (the same set as in `<maintenance>`)
  and a light syntax check (`tnt_sql_validate`: split on `;`, paren balance,
  unterminated strings/comments, keyword recognition). An unknown variable or
  invalid SQL is rejected with a human-readable reason in the console — no
  round-trip to the server (there may be no connection at startup/after a
  break).
- **CLI API**: the `tarantool` command in fs_cli.

---

## Module layout

```
src/mod/databases/mod_tarantool/
├── Makefile.am            # built via build/modmake.rulesam
├── README.md              # this file
├── conf/
│   └── tarantool.conf.xml # configuration (installed into autoload_configs)
├── mod_tarantool.c        # interface registration, XML parsing, batch, API
│                          #   (incl. api_resolve_validate: ${...} resolution +
│   │                       #   syntax validation for sql/translate)
├── tnt_client.c/.h        # IPROTO client: connect/AUTH/EXECUTE/PING,
│                          #   timeouts, failover, backoff, response parser
├── tnt_sql.c/.h           # SQL splitter on top-level ';' and statement
│                          #   classifier (CREATE/DROP/INSERT/UPDATE/...),
│                          #   tnt_sql_validate (light syntax validation),
│                          #   tnt_sql_inject_seqscan (FROM SEQSCAN t)
├── tnt_rules.c/.h         # DDL translator (primaryKey/primaryKeyAdd/
│                          #   addField, BIGINT→INTEGER, VIEW no-op, etc.)
├── tnt_stats.c/.h         # runtime SQL statistics ("tarantool sqlstats"),
│                          #   a clean layer without switch_* (table filter,
│                          #   literal normalization, FNV template hashing)
├── tnt_tcache.c/.h        # dynamic translate cache ("tarantool cache"),
│                          #   a clean layer without switch_* (fetch/store/show,
│                          #   least-hits eviction, private copies on hit)
├── circuit_breaker.c/.h   # shared circuit breaker (per-host state machine
│                          #   CLOSED→OPEN→HALF_OPEN, off/wait/fast modes)
└── mp_decode.c/.h         # minimal MessagePack decoder/encoder
                           #   (no external dependencies)
```

The layer tests (`mp_decode_test.c`, `tnt_sql_test.c`, `tnt_rules_test.c`,
`tnt_client_test.c`) are temporary and are not part of the build.

---

## Requirements

| Component | Version |
|---|---|
| FreeSWITCH | any modern branch (uses `switch_xml_attr`, the new `SWITCH_STANDARD_API` signature); tested on branch **1.11.3** |
| Tarantool | **2.10** or **3.x** (tested on **3.8**), with SQL enabled |
| C compiler | GCC/Clang with `-std=gnu99`, `-D_GNU_SOURCE` support |
| Dependencies | none external (only `libfreeswitch.la`) |

> TLS note: server-side TLS in Tarantool **Community Edition** 3.x is not
> available (`iproto.ssl` is an Enterprise feature). The `<tls>` section exists
> in the config but is commented out; uncomment it when working with Tarantool
> Enterprise or when terminating TLS with an external proxy (e.g. stunnel).

---

## Installation and integration into the FreeSWITCH build

The module is developed as part of the FreeSWITCH source tree and is built via
autotools (`modmake.rulesam`). To integrate it into your build:

### 1. Copy the module directory

```bash
# from the mod_tarantool repository
cp -r mod_tarantool <freeSWITCH>/src/mod/databases/mod_tarantool
```

### 2. Register the Makefile in configure.ac

Add the following line to the `AC_CONFIG_FILES` list (the
`src/mod/.../Makefile` section):

```
src/mod/databases/mod_tarantool/Makefile
```

### 3. Enable the module in the build configuration

Add an active line to `build/modules.conf.in` (or `build/modules.conf.most`):

```
databases/mod_tarantool
```

(lines starting with `#` disable the module; lines with `|https://...` are
external repositories).

### 3.1. (optional) deb package: an entry in `debian/control-modules`

If the module is built as part of the FreeSWITCH deb packages
(`dpkg-buildpackage`), add an entry to `debian/control-modules` (format — like
the other modules):

```
Module: databases/mod_tarantool
Description: Adds mod_tarantool
 Adds mod_tarantool, a direct Tarantool Core DB driver for FreeSWITCH.
```

`Depends:` and `Build-Depends:` for this entry are **not needed**: the module
has no external dependencies — it links only `libfreeswitch` (see `Makefile.am`,
`mod_tarantool_la_LIBADD`); everything else is the standard C library
(including pthread, which is part of glibc). All packages required for the
build are already listed in the `Build-Depends` of the main FreeSWITCH package.

### 4. Configure and build

```bash
cd <freeSWITCH>
./bootstrap.sh          # regenerates configure and src/mod/modules.inc
./configure             # --with-module-list=... or the default
make                    # builds mod_tarantool.la
sudo make install       # installs the module and conf/tarantool.conf.xml
```

After installation the module lands in `$(libdir)/freeswitch/mod/` and the
config in `$(sysconfdir)/freeswitch/autoload_configs/tarantool.conf.xml`
(`install-data-local` in Makefile.am does this automatically).

### 5. Load the module in the runtime configuration

Add to `conf/vanilla/autoload_configs/pre_load_modules.conf.xml`:

```xml
<load module="mod_tarantool"/>
```

### 6. Assign Tarantool as the Core DB

In `conf/vanilla/autoload_configs/switch.conf.xml`:

```xml
<param name="core-db-dsn" value="tarantool://local"/>
```

`local` is the profile name from `tarantool.conf.xml`. Profiles can be added
arbitrarily: the DSN `tarantool://<name>` selects the matching `<profile>`.

> If you previously used `core-db-dsn` with another driver (e.g.
> `pgsql://host:port/...`), do not forget to unload the corresponding module
> from `pre_load_modules.conf.xml` so the interfaces do not conflict over the
> shared DSN prefix.

---

## Configuration (`tarantool.conf.xml`)

The file is installed into `autoload_configs` and has the following sections:

### `<settings>` — common parameters

| Parameter | Description |
|---|---|
| `default-action` | `add` — automatically inject a synthetic PK `<table>_uuid` for tables not covered by rules; `none` — pass through unchanged (Tarantool will reject CREATE TABLE without a PK) |
| `primaryKeyAddDefault-3.2` | informational: the UUID generator for the 3.x branch. **Not used as a DEFAULT** — Tarantool does not evaluate expression DEFAULTs on INSERT, so v1 injects `uuid()` directly into every INSERT (see `primaryKeyAdd`) |
| `primaryKeyAddDefault-2.10` | the same for the 2.10 branch (informational) |
| `reconnect-interval` | reconnect attempt period, ms |
| `ping-idle` | idle-connection probing via IPROTO_PING, ms (0 = off) |
| `ping-timeout` | ping timeout, ms |
| `debug` | `true`/`false` — SQL tracing of every request (in from the core, out to Tarantool) by default; toggled at runtime with the `tarantool debug` command |
| `cache` | `false` (default)/`true` — statement translate cache keyed by the **normalized template** (see `tarantool cache`): data literals are replaced with markers, the key is stable across different values; on a hit the literals of the specific call are substituted back. `false` — bypass the cache and clear it (kill-switch); applied hot via `reload` and the `tarantool cache on/off` command |
| `cb-mode` | `off` (default)/`wait`/`fast` — shared circuit breaker mode (see the "Circuit breaker" section). `wait` — on OPEN requests sleep until the window ends (safe for the FreeSWITCH core); `fast` — immediate failure; `off` — classic per-conn backoff |
| `cb-failure-threshold` | number of consecutive transport failures before the breaker opens (default `3`) |
| `cb-timeout` | OPEN window length, ms (default `5000`) |

### `<profiles>` — connection pools

Each profile has a `name` attribute (the name from the DSN), `version`
(Tarantool branch: `2.10` or `3.2`) and `skip-index` (index ignoring
`false`/`true`). The address is either inline parameters or a `<hosts>`
section for failover.

The profile-level `<param name="prepare"/>` is the **default for all hosts of
the profile**; a host can override it with its own `<param name="prepare"/>`
inside `<host>` (profile → host inheritance). Details in the
[`prepare` — prepared statements](#prepare--prepared-statements-iproto_prepare--bind)
section.

> **Maximum number of profiles — 32** (`TNT_PROFILES_MAX`). Profiles beyond
> the limit are ignored with a warning in the log. The profile array lives on
> the heap inside `tnt_global_t`: the config is parsed into a heap snapshot and
> atomically swapped under a mutex, so the limit only bounds memory usage
> (~3 KB per profile), not the stack size.

> **Maximum number of hosts in one `<hosts>` — 8** (`TNT_HOST_MAX`). Hosts
> beyond the limit are ignored with a warning in the log. The limit is tied to
> the fixed failover-state arrays in `tnt_session_t`
> (`dead_until_host[]`, `backoff_host[]`).

Profile/host parameters:

| Parameter | Description |
|---|---|
| `unix-socket` | path to the unix socket (takes priority over host:port) |
| `host` | host IP/name (TCP) |
| `port` | TCP port |
| `user` / `password` | credentials (chap-sha1) |
| `connect-timeout` | connect() timeout, ms |
| `query-timeout` | per-query timeout, ms |

> ⚠️ **`full-scan` and `init-sql` are PROFILE-LEVEL parameters** (placed on
> `<profile>`, NOT inside `<host>`). They apply to **every** host of the
> profile — both the single host and all hosts in `<hosts>` (including after
> every failover reconnect). Placing them in `<host>` is ignored with a
> warning in the log so a typo does not disappear silently.

Profile-level parameters (`<profile>`):

| Parameter | Description |
|---|---|
| `full-scan` | `true` (default) — on every new connection to any host of the profile `SET SESSION sql_full_scan=true` is executed (Tarantool 2.11/3.x); on an unknown variable — fallback `SET SESSION sql_seq_scan=true` (older 2.x); `false` — do not enable. **v1.0.8**: when `true` the module additionally inserts `SEQSCAN` before tables in `FROM`/`JOIN` (including DML subqueries) on all execution paths — the only reliable way on forks that do not know `sql_full_scan` (node01: `Session setting sql_full_scan doesn't exist`). Without this the FreeSWITCH core cannot run full scans (reactive probe queries) — every startup ends with DROP+CREATE of all tables |
| `init-sql` | arbitrary SQL executed right after a successful connect to any host of the profile (and after every failover reconnect); an "unknown variable" error is benign, other errors are logged as warnings without dropping the connection |
| `pool-size` | connection pool size per handle (1..8, default 1). Each handle keeps up to `pool-size` independent connections (each with its own failover loop and prepared-statement cache) and distributes statements round-robin. `1` = classic "one handle — one connection"; `>1` allows executing statements in parallel on different connections (see the [«Connection pool»](#connection-pool-pool-size) section) |

Profile attribute:

| Attribute | Description |
|---|---|
| `skip-index` | `true` — suppress all `CREATE INDEX` coming from the FreeSWITCH core/mod_sofia (indexes are managed manually, e.g. composite ones for JOINs); `false` (default) — translate as usual |

Example (single host, unix-socket):

```xml
<profile name="local" version="3.2" skip-index="false">
  <param name="unix-socket" value="/var/lib/tarantool/socket/tarantool.control"/>
  <param name="user" value="freeswitch"/>
  <param name="password" value="P@ssw0rd"/>
  <param name="connect-timeout" value="2000"/>
  <param name="query-timeout" value="3000"/>
  <param name="full-scan" value="true"/>
  <!-- <param name="init-sql" value="SET SESSION sql_full_scan=true"/> -->
  <!-- <param name="pool-size" value="4"/> -->
</profile>
```

Example (failover, master via socket + TCP replica):

```xml
<profile name="local" skip-index="false">
  <!-- profile-level: applies to EVERY host in <hosts> -->
  <param name="full-scan" value="true"/>
  <param name="init-sql" value="SET SESSION sql_full_scan=true"/>
  <hosts>
    <host name="master-sock" version="3.2">
      <param name="unix-socket" value="/var/lib/tarantool/socket/tarantool.control"/>
      <param name="user" value="freeswitch"/>
      <param name="password" value="P@ssw0rd"/>
      <param name="query-timeout" value="3000"/>
    </host>
    <host name="replica-tcp" version="3.2">
      <param name="host" value="127.0.0.1"/>
      <param name="port" value="3302"/>
      <param name="user" value="freeswitch"/>
      <param name="password" value="P@ssw0rd"/>
      <param name="query-timeout" value="3000"/>
    </host>
    <host name="replica-tls" version="3.2">
      <param name="host" value="127.0.0.1"/>
      <param name="port" value="3303"/>
      <param name="user" value="freeswitch"/>
      <param name="password" value="P@ssw0rd"/>
      <param name="query-timeout" value="3000"/>
      <param name="tls-profile" value="client-local-tls"/>
    </host>
  </hosts>
</profile>
```

> ⚠️ **Important**: in v1 `tls-profile` and the `<tls>` section are **only a
> stub** — they are parsed and stored in the configuration but do **NOT
> activate encryption**: the connection is always established over an
> unencrypted TCP/unix socket. Working encryption will arrive in v1.1 (or can
> be implemented with an external proxy, e.g. stunnel, which encrypts the
> channel to Tarantool independently of the module).

### `<tls>` — client TLS profiles (v1.1 placeholder)

Server-side TLS in Tarantool **Community Edition** 3.x is not available
(`iproto.ssl` is an Enterprise feature), so client TLS support is deferred.
The section is already parsed for backward compatibility and validation (limit
`TNT_PROFILES_TLS_MAX`), but active use will start in v1.1. The `tls-profile`
parameter on a host is likewise parsed into the `tls_profile` field of the
host structure but does not affect the connection yet.

```xml
<tls>
  <profile name="client-local-tls">
    <param name="ca-file" value="$${conf_dir}/tls/ca.pem"/>
    <param name="cert-file" value="$${conf_dir}/tls/client.crt"/>
    <param name="key-file" value="$${conf_dir}/tls/client.key"/>
    <param name="verify" value="true"/>
    <param name="verify-host" value="true"/>
    <param name="min-version" value="TLSv1.2"/>
  </profile>
</tls>
```

> **Maximum number of TLS profiles — 16** (`TNT_PROFILES_TLS_MAX`). Profiles
> beyond the limit are ignored with a warning in the log.

### `<primaryKey>` / `<primaryKeyAdd>` — declarative PK rules

FreeSWITCH creates tables WITHOUT an explicit PRIMARY KEY (e.g.
`CREATE TABLE channels (...)` without a PK) — Tarantool requires a PK
mandatorily. The rules translate CREATE TABLE before it is executed.

- `<primaryKey table field>` — an existing column is declared
  `PRIMARY KEY (field)`. Values are supplied by the core as plain strings,
  the type comes from the core DDL (varchar), no CAST: comparisons work as-is
  (verified by a probe on 3.8: the wire type is `string`).
- `<primaryKeyAdd table field [type] [core-uuid]>` — `field <type> NOT NULL,
  PRIMARY KEY (field)` is appended to the end of the table definition; the
  module injects the PK value on every `INSERT ... VALUES (...)`.
  Tarantool does NOT apply expression DEFAULTs like `DEFAULT uuid()` on INSERT
  ("NOT NULL constraint failed", verified on 3.8), so no DEFAULT is emitted
  and the value is substituted directly into the query.

`primaryKeyAdd` attributes:

| attribute   | values                                                               | default      |
|-------------|----------------------------------------------------------------------|--------------|
| `type`      | SQL column type: `varchar(36)` or `uuid`; varchar(N)/char/text/string normalize to STRING on the wire | `varchar(36)` |
| `core-uuid` | `true` — core UUIDv7 injected into INSERT; `false` — Tarantool-side `uuid()`; absent — auto | auto |

The `type × core-uuid` matrix (effective_core_uuid = attribute, otherwise
`string type ? true : false`):

| type      | core-uuid | injection into INSERT                    |
|-----------|-----------|------------------------------------------|
| uuid      | `true`    | `CAST('<core-v7>' AS UUID)`              |
| uuid      | `false`   | `uuid()`                                 |
| varchar   | `true`    | literal `'<core-v7>'` (recommended: core comparisons are bare strings) |
| varchar   | `false`   | warning + auto-switch to core UUIDv7 (probe: `uuid()` yields a UUID that a varchar column rejects) |

```xml
<primaryKey>
  <param table="channels" field="uuid"/>
  <param table="recovery" field="uuid"/>
</primaryKey>

<primaryKeyAdd>
  <param table="calls"         field="calls_uuid"         type="varchar(36)" core-uuid="true"/>
  <param table="registrations" field="registrations_uuid" type="varchar(36)" core-uuid="true"/>
</primaryKeyAdd>
```

### `<addField>` — adding arbitrary columns

```xml
<addField>
  <!-- global-variable resolution is enabled ONLY by the in-line="true" attribute -->
  <param table="recovery" field="switch_uuid" type="varchar(36)" isNull="false" value="$${switch_uuid}" in-line="true"/>
  <!-- without in-line the value is inserted as-is (literal) -->
  <param table="interfaces" field="switchs_uuid" type="varchar(36)" isNull="false" value="$${switchs_uuid}"/>
</addField>
```

`addField` attributes:

| attribute | value                                                                   | default |
|-----------|-------------------------------------------------------------------------|---------|
| `type`    | SQL column type (passed through as-is)                                  | `STRING` |
| `isNull`  | `true` (no NOT NULL) / `false` (adds `NOT NULL`)                        | `true`  |
| `default` | `DEFAULT <expr>`; expressions with `(` are skipped (not evaluated on INSERT) | empty |
| `in-line` | `true` — resolve `$${global}`/`${var}` from the environment (switch_core_get_variable) before INSERT; an unresolved variable is inserted as-is (with a warning); absent/`false` — the value is inserted as-is (literal, `''`-escaped) | empty |
| `value`   | injection into every INSERT: with `type="uuid"` — `CAST('<value>' AS UUID)`; empty — the column is not injected | empty |

Injection is performed only for INSERTs with an explicit column list that does
not yet contain the injected column; multi-row INSERTs and queries without a
column list pass through unchanged.

### `<reserved>` — identifier quoting dictionary

Tarantool SQL does not allow reserved words as bare identifiers
(`where alias=...` fails with a syntax error). The module automatically wraps
such words in double quotes in all outgoing SQL — both our DDL and the core
DML (`alias` → `"alias"`). The words `uuid` and `alias` are built into the
module by default; additional words are added to the config without
recompiling:

```xml
<reserved>
  <param name="uuid"/>
  <param name="alias"/>
  <param name="key"/>      <!-- example: if the core uses a key column -->
</reserved>
```

Lexer rules: only an identifier in identifier position is quoted — string
literals (`'alias'`), already-quoted names and function calls (`uuid(...)`)
are left alone. Quoting is always semantically safe (`"alias"` is the same
`alias` column). Dictionary limit — 32 words; extra words are ignored with a
warning in the log.

### `<intColumns>` — numeric columns with string values

FreeSWITCH formats some integer fields **in quotes**: e.g. `created_epoch`
goes into SQL as `'1791322069'` (the `'%ld'` format in
[`switch_core_sqldb.c`](../src/switch_core_sqldb.c:2478)). Tarantool does not
implicitly convert string→integer on INSERT/UPDATE, and the write fails with
`Type mismatch: can not convert string('...') to integer`. The section
declares which fields may arrive in this form:

```xml
<intColumns>
  <param table="channels" field="created_epoch" type="INTEGER"/>
  <param table="calls"    field="call_created_epoch" type="INTEGER"/>
</intColumns>
```

Rules:

- on INSERT/UPDATE a string literal `'…'` value for a declared column is
  rewritten to `CAST('…' AS <type>)`; an empty string `''` → `NULL`;
- bare numbers (`expires = 1700000000`), `NULL`, `uuid()` and expressions are
  left alone;
- string columns are never touched (the "table/field" dictionary);
- the type is checked at `load`/`tarantool reload`: an unknown name → WARNING
  in the console and the entry is ignored; valid types:
  `INTEGER|INT|BIGINT|SMALLINT|TINYINT|MEDIUMINT` → `INTEGER`,
  `NUMBER` → `NUMBER`, `DOUBLE|REAL|FLOAT` → `DOUBLE`,
  `DECIMAL|NUMERIC` → `DECIMAL`;
- limit: 64 entries (`TNT_INTCOL_MAX`), overflow — WARNING + ignore;
  duplicate "table+field" pairs are deduplicated.

### `<views>` — VIEW emulation (Tarantool has none)

`show calls`, `show detailed_calls`, `show bridged_calls` and the core's
reactive probes access `basic_calls` / `detailed_calls` — such objects do not
exist in Tarantool, so the module rewrites every core `SELECT ... FROM <view>
...` into `SELECT ... FROM (<body>) AS <view> ...` (a subquery in FROM is
supported by Tarantool 2.x+; LEFT JOIN and `NOT IN (SELECT ...)` verified on
3.2):

```xml
<views>
  <view name="basic_calls"><![CDATA[
    select a.uuid as uuid, ... c.call_created_epoch as call_created_epoch
    from channels a
    left join calls c on a.uuid = c.caller_uuid and a.hostname = c.hostname
    left join channels b on b.uuid = c.callee_uuid and b.hostname = c.hostname
    where a.uuid = c.caller_uuid or a.uuid not in (select callee_uuid from calls)
  ]]></view>
</views>
```

Rules:

- the CDATA holds the **pure SELECT body** (without the `create view ... as`
  prefix);
- the body must reference only real tables (usually `channels`/`calls`);
- only the first `FROM` section is substituted, and only for a query of the
  form `FROM <name>` without an alias (that is exactly how the core accesses
  it); `JOIN <view>` / nested SELECTs are not expanded;
- the resulting SQL goes through the same reserved-word quoting rewriter
  (`uuid` → `"uuid"`) as all other queries;
- without the `<views>` section the behavior is unchanged (the SELECT goes
  through as-is);
- the body is **validated at config load** (without contacting the server):
  it must be exactly one well-formed SELECT (split on `;`, quote/paren
  balance, recognized keywords). An invalid body is NOT applied — WARNING in
  the console
  `mod_tarantool: <views> 'name': invalid SQL, view NOT applied: <reason>`.

Limits (limits are not truncated, they are rejected):

| Condition | Behavior |
|---|---|
| view body > 4096 bytes | **hard error**: the config is rejected as a whole — at `load` the module does not start (CRIT in the console: view name, actual size, limit); at `tarantool reload` the new config is not applied (the old working one stays, CRIT in the console, the command returns `reload failed ... old config kept`) |
| more than 8 views | not fatal: extra views are ignored, WARNING "view limit reached" in the console |
| empty name/body | WARNING "empty name or body", entry ignored |
| invalid body (not one SELECT, broken quotes/parens, unrecognized SQL) | WARNING "invalid SQL, view NOT applied: <reason>", view ignored |
| placeholder found in the body (`${…}` / `$${…}`) | WARNING "placeholder '…' is not allowed in view bodies", view ignored — dynamics in a static VIEW substitution would freeze at load time |

Rule: it is better to hard-reject a broken configuration than to silently lose
a view due to truncation — after fixing the size, run `load`/`reload` again.

### `<maintenance>` — periodic SQL cleanup

The task: mod_sofia writes BLF/presence statuses into `sip_presence`/
`sip_subscriptions`; the regular TTL cleanup (`sofia_reg_check_expire`) fires
rarely, and with a long record lifetime the entries accumulate while Tarantool
presence queries (full scans) get expensive. The section starts a background
module thread that periodically executes the given DELETEs:

```xml
<maintenance interval="120" enable="true">
  <sql profile="local"><![CDATA[
    delete from sip_presence where expires > 0 and expires <= ${now};
    delete from sip_subscriptions where expires > 0 and expires <= ${now}
  ]]></sql>
  <sql profile="local"><![CDATA[delete from sip_registrations where expires > 0 and expires <= ${now}]]></sql>
</maintenance>
```

Rules:

- **`enable`** — defaults to `false`. The section is active ONLY if
  `enable="true"` AND `interval > 0` AND there is at least one `<sql>` with an
  **existing profile**; otherwise — WARNING in the console and the section
  does not work;
- **`interval`** — seconds, forcibly clamped to [60, 3600] (clamp with
  WARNING); `0` = disabled;
- each `<sql profile="...">` runs on its own profile; entries of one profile
  are grouped into a single session per iteration; order is preserved;
- one `<sql>` may list several queries separated by `;` — they are split
  (`tnt_sql_split`) and executed one by one in the same session;
- **dynamic variables** are expanded on every iteration (Tarantool SQL has no
  `CURRENT_TIMESTAMP`, so the time is always substituted fresh):
  | Variable | Value |
  |---|---|
  | `${now}` / `${epoch}` | `(long)` of the iteration time (unixtime) |
  | `${hostname}` / `${switchname}` | node name (`switch_core_get_switchname()`) |
  | `${strftime(<fmt>)}` | current time by format, e.g. `${strftime(%H%M)}` |
  Values are substituted only **outside** string literals `'…'`. An unknown
  variable → the SQL is considered broken: WARNING with the SQL text and the
  token itself (`unknown maintenance variable '${…}'`), the entry is NOT
  applied at load/reload and is skipped on iterations. Reminder: `$${…}`
  (FS globals) are expanded by the core preprocessor while reading the config
  — an unknown global is silently removed by the core, and the resulting
  broken SQL is caught by the validator;
- the thread runs **independently of other modules** (including `mod_sofia`):
  the module is a generic driver and does not know about endpoint modules; if
  the tables do not exist yet (e.g. sofia is not loaded), the statement fails
  with a Tarantool error → WARNING, the rest continue; the actual state is
  visible in `tarantool maintenance show` (runs/last run);
- unreachable profile hosts → WARNING + group skip (no crash);
- an individual statement error — WARNING (text truncated to 1 KB), the rest
  keep executing;
- the body of each `<sql>` is **validated at config load** (without the
  server): emptiness, statement count (≤ 32), quote/paren balance and
  recognized keywords; an invalid entry is NOT applied — WARNING in the
  console
  `mod_tarantool: <maintenance> <sql> (profile 'name'): invalid SQL, entry NOT applied: <reason>`;
- limits: ≤ 16 `<sql>` entries, body ≤ 4 KB — overflow is rejected (not
  truncated) with a WARNING;
- the thread starts in `mod_tarantool_load` and stops in
  `mod_tarantool_shutdown`; changes to `enable`/`interval`/the list are picked
  up from the next iteration (`tarantool reload` without reloading the
  module).

### `tarantool reload` — what is applied and what is not

`reload` fully re-reads `tarantool.conf.xml`; on any error (including unknown
types in `<intColumns>`) the config is **not applied at all** — the old
working one stays, CRIT/WARNING is written to the console. Then:

**Applied to ACTIVE connections immediately (hot-reload):**

- all rule sections — `<reserved>`, `<views>`, `<primaryKey>`/`<primaryKeyAdd>`,
  `<addField>`, `<intColumns>`. They live in a single `tnt_rules_t` structure;
  reload increments `rules_epoch`, and every active connection refreshes the
  rules **lazily** — on the first SQL call after seeing a foreign epoch (a
  ~40 KB copy once per handle per reload, under `glob->mutex` and the handle
  lock). The translate cache is cleared entirely. In the log:
  `rules epoch -> N (handles refresh lazily)`.
- `<param name="cache"/>` — hot: the new config value is applied to the cache
  (enable/disable + clear).

**Applied on the fly at `reload`:**

- `<maintenance>` (`enable`/`interval`/`<sql>`) — `reload` wakes the
  background thread (wake-up flag), which immediately re-reads the snapshot
  under the mutex and starts an iteration with the new parameters (latency
  ≤ 250 ms); `${now}` is always fresh at execution time. A full stop/restart
  of the thread is not needed — it is always running and simply "idles" while
  the section is disabled;
- `<settings>` `default-action`, the `ping-idle`/`ping-timeout` timings — from
  the next use/connection.

**NOT applied to active connections (only to new ones):**

- `<profiles>` (`<profile>`/`<hosts>`: addresses, ports, credentials, connect
  timeouts, `skip-index`, `full-scan`, `init-sql`) and `<tls>` — the
  connection is already open with its own profile copy; to apply, a FreeSWITCH
  restart is needed (or wait until the pool creates new handles);
- `debug` is controlled separately and instantly: `tarantool debug on|off
  [profile]`.

### Other DDL translation rules

- `BIGINT` → `INTEGER` (Tarantool has only INTEGER);
- `CREATE VIEW` → no-op (logged by the caller);
- `IF NOT EXISTS` / `IF EXISTS` → removed (Tarantool rejects them);
- `CREATE INDEX` → normalized to the `IF NOT EXISTS` form;
- with `skip-index="true"` on the profile — all `CREATE INDEX` from the
  core/mod_sofia are suppressed entirely (logged as info); indexes must be
  managed manually (e.g. composite ones for JOIN queries); PK injection
  (`primaryKey`/`primaryKeyAdd`) keeps working;
- "benign" errors (`already exists`, `duplicate key`) are mapped to success;
  `Space X does not exist` — only for idempotent batches of
  `DROP`/`CREATE INDEX`; for data (SELECT/DELETE/INSERT/UPDATE) it must reach
  the core so that reactive schema creation fires
  (`switch_cache_db_test_reactive`);
- Tarantool reserved words in identifier position (`uuid`, `alias`) are
  automatically quoted (`"uuid"`, `"alias"`) in all outgoing SQL — both our
  DDL and the core DML. The list is extended by the `<reserved>` section
  without recompiling the module (see below);
- function DEFAULTs (`DEFAULT uuid()`, `DEFAULT uuid7()`) are not emitted in
  DDL: Tarantool 3.x parses them but does not evaluate them on INSERT.

---

## Preparing Tarantool

A minimal server-side example (master instance):

```lua
box.cfg({
    listen = {'127.0.0.1:3301', '/var/lib/tarantool/socket/tarantool.control'},
    memtx_dir = '/var/lib/tarantool/socket',
    wal_dir   = '/var/lib/tarantool/socket',
    read_only = false,
    -- MVCC for memtx: parallel DML from several FreeSWITCH connections
    -- (channels/calls/sip_registrations) without mutual locks; mandatory
    -- under load above a few CPS and for future transactional batches
    -- (IPROTO streams).
    memtx_use_mvcc_engine = true,
})
-- Auth user (as before)
local ok, err = pcall(function()
    box.schema.user.create('freeswitch', {password = 'P@ssw0rd', if_not_exists = true})
    box.schema.user.grant('freeswitch', 'read,write,execute,create,alter,drop', 'universe', nil, {if_not_exists = true})
end)
if not ok then error(err, 2) end
```

SQL is enabled in Tarantool 3.x by default; in 2.10 — `box.cfg({sql_cache_size = ...})`.

> Master→replica replication is configured the standard way
> (`box.cfg.replication`). Module v1 switches to another host only on a
> network failure; the role (`master`/`replica`/`any`) is a v1.1 stub.

### Unix-socket permissions

`connect()` to AF_UNIX requires two permissions: **write** (`w`) on the socket
inode itself and **traverse** (`x`) through all parent directories. The
FreeSWITCH user (usually `freeswitch`/`xfreeswitch`) has neither by default:

- the `/run/tarantool` directory is created by systemd (`RuntimeDirectory=`) as
  `drwxr-x--- tarantool tarantool` — not traversable by others;
- the instance creates the socket itself as `srwxr-xr-x` — no `w` for
  group/others, so merely adding the user to the `tarantool` group is not
  enough.

A ready drop-in `/etc/systemd/system/tarantool.service.d/access.conf`
(check the unit name with `systemctl cat tarantool | head -1`):

```ini
[Service]
# /run/tarantool is created by RuntimeDirectory before ExecStartPre: allow traverse for all
ExecStartPre=/usr/bin/chmod 755 /run/tarantool
# the socket is created by tarantool itself after start — wait for it and grant connect() rights
ExecStartPost=/usr/bin/sh -c 'for i in $(seq 1 50); do [ -S /run/tarantool/tarantool-socket.control ] && break; sleep 0.1; done; chmod o+w /run/tarantool/tarantool-socket.control'
```

Then:

```bash
sudo systemctl daemon-reload
sudo systemctl restart tarantool
ls -l /run/tarantool/        # drwxr-xr-x ... ; srwxrwxrwx ...tarantool-socket.control
```

The permissions are reset on every restart (the directory is recreated by
systemd, the socket by the instance), so permanent `ExecStartPre`/`ExecStartPost`
are mandatory. An alternative without permission fiddling — connecting over
TCP: replace `unix-socket` with `host`/`port` in the `tarantool.conf.xml`
profile.

---

## CLI API

The command is available in fs_cli:

```
tarantool help
tarantool status              # profile status
tarantool list                # profile list
tarantool stats               # counters
tarantool debug on|off [profile]  # SQL tracing of requests in/out
tarantool conns [profile]     # active connections and their callers
tarantool ping [profile]      # pool ping — first reachable host (default local)
tarantool check [profile]     # ping every host of the profile individually
tarantool sql <profile> <q>   # run a raw SQL query
tarantool translate [<profile>] <q>  # show the DDL translation result
                                     # (profile optional: if the first
                                     #  token matches a profile name;
                                     #  otherwise — the first profile in the config)
tarantool sqlstats on|off|reset|tables|show   # SQL template statistics
tarantool cache on|off|reset|show [N] [full]  # translate cache
tarantool prepare on|off|reset|show           # prepared statements
tarantool cb show|reset                       # shared circuit breaker
tarantool maintenance show|restart            # <maintenance> scheduler:
                                              #   show — state and entries,
                                              #   restart — immediate tick
tarantool reload              # re-read tarantool.conf.xml
tarantool drop                # release resources (no-op in v1)
tarantool version             # module version
```

Examples:

```
tarantool ping local
tarantool check local
tarantool sql local "select * from channels limit 1"
tarantool translate "create table channels (uuid varchar(255), name varchar(255))"
tarantool translate local "delete from sip_subscriptions where sip_subscriptions_uuid in (select s.sip_subscriptions_uuid from sip_subscriptions s left join sip_registrations r on s.sip_user = r.sip_user where r.sip_user is null)"
```

### `sql`/`translate` validation (v1.0.9)

Both commands run the query through `api_resolve_validate` before processing:

1. **Variable resolution** — the same set as in `<maintenance>`:
   `${now}`, `${epoch}`, `${hostname}`, `${switchname}`, `${strftime(<fmt>)}`.
   An unknown variable is rejected:
   ```
   > tarantool translate "select * from t where ts > ${unknown}"
   translate error: unknown maintenance variable 'unknown'
   ```
2. **Light syntax check** (`tnt_sql_validate`): split on top-level `;`, paren
   balance, unterminated strings/comments, keyword recognition. Invalid SQL is
   rejected without a round-trip to the server (there may be no connection at
   startup/after a break):
   ```
   > tarantool sql local "SELCT * FRM t"
   sql error: statement starts with unrecognized keyword 'selct'
   ```

Resolution runs with sample-time (`switch_epoch_time_now`), so `${now}` in
`translate`/`sql` is substituted with the current unix time.

Example `check` output for a profile with two hosts:

```
  host[0] unix:/var/lib/tarantool/socket/tarantool.control:3301 v3.2: UP
  host[1] 127.0.0.1:3302 v3.2: DOWN (connect failed)
local: 1/2 hosts up
```

Example `conns` output (active connections with callers; with
`pool-size > 1` each handle shows all its pool connections,
`conn=N/M` — connection number and pool size, `[tx]` — the connection pinned
to the current transaction):

```
[0] profile=local conn=0/1 fd=23 unix:/var/lib/tarantool/socket/tarantool.control:3301 v3.2 cur=0 queries=842
    caller=sofia_reg.c:2578 sofia_reg_handle_sip_r_register idle=12ms auto_commit=true last_affected=1 age=36s
[1] profile=local conn=0/1 fd=24 unix:/var/lib/tarantool/socket/tarantool.control:3301 v3.2 cur=0 queries=57
    caller=switch_core_sqldb.c:2829 switch_core_db_handle_new idle=800ms auto_commit=true last_affected=-1 age=1d 02:03:04
2 connection(s), 2 up
```

> `conns` shows up to **256** tracked connections (`TNT_HANDLES_MAX`). This is
> a diagnostic registry, not a connection limit: the real connection count is
> bounded by the FreeSWITCH core's `switch_cache_db` pool. On registry
> overflow new connections work normally but are not shown in `conns` (a
> warning is written to the log).

Example `debug on local` trace (in freeswitch.log):

```
mod_tarantool[debug] profile=local in sofia_reg.c:2578 sofia_reg_handle_sip_r_register
  sql: INSERT INTO sip_registrations (...) VALUES (...);
mod_tarantool[debug] profile=local out sofia_reg.c:2578 sofia_reg_handle_sip_r_register
  sql: INSERT INTO sip_registrations (...) VALUES (...);
```

### `sqlstats` — runtime SQL template statistics

The `tarantool sqlstats ...` command family counts what share of the SQL
stream repeats (by normalized templates) — the basis for estimating the
translate cache and `IPROTO_PREPARE + bind`. The original SQL strings with
data are not stored: only normalized samples stay in memory (literals replaced
with `?`), so no PII reaches the output.

```
tarantool sqlstats on                     # start accumulation
tarantool sqlstats off                    # stop (data is kept)
tarantool sqlstats reset                  # counters=0 AND filter=all (does not touch on/off)
tarantool sqlstats tables                 # show the current filter
tarantool sqlstats tables t1 t2 ...       # REPLACE the filter with a table list
tarantool sqlstats tables all             # filter = all tables
tarantool sqlstats show [N] [table]       # summary + top-N templates
```

Table filter rules:

- **replace semantics**: every `tables` call fully replaces the list;
- duplicates are silently collapsed, names are lowercased, comparison is
  case-insensitive (`Channels` ≡ `channels`);
- limit **32** names (extra ones are ignored with a WARNING in the command
  output);
- view names are supported without special syntax: for `SELECT` the first name
  after the top-level `FROM` is taken (a view name as-is, `(...)` →
  `*subquery*`, a second source → the ` (join)` suffix);
- the filter check runs **before** normalization/hashing: a table outside the
  list costs only a name scan, not a hash.

While `sqlstats off` — zero overhead: nothing is counted or hashed. When `on`
the overhead is one pass over the statement (normalization + FNV-1a-64 hash),
negligible for diagnostic off-peak loads.

Example `show` output:

```
sqlstats: state=on filter: all
statements: total=501911 counted=501911 filtered-out=0
type      total      distinct   top-hit
SELECT    182344         12         96214
INSERT    153980        340          2103
UPDATE     88721         96         34011
DELETE     76554         88         40231
by (type, table):
  INSERT  channels                total=152000   distinct=331
  UPDATE  sip_registrations       total=34011    distinct=3
  ...
top templates (limit 20):
  1. count=40231    [DELETE] sip_registrations
     delete from sip_registrations where sip_user = ? and hostname = ?
  ...
counters: templates=552/4096 (overflow 0) type-table=18/128 (overflow 0)
```

Interpretation: the `total / distinct` ratio on hot tables is a direct
prediction of the translate-cache win (module level) and of the number of
prepared templates when moving to `IPROTO_PREPARE + bind` (server level).
Individual literals do not inflate distinct: normalization masks strings
(`'...'` → `?`, including `''`), bare numbers (`123` → `?`), strips comments,
lowercases keywords and collapses whitespace.

Limits: 4096 templates / 128 (type × table) pairs — beyond the limit the
overflow is summed into the `overflow` counters; the statistics live in
runtime and survive `reload`, but are reset on module restart.

### `cache` — statement translate cache (by normalized template)

A dynamic, self-populating cache with a **template** key: data literals
(`'...'`, bare numbers) are replaced with a marker, the text is lowercased and
whitespace-collapsed — the key is stable across different values of the same
statement (a first measurement with a raw key showed a 4.19% hit-rate:
uuid/status/timestamp literals are almost always unique; the normalized key —
~99% per `sqlstats` data). The cache stores the translated+quoted **template**;
on a hit the literals of the specific call are extracted from the input
(`tnt_sql_templateize`) and substituted into the template (`tnt_sql_rebuild`).

Invalidation — wholesale on every rules `reload` (epoch mechanism, handles
refresh lazily). Statements whose rewrite depends on literal values (intColumns
CAST, `''`→NULL), literal core-UUID in pkadd injection, `$${var}` resolution —
are **not cached** (`tnt_translate_ctx_t.ctx_dep`) and go the raw path.
Safety-net: a marker-count mismatch after translation → the statement runs the
raw path and is not stored. Limits: 512 entries (least-hits eviction), ≤ 8 KB
per statement, ≤ 32 literals of 512 bytes each.

**Only DML/SELECT are cached** (`SELECT`/`INSERT`/`UPDATE`/`DELETE`) — the
same criterion as for prepared statements. Startup DDL (`CREATE/DROP/ALTER`,
~70+ one-shot templates during sofia warm-up) does not enter the cache: they
run once per process lifetime, and their `hits=1` entries would be ballast
spoiling `cache show` and the hit-rate. This is also critical for prepared
statements (`box.prepare` does not accept DDL).

```
tarantool cache on | off | reset
tarantool cache show [N] [full]   # summary + top-N by hits; full — the whole text
```

- `off` — bypass the cache + clear (kill-switch, applied immediately);
- `reset` — clear entries and the hits/misses counters;
- the `<param name="cache"/>` parameter in `<settings>` sets the permanent
  configuration (default `true`), also applied via `reload`.

Example `show` output:

```
cache: enabled (limit 512, used 69)
hit-rate: 8544/8613 = 99.20%
misses since reset: 69
total hits over entries: 8544
top entries (by hits, limit 20):
  1. hits=2848     len=214    age=112s 14:03:21
      select call_id,sip_user,... from sip_registrations where force_ping= '0' ...
```

### `prepare` — prepared statements (IPROTO_PREPARE + bind)

Optimization stage 2. Cacheable SELECT/INSERT/UPDATE/DELETE templates are
executed via the protocol's `box.prepare` equivalent: a translated template
with `\001` markers becomes SQL with positional `'?'` parameters
(`IPROTO_PREPARE = 0x0d`), the literals of the specific call are encoded into
a msgpack bind (`IPROTO_SQL_BIND = 0x41`), execution — `IPROTO_EXECUTE (11)`
with `IPROTO_STMT_ID = 0x43`. Both client work (rebuild → bind) and server
work (SQL parsing/planning per call) are saved.

Key invariants:

- **configuration is per-host**: `<param name="prepare"/>` is set on
  `<profile>` (default for all its hosts) and/or in `<host>` (overrides);
  profile → host inheritance, default `false`;
- **gate on the REAL server version**: the version is taken from the greeting
  (`"Tarantool X.Y.Z"`), not from the profile's `version` dial attribute —
  `IPROTO_PREPARE` appeared in 2.11; on older servers the prepared path is not
  activated at all (`prepare show` shows `PREPARE unsupported`);
- **`stmt_id` is bound to the connection** — the prep-cache (hash of the
  translated template → `stmt_id`, FIFO eviction, ≤ 128 entries) lives in
  `tnt_conn_t` and is cleared on connection close/reopen and on hot-reload of
  the rules (the translated template text changes);
- **transparent fallback**: any prepare error (DDL/PRAGMA are not preparable)
  or execute error (a schema change invalidates the statement) returns the
  statement to the plain path — literal rebuild + `IPROTO_EXECUTE` with text;
  16 consecutive failed PREPAREs disable prepare for the rest of the
  connection's life;
- strings are bound as `MP_STR`, integers as `MP_INT/MP_UINT`, other numbers
  as `MP_DOUBLE` (`tnt_bind_encode`, ≤ 32 values of 512 bytes).

**SELECT literals (v1.0.7)**: Tarantool SQL cannot infer the type of an
anonymous `'?'` in a SELECT list — `PREPARE` fails with
`Supplied key type of part N does not match index part type: expected ...`
(a typical case is mod_sofia autocomplete:
`select distinct a2,?,? from complete where ...`). Solution: at translate-cache
insert time the `literal` flag is computed once
(`tnt_sql_has_select_markers` — are there markers before the first `FROM`); on
the prepare path such literals are materialized into constants
(`tnt_sql_materialize_select`) and their values are removed from the bind
array. The resulting prepared SQL: constants in SELECT, `'?'` only in WHERE.
The cache and prep-cache keys stay stable (markers everywhere), so different
SELECT-constant values do not spawn entries. Heuristic limitation: the first
`FROM` outside strings/comments — subqueries in WHERE with literals in their
own SELECT list are not covered (not seen in live traffic).

```
tarantool prepare on | off | reset
tarantool prepare show
# profile 'local': prepare on 1/1 host(s)
# conn 'local': prepare allowed, server 3.2, PREPARE ok
# prepares: 74  execs: 8421  prep-cache hits: 8347 (99.12%)  fallbacks: 12  errors: 0
```

- `on`/`off` — runtime override for all live connections; on reconnect the
  host config returns (`tnt_conn_init`);
- `reset` — zero the counters;
- `show` — per-profile config, live conn state (allowed/real server version)
  and counters: `prepares` — IPROTO_PREPARE sent, `execs` — executed via the
  prepared path, `prep-cache hits` — per-conn stmt_id-cache hits,
  `fallbacks` — drops to the plain path, `errors` — server refusals.

**Fallback diagnostics**: with `tarantool debug on` every fallback is logged
with the reason and template:
`prepare fallback (sql-error: <error text>): [<template with '?'>]`.
Reasons: `no-connection`, `prepare-broken`, `prepare-disabled`,
`prepare-unsupported`, `oom:qsql`/`oom:bind`, `prepare-rejected`,
`sql-error` (incl. prepared invalidation on schema change — the most common
case in live traffic), `transport`.

---

## Connection pool (`pool-size`)

Each handle (`tarantool://<profile>`) keeps **`pool-size` independent
connections** (1..8, default 1) and distributes statements between them
**round-robin**. Each connection has its own failover loop, its own
prepared-statement cache (`stmt_id` is bound to the connection) and its own
per-connection mutex: statements on **different** connections run in parallel,
on one — serialized. `pool-size=1` fully reproduces the classic "one handle —
one connection" behavior.

Key properties:

- **Parallelism**: `h->lock` is no longer held for the duration of an SQL
  call — it is used only for short critical sections (connection selection,
  transaction pinning, first callsite, CLI). Execution is serialized by the
  per-connection mutex, so `pool-size>1` spreads concurrent requests across
  several connections.
- **Transactions**: `set_auto_commit(false)` pins the current connection to
  the handle (`[tx]` in `tarantool conns`); all transaction statements go to
  it, while other calls with `pool-size>1` bypass the pinned connection.
  `commit`/`rollback` release the pin.
- **Failover ×N**: each pool session independently selects a host and
  reconnects; with `pool-size>1` the failure of one connection does not block
  the others.
- **Prepared statements**: the per-conn cache was already designed for the
  pool — `stmt_id` is bound to the connection, the cache is reset on reconnect
  (`tnt_conn_init`), and on rules reload all pool connections are reset.
- **Translation rules (immutable snapshot)**: reload creates a new copy of
  `tnt_rules_t` and atomically swaps the pointer; the old snapshot goes into a
  grace list and is freed after 60 s (in-flight statements may still read it
  without locks). Translation on different connections runs in parallel.
- **Partial pool**: if opening connection `k>0` fails at handle creation, the
  pool shrinks to `k` (warning in the log); if the first one fails — the
  handle is not created.

Diagnostics: `tarantool conns [profile]` shows every pool connection
(`conn=N/M`, `[tx]` for the pinned one), `tarantool prepare show` — the
PREPARE state per connection, `tarantool list` — the profile's `pool-size`.

### `pool-size` and the number of hosts are orthogonal axes

**`pool-size` is NOT related to the number of hosts in `<profile>`.** These
are two independent axes:

- **`<hosts>` — the failover list**: ordered *alternatives* from which each
  session selects **one** live host (master + replicas for failure cases).
  This is not "simultaneous connections".
- **`pool-size` — parallel connections per handle**: each pool connection
  **itself** runs failover over the whole host list.

Therefore `pool-size = nhosts` does **not** distribute connections across
hosts: all pool sessions start with the first host of the list and, with a
live master, connect to it (replicas stay unused while the master gets extra
sockets + fibers + prep-caches). Load distribution across replicas
(SELECT → replica, write → master) is a separate role-aware routing feature
(the `role` attribute is ignored in v1).

### What `pool-size` really determines

| Factor | Impact |
|---|---|
| Query parallelism through one handle | How many FreeSWITCH threads send SQL simultaneously (RPS × RTT) |
| RTT to Tarantool | At RTT ~1 ms one connection passes ~1000 req/s; more connections — more throughput |
| Resources | Each conn = socket + server fiber + prep-cache (128 entries); at 256 handles × 8 conn = 2048 sockets — too much |
| Host topology | **No impact** — the failover list is served by each pool session independently |

### Recommendation for the installer

- **Keep `pool-size=1`** (safe default, classic "one handle — one connection"
  behavior).
- If a "reasonable load default" is needed — **2–4**, but this is a
  performance parameter tuned by measurements (RPS, RTT), not by the host
  count.
- **Do not derive `pool-size` from `nhosts`**: with 3–4 hosts in the failover
  list the installer would silently create 3–4 connections to a single master
  — extra resources without benefit.

---

## Circuit breaker (`cb-mode`)

The shared circuit breaker protects FreeSWITCH from a "tight" error loop when
a Tarantool host dies: the core (`switch_cache_db_test_reactive`) makes a fast
test query, and if the driver instantly returns `FAIL`, the core considers the
tables missing, tries to create them, gets an instant refusal again and enters
an infinite loop (CPU 100%).

### State machine (per-host, key `addr:port`)

```
CLOSED ──(threshold of transport failures)──▶ OPEN ──(cb-timeout)──▶ HALF_OPEN
  ▲                                                                    │
  └──────────────(probe succeeded)─────────────────────────────────────┘
  OPEN ◀──(probe failed: new cb-timeout window)── HALF_OPEN
```

- **CLOSED** — the host is healthy, all requests go to the network. Every
  transport failure (connect, disconnect, PING/pre-read fail) increments the
  counter; at `cb-failure-threshold` (default 3) the breaker opens. The
  counter is shared by all sessions/pools, so a pool "wave" of failures = one
  open, not N.
- **OPEN** — requests do not go to the network:
  - `cb-mode=wait` — the thread sleeps until the window ends (sleep bounded by
    `query-timeout`), then re-checks the state and, if the window has expired,
    competes for the right to become the probe;
  - `cb-mode=fast` — immediate error `circuit breaker OPEN (fail-fast)`.
- **HALF_OPEN** — exactly one thread ("first wins") makes a real `connect()`;
  the rest wait. Success → `CLOSED` (all waiters wake up), failure → `OPEN`
  again for `cb-timeout`.

### Modes

| `cb-mode` | Behavior on OPEN | When to use |
|---|---|---|
| `off` | classic per-conn exponential backoff (v1.0.5) | default behavior; compatibility |
| `wait` | requests sleep until the window ends, then probe | **recommended**: safe for `switch_cache_db_test_reactive`, the network is not hammered (1 connect per window) |
| `fast` | immediate failure | only if the core/application handles an instant FAIL correctly (otherwise a CPU loop) |

### Peculiarities

- **Per-host, not per-profile**: two profiles on one `addr:port` share one
  breaker; the state lives in the module's global registry.
- **Backoff replacement**: with `cb-mode != off` the per-conn
  `dead_until_host[]`/`backoff_host[]` are not used — the breaker takes their
  role.
- **Live connections are not gated**: the breaker only intervenes in
  `connect()`/session opening and counts transport failures; already
  established connections work without checks.
- **Writes on OPEN** (`fast`) return an error without a retry — only
  SELECT/BEGIN are retryable (duplicate protection).
- **Reload** updates `cb-mode`/`cb-failure-threshold`/`cb-timeout` hot without
  resetting the state and counters; `tarantool cb reset` resets the state and
  counters manually.
- **Single-host profile + `wait`**: the only host in OPEN — requests sleep
  until the window ends (≤ `query-timeout`), then one probe; the real connect
  frequency ≈ 1 per `cb-timeout`. This "simulates a hung DB" for the core —
  safer than an instant FAIL.

### CLI

```
tarantool cb show    # mode, threshold, window + per-host table:
                     #   state, fails, open_until, probing,
                     #   opens / fail_fast / probes / successes
tarantool cb reset   # all breakers to CLOSED, counters zeroed
```

Example output:

```
circuit breaker: mode=wait threshold=3 timeout=5000ms hosts=1
  127.0.0.1:3301            state=open     fails=3 open_until=1750000000000 probing=0 opens=1 fail_fast=42 probes=1 successes=0
```

### CLI

```
tarantool maintenance show    # enable/interval/entries/runs/last run + the <sql> list
tarantool maintenance restart # immediate tick (wakeup); if the thread died —
                              # it is recreated
```

Example output:

```
maintenance: enabled, interval=120s, entries=1, runs=42, last run 00:00:37 ago
  [0] profile=local
    sql: delete from sip_subscriptions where sip_subscriptions_uuid not in (select ...)
```

`runs` — the number of completed iterations, `last run` — the time since the
last one ("never" if the thread has not run yet — e.g. right after startup or
with `enable="false"`). With `tarantool debug on` every iteration writes
`in`/`out` pairs for each `<maintenance>` statement to the log (like regular
queries).

### Configuration (`<settings>`)

```xml
<param name="cb-mode" value="wait"/>
<param name="cb-failure-threshold" value="3"/>
<param name="cb-timeout" value="5000"/>
```

---

## Protocol features taken into account

- **Two-object IPROTO 3.x responses**: a request and a response are two
  separate msgpack objects (header-map `{0x00: type, 0x01: sync, 0x05:
  schema_version}` + body-map `{0x30: DATA, 0x32: METADATA, 0x42: SQL_INFO,
  0x71: error-stack}`).
- **`IPROTO_METADATA = 0x32` in 3.x** (in <3.x it was `0x52`; now `0x52` is
  the ERROR-map). This is exactly why the `2.10`/`3.2` version is required in
  the config.
- **AUTH chap-sha1**: TUPLE = `[method, scramble]`; scramble =
  `SHA1(salt || SHA1(SHA1(pass))) XOR SHA1(pass)`; in the 3.x greeting the
  salt is taken from the first 27 base64 characters plus a padding `=` (20
  bytes).
- **NULL → `""`** when filling results (as the FreeSWITCH core expects).
- **Errors**: the text is extracted from the error-stack (`0x71`, field
  `0x03`).
- Column names are copied deeply (pointers into the response buffer cannot be
  stored).
- **MP_EXT types** are decoded into strings: `uuid` (ext 0x03, 16 bytes →
  `8-4-4-4-12`), `datetime`/`date` (ext 0x02: scale/tzoffset/epoch/nsec →
  `YYYY-MM-DDTHH:MM:SS[.fff]±HHMM`, offset without a colon). `decimal` (0x00)
  and `interval` (0x04) are returned as hex with a warning until the wire
  layout is pinned; an unknown ext — `(ext<type>)` without failing the query.
- **INTERVAL dialect**: the literal is written with map syntax
  `CAST({'day': 1} AS INTERVAL)` (keys `year/month/day/hour/minute/second`);
  the standard `INTERVAL '1' DAY` does not exist.
- **Full table scan is forbidden by default**: `SELECT * FROM t` fails
  (`Scanning is not allowed`); `FROM SEQSCAN t` is required. The module
  enables `SET SESSION sql_full_scan=true` per connection (see the full-scan
  parameter), and with `full-scan="true"` (v1.0.8) additionally inserts
  `SEQSCAN` before tables in `FROM`/`JOIN` — on forks without `sql_full_scan`
  (node01) this is the only way to allow a scan.
- **Transactions are bound to a fiber** (important for the future "IPROTO
  streams for DML batches" phase): sequentially sent requests without a stream
  are executed by the server in **different fibers**, so `COMMIT` returns
  `no transaction is active`. A transactional batch requires sending
  **separate network packets** `IPROTO_EXECUTE` with the same non-zero
  `IPROTO_STREAM_ID` in the header of each packet — then the server binds them
  to one fiber and the transaction becomes end-to-end:

  ```
  Packet 1: START TRANSACTION;  (or BEGIN;)
  Packet 2: INSERT INTO users VALUES (1, 'Ivan');   -- stmt1
  Packet 3: INSERT INTO users VALUES (2, 'etc');    -- stmt2
  Packet 4: COMMIT;
  ```

  Tarantool does not support multiple statements in one `IPROTO_EXECUTE`, and
  anonymous `BEGIN … END` blocks are not a documented SQL form — so batch
  transactionality is implemented only via streams. DDL is not wrapped into a
  stream (DDL in Tarantool is not transactional), and on a timeout inside a
  stream an explicit `ROLLBACK` in the same stream is mandatory — the server
  does not do it itself.

---

## License and author

**License**: Mozilla Public License 2.0.

**Author**: Filippov Anatoliy <error.email@mail.ru>

The module is derived from FreeSWITCH
(FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application)
and uses its API (`switch_database_interface_t` from
`src/include/switch_module_interfaces.h`), but contains no code of other
database drivers.
