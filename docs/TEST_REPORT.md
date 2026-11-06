# mewnet — отчёт о тестировании

Дата: 2026-11-05
Версия: 0.2.0 (ABI v2), mihomo v1.19.33

## Окружение

| Параметр | Значение |
|---|---|
| ОС | Fedora Linux 45 (Workstation) |
| Ядро | 7.2.11-300.fc45.x86_64 |
| libc | glibc 2.44 |
| Компилятор | clang 23.1.4 (mise), gcc 16.2.1 |
| Go | 1.27.3 linux/amd64 (mise) |
| CMake | 4.4.5 (mise) |
| Python | 3.15.0 |
| Xray (e2e) | 26.10.31 |

Сборка:

```bash
export PATH="$HOME/.local/go/bin:$PATH"
export GOPATH="$HOME/.cache/mewnet/gopath"
export GOCACHE="$HOME/.cache/mewnet/gocache"
make build
```

Результат: `dist/libmewnet.so` — 55 MiB.

## 1. Go-тесты задач

Команда: `go test ./internal/core`

Проверено:

- `cancelLinks` отменяет задачу и ждёт завершения callback;
- `cancelLinks` не трогает задачи других ссылок;
- `FreeTask` не возвращается, пока callback ещё выполняется.

```
ok  	mewnet/internal/core
```

## 2. Базовые ABI/unit-тесты (Python ctypes)

Команда: `python3 tests/python/test_api.py`

```
Ran 10 tests
OK
```

Покрыто: версия/ABI, init/status, невалидные ссылки, add/load/remove,
защита встроенных имён, уникализация имён, select, ошибка delay для мёртвого
прокси, повторный init после shutdown, отказ второго экземпляра.

## 3. SharedVPNLink без сети

Команда: `python3 tests/python/test_links.py`

```
Ran 14 tests
OK
```

Покрыто:

- создание `SharedVPNLink` и `mewnet_link_get_info`;
- генерация логина/пароля per-link SOCKS5;
- ручной пароль (`password`) и upstream-пароль (`proxy_password`);
- SOCKS5-авторизация: неверный пароль отклоняется, верный принимается;
- `local_proxy = false` отключает локальный SOCKS;
- ping/speed/exit_ip для мёртвого прокси возвращают ошибку без падения;
- `mewnet_link_ping_many` для трёх мёртвых ссылок: три изолированных результата;
- async ping/speed/exit_ip с C-callback;
- poll async-задачи и `mewnet_task_destroy`;
- `mewnet_remove_proxy` не даёт удалить прокси, принадлежащий ссылке;
- `mewnet_link_destroy`: ссылка исчезает из реестра, SOCKS-порт закрывается.

## 4. C ABI

Команда: `make test-c`

```
PASS  abi version
PASS  version
PASS  init
PASS  status ok
PASS  add proxy
PASS  load proxies
PASS  added proxy is selected
PASS  select DIRECT
PASS  remove proxy
PASS  remove missing fails
PASS  link create
PASS  link info
PASS  link info has auth
PASS  load links
PASS  load links after destroy

all C tests passed
```

Тест собран с `#include "mewnet.h"` и линкуется с `libmewnet.so`.

Проверка ping из C через локальный Xray:

```
$ make test-c-ping-local
2
```

`2` — задержка в миллисекундах из `mewnet_ping_result.delay_ms`.

## 5. Stress: 100 циклов add/remove

Команда: `python3 tests/python/test_stress.py`

```
cycles=100 fds=11->11 goroutines=5->5 rss=60760KiB->65172KiB
OK
```

Утечек FD и goroutine нет; рост RSS ~4.3 MiB на 100 hot-swap циклов.

## 6. E2E: один VLESS + mixed inbound

Команда: `python3 tests/e2e/run.py` → **18 passed, 0 failed**.

Подтверждено:

- delay через VLESS > 0 (локальный `204` и gstatic);
- `curl -x http://127.0.0.1:<mixed>` реально идёт через Xray (access-лог растёт);
- после `remove` трафик возвращается на `DIRECT`, Xray больше не используется;
- при остановленном Xray delay и трафик дают ошибку без падения;
- async callback, `test_link` без добавления, log callback.

## 7. E2E: 4 прокси, hot-swap и конкурентные задачи

