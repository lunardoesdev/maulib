<img src="assets/icon.svg" width="64" height="64" alt="">

# mewnet

C-ABI (`c-shared`) обёртка над ядром [mihomo](https://github.com/MetaCubeX/mihomo)
**v1.19.33** для Linux x86_64. Позволяет управлять прокси из C/Python/любого
языка с FFI без запуска отдельного процесса.

Возможности:

- `init/close` ядра mihomo в процессе;
- локальный mixed SOCKS/HTTP inbound на `127.0.0.1`;
- добавление прокси по VLESS/VMess/Trojan/... ссылке или base64-подписке;
- **`SharedVPNLink`** — объект-ссылка с методами `ping`, `speed`, `exit_ip`;
- авторизация per-link SOCKS5 генерируемыми ключами (пароль можно задать вручную);
- конкурентное тестирование нескольких прокси;
- hot-swap активного прокси без разрыва текущих соединений;
- async-задачи с callback или poll;
- C и Python тесты, e2e с локальным Xray.

Публичный ABI — **v2**, raylib-style: простые функции, value-структуры,
`load/unload` для списков, opaque handles и никаких JSON-строк.
Заголовки: [`libs/mewnet/include/mewnet.h`](libs/mewnet/include/mewnet.h) и
[`libs/mewnet/include/mewnet_types.h`](libs/mewnet/include/mewnet_types.h).

## Структура

```text
libs/mewnet/       Go-ядро, c-shared libmewnet
libs/mewnet-subs/  C/CMake библиотека libmewnet_subs + CLI
apps/gui/          Odin + raylib GUI
apps/mewnet-gtk/   C + GTK4 control panel
third_party/       SQLite amalgamation
tests/             C, Python, e2e
docs/
```

## Сборка

Нужен Go 1.26+ и CGO (gcc/clang). Подробности — в
[`docs/BUILD.md`](docs/BUILD.md).

Toolchain ставится через mise:

```bash
make deps           # go, odin, android-sdk, clang, cmake
make build          # dist/libmewnet.so
make subs           # build/libs/mewnet-subs/libmewnet_subs.so + mewnet-subs
make gui            # build/mewnet-gui
```

Кросс-компиляция под Windows/mingw:

```bash
make build-mingw-core   # dist/windows/mewnet.dll + import lib
make subs-mingw         # build-mingw/libs/mewnet-subs/libmewnet_subs.dll
```

## Быстрый старт (C)

Ошибки — в возвращаемом значении. Для `init` и `link_create` есть полная
структура `mewnet_error` с сообщением; для остальных операций — код ошибки и
log callback.

```c
#include "mewnet.h"

mewnet_config config = mewnet_config_default();
config.mixed_port = 0;          /* 0: из YAML или свободный, -1: выключить */
config.log_level = "warning";

mewnet_init_result init = mewnet_init(&config);
if (init.instance == NULL) {
    fprintf(stderr, "init: %s\n", init.error.message);
    return 1;
}
mewnet_instance *m = init.instance;

mewnet_link_options options = mewnet_link_options_default();
options.password = "manual-pass"; /* можно не задавать */
options.local_proxy = true;

mewnet_link_result created = mewnet_link_create(m, "vless://...", &options);
if (created.link == NULL) {
    fprintf(stderr, "link: %s\n", created.error.message);
    mewnet_close(m);
    return 1;
}
mewnet_link *link = created.link;

mewnet_link_info info = mewnet_link_get_info(link);
if (info.ok) {
    printf("socks5 %s user=%s pass=%s\n",
           info.socks_addr, info.socks_username, info.socks_password);
}

mewnet_delay_options delay = mewnet_delay_options_default();
delay.url = "https://www.gstatic.com/generate_204";
delay.timeout_ms = 5000;
delay.expected = "204";

mewnet_ping_result ping = mewnet_link_ping(link, &delay);
if (ping.ok) {
    printf("delay: %u ms\n", ping.delay_ms);
}

mewnet_speed_options speed_options = mewnet_speed_options_default();
speed_options.bytes = 5 * 1024 * 1024;
mewnet_speed_result speed = mewnet_link_speed(link, &speed_options);
if (speed.ok) {
    printf("speed: %.2f Mbit/s\n", speed.mbps);
}

mewnet_exit_ip_options exit_options = mewnet_exit_ip_options_default();
mewnet_exit_ip_result exit_ip = mewnet_link_exit_ip(link, &exit_options);
if (exit_ip.ok && exit_ip.valid) {
    printf("exit ip: %s\n", exit_ip.ip);
}

mewnet_link_destroy(link);
mewnet_close(m);
```

## SharedVPNLink

`mewnet_link_create` разбирает ссылку и создаёт:

1. прокси в группе `MEWNET` (hot-swap через `mewnet_select_proxy`);
2. локальный SOCKS5 endpoint на `127.0.0.1:<random>` **с авторизацией**.

Логин по умолчанию `mewnet`, пароль генерируется (18 случайных байт).
Пароль можно задать вручную через `password`, а upstream-credential
(VLESS UUID / Trojan password) — через `proxy_password`.
`mewnet_link_options_default()` включает локальный SOCKS5 и не выбирает ссылку
активной.

Методы:

| Метод | Что делает |
|---|---|
| `mewnet_link_get_info` | id, имя, тип, сервер, порт, SOCKS-адрес и авторизация |
| `mewnet_link_ping` | HTTP-задержка **трафиком через прокси** (`URLTest`) |
| `mewnet_link_speed` | скачивает N байт через прокси, отдаёт `bytes`/`mbps` |
| `mewnet_link_exit_ip` | GET к echo-сервису через прокси, отдаёт exit IP |
| `mewnet_link_*_async` | то же в фоновой задаче + callback или poll |
| `mewnet_link_ping_many` | пингует несколько ссылок одновременно |
| `mewnet_load_links` | все живые ссылки в массив `mewnet_link_info` |
| `mewnet_link_destroy` | удаляет прокси, закрывает SOCKS и освобождает handle |

Трафик через конкретную ссылку можно гнать двумя способами:

```bash
# 1. через mixed inbound + выбранную ссылку
curl -x http://127.0.0.1:<mixed_port> https://example.com

# 2. через per-link SOCKS5 с авторизацией
curl --socks5-hostname 127.0.0.1:<link_port> \
     --proxy-user mewnet:<generated-password> https://example.com
```

## Списки и владение

Списки выделяются библиотекой и освобождаются одной функцией:

```c
mewnet_proxy_list proxies = mewnet_load_proxies(m);
for (size_t i = 0; i < proxies.count; i++) {
    printf("%s (%s)\n", proxies.items[i].name, proxies.items[i].proxy_type);
}
printf("selected: %s\n", proxies.selected);
mewnet_unload_proxies(proxies);
```

То же для `mewnet_load_links` / `mewnet_unload_links` и
`mewnet_link_ping_many` / `mewnet_unload_ping_many`.

| Что | Кто освобождает |
|---|---|
| `mewnet_instance*` | `mewnet_close()` |
| `mewnet_link*` | `mewnet_link_destroy()` |
| `mewnet_task*` | `mewnet_task_destroy()` |
| `mewnet_load_*()` | `mewnet_unload_*()` |
| `mewnet_version()` | не нужно, статическая строка |

## Async

```c
static void on_ping(const mewnet_task_result *result, void *user) {
    if (result->state == MEWNET_TASK_DONE && result->error == MEWNET_ERROR_NONE) {
        printf("delay: %u\n", result->ping.delay_ms);
    }
}

mewnet_task *task = mewnet_link_ping_async(link, &delay, on_ping, NULL);
/* ... */
mewnet_task_destroy(task);
```

Callback вызывается из Go-горутины, то есть на произвольном OS-потоке. Внутри
callback нельзя вызывать `mewnet_close`, `mewnet_link_destroy` и
`mewnet_task_destroy`. Если нужен main-thread loop, используйте poll:

```c
mewnet_task *task = mewnet_link_ping_async(link, &delay, NULL, NULL);
mewnet_task_result result;

while ((result = mewnet_task_poll(task)).state == MEWNET_TASK_RUNNING) {
    /* main loop */
}
mewnet_task_destroy(task);
```

`mewnet_link_destroy` и `mewnet_close` отменяют и ждут связанные задачи.

## Подписки

Рядом с `mewnet` есть CMake-библиотека `libmewnet_subs` и CLI `mewnet-subs`:

- потоково качает plain/base64 подписки;
- пишет конфиги в SQLite;
- группирует подписки; по умолчанию есть группа `All` со всеми подписками;
- дедуплицирует конфиги в пределах группы по полной ссылке без `#name`;
  смена пароля/UUID создаёт новую запись, дедупликации по host нет;
- хранит числовой `priority` подписки и группы;
- тестирует конфиги и ведёт рейтинг `+1/-1` с суточными ограничениями;
- соблюдает общий лимит запросов (по умолчанию 10 за 240 секунд).

```bash
make subs
./build/libs/mewnet-subs/mewnet-subs --db data/subs.db sub add URL 100 name
./build/libs/mewnet-subs/mewnet-subs --db data/subs.db run --limit 10 --window 240
```

Подробности — в [`docs/SUBSCRIPTIONS.md`](docs/SUBSCRIPTIONS.md).

## CLI discovery: `mewnet-watch`

```bash
make subs
./build/apps/mewnet-watch/mewnet-watch \
  --db data/watch.db --proxy-port 18085 \
  --batch 70 --concurrency 70 \
  https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub
```

Пока рабочий узел не найден, утилита проверяет по 70 прокси одновременно.
Когда найден — печатает `PROXY http://127.0.0.1:<port>` и `READY`, добавляет
узел в `MILIB` и продолжает discovery с обычным rate limit. Подписки
обновляются не чаще одной в час, либо сразу, если проверять больше нечего и
рабочих узлов нет.

Подробности — в [`apps/mewnet-watch/README.md`](apps/mewnet-watch/README.md).

## GTK4 control panel: `mewnet-gtk`

```bash
make gtk
# или
./build/apps/mewnet-gtk/mewnet-gtk --db data/mewnet-gtk.db --proxy-port 18085 \
  https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub
```

Окно умеет добавлять/удалять подписки, менять активный прокси, показывает
текущий узел и журнал discovery. Локальный прокси слушает
`http://127.0.0.1:<port>`; адрес виден в верхней панели.

Надёжность: активный узел health-check'ается каждые 6 с, после двух отказов
происходит автоматический failover; живые узлы не перепроверяются; пул
держится до 4 прокси; при полном отказе discovery снова становится
агрессивным и тянет следующую подписку. При закрытии окна прокси
останавливается.

Подробности — в [`apps/mewnet-gtk/README.md`](apps/mewnet-gtk/README.md) и
[`docs/GTK_RELIABILITY.md`](docs/GTK_RELIABILITY.md).

Автотест отказоустойчивости (офлайн, локальные прокси):

```bash
make test-gtk-agent
```

## GUI на Odin

Есть простая raylib-программа:

```bash
make gui
./build/mewnet-gui --db data/subs.db
```

Она умеет работать с группами подписок, добавлять/удалять подписки, листать
прокси группы страницами, смотреть полную ссылку выбранного конфига,
добавлять ручные прокси и удалять их. GUI не загружает все прокси группы:
только страницу и одну выбранную запись.

Подробности — в [`gui/README.md`](gui/README.md).

## API

Полное описание — в [`libs/mewnet/include/mewnet.h`](libs/mewnet/include/mewnet.h).

Основные группы:

- lifecycle: `mewnet_init`, `mewnet_close`, `mewnet_get_status`;
- proxy management: `mewnet_load_proxies`, `mewnet_add_proxy`,
  `mewnet_remove_proxy`, `mewnet_select_proxy`;
- latency: `mewnet_test_delay`, `mewnet_test_link`;
- `SharedVPNLink`: `mewnet_link_create`, `mewnet_link_destroy`,
  `mewnet_link_get_info`, `mewnet_load_links`, `mewnet_link_ping`,
  `mewnet_link_speed`, `mewnet_link_exit_ip`;
- async: `mewnet_*_async`, `mewnet_task_poll`, `mewnet_task_cancel`,
  `mewnet_task_destroy`;
- logging: `mewnet_set_log_level`, `mewnet_set_log_callback`.

## Тесты

```bash
make test-unit       # ABI/логика через ctypes
make test-links      # SharedVPNLink без сети (auth, ошибки, destroy)
make test-c          # C-тест с #include "mewnet.h"
make test-stress     # 100 циклов add/remove, утечки FD/goroutine/RSS
make xray            # скачать Xray для e2e (в /tmp, не коммитится)
make test-e2e        # локальный Xray + VLESS + mixed inbound
make test-e2e-links  # 4 прокси, hot-swap, конкурентные пинги
make test-gtk-agent  # офлайн failover/recovery тест GTK-движка
make test-c-ping-local                          # ping из C, локальный Xray
make test-c-ping MEWNET_TEST_VLESS='vless://...' # ping из C, реальная ссылка
```

`test-e2e-links` — главный сценарий: Python создаёт 4 прокси через FFI,
гоняет трафик через mihomo, меняет активный прокси на горячую и одновременно
пингует три других. Результаты — в [`docs/TEST_REPORT.md`](docs/TEST_REPORT.md).

`make test-subs` запускает C-тесты подписок (base64, SHA-256, рейтинг),
`make test-e2e-subs` — локальный Xray + HTTP-подписка + SQLite/rating.

## Ограничения

- один экземпляр mihomo на процесс (глобальное состояние ядра);
- `dlclose` для `.so` запрещён — Go runtime не выгружается;
- TUN/системный VPN не входит в эту сборку (нужен тег `with_gvisor`);
- «пинг» — это HTTP-задержка (`URLTest`), не ICMP;
- per-link SOCKS5 поддерживает TCP (UDP не проксируется);
- `mewnet_close`/`mewnet_link_destroy` нельзя вызывать из async callback;
- panic в фоновой горутине mihomo роняет процесс: для недоверенных конфигов
  используйте отдельный процесс;
- сборка glibc-зависимая: для старых дистрибутивов собирайте в старом base image.

## Лицензия

GPL-3.0, как у mihomo. Распространение `.so` требует соблюдения GPL.
