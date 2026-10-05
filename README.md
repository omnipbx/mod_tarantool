# mod_tarantool

Прямой драйвер базы данных Tarantool для FreeSWITCH, реализующий интерфейс
`switch_database_interface_t`. Позволяет использовать Tarantool в качестве
**Core DB** (таблицы `channels`, `calls`, `registrations` и всё, что идёт через
`switch_core_db_*` / `switch_cache_db_*`) вместо встроенного SQLite или
PostgreSQL.

Общение с сервером идёт напрямую по бинарному протоколу **IPROTO**
(`IPROTO_EXECUTE` + SQL), **без** ODBC, libpq и клиентской библиотеки
`libtarantool`. MessagePack-кодирование/декодирование выполнено собственной
минимальной реализацией (`mp_decode.c`, ~500 строк), поэтому у модуля нет
никаких внешних зависимостей, кроме стандартной libc и самого FreeSWITCH.

---

## Возможности (v1.0.2)

- **Регистрация как Core DB**: полный набор из 10 колбеков
  `switch_database_interface_t` (`handle_new`/`destroy`, `exec_detailed`,
  `exec_string`, `sql_set_auto_commit_attr`, `commit`, `rollback`,
  `callback_exec_detailed`, `affected_rows`, `flush`).
- **Профили подключения**: независимые пулы, ключуются по DSN
  `tarantool://<имя_профиля>`.
- **Транспорт**: unix-socket или TCP (`host:port`), приоритет unix-socket.
- **Аутентификация**: `chap-sha1` (и заготовка под `chap-sha256`),
  SHA1/SHA256/base64 реализованы с нуля, без OpenSSL.
- **Версии сервера Tarantool**: `2.10+` и `>=3.2` (ветки), различия протокола учитываются
  (например, `IPROTO_METADATA = 0x32` в 3.x против `0x52` в старых).
- **Надёжность**:
  - bounded-таймауты (`connect-timeout`, `query-timeout`);
  - детекция «битых» соединений: `SO_KEEPALIVE`, pre-read `recv(MSG_PEEK)`,
    `IPROTO_PING` для простаивающих соединений;
  - мультихостовый failover (первый живой хост, переключение только по
    сетевым сбоям) с экспоненциальным backoff;
  - повтор запроса после разрыва только для **SELECT** (read-only),
    транзакции прилипают к одному хосту.
- **DDL-транслятор**: FreeSWITCH/core и mod_sofia выдают SQLite-подобный DDL,
  который Tarantool-диалект не понимает «как есть». Модуль транслирует его по
  декларативным правилам из конфига (см. ниже).
- **Батч-запросы**: SQL вида `stmt1;stmt2;...` (типично для mod_sofia)
  разбивается корректным сплиттером (строки `''`, комментарии, скобки) и
  выполняется последовательно.
- **Мультипоточность**: соединение на (поток × callsite); каждый
  `handle_new` создаёт глубокую копию профиля+правил, поэтому reload
  конфигурации не рвёт активные соединения.
- **CLI API**: команда `tarantool` в fs_cli.

---

## Состав модуля

```
src/mod/databases/mod_tarantool/
├── Makefile.am            # сборка через build/modmake.rulesam
├── README.md              # этот файл
├── conf/
│   └── tarantool.conf.xml # конфигурация (ставится в autoload_configs)
├── mod_tarantool.c        # регистрация интерфейса, парсинг XML, батч, API
├── tnt_client.c/.h        # IPROTO-клиент: connect/AUTH/EXECUTE/PING,
│                          #   таймауты, failover, backoff, парсер ответов
├── tnt_sql.c/.h           # сплиттер SQL по top-level ';' и классификатор
│                          #   операторов (CREATE/DROP/INSERT/UPDATE/...)
├── tnt_rules.c/.h         # транслятор DDL (primaryKey/primaryKeyAdd/
│                          #   addField, BIGINT→INTEGER, VIEW no-op и т.д.)
└── mp_decode.c/.h         # минимальный MessagePack decoder/encoder
                          #   (без внешних зависимостей)
```

Тесты слоёв (`mp_decode_test.c`, `tnt_sql_test.c`, `tnt_rules_test.c`,
`tnt_client_test.c`) являются временными и не входят в сборку.

---

## Требования