Команда: `python3 tests/e2e/run_links.py` → **39 passed, 0 failed**.

Сценарий:

1. Python создаёт 4 `SharedVPNLink` через FFI; каждая ссылка указывает на
   отдельный локальный Xray с собственным access-логом.
2. Для каждой ссылки проверены `ping`, `speed` (1 MiB) и `exit_ip`.
3. Per-link SOCKS5: неверный пароль отклоняется, верный — маршрутизирует
   запрос через свой Xray.
4. Hot-swap: выбран прокси №1, запущена медленная загрузка, затем активный
   прокси переключён на №2. Загрузка завершилась полностью, её трафик остался
   в логе Xray №1; новый запрос ушёл в Xray №2.
5. Параллельность: пока медленная загрузка идёт через прокси №1,
   `mewnet_link_ping_many` пингует три других. Все три `ok`, загрузка в этот
   момент ещё выполняется и затем завершается полностью.
6. Async `mewnet_link_ping_many_async` с C-callback возвращает `ok` для всех трёх;
   тот же сценарий через `mewnet_task_poll` также возвращает `ok`.
7. `mewnet_link_destroy` для одной ссылки: она исчезает из реестра, её SOCKS-порт
   закрыт, запущенная async-задача отменена, остальные ссылки продолжают работать.

Фрагмент вывода:

```
PASS  xray-1: ping through proxy
PASS  xray-1: speed through proxy
PASS  xray-1: exit ip through proxy
...
PASS  async many-ping results are ok
PASS  many-ping poll results are ok
PASS  link task is running
PASS  destroy link
PASS  destroy cancels the link task
PASS  pinging a destroyed link fails
PASS  surviving links still work

39 passed, 0 failed
```

## Ограничения

- TUN/системный VPN не тестировались (сборка без тега `with_gvisor`);
- проверен только Linux x86_64/glibc 2.44;
- per-link SOCKS5 — TCP only;
- внешняя реальная VLESS-ссылка проверяется опционально через
  `MEWNET_TEST_VLESS`;
- «пинг» — HTTP-задержка, не ICMP;
- один экземпляр ядра на процесс;
- `mewnet_close` и `mewnet_link_destroy` нельзя вызывать из async callback;
- Xray 26.x блокирует loopback/private цели из VLESS-inbound по умолчанию;
  в тестовом конфиге добавлено `finalRules: allow 127.0.0.0/8`.

## 8. Подписки (`libmewnet_subs`)

Сборка:

```bash
make subs
make test-subs
```

CTest:

```
1/3 Test #1: base64 ........................... Passed
2/3 Test #2: sha256 ........................... Passed
3/3 Test #3: rating ........................... Passed

100% tests passed out of 3
```

Проверено вручную:

- plain-подписка с `#profile-title`, `#profile-update-interval`,
  `#subscription-userinfo`;
- base64-подписка, в том числе с переносами и без padding;
- реальные GitHub-подписки:
  - `barry-far/.../All_Configs_base64_Sub.txt` — 9167 конфигов,
    ~2.7 с, RSS ~48 MB;
  - `ALIILAPRO/.../server.txt` — 4323 конфига;
  - `Pawdroid/Free-servers/main/sub` — 9 конфигов;
- повторный fetch с `ETag` возвращает `304` и не меняет БД;
- смена содержимого подписки удаляет устаревшие конфиги (`removed=2`);
- тест живого прокси через локальный Xray: `alive=1`, `rating 0->1`,
  метаданные имя/сервер/порт обновились;
- тест мёртвого прокси: `rating 0->-1`, повторный тест в тот же день `+0`;
- в `test_events` пишутся все результаты;
- `libmewnet_subs` экспортирует только `mewnet_subs_*` символы.

E2E:

```bash
make test-e2e-subs
```

```
20 passed, 0 failed
```

Проверено: plain/base64 ingest, группа `All`, дедупликация конфигов между
подписками одной группы, `#name` не влияет на идентичность, смена пароля создаёт новую запись,
ручной прокси (`manual=1`) переживает повторный fetch с generation-cleanup,
живой прокси
через локальный Xray (`alive=1`, `rating 0->1`, метаданные), мёртвый прокси
(`rating 0->-1`, повторный тест в тот же день не меняет рейтинг),
`test_events`.

