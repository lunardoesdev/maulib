# milib — отчёт о тестировании

Дата: 2026-11-05
Версия: 0.2.0 (ABI v2), mihomo v1.19.33

## Окружение

| Параметр | Значение |
|---|---|
| ОС | Fedora Linux 45 (Workstation) |
| Ядро | 7.2.11-300.fc45.x86_64 |
| libc | glibc 2.44 |
| Компилятор | gcc 16.2.1 |
| Go | 1.26.0 linux/amd64 |
| Python | 3.15.0 |
| Xray (e2e) | 26.10.31 |

Сборка:

```bash
export PATH="$HOME/.local/go/bin:$PATH"
export GOPATH="$HOME/.cache/milib/gopath"
export GOCACHE="$HOME/.cache/milib/gocache"
make build
```

Результат: `dist/libmilib.so` — 55 MiB.

## 1. Go-тесты задач

Команда: `go test ./internal/core`

Проверено:

- `cancelLinks` отменяет задачу и ждёт завершения callback;
- `cancelLinks` не трогает задачи других ссылок;
- `FreeTask` не возвращается, пока callback ещё выполняется.

```
ok  	milib/internal/core
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

- создание `SharedVPNLink` и `milib_link_get_info`;
- генерация логина/пароля per-link SOCKS5;
- ручной пароль (`password`) и upstream-пароль (`proxy_password`);
- SOCKS5-авторизация: неверный пароль отклоняется, верный принимается;
- `local_proxy = false` отключает локальный SOCKS;
- ping/speed/exit_ip для мёртвого прокси возвращают ошибку без падения;
- `milib_link_ping_many` для трёх мёртвых ссылок: три изолированных результата;
- async ping/speed/exit_ip с C-callback;
- poll async-задачи и `milib_task_destroy`;
- `milib_remove_proxy` не даёт удалить прокси, принадлежащий ссылке;
- `milib_link_destroy`: ссылка исчезает из реестра, SOCKS-порт закрывается.

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

Тест собран с `#include "milib.h"` и линкуется с `libmilib.so`.

Проверка ping из C через локальный Xray:

```
$ make test-c-ping-local
2
```

`2` — задержка в миллисекундах из `milib_ping_result.delay_ms`.

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
   `milib_link_ping_many` пингует три других. Все три `ok`, загрузка в этот
   момент ещё выполняется и затем завершается полностью.
6. Async `milib_link_ping_many_async` с C-callback возвращает `ok` для всех трёх;
   тот же сценарий через `milib_task_poll` также возвращает `ok`.
7. `milib_link_destroy` для одной ссылки: она исчезает из реестра, её SOCKS-порт
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
  `MILIB_TEST_VLESS`;
- «пинг» — HTTP-задержка, не ICMP;
- один экземпляр ядра на процесс;
- `milib_close` и `milib_link_destroy` нельзя вызывать из async callback;
- Xray 26.x блокирует loopback/private цели из VLESS-inbound по умолчанию;
  в тестовом конфиге добавлено `finalRules: allow 127.0.0.0/8`.