| Компонент | Версия |
|---|---|
| FreeSWITCH | любая современная ветка (используется API `switch_xml_attr`, новая сигнатура `SWITCH_STANDARD_API`); тестирование проводилось на ветке **1.11.3** |
| Tarantool | **2.10** или **3.x** (тестирование на **3.8**), с включённым SQL |
| C-компилятор | GCC/Clang с поддержкой `-std=gnu99`, `-D_GNU_SOURCE` |
| Зависимости | нет внешних (только `libfreeswitch.la`) |

> Примечание по TLS: серверный TLS в **Community Edition** Tarantool 3.x
> недоступен (`iproto.ssl` — Enterprise-функция). Секция `<tls>` в конфиге
> есть, но закомментирована; раскомментируйте её при работе с Tarantool
> Enterprise или при терминации TLS внешним прокси (например, stunnel).

---

## Установка и подключение к сборке FreeSWITCH

Модуль разработан как часть дерева исходников FreeSWITCH и собирается через
autotools (`modmake.rulesam`). Чтобы подключить его к своей сборке:

### 1. Скопируйте каталог модуля

```bash
# из репозитория mod_tarantool
cp -r mod_tarantool <freeSWITCH>/src/mod/databases/mod_tarantool
```

### 2. Зарегистрируйте Makefile в configure.ac

В список `AC_CONFIG_FILES` (секция `src/mod/.../Makefile`) добавьте строку:

```
src/mod/databases/mod_tarantool/Makefile
```

### 3. Включите модуль в конфигурацию сборки

В файл `build/modules.conf.in` (или `build/modules.conf.most`) добавьте активную строку:

```
databases/mod_tarantool
```

(строки, начинающиеся с `#`, отключают модуль; строки с `|https://...` —
внешние репозитории).

### 3.1. (опционально) deb-пакет: запись в `debian/control-modules`

Если модуль собирается в составе deb-пакетов FreeSWITCH (`dpkg-buildpackage`),
добавьте в `debian/control-modules` запись (формат — как у остальных модулей):

```
Module: databases/mod_tarantool
Description: Adds mod_tarantool
 Adds mod_tarantool, a direct Tarantool Core DB driver for FreeSWITCH.
```

`Depends:` и `Build-Depends:` для этой записи **не нужны**: у модуля нет внешних
зависимостей — линкуется только `libfreeswitch` (см. `Makefile.am`,
`mod_tarantool_la_LIBADD`), остальное — стандартная библиотека C (в т.ч. pthread,
входящий в состав glibc). Все необходимые для сборки пакеты уже перечислены в
`Build-Depends` основного пакета FreeSWITCH.

### 4. Сконфигурируйте и соберите

```bash
cd <freeSWITCH>
./bootstrap.sh          # перегенерирует configure и src/mod/modules.inc
./configure             # --with-module-list=... или дефолт
make                    # соберёт mod_tarantool.la
sudo make install       # установит модуль и conf/tarantool.conf.xml
```

После установки модуль окажется в `$(libdir)/freeswitch/mod/`, а конфиг — в
`$(sysconfdir)/freeswitch/autoload_configs/tarantool.conf.xml`
(`install-data-local` в Makefile.am делает это автоматически).

### 5. Подключите модуль в runtime-конфигурации

В `conf/vanilla/autoload_configs/pre_load_modules.conf.xml` добавьте:

```xml
<load module="mod_tarantool"/>
```

### 6. Назначьте Tarantool Core DB

В `conf/vanilla/autoload_configs/switch.conf.xml`:

```xml
<param name="core-db-dsn" value="tarantool://local"/>
```

`local` — имя профиля из `tarantool.conf.xml`. Профили можно добавлять
произвольно: DSN `tarantool://<имя>` выбирает соответствующий `<profile>`.

> Если вы уже использовали `core-db-dsn` с другим драйвером (например,
> `pgsql://host:port/...`), не забудьте выгрузить соответствующий модуль из
> `modules.conf.xml`, чтобы интерфейсы не конфликтовали за общий префикс DSN.

---

## Конфигурация (`tarantool.conf.xml`)

Файл ставится в `autoload_configs` и имеет секции:

### `<settings>` — общие параметры