## 9. GUI на Odin/raylib

Сборка:

```bash
make gui
```

Проверено:

- `odin build` с `vendor:raylib` и `libmewnet_subs`;
- запуск окна 1200x760 на текущем DISPLAY;
- GUI использует `mewnet_subs_load_proxy_summaries` (страница без `link`) и
  `mewnet_subs_get_proxy` только для выбранной записи;
- добавление/удаление подписок и ручных прокси идёт через публичное API
  `libmewnet_subs`, без прямых SQL-запросов из GUI;
- ручные прокси (`manual=1`) не удаляются generation-cleanup при fetch.

## 10. Кросс-компиляция mingw

```bash
make build-mingw-core
make subs-mingw
```

Получено:

- `dist/windows/mewnet.dll` — PE32+ x86-64;
- `dist/windows/libmewnet.dll.a` — import library;
- `build-mingw/libs/mewnet-subs/libmewnet_subs.dll` — PE32+ x86-64;
- `build-mingw/libs/mewnet-subs/mewnet-subs.exe` — PE32+ x86-64.

Запуск под Wine не проверялся.

## 11. Discovery CLI `mewnet-watch`

Проверка без Xray, на публичной подписке GitHub.

Команда:

```bash
./build/apps/mewnet-watch/mewnet-watch \
  --db /tmp/opencode/final-watch.db --proxy-port 18085 \
  --batch 70 --concurrency 70 --timeout 3000 \
  https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub \
  https://raw.githubusercontent.com/ALIILAPRO/v2rayNG-Config/main/server.txt \
  https://raw.githubusercontent.com/barry-far/V2ray-Config/main/All_Configs_base64_Sub.txt
```

Результат:

```text
ready=1 ready_ms=3412
curl_rc=0 total_ms=4085
HTTP/1.1 200 Connection established
HTTP/1.1 200 OK
<TITLE>The Programming Language Lua
```

Фрагмент лога:

```text
DB /tmp/opencode/final-watch.db
PROXY http://127.0.0.1:18085
FETCH t=... id=1 status=200 added=7 updated=0 removed=0
BATCH t=... tested=3 alive=1
WORKING id=5 name=新西兰+V2CROSS.COM
PROXY http://127.0.0.1:18085
READY
TEST t=... id=1 alive=1 delay=504 rating=1->1
TEST t=... id=5 alive=1 delay=641 rating=1->1
```

Итог:

- рабочий прокси найден за **3.412 с**;
- `curl https://lua.org` через `http://127.0.0.1:18085` завершился за
  **4.085 с** от запуска;
- получен HTTP `200` и HTML `The Programming Language Lua`;
- Xray не использовался;
- после `READY` следующий rate-limited тест прошёл примерно через 20 с,
  то есть стандартный лимитер работает.

`mewnet-watch` также кросс-компилируется под mingw:

```text
build-mingw/apps/mewnet-watch/mewnet-watch.exe
```

## 12. GTK4 control panel `mewnet-gtk`

Сборка:

```bash
make subs   # собирает mewnet-gtk, если найден gtk4.pc
```

Запуск на чистой базе, одна публичная подписка, без Xray:

```bash
./build/apps/mewnet-gtk/mewnet-gtk \
  --db /tmp/opencode/gtk-fresh.db \
  --proxy-port 18097 \
  --batch 70 --concurrency 70 --timeout 3000 \
  https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub
```

Результат:

```text
ready_ms=3940
curl_rc=0 total_ms=5237
<TITLE>The Programming Language Lua
```

Лог окна:

```text
DB /tmp/opencode/gtk-fresh.db
PROXY http://127.0.0.1:18097
batch=70 concurrency=70 fetch_interval=3600s slow_interval=24s
DISCOVERY start batch=70 concurrency=70 timeout=3000ms
FETCH id=1 status=200 added=7 updated=0 removed=0
SUBS n=1
BATCH tested=3 alive=1
ACTIVE id=2 name=新西兰+V2CROSS.COM delay=1366
READY local=http://127.0.0.1:18097
```

Проверено:

- при пустой базе окно пишет `WAIT подписок нет — добавьте URL в окне` и
  ждёт; после добавления/запуска с URL сразу качает подписку и активирует
  найденный узел;
