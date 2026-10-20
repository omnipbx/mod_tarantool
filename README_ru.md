# mod_tarantool

**🌐 [English](README.md) | [Русский](README_ru.md)**

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

## Возможности (v1.0.13)

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
- **Транзакционность батчей**: DML-батчи выполняются атомарно
  через IPROTO streams — отдельные пакеты `IPROTO_EXECUTE` с общим
  ненулевым `IPROTO_STREAM_ID` (сервер привязывает их к одному файберу,
  транзакция `BEGIN; stmt1; …; COMMIT;` становится сквозной; при разрыве
  связи сервер откатывает её сам, при таймауте модуль шлёт явный
  `ROLLBACK` в том же стриме). DDL-батчи остаются вне стрима — DDL в
  Tarantool не транзакционен. Требование сервера: Tarantool ≥2.11
  (у 3.x — есть). См. «Особенности протокола, учтённые в реализации».
- **Мультипоточность**: соединение на (поток × callsite); каждый
  `handle_new` создаёт глубокую копию профиля+правил, поэтому reload
  конфигурации не рвёт активные соединения.
- **Пул соединений** (`pool-size`, v1.0.5): каждый handle держит до 8
  независимых соединений (round-robin, per-conn мьютекс вместо глобальной
  сериализации, транзакции на закреплённом соединении, immutable-снапшот
  правил для параллельной трансляции). `pool-size=1` — классическое
  поведение.
- **Shared circuit breaker** (`cb-mode`, v1.0.6): per-host автомат
  `CLOSED → OPEN → HALF_OPEN` в глобальном реестре (ключ `addr:port`),
  общий для всех профилей/пулов. Режимы `off` (классический backoff),
  `wait` (запросы спят до конца окна — безопасно для
  `switch_cache_db_test_reactive`) и `fast` (мгновенный отказ). Порог
  отказов, окно OPEN, метрики и CLI `tarantool cb show|reset`.
- **Материализация SELECT-литералов** (`literal`, v1.0.7): Tarantool SQL не
  умеет типизировать анонимные `'?'` в SELECT-списке (`PREPARE` падает с
  «Supplied key type of part N …»). Записи кэша, у которых в SELECT-блоке
  есть литералы, помечаются флагом `literal` (вычисляется один раз при
  записи); перед `IPROTO_PREPARE` такие литералы материализуются в
  константы, а их значения изымаются из bind-массива. Ключ кэша остаётся
  стабильным (маркеры везде) — раздувания `cache` нет.
- **SEQSCAN-инъекция** (`full-scan`, v1.0.8): форк Tarantool 3.8 не знает
  сессионную переменную `sql_full_scan` (`Session setting sql_full_scan
  doesn't exist`), поэтому `SET SESSION` молча не срабатывает и полные
  сканы запрещены. При `full-scan="true"` на профиле модуль вставляет
  `SEQSCAN` перед именами таблиц в `FROM`/`JOIN` (в т.ч. в подзапросах
  DML: `DELETE ... IN (SELECT ... FROM t1 JOIN t2 ...)`) — на всех путях
  исполнения (ядро, `<maintenance>`, `tarantool sql`) и в `tarantool
  translate`. `DELETE FROM t` без подзапроса не трогается, уже
  присутствующий `SEQSCAN` не дублируется.
- **Валидация API-команд** (`translate`/`sql`, v1.0.9): перед обработкой
  запрос проходит резолв переменных `${now}`/`${epoch}`/`${hostname}`/
  `${switchname}`/`${strftime(<fmt>)}` (тот же набор, что в
  `<maintenance>`) и лёгкую проверку синтаксиса (`tnt_sql_validate`:
  сплит на `;`, баланс скобок, незакрытые строки/комментарии,
  распознавание ключевого слова). Неизвестная переменная или невалидный
  SQL отклоняются с человекочитаемой причиной в консоли — без
  round-trip к серверу (соединения может не быть при старте/обрыве).
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
│                          #   (вкл. api_resolve_validate: резолв ${...} +
│   │                       #   валидация синтаксиса для sql/translate)
├── tnt_client.c/.h        # IPROTO-клиент: connect/AUTH/EXECUTE/PING,
│                          #   таймауты, failover, backoff, парсер ответов
├── tnt_sql.c/.h           # сплиттер SQL по top-level ';' и классификатор
│                          #   операторов (CREATE/DROP/INSERT/UPDATE/...),
│                          #   tnt_sql_validate (лёгкая валидация синтаксиса),
│                          #   tnt_sql_inject_seqscan (FROM SEQSCAN t)
├── tnt_rules.c/.h         # транслятор DDL (primaryKey/primaryKeyAdd/
│                          #   addField, BIGINT→INTEGER, VIEW no-op и т.д.)
├── tnt_stats.c/.h         # runtime SQL-статистика ("tarantool sqlstats"),
│                          #   чистый слой без switch_* (фильтр таблиц,
│                          #   нормализация литералов, FNV-хэш шаблонов)
├── tnt_tcache.c/.h        # динамический кэш трансляции ("tarantool cache"),
│                          #   чистый слой без switch_* (fetch/store/show,
│                          #   eviction least-hits, приватные копии на hit)
├── circuit_breaker.c/.h   # shared circuit breaker (per-host машина состояний
│                          #   CLOSED→OPEN→HALF_OPEN, режимы off/wait/fast)
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