| Параметр | Описание |
|---|---|
| `default-action` | `add` — автоматически инжектировать синтетический PK `<table>_uuid` для таблиц, не покрытых правилами; `none` — пропускать без изменений (Tarantool отклонит CREATE TABLE без PK) |
| `primaryKeyAddDefault-3.2` | информационно: генератор UUID для ветки 3.x. **Не используется как DEFAULT** — Tarantool не вычисляет expression-DEFAULT на INSERT, поэтому v1 инжектирует `uuid()` прямо в каждый INSERT (см. `primaryKeyAdd`) |
| `primaryKeyAddDefault-2.10` | то же для ветки 2.10 (информационно) |
| `reconnect-interval` | период попыток переподключения, мс |
| `ping-idle` | проверка простаивающих соединений через IPROTO_PING, мс (0 = выкл.) |
| `ping-timeout` | таймаут пинга, мс |
| `debug` | `true`/`false` — SQL-трассировка каждого запроса (in от ядра, out в Tarantool) по умолчанию; переключается в рантайме командой `tarantool debug` |

### `<profiles>` — пулы подключений

Каждый профиль имеет атрибут `name` (имя из DSN) , `version` (ветка
Tarantool: `2.10` или `3.2`) и `skip-index` (игнорирование index `false` / `true`) . Адрес — либо inline-параметры, либо секция
`<hosts>` для failover.

> **Максимальное число профилей — 32** (`TNT_PROFILES_MAX`). Профили сверх
> лимита игнорируются с предупреждением в лог. Массив профилей хранится на
> куче внутри `tnt_global_t`: конфиг парсится в heap-снапшот и атомарно
> подменяется под мьютексом, поэтому лимит ограничивает лишь потребление
> памяти (~3 КБ на профиль), а не размер стека.

> **Максимальное число хостов в одном `<hosts>` — 8** (`TNT_HOST_MAX`).
> Хосты сверх лимита игнорируются с предупреждением в лог. Ограничение
> связано с фиксированными массивами failover-состояния в `tnt_session_t`
> (`dead_until_host[]`, `backoff_host[]`).

Параметры профиля/host:

| Параметр | Описание |
|---|---|
| `unix-socket` | путь к unix-сокету (имеет приоритет над host:port) |
| `host` | IP/имя хоста (TCP) |
| `port` | TCP-порт |
| `user` / `password` | учётные данные (chap-sha1) |
| `connect-timeout` | таймаут connect(), мс |
| `query-timeout` | таймаут на запрос, мс |

> ⚠️ **`full-scan` и `init-sql` — ПАРАМЕТРЫ УРОВНЯ ПРОФИЛЯ** (указываются на
> `<profile>`, НЕ внутри `<host>`). Они применяются к **каждому** хосту
> профиля — и к единственному, и ко всем в `<hosts>` (в т.ч. после каждого
> failover-переподключения). Указание их в `<host>` игнорируется с warning
> в лог, чтобы опечатка не пропала молча.

Параметры уровня профиля (`<profile>`):

| Параметр | Описание |
|---|---|
| `full-scan` | `true` (по умолчанию) — на каждом новом соединении к любому хосту профиля выполняется `SET SESSION sql_full_scan=true` (Tarantool 2.11/3.x), при неизвестной переменной — фолбэк `SET SESSION sql_seq_scan=true` (старые 2.x); `false` — не включать. Без этого ядро FreeSWITCH не сможет выполнять полные сканы (реактивные probe-запросы) — каждый старт заканчивается DROP+CREATE всех таблиц |
| `init-sql` | произвольный SQL, выполняемый сразу после успешного connect к любому хосту профиля (и после каждого failover-переподключения); ошибка «неизвестная переменная» — benign, остальные логируются warning без обрыва соединения |

Атрибут профиля:

| Атрибут | Описание |
|---|---|
| `skip-index` | `true` — подавлять все `CREATE INDEX`, поступающие от ядра FreeSWITCH/mod_sofia (индексами управляете вручную, например составными для JOIN); `false` (по умолчанию) — транслировать как обычно |

Пример (одиночный хост, unix-socket):

```xml
<profile name="local" version="3.2" skip-index="false">
  <param name="unix-socket" value="/var/lib/tarantool/socket/tarantool.control"/>
  <param name="user" value="freeswitch"/>
  <param name="password" value="P@ssw0rd"/>
  <param name="connect-timeout" value="2000"/>
  <param name="query-timeout" value="3000"/>
  <param name="full-scan" value="true"/>
  <!-- <param name="init-sql" value="SET SESSION sql_full_scan=true"/> -->
</profile>
```