- при существующей базе с alive-узлом прокси активируется сразу при старте;
- кнопка «Сменить прокси» переключает на следующий живой узел:
  ```text
  SWITCH requested
  ACTIVE id=5 name=新西兰+V2CROSS.COM delay=2814
  SWITCH id=5 name=新西兰+V2CROSS.COM
  ```
- после `READY` discovery не трогает живые узлы и добирает пул: при пуле
  меньше 4 живых шаг около 2 с, при полном — 24 с;
  ```text
  TEST id=5 alive=1 delay=770 rating=1->1
  TEST id=5 alive=1 delay=2814 rating=1->1
  ```
- `--proxy-port 0` выбирает свободный порт и показывает его в окне:
  `PROXY http://127.0.0.1:36617`;
- при закрытии окна/SIGTERM процесс завершает discovery и Mihomo, порт больше
  не слушается.

### 12.1. Автоматическая проверка отказоустойчивости

```bash
make test-agent
```

Тест офлайновый: локальный `204` origin, три локальных HTTP-прокси, подписка
на них, запуск `mewnet-cli --headless`. Результат:

```text
PASS  bootstrap: READY
PASS  bootstrap: local proxy port
PASS  bootstrap: traffic through proxy
PASS  pool: grew to three working proxies
PASS  switch: moved to another proxy
PASS  switch: random among best, not just two
PASS  switch: traffic still works
PASS  failover: active proxy identified
PASS  failover: health check detected dead proxy
PASS  failover: switched to another proxy
PASS  failover: traffic works after switch
PASS  exhausted: aggressive discovery started
PASS  exhausted: process still alive
PASS  recovery: active proxy selected again
PASS  recovery: traffic works again
PASS  wait: empty database waits
PASS  wait: subscription inserted
PASS  wait: fetched and activated
PASS  wait: traffic works

19 passed, 0 failed
```

Проверены: bootstrap, рост пула, случайное переключение среди лучших, health-check и
автоматический failover, полный отказ всех прокси, возврат нового узла,
а также сценарий «пустая база → подписка появилась → автоматический запуск».
Спецификация и матрица отказов — в
[`docs/GTK_RELIABILITY.md`](GTK_RELIABILITY.md).

Память UI проверена на базах с 7 и 4226 прокси: RSS ~186 МБ и ~190 МБ
соответственно, то есть разница около 4 МБ.

UI smoke без ручных кликов:

```bash
./build/apps/mewnet-gtk/mewnet-gtk --db /tmp/ui.db --screenshot /tmp/ui.png
```

Окно отрисовывается в PNG и приложение завершается.

## 13. Agent library, dedup and memory scaling

### 13.1. Dedup

На копии рабочей базы (21 184 прокси, 30 подписок) открытие с новой схемой:

```text
до:      21184 прокси
после:   12638 прокси
удалено:  8546 дубликатов
дублей identity: 0
время миграции: ~2.2 с
```

Identity = SHA-256(protocol + host + port + credentials). Имя, `#fragment` и
транспортные опции не учитываются. Уникальный индекс:
`proxies(group_id, identity_hash)`.

### 13.2. Память

```text
30 подписок, 12 638 прокси:      GTK ~159 МБ RSS
10 030 подписок, 12 638 прокси:  GTK ~159 МБ RSS
```

Список подписок в UI — страница по 50 строк; agent использует SQL-счётчики,
`ORDER BY ... LIMIT 1` и не загружает миллионы строк. SQLite cache ограничен
8 МБ, `temp_store=FILE`.

### 13.3. Долгие подписки

Подписка скачивается во временный файл; транзакция SQLite начинается только
после загрузки. Остановка CLI во время скачивания большой подписки:

```text
exit_ms=108
```

То есть fetch отменяется и не блокирует закрытие/health-check.

### 13.4. Общий agent

`mewnet_agent` в `libmewnet_subs` используется GTK4 и CLI/TUI. Офлайновый
тест `make test-agent` (ранее `test-gtk-agent`) проходит 19/19 на CLI:

```text
bootstrap, pool growth, random switch across 3 nodes, health failover,
total outage, recovery, wait-for-subscription
```

CLI/TUI:

```bash
make cli
# s — случайный лучший прокси, q — выход
```
