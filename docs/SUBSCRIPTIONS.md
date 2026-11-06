# Подписки: `libmewnet_subs`

`libmewnet_subs` — C-библиотека для работы с подписками:

- качает подписку потоково, не загружая тело целиком в память;
- поддерживает plain text и base64 (стандартный и URL-safe);
- парсит `vless://`, `vmess://`, `trojan://`, `ss://`, `hysteria2://`, ...
- пишет конфиги, рейтинг и историю тестов в SQLite;
- удаляет устаревшие конфиги через generation-механизм;
- проверяет конфиги через `mewnet_test_link_info`;
- соблюдает общий лимит запросов.

## Сборка

```bash
make deps          # mise install: go, odin, android-sdk, clang, cmake
make build         # dist/libmewnet.so
make subs          # build/libs/mewnet-subs/libmewnet_subs.so + mewnet-subs
make test-subs     # ctest
```

CMake использует:

- `mewnet` из `dist/libmewnet.so`;
- `third_party/sqlite3` (amalgamation, public domain);
- clang из mise.

## CLI

```bash
./build/libs/mewnet-subs/mewnet-subs --db data/subs.db sub add URL 100 name
./build/libs/mewnet-subs/mewnet-subs --db data/subs.db sub ls
./build/libs/mewnet-subs/mewnet-subs --db data/subs.db sub rm 1
./build/libs/mewnet-subs/mewnet-subs --db data/subs.db sub priority 1 200

./build/libs/mewnet-subs/mewnet-subs --db data/subs.db --limit 0 fetch 1
./build/libs/mewnet-subs/mewnet-subs --db data/subs.db --limit 0 fetch --due
./build/libs/mewnet-subs/mewnet-subs --db data/subs.db --limit 0 test
./build/libs/mewnet-subs/mewnet-subs --db data/subs.db --limit 10 --window 240 run

./build/libs/mewnet-subs/mewnet-subs --db data/subs.db proxies --limit 20
./build/libs/mewnet-subs/mewnet-subs --db data/subs.db proxy add 1 'vless://...'
./build/libs/mewnet-subs/mewnet-subs --db data/subs.db proxy rm 42
./build/libs/mewnet-subs/mewnet-subs --db data/subs.db proxy show 42
./build/libs/mewnet-subs/mewnet-subs --db data/subs.db stats
```

`run` — основной цикл:

1. если есть due-подписка — качает самую приоритетную;
2. иначе тестирует следующего кандидата;
3. каждый внешний запрос проходит через общий rate limiter;
4. результат теста и рейтинг пишутся в SQLite.

## Seed-подписки

`subscriptions/seed.txt`:

```text
100 https://raw.githubusercontent.com/barry-far/V2ray-Config/main/All_Configs_base64_Sub.txt
90  https://raw.githubusercontent.com/ALIILAPRO/v2rayNG-Config/main/server.txt
80  https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub
```

Загрузить seed в БД:

```bash
./subscriptions/seed.sh data/subs.db
```

Проверенные вручную:

- большой base64-поток ~2.9 MB;
- plain text с `#`-метаданными;
- маленькая base64-подписка.

Публичные подписки недоверенные: конфиги могут быть кривыми или вредоносными.
Тестер лучше запускать отдельным процессом.

## Форматы

Plain:

```text
#profile-title: base64:...
#profile-update-interval: 1
#subscription-userinfo: upload=0; download=123; total=456; expire=789
vless://...
vmess://...
```

Base64:

```text
<base64 от того же текста, можно с переносами и без padding>
```

По первым 512 байтам определяется режим:

- `#` или `://` → plain;
- только base64-алфавит → base64.

Поддерживаются BOM, CRLF, пробелы, `+`/`-`, `/`/`_`, `=` и отсутствие `=`.

## Группы подписок

Подписки объединяются в группы. При первом открытии БД создаётся группа
`All`, и все новые подписки попадают в неё. Это и есть одна большая группа
со всеми подписками.

```c
mewnet_subs_group_list groups = mewnet_subs_load_groups(db);
mewnet_subs_add_subscription_in_group(db, group_id, url, priority, name);
```

Для GUI/CLI доступны:

```bash
mewnet-subs --db data/subs.db groups
mewnet-subs --db data/subs.db group add NAME [priority]
mewnet-subs --db data/subs.db group rm ID
mewnet-subs --db data/subs.db sub add URL 100 name
```

## Дедупликация конфигов

Конфиги дедуплицируются в пределах группы по `SHA-256` от полной ссылки
**без фрагмента `#name`**.

- одинаковый конфиг из двух подписок одной группы = одна запись `proxies`;
- одинаковый конфиг с разными `#name` = одна запись;
- изменение пароля/UUID/транспорта = другая ссылка = новая запись;
- дедупликации по host:port нет.

Связь «конфиг ↔ подписки» хранится в `proxy_sources`. Если конфиг есть в
нескольких подписках, он остаётся, пока есть хотя бы один источник.

## SQLite