Пример (failover, мастер через сокет + TCP-реплика):

```xml
<profile name="local" skip-index="false">
  <!-- profile-level: применяется к КАЖДОМУ хосту в <hosts> -->
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

> ⚠️ **Важно**: в v1 `tls-profile` и секция `<tls>` — **только задел**, они
> парсятся и сохраняются в конфигурации, но **НЕ активируют шифрование**:
> соединение всегда устанавливается по незашифрованному TCP/unix-сокету.
> Рабочее шифрование появится в v1.1 (или реализуется внешним прокси,
> например stunnel, который шифрует канал до Tarantool независимо от модуля).

### `<tls>` — TLS-профили клиента (v1.1 placeholder)

Серверный TLS в Tarantool **Community Edition** 3.x недоступен
(`iproto.ssl` — Enterprise-функция), поэтому поддержка TLS клиентом
отложена. Секция парсится уже сейчас для обратной совместимости и
валидации (лимит `TNT_PROFILES_TLS_MAX`), но активное использование
начнётся в v1.1. Параметр `tls-profile` на хосте так же парсится в
поле `tls_profile` структуры хоста, но пока не влияет на соединение.

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

> **Максимальное число TLS-профилей — 16** (`TNT_PROFILES_TLS_MAX`).
> Профили сверх лимита игнорируются с предупреждением в лог.

### `<primaryKey>` / `<primaryKeyAdd>` — декларативные PK-правила

FreeSWITCH создаёт таблицы БЕЗ явного PRIMARY KEY (например,
`CREATE TABLE channels (...)` без PK) — Tarantool требует PK обязательно.
Правила транслируют CREATE TABLE до его выполнения.

- `<primaryKey table field>` — существующая колонка объявляется
  `PRIMARY KEY (field)`. Значения поставляются ядром как обычные строки,
  тип — из DDL ядра (varchar), никакого CAST: сравнения работают как есть
  (проверено probe на 3.8: тип на проводе `string`).
- `<primaryKeyAdd table field [type] [core-uuid]>` — в конец определения
  таблицы добавляется `field <type> NOT NULL, PRIMARY KEY (field)`;
  значение PK модуль инжектирует при каждом `INSERT ... VALUES (...)`.
  Tarantool НЕ применяет expression-DEFAULT вида `DEFAULT uuid()` на INSERT
  («NOT NULL constraint failed», проверено на 3.8), поэтому DEFAULT не
  выдаётся, а значение подставляется прямо в запрос.

Атрибуты `primaryKeyAdd`:

| атрибут      | значения                                                              | по умолчанию |
|--------------|-----------------------------------------------------------------------|--------------|
| `type`       | SQL-тип колонки: `varchar(36)` или `uuid`; varchar(N)/char/text/string нормализуются к STRING на проводе | `varchar(36)` |
| `core-uuid`  | `true` — core UUIDv7 инжектируется в INSERT; `false` — Tarantool-side `uuid()`; отсутствует — авто | авто |

Матрица `type × core-uuid` (effective_core_uuid = атрибут, иначе
`строковый тип ? true : false`):

| type      | core-uuid | инжекция в INSERT                          |
|-----------|-----------|--------------------------------------------|
| uuid      | `true`    | `CAST('<core-v7>' AS UUID)`                |
| uuid      | `false`   | `uuid()`                                   |
| varchar   | `true`    | литерал `'<core-v7>'` (рекомендуется: сравнения ядра — голые строки) |
| varchar   | `false`   | warning + автопереключение на core UUIDv7 (probe: `uuid()` даёт UUID, который varchar-колонка отвергает) |

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

### `<addField>` — добавление произвольных колонок

```xml
<addField>
  <!-- резолвинг глобальной переменной включается ТОЛЬКО атрибутом in-line="true" -->
  <param table="recovery" field="switch_uuid" type="varchar(36)" isNull="false" value="$${switch_uuid}" in-line="true"/>
  <!-- без in-line значение вставляется как есть (литерал) -->
  <param table="interfaces" field="switchs_uuid" type="varchar(36)" isNull="false" value="$${switchs_uuid}"/>