#### Ручная сборка на уже-скомпилированном дереве FreeSWITCH

**Важно:** модуль состоит из нескольких единиц трансляции
(`tnt_client.c`, `tnt_rules.c`, `tnt_sql.c`, `tnt_stats.c`, `tnt_tcache.c`,
`circuit_breaker.c`, `mp_decode.c` + `mod_tarantool.c`). Штатный
`modmake.rules` FreeSWITCH линкует модуль только из `$(MODNAME).lo` +
`$(LOCAL_OBJS)`: если просто скопировать модуль в **уже-сконфигурированное**
дерево и выполнить `make mod_tarantool`, скомпилируется только
`mod_tarantool.c`, а все `tnt_*`-хелперы не попадут в `.so`, что приведёт к
ошибке при загрузке:

```
Error Loading module /usr/lib/freeswitch/mod/mod_tarantool.so:
  undefined symbol: tnt_result_free
```

Причина двойная: в Makefile модуля переменная `LOCAL_OBJS` должна объявляться
**до** `include modmake.rules` (GNU make раскрывает примитивы в момент
парсинга), а суффиксное правило `.c.lo` обходится рекурсивным вызовом make
FreeSWITCH.

**Решение — используйте прилагаемый `build.sh` как пред-make шаг**
(автоопределяет корень FreeSWITCH, перегенерирует Makefile модуля с
`LOCAL_OBJS` до include и явными правилами `.lo` для каждого хелпера):

```bash
cd <freeSWITCH>                       # из корня исходников
./src/mod/databases/mod_tarantool/build.sh   # пред-make: чинит Makefile
make mod_tarantool                    # соберёт ВСЕ исходники в .so
make mod_tarantool-install            # установит модуль
```

Примечания:

- на тулчейнах с gcc >= 8 сборка дополнительно использует
  `-Wno-error=format-truncation` (иначе length-guarded `snprintf()` на сокетах
  может сломать сборку через `-Werror`);
- `build.sh` идемпотентен — безопасно повторять после любых правок исходников;
- для `dpkg-buildpackage` этот шаг **не нужен**: чистая сборка из tar
  перегенерирует Makefile из полного `Makefile.am` (полный
  `mod_tarantool_la_SOURCES`), поэтому все единицы трансляции линкуются и там;
- проверка после сборки: `nm -D .libs/mod_tarantool.so | grep tnt_result_free`
  должен показать `T tnt_result_free`.

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
> `pre_load_modules.conf.xml`, чтобы интерфейсы не конфликтовали за общий префикс DSN.

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
| `cache` | `false` (по умолчанию)/`true` — кэш трансляции statement'ов по **нормализованному шаблону** (см. `tarantool cache`): литералы данных заменяются маркерами, ключ устойчив к разным значениям; на hit литералы конкретного вызова подставляются обратно. `false` — обход кэша и очистка (kill-switch); применяется hot через `reload` и командой `tarantool cache on/off` |
| `cb-mode` | `off` (по умолчанию)/`wait`/`fast` — режим shared circuit breaker (см. раздел «Circuit breaker»). `wait` — при OPEN запросы спят до конца окна (безопасно для ядра FreeSWITCH); `fast` — мгновенный отказ; `off` — классический per-conn backoff |
| `cb-failure-threshold` | число подряд идущих транспортных отказов до открытия breaker (по умолчанию `3`) |
| `cb-timeout` | длина окна OPEN, мс (по умолчанию `5000`) |

### `<profiles>` — пулы подключений

Каждый профиль имеет атрибут `name` (имя из DSN) , `version` (ветка
Tarantool: `2.10` или `3.2`) и `skip-index` (игнорирование index `false` / `true`) . Адрес — либо inline-параметры, либо секция
`<hosts>` для failover.

