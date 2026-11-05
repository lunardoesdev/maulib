# milib — отчёт о тестировании

Дата: 2026-11-05
Версия: 0.1.0, mihomo v1.19.33

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

Полный прогон: `make test` → exit code 0.

## 1. Базовые ABI/unit-тесты (Python ctypes)

Команда: `python3 tests/python/test_api.py`

```
Ran 10 tests in 0.059s
OK
```

Покрыто: версия, init/status, невалидные ссылки, add/list/remove,
защита встроенных имён, уникализация имён, select, ошибка delay для мёртвого
прокси, повторный init после shutdown, отказ второго экземпляра.

## 2. SharedVPNLink без сети

Команда: `python3 tests/python/test_links.py`

```
Ran 13 tests in 0.069s
OK
```

Покрыто:

- создание `SharedVPNLink` и `milib_link_info`;
- генерация логина/пароля per-link SOCKS5;
- ручной пароль (`password`) и upstream-пароль (`proxy_password`);
- SOCKS5-авторизация: неверный пароль отклоняется, верный принимается;
- `local_proxy: false` отключает локальный SOCKS;
- ping/speed/exit_ip для мёртвого прокси возвращают `ok:false`, без падения;
- `milib_links_ping_many` для трёх мёртвых ссылок: три изолированных результата;
- async ping/speed/exit_ip с C-callback;
- `milib_remove_proxy` не даёт удалить прокси, принадлежащий ссылке;
- `milib_link_destroy`: ссылка исчезает из реестра, SOCKS-порт закрывается.

## 3. C ABI

Команда: `make test-c`

```
PASS  version
PASS  init
PASS  status ok
PASS  add proxy
PASS  list proxies
PASS  select DIRECT
PASS  remove proxy
PASS  remove missing fails
PASS  link create
PASS  link info
PASS  link list
PASS  link destroy
PASS  link list after destroy

all C tests passed
```

Тест собран с `#include "milib.h"` и линкуется с `libmilib.so`.

Отдельная проверка ping из C через локальный Xray:

```
$ make test-c-ping-local
ping: {"delay":3,"link_id":1,"mode":"proxy","name":"xray-1",
       "ok":true,"url":"http://127.0.0.1:38035/204"}
```

## 4. Stress: 100 циклов add/remove

Команда: `python3 tests/python/test_stress.py`

```
cycles=100 fds=11->11 goroutines=5->5 rss=62532KiB->67436KiB
OK
```

Утечек FD и goroutine нет; рост RSS ~4.8 MiB на 100 hot-swap циклов.

## 5. E2E: один VLESS + mixed inbound

Команда: `python3 tests/e2e/run.py` → **19 passed, 0 failed**.

Подтверждено:

- delay через VLESS > 0 (локальный `204` и gstatic);
- `curl -x http://127.0.0.1:<mixed>` реально идёт через Xray (access-лог растёт);
- после `remove` трафик возвращается на `DIRECT`, Xray больше не используется;
- при остановленном Xray delay и трафик дают ошибку без падения;
- async callback и `test_link` без добавления.

## 6. E2E: 4 прокси, hot-swap и конкурентные задачи

Команда: `python3 tests/e2e/run_links.py` → **35 passed, 0 failed**.

Сценарий полностью повторяет требование:

1. Python создаёт 4 `SharedVPNLink` через FFI; каждая ссылка указывает на
   отдельный локальный Xray с собственным access-логом.
2. Для каждой ссылки проверены `ping`, `speed` (1 MiB) и `exit_ip` —
   все три метода идут трафиком через прокси.
3. Per-link SOCKS5: неверный пароль отклоняется, верный — маршрутизирует
   запрос через свой Xray (access-лог соответствующего процесса растёт).
4. **Hot-swap**: выбран прокси №1, запущена медленная загрузка
   (`/slow`, ~5 с), затем активный прокси переключён на №2. Загрузка
   завершилась с кодом 200 и полным размером, её трафик остался в логе Xray №1;
   новый запрос ушёл в Xray №2, лог №1 после этого не изменился.
5. **Параллельность**: пока медленная загрузка идёт через прокси №1,
   `milib_links_ping_many([2,3,4])` пингует три других. Все три `ok`,
   загрузка в этот момент ещё выполняется и затем завершается полностью;
   логи Xray №2–№4 выросли, что доказывает независимость задач.
6. Async `milib_links_ping_many_async` с C-callback возвращает `ok` для всех трёх.
7. `milib_link_destroy` для одной ссылки: она исчезает из реестра, её SOCKS-порт
   закрыт, ping по её id возвращает ошибку, остальные ссылки продолжают работать.

Фрагмент вывода:

```
PASS  xray-1: ping through proxy
PASS  xray-1: speed through proxy
PASS  xray-1: exit ip through proxy
...
PASS  per-link SOCKS rejects wrong password
PASS  per-link SOCKS routes through its proxy
PASS  in-flight download survives hot-swap
PASS  in-flight download stayed on the first link
PASS  new traffic goes through the second link
PASS  pings finish while the download is still running
PASS  each ping used its own link
PASS  async many-ping results are ok
PASS  pinging a destroyed link fails
PASS  surviving links still work

35 passed, 0 failed
```

## Ограничения

- TUN/системный VPN не тестировались (сборка без тега `with_gvisor`);
- проверен только Linux x86_64/glibc 2.44;
- per-link SOCKS5 — TCP only;
- внешняя реальная VLESS-ссылка проверяется опционально через
  `MILIB_TEST_VLESS` и в этом прогоне не использовалась;
- «пинг» — HTTP-задержка, не ICMP;
- один экземпляр ядра на процесс;
- Xray 26.x блокирует loopback/private цели из VLESS-inbound по умолчанию;
  в тестовом конфиге добавлено `finalRules: allow 127.0.0.0/8`.
