# Подписки: `libmilib_subs`

`libmilib_subs` — C-библиотека для работы с подписками:

- качает подписку потоково, не загружая тело целиком в память;
- поддерживает plain text и base64 (стандартный и URL-safe);
- парсит `vless://`, `vmess://`, `trojan://`, `ss://`, `hysteria2://`, ...
- пишет конфиги, рейтинг и историю тестов в SQLite;
- удаляет устаревшие конфиги через generation-механизм;
- проверяет конфиги через `milib_test_link_info`;
- соблюдает общий лимит запросов.

## Сборка

```bash
make deps          # mise install: go, odin, android-sdk, clang, cmake
make build         # dist/libmilib.so
make subs          # build/subscriptions/libmilib_subs.so + milib-subs
make test-subs     # ctest
```

CMake использует:

- `milib` из `dist/libmilib.so`;
- `third_party/sqlite3` (amalgamation, public domain);
- clang из mise.

## CLI

```bash
./build/subscriptions/milib-subs --db data/subs.db sub add URL 100 name
./build/subscriptions/milib-subs --db data/subs.db sub ls
./build/subscriptions/milib-subs --db data/subs.db sub rm 1
./build/subscriptions/milib-subs --db data/subs.db sub priority 1 200

./build/subscriptions/milib-subs --db data/subs.db --limit 0 fetch 1
./build/subscriptions/milib-subs --db data/subs.db --limit 0 fetch --due
./build/subscriptions/milib-subs --db data/subs.db --limit 0 test
./build/subscriptions/milib-subs --db data/subs.db --limit 10 --window 240 run

./build/subscriptions/milib-subs --db data/subs.db proxies --limit 20
./build/subscriptions/milib-subs --db data/subs.db proxy add 1 'vless://...'
./build/subscriptions/milib-subs --db data/subs.db proxy rm 42
./build/subscriptions/milib-subs --db data/subs.db proxy show 42
./build/subscriptions/milib-subs --db data/subs.db stats
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

## SQLite

```sql
subscriptions(
  id, url, name, priority, enabled,
  fetch_interval_sec, next_fetch_at, last_fetch_at, last_status,
  failure_count, etag, last_modified, userinfo, last_error,
  generation, created_at
)

proxies(
  id, subscription_id, link, link_hash, name, protocol, server, port,
  rating, alive, last_tested_at, last_delay_ms, last_error,
  last_up_day, last_down_day, last_seen_generation
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

```sql
ORDER BY s.priority DESC,
         p.rating DESC,
         COALESCE(p.last_tested_at, 0) ASC,
         p.id ASC
```

Сначала подписки с максимальным `priority`, затем лучший рейтинг, затем
давно не тестировавшиеся.

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
milib_http_stream
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

## C API

```c
milib_subs_config cfg = milib_subs_config_default();
cfg.db_path = "data/subs.db";
milib_subs_open_result opened = milib_subs_open(&cfg);

milib_error err = milib_subs_add_subscription(opened.db, url, 100, "name");
milib_subs_fetch_result fetched = milib_subs_fetch_due(opened.db);

milib_instance* m = ...; /* milib_init с mixed_port = -1 */
milib_subs_test_result tested = milib_subs_test_next(opened.db, m);

milib_subs_close(opened.db);
```

Для GUI добавлены:

- `milib_subs_load_proxy_summaries` — страница без полной `link`;
- `milib_subs_get_proxy` — одна полная запись по id;
- `milib_subs_add_proxy` — ручной прокси (`manual = 1`);
- `milib_subs_remove_proxy`;
- `milib_subs_count_proxies`.

Ручные прокси не удаляются generation-cleanup, если их нет в подписке:
`DELETE ... AND manual = 0`. Если ссылка снова появляется в подписке, она
становится управляемой (`manual = 0`).

Полный список — в `subscriptions/include/milib_subs.h`.