Профильный параметр `<param name="prepare"/>` — **default для всех хостов
профиля**; хост может переопределить его своим `<param name="prepare"/>`
внутри `<host>` (наследование профиль → хост). Подробности — в разделе
[`prepare` — подготовленные statements](#prepare--подготовленные-statements-iproto_prepare--bind).

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
| `full-scan` | `true` (по умолчанию) — на каждом новом соединении к любому хосту профиля выполняется `SET SESSION sql_full_scan=true` (Tarantool 2.11/3.x), при неизвестной переменной — фолбэк `SET SESSION sql_seq_scan=true` (старые 2.x); `false` — не включать. **v1.0.8**: при `true` модуль дополнительно вставляет `SEQSCAN` перед таблицами в `FROM`/`JOIN` (в т.ч. в подзапросах DML) на всех путях исполнения — это единственный надёжный способ на форках, не знающих `sql_full_scan` (node01: `Session setting sql_full_scan doesn't exist`). Без этого ядро FreeSWITCH не сможет выполнять полные сканы (реактивные probe-запросы) — каждый старт заканчивается DROP+CREATE всех таблиц |
| `init-sql` | произвольный SQL, выполняемый сразу после успешного connect к любому хосту профиля (и после каждого failover-переподключения); ошибка «неизвестная переменная» — benign, остальные логируются warning без обрыва соединения |
| `pool-size` | размер пула соединений на один handle (1..8, по умолчанию 1). Каждый handle держит до `pool-size` независимых соединений (у каждого — свой failover-цикл и свой кэш prepared statements) и распределяет statement'ы round-robin. `1` = классическое «один handle — одно соединение»; `>1` позволяет параллельно выполнять statement'ы на разных соединениях (см. раздел [«Пул соединений»](#пул-соединений-pool-size)) |

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
  <!-- <param name="pool-size" value="4"/> -->
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

### `<intColumns>` — числовые колонки со строковыми значениями

FreeSWITCH форматирует часть целочисленных полей **в кавычках**: например,
`created_epoch` уходит в SQL как `'1791322069'` (формат `'%ld'` в
[`switch_core_sqldb.c`](../src/switch_core_sqldb.c:2478)). Tarantool не делает
неявную конвертацию string→integer при INSERT/UPDATE, и запись падает с
`Type mismatch: can not convert string('...') to integer`. Секция объявляет,
какие поля могут приходить в таком виде:

```xml
<intColumns>
  <param table="channels" field="created_epoch" type="INTEGER"/>
  <param table="calls"    field="call_created_epoch" type="INTEGER"/>
</intColumns>
```

Правила:

- на INSERT/UPDATE значение-строковый литерал `'…'` для объявленной колонки
  переписывается в `CAST('…' AS <тип>)`; пустая строка `''` → `NULL`;
- голые числа (`expires = 1700000000`), `NULL`, `uuid()` и выражения не трогаются;
- строковые колонки никогда не затрагиваются (справочник «таблица/поле»);
- тип проверяется при `load`/`tarantool reload`: незнакомое имя → WARNING в
  консоль и запись игнорируется; допустимые типы:
  `INTEGER|INT|BIGINT|SMALLINT|TINYINT|MEDIUMINT` → `INTEGER`,
  `NUMBER` → `NUMBER`, `DOUBLE|REAL|FLOAT` → `DOUBLE`,
  `DECIMAL|NUMERIC` → `DECIMAL`;
- лимит: 64 записи (`TNT_INTCOL_MAX`), превышение — WARNING + игнор;
  дубликаты «таблица+поле» дедуплицируются.

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
| в теле найден placeholder (`${…}` / `$${…}`) | WARNING «placeholder '…' is not allowed in view bodies», view игнорируется — динамика в статической подстановке VIEW замёрзла бы на момент загрузки |

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
- **динамические переменные** раскрываются на каждой итерации (Tarantool SQL
  не имеет `CURRENT_TIMESTAMP`, поэтому время всегда подставляется свежим):
  | Переменная | Значение |
  |---|---|
  | `${now}` / `${epoch}` | `(long)` времени итерации (unixtime) |
  | `${hostname}` / `${switchname}` | имя узла (`switch_core_get_switchname()`) |
  | `${strftime(<fmt>)}` | текущее время по формату, например `${strftime(%H%M)}` |
  Значения подставляются только **вне** строковых литералов `'…'`. Неизвестная
  переменная → SQL считается битым: WARNING с текстом SQL и самим токеном
  (`unknown maintenance variable '${…}'`), запись НЕ применяется на
  load/reload и пропускается на итерации. Напоминание: `$${…}` (глобалы FS)
  раскрывает препроцессор ядра ещё при чтении конфига — неизвестный глобал
  ядро удаляет молча, получившийся битый SQL ловит валидатор;
- поток выполняется **независимо от других модулей** (в т.ч. `mod_sofia`):
  модуль — generic-драйвер и не знает о существовании endpoint-модулей;
  если таблицы ещё не созданы (например, sofia не загружен), statement
  завершится ошибкой Tarantool → WARNING, остальные продолжаются;
  фактическое состояние видно в `tarantool maintenance show` (runs/last run);
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

### `tarantool reload` — что применяется, а что нет

`reload` полностью перечитывает `tarantool.conf.xml`; при любой ошибке
(включая незнакомые типы в `<intColumns>`) конфиг **не применяется вообще** —
остаётся старый рабочий, в консоль пишется CRIT/WARNING. Далее:

**Применяется к АКТИВНЫМ соединениям сразу (hot-reload):**

- все секции правил — `<reserved>`, `<views>`, `<primaryKey>`/`<primaryKeyAdd>`,
  `<addField>`, `<intColumns>`. Они лежат в одной структуре `tnt_rules_t`;
  reload инкрементирует `rules_epoch`, а каждое активное соединение обновляет
  правила **лениво** — при первом же SQL-вызове, увидев чужой epoch (копия
  ~40 КБ один раз на хендл за reload, под `glob->mutex` и локом хендла).
  Кэш трансляции при этом очищается целиком. В лог:
  `rules epoch -> N (handles refresh lazily)`.
- `<param name="cache"/>` — горячо: новое значение из конфига применяется к
  кэшу (включение/выключение + очистка).

**Применяется на лету при `reload`:**

- `<maintenance>` (`enable`/`interval`/`<sql>`) — `reload` будит фоновый поток
  (флаг wake-up), тот немедленно перечитывает снапшот под мьютексом и запускает
  итерацию с новыми параметрами (латентность ≤ 250 мс); `${now}` всегда свежий
  на момент выполнения. Полная остановка/перезапуск потока не нужны — он
  запущен всегда и просто «простаивает», пока секция выключена;
- `<settings>` `default-action`, тайминги `ping-idle`/`ping-timeout` —
  со следующего использования/подключения.

**НЕ применяется к активным соединениям (только к новым):**

- `<profiles>` (`<profile>`/`<hosts>`: адреса, порты, учётные данные, таймауты
  подключения, `skip-index`, `full-scan`, `init-sql`) и `<tls>` — соединение
  уже открыто со своей копией профиля; чтобы применить, нужен перезапуск
  FreeSWITCH (или дождаться, пока пул создаст новые хендлы);
- `debug` управляется отдельно и мгновенно: `tarantool debug on|off [profile]`.

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
    -- MVCC для memtx: параллельный DML из нескольких соединений FreeSWITCH
    -- (channels/calls/sip_registrations) без взаимных блокировок; обязателен
    -- при нагрузке выше единиц CPS и для будущих транзакционных батчей
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
tarantool translate [<profile>] <q>  # показать результат трансляции DDL
                                     # (профиль опционально: если первый
                                     #  токен совпадает с именем профиля;
                                     #  иначе — первый профиль в конфиге)
tarantool sqlstats on|off|reset|tables|show   # статистика SQL-шаблонов
tarantool cache on|off|reset|show [N] [full]  # кэш трансляции
tarantool prepare on|off|reset|show           # подготовленные statements
tarantool cb show|reset                       # shared circuit breaker
tarantool maintenance show|restart            # шедулер <maintenance>:
                                              #   show — состояние и записи,
                                              #   restart — немедленный тик
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
tarantool translate local "delete from sip_subscriptions where sip_subscriptions_uuid in (select s.sip_subscriptions_uuid from sip_subscriptions s left join sip_registrations r on s.sip_user = r.sip_user where r.sip_user is null)"
```

### Валидация `sql`/`translate` (v1.0.9)

Обе команды перед обработкой прогоняют запрос через `api_resolve_validate`:

1. **Резолв переменных** — тот же набор, что в `<maintenance>`:
   `${now}`, `${epoch}`, `${hostname}`, `${switchname}`, `${strftime(<fmt>)}`.
   Неизвестная переменная отклоняется:
   ```
   > tarantool translate "select * from t where ts > ${unknown}"
   translate error: unknown maintenance variable 'unknown'
   ```
2. **Лёгкая проверка синтаксиса** (`tnt_sql_validate`): сплит на top-level
   `;`, баланс скобок, незакрытые строки/комментарии, распознавание
   ключевого слова. Невалидный SQL отклоняется без round-trip к серверу
   (соединения может не быть при старте/обрыве):
   ```
   > tarantool sql local "SELCT * FRM t"
   sql error: statement starts with unrecognized keyword 'selct'
   ```

Резолв выполняется с sample-time (`switch_epoch_time_now`), поэтому
`${now}` в `translate`/`sql` подставляется текущим unix-временем.

Пример вывода `check` для профиля с двумя хостами:

```
  host[0] unix:/var/lib/tarantool/socket/tarantool.control:3301 v3.2: UP
  host[1] 127.0.0.1:3302 v3.2: DOWN (connect failed)
local: 1/2 hosts up
```

Пример вывода `conns` (активные соединения с инициаторами; при
`pool-size > 1` каждый handle показывает все свои соединения пула,
`conn=N/M` — номер соединения и размер пула, `[tx]` — соединение,
закреплённое за текущей транзакцией):

```
[0] profile=local conn=0/1 fd=23 unix:/var/lib/tarantool/socket/tarantool.control:3301 v3.2 cur=0 queries=842
    caller=sofia_reg.c:2578 sofia_reg_handle_sip_r_register idle=12ms auto_commit=true last_affected=1 age=36s
[1] profile=local conn=0/1 fd=24 unix:/var/lib/tarantool/socket/tarantool.control:3301 v3.2 cur=0 queries=57
    caller=switch_core_sqldb.c:2829 switch_core_db_handle_new idle=800ms auto_commit=true last_affected=-1 age=1d 02:03:04
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

### `sqlstats` — статистика SQL-шаблонов в runtime

Семейство команд `tarantool sqlstats ...` считает, какая доля SQL-потока
повторяется (по нормализованным шаблонам) — основа для оценки кэша
трансляции и `IPROTO_PREPARE + bind`. Исходные SQL-строки с данными не
хранятся: в памяти остаются только нормализованные сэмплы (литералы
заменены на `?`), PII в вывод не попадает.

```
tarantool sqlstats on                     # начать накопление
tarantool sqlstats off                    # остановить (данные сохраняются)
tarantool sqlstats reset                  # счётчики=0 И фильтр=all (on/off не трогает)
tarantool sqlstats tables                 # показать текущий фильтр
tarantool sqlstats tables t1 t2 ...       # ЗАМЕНИТЬ фильтр списком таблиц
tarantool sqlstats tables all             # фильтр = все таблицы
tarantool sqlstats show [N] [table]       # сводка + топ-N шаблонов
```

Правила фильтра таблиц:

- **replace-семантика**: каждый вызов `tables` полностью заменяет список;
- дубликаты сворачиваются молча, имена приводятся к нижнему регистру,
  сравнение регистронезависимо (`Channels` ≡ `channels`);
- лимит **32** имени (лишние игнорируются с WARNING в выводе команды);
- view-имена поддерживаются без отдельного синтаксиса: для `SELECT`
  учитывается первое имя после верхнеуровневого `FROM` (имя view — как
  есть, `(...)` → `*subquery*`, второй источник → суффикс ` (join)`);
- проверка фильтра выполняется **до** нормализации/хэширования: таблица
  вне списка стоит только скан имени, не хэш.

Пока `sqlstats off` — нулевой overhead: ничего не считается и не
хэшируется. При `on` overhead — один проход по statement (нормализация +
FNV-1a-64 хэш), при off-нагрузке для диагностики это несущественно.

Пример вывода `show`:

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

Интерпретация: отношение `total / distinct` на горячих таблицах — прямой
прогноз выигрыша кэша трансляции (уровень модуля) и количества
prepared-шаблонов при переходе на `IPROTO_PREPARE + bind` (уровень
сервера). Отдельные литералы не раздувают distinct: нормализация
маскирует строки (`'...'` → `?`, включая `''`), bare-числа (`123` → `?`),
вырезает комментарии, приводит ключевые слова к нижнему регистру и
схлопывает пробелы.

Ограничения: 4096 шаблонов / 128 пар (тип × таблица) — сверх лимита
переполнение суммируется в `overflow`-счётчики; статистика живёт в
runtime и переживает `reload`, но сбрасывается при перезапуске модуля.

### `cache` — кэш трансляции statement'ов (по нормализованному шаблону)

Динамический, самонаполняющийся кэш с ключом-**шаблоном**: литералы данных
(`'...'`, bare-числа) заменяются маркером, текст приводится к нижнему
регистру и схлопнутым пробелам — ключ устойчив к разным значениям одного
и того же statement (первый замер с raw-ключом показал hit-rate 4.19%:
литералы uuid/статусы/таймстемпы почти всегда уникальны; нормализованный
ключ — ~99% по данным `sqlstats`). В кэше хранится translated+quoted
**шаблон**; на hit литералы конкретного вызова извлекаются из входа
(`tnt_sql_templateize`) и подставляются в шаблон (`tnt_sql_rebuild`).

Инвалидация — целиком на каждый `reload` правил (epoch-механизм, хендлы
обновляются лениво). Statement'ы, чей rewrite зависит от значений
литералов (intColumns CAST, `''`→NULL), literal core-UUID в pkadd-инъекции,
`$${var}`-резолв — **не кэшируются** (`tnt_translate_ctx_t.ctx_dep`) и
идут raw-путём. Safety-net: несовпадение числа маркеров после трансляции
→ statement исполняется raw-путём и не сохраняется. Ограничения: 512
записей (eviction least-hits), ≤ 8 КБ на statement, ≤ 32 литералов по
512 байт.

**Кэшируются только DML/SELECT** (`SELECT`/`INSERT`/`UPDATE`/`DELETE`) —
тот же критерий, что и для prepared statements. Стартовые DDL
(`CREATE/DROP/ALTER`, ~70+ одноразовых шаблонов за прогрев sofia) не
попадают в кэш: они исполняются один раз за жизнь процесса, и их записи с
`hits=1` были бы балластом, портящим `cache show` и hit-rate. Это же
критично для подготовленных statements (`box.prepare` не принимает DDL).

```
tarantool cache on | off | reset
tarantool cache show [N] [full]   # сводка + топ-N по hits; full — весь текст
```

- `off` — обход кэша + очистка (kill-switch, применяется немедленно);
- `reset` — очистка записей и счётчиков hits/misses;
- параметр `<param name="cache"/>` в `<settings>` задаёт постоянную
  конфигурацию (default `true`), применяется и через `reload`.

Пример вывода `show`:

```
cache: enabled (limit 512, used 69)
hit-rate: 8544/8613 = 99.20%
misses since reset: 69
total hits over entries: 8544
top entries (by hits, limit 20):
  1. hits=2848     len=214    age=112s 14:03:21
      select call_id,sip_user,... from sip_registrations where force_ping= '0' ...
```

### `prepare` — подготовленные statements (IPROTO_PREPARE + bind)

Этап 2 оптимизации. Кэшируемые шаблоны SELECT/INSERT/UPDATE/DELETE
исполняются через `box.prepare`-эквивалент протокола: translated-шаблон с
маркерами `\001` превращается в SQL с позиционными `'?'`-параметрами
(`IPROTO_PREPARE = 0x0d`), литералы конкретного вызова кодируются в
msgpack-bind (`IPROTO_SQL_BIND = 0x41`), исполнение — `IPROTO_EXECUTE (11)`
с `IPROTO_STMT_ID = 0x43`. Экономится и клиентская работа (rebuild → bind),
и серверная (парсинг/планирование SQL на каждый вызов).

Ключевые инварианты:

- **конфигурация — per-host**: `<param name="prepare"/>` задаётся на
  `<profile>` (default для всех его хостов) и/или в `<host>` (переопределяет);
  наследование профиль → хост, по умолчанию `false`;
- **gate по РЕАЛЬНОЙ версии сервера**: версия берётся из greeting
  (`"Tarantool X.Y.Z"`), а не из dial-атрибута `version` профиля —
  `IPROTO_PREPARE` появился в 2.11, на серверах старше prepared-путь не
  активируется вовсе (`prepare show` показывает `PREPARE unsupported`);
- **`stmt_id` привязан к соединению** — prep-кэш (хэш переведённого шаблона
  → `stmt_id`, FIFO-вытеснение, ≤ 128 записей) живёт в `tnt_conn_t` и
  очищается при закрытии/переоткрытии соединения и при hot-reload правил
  (переведённый текст шаблона меняется);
- **прозрачный fallback**: любая ошибка prepare (DDL/PRAGMA не подготавливаются)
  или execute (смена схемы инвалидирует statement) возвращает statement на
  обычный путь — rebuild литералов + `IPROTO_EXECUTE` с текстом; 16 подряд
  неудачных PREPARE отключают prepare до конца жизни соединения;
- строки bind'ятся как `MP_STR`, целые — `MP_INT/MP_UINT`, прочие числа —
  `MP_DOUBLE` (`tnt_bind_encode`, ≤ 32 значений по 512 байт).

**SELECT-литералы (v1.0.7)**: Tarantool SQL не может вывести тип
анонимного `'?'` в SELECT-списке — `PREPARE` падает с
`Supplied key type of part N does not match index part type: expected ...`
(типичный случай — автодополнение mod_sofia:
`select distinct a2,?,? from complete where ...`). Решение: при записи в
кэш трансляции флаг `literal` вычисляется один раз
(`tnt_sql_has_select_markers` — есть ли маркеры до первого `FROM`); на
prepare-пути такие литералы материализуются в константы
(`tnt_sql_materialize_select`), а их значения изымаются из bind-массива.
Итоговый prepared-SQL: константы в SELECT, `'?'` только в WHERE. Ключ
кэша и prep-кэша остаются стабильными (маркеры везде), поэтому разные
значения SELECT-констант не плодят записи. Ограничение эвристики: первый
`FROM` вне строк/комментариев — подзапросы в WHERE с литералами в своём
SELECT-списке не покрываются (на живом трафике не встречаются).

```
tarantool prepare on | off | reset
tarantool prepare show
# profile 'local': prepare on 1/1 host(s)
# conn 'local': prepare allowed, server 3.2, PREPARE ok
# prepares: 74  execs: 8421  prep-cache hits: 8347 (99.12%)  fallbacks: 12  errors: 0
```

- `on`/`off` — runtime-override для всех живых соединений; при
  переподключении возвращается конфиг хоста (`tnt_conn_init`);
- `reset` — обнулить счётчики;
- `show` — конфиг по профилям, состояние живых conn (разрешено/реальная
  версия сервера) и счётчики: `prepares` — отправленные IPROTO_PREPARE,
  `execs` — исполнено prepared-путём, `prep-cache hits` — попадания в
  per-conn stmt_id-кэш, `fallbacks` — уходы на обычный путь, `errors` —
  отказы сервера.

**Диагностика fallback**: при `tarantool debug on` каждый fallback
логируется с причиной и шаблоном:
`prepare fallback (sql-error: <текст ошибки>): [<шаблон с '?'>]`.
Причины: `no-connection`, `prepare-broken`, `prepare-disabled`,
`prepare-unsupported`, `oom:qsql`/`oom:bind`, `prepare-rejected`,
`sql-error` (в т.ч. инвалидация prepared при смене схемы — самый частый
случай на живом трафике), `transport`.

---

## Пул соединений (`pool-size`)

Каждый handle (`tarantool://<profile>`) держит **`pool-size` независимых
соединений** (1..8, по умолчанию 1) и распределяет statement'ы между ними
**round-robin**. У каждого соединения — свой failover-цикл, свой
prepared-statement кэш (`stmt_id` привязан к соединению) и свой
per-connection мьютекс: statement'ы на **разных** соединениях выполняются
параллельно, на одном — сериализуются. `pool-size=1` полностью повторяет
классическое поведение «один handle — одно соединение».

Ключевые свойства:

- **Параллельность**: `h->lock` больше не держится на время SQL-вызова —
  он используется только для коротких критических секций (выбор
  соединения, закрепление транзакции, первый callsite, CLI). Выполнение
  сериализует per-connection мьютекс, поэтому `pool-size>1` размазывает
  конкурентные запросы по нескольким соединениям.
- **Транзакции**: `set_auto_commit(false)` закрепляет текущее соединение
  за handle'ом (`[tx]` в `tarantool conns`); все statement'ы транзакции
  идут на него, а остальные вызовы при `pool-size>1` обходят закреплённое
  соединение. `commit`/`rollback` снимают закрепление.
- **Failover ×N**: каждая сессия пула независимо выбирает хост и
  переподключается; при `pool-size>1` отказ одного соединения не
  блокирует остальные.
- **Prepared statements**: per-conn кэш уже был спроектирован под пул —
  `stmt_id` привязан к соединению, при reconnect кэш сбрасывается
  (`tnt_conn_init`), при reload правил сбрасываются все соединения пула.
- **Правила трансляции (immutable snapshot)**: reload создаёт новую копию
  `tnt_rules_t` и атомарно подменяет указатель; старый снапшот уходит в
  grace-список и освобождается через 60 с (in-flight statement'ы могут
  ещё читать его без блокировок). Трансляция на разных соединениях идёт
  параллельно.
- **Частичный пул**: если при создании handle'а не удалось открыть
  соединение `k>0`, пул ужимается до `k` (лог warning); если не открылось
  первое — handle не создаётся.

Диагностика: `tarantool conns [profile]` показывает каждое соединение
пула (`conn=N/M`, `[tx]` для закреплённого), `tarantool prepare show` —
состояние PREPARE по каждому соединению, `tarantool list` — `pool-size`
профиля.

### `pool-size` и число хостов — ортогональные оси

**`pool-size` НЕ связан с количеством хостов в `<profile>`.** Это две
независимые оси:

- **`<hosts>` — failover-список**: упорядоченные *альтернативы*, из
  которых каждая сессия выбирает **один** живой хост (master + реплики
  на случай отказа). Это не «одновременные соединения».
- **`pool-size` — параллельные соединения на один handle**: каждое
  соединение пула **само по себе** проходит failover по всему списку
  хостов.

Поэтому `pool-size = nhosts` **не распределяет** соединения по хостам:
все сессии пула стартуют с первого хоста списка и при живом master'е
подключатся к нему (реплики останутся неиспользованными, а на master'е
появятся лишние сокеты + файберы + prep-кэши). Распределение нагрузки
по репликам (SELECT → replica, write → master) — отдельная фича
role-aware routing (атрибут `role` в v1 игнорируется).

### Что реально определяет `pool-size`

| Фактор | Влияние |
|---|---|
| Параллельность запросов через один handle | Сколько потоков FreeSWITCH одновременно шлют SQL (RPS × RTT) |
| RTT до Tarantool | При RTT ~1 мс одно соединение пропускает ~1000 запросов/с; больше соединений — больше пропускная способность |
| Ресурсы | Каждый conn = сокет + файбер на сервере + prep-кэш (128 записей); при 256 хендлах × 8 conn = 2048 сокетов — перебор |
| Топология хостов | **Не влияет** — failover-список обслуживается каждой сессией пула независимо |

### Рекомендация инсталятору

- **Оставить `pool-size=1`** (безопасный дефолт, классическое поведение
  «один handle — одно соединение»).
- Если нужен «разумный дефолт под нагрузку» — **2–4**, но это параметр
  производительности, который подбирается по замерам (RPS, RTT), а не по
  числу хостов.
- **Не выводить `pool-size` из `nhosts`**: при 3–4 хостах в failover-
  списке инсталятор молча создаст 3–4 соединения к одному master'у —
  лишние ресурсы без выигрыша.

---

## Circuit breaker (`cb-mode`)

Shared circuit breaker защищает FreeSWITCH от «плотного» цикла ошибок,
когда Tarantool-хост умирает: ядро (`switch_cache_db_test_reactive`)
делает быстрый тестовый запрос, и если драйвер мгновенно возвращает
`FAIL`, ядро считает таблицы отсутствующими, пытается их создать, снова
получает мгновенный отказ и уходит в бесконечный цикл (CPU 100%).

### Машина состояний (per-host, ключ `addr:port`)

```
CLOSED ──(threshold транспортных отказов)──▶ OPEN ──(cb-timeout)──▶ HALF_OPEN
  ▲                                                                    │
  └──────────────(probe успешен)──────────────────────────────────────┘
  OPEN ◀──(probe провален: новое окно cb-timeout)── HALF_OPEN
```

- **CLOSED** — хост здоров, все запросы идут в сеть. Каждый транспортный
  отказ (connect, обрыв, PING/pre-read fail) увеличивает счётчик; при
  `cb-failure-threshold` (по умолчанию 3) breaker открывается. Счётчик
  общий для всех сессий/пулов, поэтому «волна» отказов пула = одно
  открытие, а не N.
- **OPEN** — запросы не ходят в сеть:
  - `cb-mode=wait` — поток спит до конца окна (сон ограничен
    `query-timeout`), затем повторно проверяет состояние и, если окно
    истекло, борется за право стать probe;
  - `cb-mode=fast` — мгновенная ошибка `circuit breaker OPEN (fail-fast)`.
- **HALF_OPEN** — ровно один поток («первый победил») делает реальный
  `connect()`; остальные ждут. Успех → `CLOSED` (все ждущие просыпаются),
  провал → снова `OPEN` на `cb-timeout`.

### Режимы

| `cb-mode` | Поведение при OPEN | Когда использовать |
|---|---|---|
| `off` | классический per-conn экспоненциальный backoff (v1.0.5) | поведение по умолчанию; совместимость |
| `wait` | запросы спят до конца окна, затем probe | **рекомендуется**: безопасно для `switch_cache_db_test_reactive`, сеть не долбится (1 connect за окно) |
| `fast` | мгновенный отказ | только если ядро/приложение корректно обрабатывают мгновенный FAIL (иначе CPU-цикл) |

### Особенности

- **Per-host, не per-profile**: два профиля на один `addr:port` делят один
  breaker; состояние живёт в глобальном реестре модуля.
- **Замена backoff**: при `cb-mode != off` per-conn `dead_until_host[]`/
  `backoff_host[]` не используются — их роль выполняет breaker.
- **Живые соединения не гейтятся**: breaker вмешивается только в
  `connect()`/открытие сессии и учитывает транспортные отказы; уже
  установленные соединения работают без проверок.
- **Writes при OPEN** (`fast`) возвращают ошибку без ретрая — retryable
  только SELECT/BEGIN (защита от дублей).
- **Reload** обновляет `cb-mode`/`cb-failure-threshold`/`cb-timeout` hot,
  не сбрасывая состояние и счётчики; `tarantool cb reset` сбрасывает
  состояние и счётчики вручную.
- **Single-host профиль + `wait`**: единственный хост в OPEN — запросы
  спят до конца окна (≤ `query-timeout`), затем один probe; частота
  реальных connect'ов ≈ 1 за `cb-timeout`. Это «имитация зависания» БД
  для ядра — безопаснее, чем мгновенный FAIL.

### CLI

```
tarantool cb show    # режим, порог, окно + таблица по хостам:
                     #   state, fails, open_until, probing,
                     #   opens / fail_fast / probes / successes
tarantool cb reset   # все breaker'ы в CLOSED, счётчики обнулены
```

Пример вывода:

```
circuit breaker: mode=wait threshold=3 timeout=5000ms hosts=1
  127.0.0.1:3301            state=open     fails=3 open_until=1750000000000 probing=0 opens=1 fail_fast=42 probes=1 successes=0
```

### CLI

```
tarantool maintenance show    # enable/interval/entries/runs/last run + список <sql>
tarantool maintenance restart # немедленный тик (wakeup); если поток умер —
                              # пересоздаётся
```

Пример вывода:

```
maintenance: enabled, interval=120s, entries=1, runs=42, last run 00:00:37 ago
  [0] profile=local
    sql: delete from sip_subscriptions where sip_subscriptions_uuid not in (select ...)
```

`runs` — число завершённых итераций, `last run` — время с последней
(«never», если поток ещё ни разу не отработал — например, сразу после
старта или при `enable="false"`). При `tarantool debug on` каждая итерация
пишет в лог пары `in`/`out` для каждого statement'а `<maintenance>` (как и
обычные запросы).

### Конфигурация (`<settings>`)

```xml
<param name="cb-mode" value="wait"/>
<param name="cb-failure-threshold" value="3"/>
<param name="cb-timeout" value="5000"/>
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
  `SET SESSION sql_full_scan=true` на соединение (см. параметр full-scan),
  а при `full-scan="true"` (v1.0.8) дополнительно вставляет `SEQSCAN`
  перед таблицами в `FROM`/`JOIN` — на форках без `sql_full_scan`
  (node01) это единственный способ разрешить скан.
- **Транзакции привязаны к файберу** (важно для будущей фазы
  "IPROTO streams для DML-батчей"): последовательно отправленные запросы
  без стрима сервер выполняет в **разных файберах**, поэтому `COMMIT`
  вернёт ошибку `no transaction is active`. Транзакционный батч требует
  отправки **отдельных сетевых пакетов** `IPROTO_EXECUTE` с одним и тем же
  ненулевым `IPROTO_STREAM_ID` в заголовке каждого пакета — тогда сервер
  привязывает их к одному файберу и транзакция становится сквозной:

  ```
  Пакет 1: START TRANSACTION;  (или BEGIN;)
  Пакет 2: INSERT INTO users VALUES (1, 'Ivan');   -- stmt1
  Пакет 3: INSERT INTO users VALUES (2, 'etc');    -- stmt2
  Пакет 4: COMMIT;
  ```

  Мульти-statement в одном `IPROTO_EXECUTE` Tarantool не поддерживает, а
  анонимные блоки `BEGIN … END` не являются документированной SQL-формой —
  поэтому батч-транзакционность реализуется только через streams. При этом
  DDL в стрим не заворачивается (DDL в Tarantool не транзакционен), а при
  таймауте внутри стрима обязателен явный `ROLLBACK` в том же стриме —
  сервер сам его не делает.

---

## Лицензия и автор

**Лицензия**: Mozilla Public License 2.0.

**Автор**: Filippov Anatoliy <error.email@mail.ru>

Модуль является производным от FreeSWITCH
(FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application)
и использует его API (`switch_database_interface_t` из
`src/include/switch_module_interfaces.h`), но не содержит кода других
драйверов баз данных.