```sql
subscription_groups(
  id, name, priority, enabled, created_at
)

subscriptions(
  id, group_id, url, name, priority, enabled,
  fetch_interval_sec, next_fetch_at, last_fetch_at, last_status,
  failure_count, etag, last_modified, userinfo, last_error,
  generation, created_at
)

proxies(
  id, group_id, link, link_hash, name, protocol, server, port,
  rating, alive, manual, last_tested_at, last_delay_ms, last_error,
  last_up_day, last_down_day,
  UNIQUE(group_id, link_hash)
)

proxy_sources(
  proxy_id, subscription_id, last_seen_generation
)

test_events(
  id, proxy_id, ts, ok, delay_ms, error,
  rating_before, rating_after, rating_delta
)
```

Включены WAL, `busy_timeout`, `foreign_keys`, `ON DELETE CASCADE`.

## Рейтинг

При успехе:

- `rating += 1`, но не чаще одного раза в UTC-сутки;
- `last_up_day = YYYYMMDD`.

При ошибке:

- `rating -= 1`, но не чаще одного раза в сутки;
- `last_down_day = YYYYMMDD`.

Ограничения независимые: за день можно получить и `+1`, и `-1`.

## Выбор кандидата

Кандидаты выбираются **случайно, с весами** по приоритету группы, приоритету
подписки-источника и рейтингу прокси:

```text
weight = 1 + max(0, group_priority)
           + max(0, source_priority)
           + max(0, rating + 10)

key = random_uniform ^ (1 / weight)
ORDER BY key DESC
```

То есть высокоприоритетные и высокорейтинговые прокси выпадают чаще, но
порядок не фиксированный: одинаковые по весу конфиги перемешиваются.
Это используется и в batch-discovery, и в обычном `mewnet_subs_test_next`.

## Rate limit

По умолчанию:

- 10 запросов;
- окно 240 секунд;
- интервал ~24 секунды с jitter.

Лимит общий для fetch и test. Настраивается:

```bash
--limit 10 --window 240
--limit 0          # отключить (для отладки)
```

## Потоковый ingest

```text
mewnet_http_stream
  → sniff plain/base64
  → streaming base64 decoder / passthrough
  → bounded line buffer
  → # metadata → subscriptions
  → link → SHA-256 → proxies UPSERT
  → DELETE stale by generation
```

Память ограничена:

- sniff: 512 байт;
- строка: 16 KiB;
- SQLite page cache.

Тело подписки и список всех ссылок в памяти не держатся.

## Discovery CLI: `mewnet-watch`

```bash
./build/apps/mewnet-watch/mewnet-watch --db data/watch.db --proxy-port 18085 \
  --batch 70 --concurrency 70 \
  https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub
```

Поведение:

- создаёт SQLite;
- добавляет подписки в `All`;
- пока нет alive-узла, тестирует непроверенные конфиги пачками по 70
  одновременно;
- при нахождении alive добавляет его в `MILIB`, выбирает и печатает
  `PROXY http://127.0.0.1:<port>` + `READY`;
- дальше discovery идёт через стандартный rate limiter;
- обновляет максимум одну подписку в час; если непроверенных узлов нет и
  рабочих нет — обновляет следующую немедленно.

Библиотечные API для этого:

- `mewnet_test_links_batch` / `mewnet_test_links_batch_first` /
  `mewnet_unload_batch_test` в `libmewnet`;
- `mewnet_subs_test_batch` — выбрать непроверенные, проверить пачкой,
  остановиться на первом alive (остальные проверки отменяются), записать
  результаты и вернуть первый alive;
- `mewnet_subs_count_untested`, `mewnet_subs_count_alive`,
  `mewnet_subs_first_alive`;
- `mewnet_subs_fetch_now` — fetch без ожидания rate limiter для аварийного
  обновления.

## C API

```c
mewnet_subs_config cfg = mewnet_subs_config_default();
cfg.db_path = "data/subs.db";
mewnet_subs_open_result opened = mewnet_subs_open(&cfg);

mewnet_error err = mewnet_subs_add_subscription(opened.db, url, 100, "name");
mewnet_subs_fetch_result fetched = mewnet_subs_fetch_due(opened.db);

mewnet_instance* m = ...; /* mewnet_init с mixed_port = -1 */
mewnet_subs_test_result tested = mewnet_subs_test_next(opened.db, m);

mewnet_subs_close(opened.db);
```

Для GUI добавлены:

- `mewnet_subs_load_proxy_summaries` — страница без полной `link`;
- `mewnet_subs_get_proxy` — одна полная запись по id;
- `mewnet_subs_add_proxy` — ручной прокси (`manual = 1`);
- `mewnet_subs_remove_proxy`;
- `mewnet_subs_count_proxies`.

Ручные прокси не удаляются generation-cleanup, если их нет в подписке:
`DELETE ... AND manual = 0`. Если ссылка снова появляется в подписке, она
становится управляемой (`manual = 0`).

Полный список — в `subscriptions/include/mewnet_subs.h`.