</addField>
```

Атрибуты `addField`:

| атрибут  | значение                                                                 | по умолчанию |
|----------|--------------------------------------------------------------------------|--------------|
| `type`   | SQL-тип колонки (передаётся как есть)                                    | `STRING`     |
| `isNull` | `true` (без NOT NULL) / `false` (добавляет `NOT NULL`)                   | `true`       |
| `default`| `DEFAULT <expr>`; выражения с `(` пропускаются (не вычисляются на INSERT)| пусто        |
| `in-line`| `true` — резолвить `$${global}`/`${var}` из окружения (switch_core_get_variable) перед INSERT; неразрешённая переменная вставляется как есть (с warning); отсутствует/`false` — значение вставляется как есть (литерал, `''`-экранирование) | пусто |
| `value`  | инжекция в каждый INSERT: при `type="uuid"` — `CAST('<value>' AS UUID)`; пусто — колонка не инжектируется | пусто |

Инжекция выполняется только для INSERT с явным списком колонок, в котором
инжектируемой колонки ещё нет; multi-row INSERT и запросы без списка
колонок проходят без изменений.

### `<reserved>` — словарь кавычения идентификаторов

Tarantool SQL не позволяет использовать зарезервированные слова как голые
идентификаторы (`where alias=...` падает с синтаксической ошибкой). Модуль
автоматически заключает такие слова в двойные кавычки во всём исходящем SQL —
и в нашем DDL, и в DML ядра (`alias` → `"alias"`). Слова `uuid` и `alias`
встроены в модуль по умолчанию; дополнительные слова добавляются в конфиг без
пересборки:

```xml
<reserved>
  <param name="uuid"/>
  <param name="alias"/>
  <param name="key"/>      <!-- пример: если ядро использует колонку key -->
</reserved>
```

Правила лексера: кавычится только идентификатор в позиции идентификатора —
строковые литералы (`'alias'`), уже закавыченные имена и вызовы функций
(`uuid(...)`) не трогаются. Кавычение всегда безопасно семантически
(`"alias"` — это та же колонка `alias`). Лимит словаря — 32 слова; при
превышении лишние слова игнорируются с предупреждением в лог.

### `<views>` — эмуляция VIEW (Tarantool их не имеет)

`show calls`, `show detailed_calls`, `show bridged_calls` и реактивные зонды
ядра обращаются к `basic_calls` / `detailed_calls` — в Tarantool таких объектов
нет, поэтому модуль переписывает каждый `SELECT ... FROM <view> ...` ядра в
`SELECT ... FROM (<тело>) AS <view> ...` (подзапрос в FROM поддерживается
Tarantool 2.x+; LEFT JOIN и `NOT IN (SELECT ...)` проверены на 3.2):

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

Правила:

- в CDATA кладётся **чистое тело SELECT** (без префикса `create view ... as`);
- тело обязано ссылаться только на реальные таблицы (обычно `channels`/`calls`);
- подставляется только первая `FROM`-секция и только для запроса вида
  `FROM <name>` без алиаса (именно так обращается ядро); `JOIN <view>` /
  вложенные SELECT не расширяются;
- итоговый SQL проходит тот же рерайтер кавычения зарезервированных слов
  (`uuid` → `"uuid"`), что и остальные запросы;
- без секции `<views>` поведение не меняется (SELECT уходит как есть);
- тело **валидируется при загрузке конфига** (без обращения к серверу):
  проверяется, что это ровно один корректный SELECT (разбивка по `;`,
  баланс кавычек/скобок, распознаваемые ключевые слова). Невалидное тело
  НЕ применяется — в консоль WARNING
  `mod_tarantool: <views> 'имя': invalid SQL, view NOT applied: <причина>`.

Лимиты (лимиты не обрезаются, а отбрасываются):

| Условие | Поведение |
|---|---|
| тело view > 4096 байт | **жёсткая ошибка**: конфиг отклоняется целиком — при `load` модуль не загружается (в консоль CRIT: имя view, фактический размер, лимит); при `tarantool reload` новый конфиг не применяется (остаётся старый рабочий, CRIT в консоль, команда возвращает `reload failed ... old config kept`) |
| view больше 8 | не фатально: лишние view игнорируются, в консоль WARNING «view limit reached» |
| пустое имя/тело | WARNING «empty name or body», entry игнорируется |
| невалидное тело (не один SELECT, битые кавычки/скобки, нераспознанный SQL) | WARNING «invalid SQL, view NOT applied: <причина>», view игнорируется |

Правило: лучше жёстко отказаться от битой конфигурации, чем молча потерять
view из-за усечения — после исправления размера выполните `load`/`reload`
повторно.

### `<maintenance>` — периодическая очистка SQL

Задача: mod_sofia пишет BLF/presence-статусы в `sip_presence`/
`sip_subscriptions`; штатная чистка по TTL (`sofia_reg_check_expire`) срабатывает
редко, и при большом сроке жизни записи копятся, а presence-запросы Tarantool
(полные сканы) дорожают. Секция запускает фоновый поток модуля, который
периодически выполняет заданные DELETE:

```xml
<maintenance interval="120" enable="true">
  <sql profile="local"><![CDATA[
    delete from sip_presence where expires > 0 and expires <= ${now};
    delete from sip_subscriptions where expires > 0 and expires <= ${now}
  ]]></sql>
  <sql profile="local"><![CDATA[delete from sip_registrations where expires > 0 and expires <= ${now}]]></sql>
</maintenance>
```

Правила:

- **`enable`** — по умолчанию `false`. Секция активна ТОЛЬКО если
  `enable="true"` И `interval > 0` И есть хотя бы один `<sql>` с
  **существующим профилем**; иначе — WARNING в консоль и секция не работает;
- **`interval`** — секунды, принудительно ограничивается [60, 3600] (clamp с
  WARNING); `0` = выключено;
- каждый `<sql profile="...">` выполняется на своём профиле; записи одного
  профиля группируются в одну сессию за итерацию; порядок сохраняется;
- в одном `<sql>` можно перечислять несколько запросов через `;` — они
  разбиваются (`tnt_sql_split`) и выполняются поочерёдно в той же сессии;
- **`${now}`** заменяется на текущий unixtime (Tarantool SQL не имеет
  `CURRENT_TIMESTAMP`);
- поток работает только пока загружен `mod_sofia`
  (`switch_loadable_module_exists`), иначе итерации пропускаются;
- недоступные хосты профиля → WARNING + пропуск группы (без падения);
- ошибка отдельного statement — WARNING (текст обрезан до 1 КБ), остальные
  продолжают выполняться;
- тело каждого `<sql>` **валидируется при загрузке конфига** (без сервера):
  пустота, число statement'ов (≤ 32), баланс кавычек/скобок и распознаваемые
  ключевые слова; невалидная запись НЕ применяется — в консоль WARNING
  `mod_tarantool: <maintenance> <sql> (profile 'имя'): invalid SQL, entry NOT applied: <причина>`;
- лимиты: ≤ 16 записей `<sql>`, тело ≤ 4 КБ — превышение отбрасывается
  (не обрезается) с WARNING;
- поток стартует в `mod_tarantool_load`, останавливается в `mod_tarantool_shutdown`;
  изменения `enable`/`interval`/списка подхватываются со следующей итерации
  (`tarantool reload` без перезагрузки модуля).

### Прочие правила трансляции DDL

- `BIGINT` → `INTEGER` (в Tarantool только INTEGER);
- `CREATE VIEW` → no-op (логируется вызывающей стороной);
- `IF NOT EXISTS` / `IF EXISTS` → удаляются (Tarantool их отвергает);
- `CREATE INDEX` → приводится к виду с `IF NOT EXISTS`;
- при `skip-index="true"` на профиле — все `CREATE INDEX` от ядра/mod_sofia
  подавляются полностью (журналируются как info), индексами нужно управлять
  вручную (например, составными для JOIN-запросов); PK-инъекция
  (`primaryKey`/`primaryKeyAdd`) при этом продолжает работать;
- «benign»-ошибки (`already exists`, `duplicate key`) маппятся в успех;
  `Space X does not exist` — только для идемпотентных батчей из
  `DROP`/`CREATE INDEX`; для данных (SELECT/DELETE/INSERT/UPDATE) она
  обязательно доходит до ядра, чтобы сработало реактивное создание схемы
  (`switch_cache_db_test_reactive`);
- зарезервированные слова Tarantool в позиции идентификатора
  (`uuid`, `alias`) автоматически кавычятся (`"uuid"`, `"alias"`) во всём
  исходящем SQL — и в нашем DDL, и в DML ядра. Список расширяется секцией
  `<reserved>` без пересборки модуля (см. ниже);
- function-DEFAULT (`DEFAULT uuid()`, `DEFAULT uuid7()`) не выдаётся в DDL:
  Tarantool 3.x его парсит, но не вычисляет при INSERT.

---

## Подготовка Tarantool

Минимальный пример серверной части (инстанс master):

```lua
box.cfg({
    listen = {'127.0.0.1:3301', '/var/lib/tarantool/socket/tarantool.control'},
    memtx_dir = '/var/lib/tarantool/socket',
    wal_dir   = '/var/lib/tarantool/socket',
    read_only = false,
})
-- Auth user (as before)
local ok, err = pcall(function()
    box.schema.user.create('freeswitch', {password = 'P@ssw0rd', if_not_exists = true})
    box.schema.user.grant('freeswitch', 'read,write,execute,create,alter,drop', 'universe', nil, {if_not_exists = true})
end)
if not ok then error(err, 2) end
```

SQL включается в Tarantool 3.x по умолчанию; в 2.10 — `box.cfg({sql_cache_size = ...})`.

> Репликация master→replica настраивается штатно (`box.cfg.replication`).
> Модуль v1 переключается на другой хост только при сетевом сбое; роль
> (`master`/`replica`/`any`) — задел v1.1.

### Права на unix-сокет

Для `connect()` к AF_UNIX требуются два права: **запись** (`w`) на сам inode
сокета и **проход** (`x`) через все родительские каталоги. Пользователь
FreeSWITCH (обычно `freeswitch`/`xfreeswitch`) по умолчанию не имеет ни того,
ни другого:

- каталог `/run/tarantool` создаёт systemd (`RuntimeDirectory=`) как
  `drwxr-x--- tarantool tarantool` — непроходим для остальных;
- сам сокет инстанс создаёт как `srwxr-xr-x` — без `w` для группы/остальных,
  поэтому одного добавления пользователя в группу `tarantool` недостаточно.

Готовый drop-in `/etc/systemd/system/tarantool.service.d/access.conf`
(имя юнита уточните через `systemctl cat tarantool | head -1`):

```ini
[Service]
# /run/tarantool создаётся RuntimeDirectory до ExecStartPre: разрешить проход всем
ExecStartPre=/usr/bin/chmod 755 /run/tarantool
# сокет создаёт сам tarantool после старта — дождаться его и выдать connect()-право
ExecStartPost=/usr/bin/sh -c 'for i in $(seq 1 50); do [ -S /run/tarantool/tarantool-socket.control ] && break; sleep 0.1; done; chmod o+w /run/tarantool/tarantool-socket.control'
```

Затем:

```bash
sudo systemctl daemon-reload
sudo systemctl restart tarantool
ls -l /run/tarantool/        # drwxr-xr-x ... ; srwxrwxrwx ...tarantool-socket.control
```

Права сбрасываются при каждом рестарте (каталог пересоздаёт systemd, сокет —
инстанс), поэтому постоянные `ExecStartPre`/`ExecStartPost` обязательны.
Альтернатива без возни с правами — подключение по TCP: замените `unix-socket`
на `host`/`port` в профиле `tarantool.conf.xml`.

---

## CLI API

Команда доступна в fs_cli:

```
tarantool help
tarantool status              # статус профилей
tarantool list                # список профилей
tarantool stats               # счётчики
tarantool debug on|off [profile]  # SQL-трассировка запросов in/out
tarantool conns [profile]     # активные соединения и их инициаторы
tarantool ping [profile]      # ping пула — первый доступный хост (по умолчанию local)
tarantool check [profile]     # ping каждого хоста профиля по отдельности
tarantool sql <profile> <q>   # выполнить сырой SQL-запрос
tarantool translate <q>       # показать результат трансляции DDL
tarantool reload              # перечитать tarantool.conf.xml
tarantool drop                # освобождение ресурсов (no-op в v1)
tarantool version             # версия модуля
```

Примеры:

```
tarantool ping local
tarantool check local
tarantool sql local "select * from channels limit 1"
tarantool translate "create table channels (uuid varchar(255), name varchar(255))"
```

Пример вывода `check` для профиля с двумя хостами:

```
  host[0] unix:/var/lib/tarantool/socket/tarantool.control:3301 v3.2: UP
  host[1] 127.0.0.1:3302 v3.2: DOWN (connect failed)
local: 1/2 hosts up
```

Пример вывода `conns` (активные соединения с инициаторами):

```
[0] profile=local fd=23 unix:/var/lib/tarantool/socket/tarantool.control:3301 v3.2 cur=0 queries=842
    caller=sofia_reg.c:2578 sofia_reg_handle_sip_r_register idle=12ms auto_commit=true last_affected=1 age=3600s
[1] profile=local fd=24 unix:/var/lib/tarantool/socket/tarantool.control:3301 v3.2 cur=0 queries=57
    caller=switch_core_sqldb.c:2829 switch_core_db_handle_new idle=800ms auto_commit=true last_affected=-1 age=3600s
2 connection(s), 2 up
```

> `conns` показывает до **256** отслеживаемых соединений (`TNT_HANDLES_MAX`).
> Это диагностический реестр, а не лимит подключений: реальное число
> соединений ограничивает пул `switch_cache_db` ядра FreeSWITCH. При
> переполнении реестра новые соединения работают штатно, но не
> отображаются в `conns` (в лог пишется предупреждение).

Пример трассировки `debug on local` (в freeswitch.log):

```
mod_tarantool[debug] profile=local in sofia_reg.c:2578 sofia_reg_handle_sip_r_register
  sql: INSERT INTO sip_registrations (...) VALUES (...);
mod_tarantool[debug] profile=local out sofia_reg.c:2578 sofia_reg_handle_sip_r_register
  sql: INSERT INTO sip_registrations (...) VALUES (...);
```

---

## Особенности протокола, учтённые в реализации

- **Двухобъектные ответы IPROTO 3.x**: запрос и ответ — два отдельных
  msgpack-объекта (header-map `{0x00: type, 0x01: sync, 0x05: schema_version}`
  + body-map `{0x30: DATA, 0x32: METADATA, 0x42: SQL_INFO, 0x71: error-stack}`).
- **`IPROTO_METADATA = 0x32` в 3.x** (в <3.x был `0x52`, теперь `0x52` —
  ERROR-map). Именно поэтому необходима версия `2.10`/`3.2` в конфиге.
- **AUTH chap-sha1**: TUPLE = `[method, scramble]`; scramble =
  `SHA1(salt || SHA1(SHA1(pass))) XOR SHA1(pass)`; соль в грейтинге 3.x
  берётся из первых 27 base64-символов + дополняющий `=` (20 байт).
- **NULL → `""`** при заполнении результатов (как ожидает ядро FreeSWITCH).
- **Ошибки**: текст извлекается из error-stack (`0x71`, поле `0x03`).
- Имена колонок копируются глубоко (нельзя хранить указатели в буфер ответа).
- **MP_EXT типы** декодируются в строки: `uuid` (ext 0x03, 16 байт →
  `8-4-4-4-12`), `datetime`/`date` (ext 0x02: scale/tzoffset/epoch/nsec →
  `YYYY-MM-DDTHH:MM:SS[.fff]±HHMM`, смещение без двоеточия). `decimal`
  (0x00) и `interval` (0x04) до пиннинга wire-layout возвращаются как hex
  с warning; неизвестный ext — `(ext<type>)` без падения запроса.
- **Диалект INTERVAL**: литерал пишется map-синтаксисом
  `CAST({'day': 1} AS INTERVAL)` (ключи `year/month/day/hour/minute/second`),
  стандартного `INTERVAL '1' DAY` нет.
- **Full table scan запрещён по умолчанию**: `SELECT * FROM t` падает
  (`Scanning is not allowed`); нужен `FROM SEQSCAN t`. Модуль включает
  `SET SESSION sql_full_scan=true` на соединение (см. параметр full-scan).

---

## Лицензия и автор

**Лицензия**: Mozilla Public License 2.0.

**Автор**: Filippov Anatoliy <error.email@mail.ru>

Модуль является производным от FreeSWITCH
(FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application)
и использует его API (`switch_database_interface_t` из
`src/include/switch_module_interfaces.h`), но не содержит кода других
драйверов баз данных.
