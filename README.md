<img src="assets/icon.svg" width="64" height="64" alt="">

# milib

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
Заголовки: [`include/milib.h`](include/milib.h) и
[`include/milib_types.h`](include/milib_types.h).

## Сборка

Нужен Go 1.26+ и CGO (gcc/clang). Подробности — в
[`docs/BUILD.md`](docs/BUILD.md).

```bash
export PATH="$HOME/.local/go/bin:$PATH"
export GOPATH="$HOME/.cache/milib/gopath"
export GOCACHE="$HOME/.cache/milib/gocache"

make build          # dist/libmilib.so
```

## Быстрый старт (C)

Ошибки — в возвращаемом значении. Для `init` и `link_create` есть полная
структура `milib_error` с сообщением; для остальных операций — код ошибки и
log callback.

```c
#include "milib.h"

milib_config config = milib_config_default();
config.mixed_port = 0;          /* 0: из YAML или свободный, -1: выключить */
config.log_level = "warning";

milib_init_result init = milib_init(&config);
if (init.instance == NULL) {
    fprintf(stderr, "init: %s\n", init.error.message);
    return 1;
}
milib_instance *m = init.instance;

milib_link_options options = milib_link_options_default();
options.password = "manual-pass"; /* можно не задавать */
options.local_proxy = true;

milib_link_result created = milib_link_create(m, "vless://...", &options);
if (created.link == NULL) {
    fprintf(stderr, "link: %s\n", created.error.message);
    milib_close(m);
    return 1;
}
milib_link *link = created.link;

milib_link_info info = milib_link_get_info(link);
if (info.ok) {
    printf("socks5 %s user=%s pass=%s\n",
           info.socks_addr, info.socks_username, info.socks_password);
}

milib_delay_options delay = milib_delay_options_default();
delay.url = "https://www.gstatic.com/generate_204";
delay.timeout_ms = 5000;
delay.expected = "204";

milib_ping_result ping = milib_link_ping(link, &delay);
if (ping.ok) {
    printf("delay: %u ms\n", ping.delay_ms);
}

milib_speed_options speed_options = milib_speed_options_default();
speed_options.bytes = 5 * 1024 * 1024;
milib_speed_result speed = milib_link_speed(link, &speed_options);
if (speed.ok) {
    printf("speed: %.2f Mbit/s\n", speed.mbps);
}

milib_exit_ip_options exit_options = milib_exit_ip_options_default();
milib_exit_ip_result exit_ip = milib_link_exit_ip(link, &exit_options);
if (exit_ip.ok && exit_ip.valid) {
    printf("exit ip: %s\n", exit_ip.ip);
}

milib_link_destroy(link);
milib_close(m);
```

## SharedVPNLink

`milib_link_create` разбирает ссылку и создаёт:

1. прокси в группе `MILIB` (hot-swap через `milib_select_proxy`);
2. локальный SOCKS5 endpoint на `127.0.0.1:<random>` **с авторизацией**.

Логин по умолчанию `milib`, пароль генерируется (18 случайных байт).
Пароль можно задать вручную через `password`, а upstream-credential
(VLESS UUID / Trojan password) — через `proxy_password`.
`milib_link_options_default()` включает локальный SOCKS5 и не выбирает ссылку
активной.

Методы:

| Метод | Что делает |
|---|---|
| `milib_link_get_info` | id, имя, тип, сервер, порт, SOCKS-адрес и авторизация |
| `milib_link_ping` | HTTP-задержка **трафиком через прокси** (`URLTest`) |
| `milib_link_speed` | скачивает N байт через прокси, отдаёт `bytes`/`mbps` |
| `milib_link_exit_ip` | GET к echo-сервису через прокси, отдаёт exit IP |
| `milib_link_*_async` | то же в фоновой задаче + callback или poll |
| `milib_link_ping_many` | пингует несколько ссылок одновременно |
| `milib_load_links` | все живые ссылки в массив `milib_link_info` |
| `milib_link_destroy` | удаляет прокси, закрывает SOCKS и освобождает handle |

Трафик через конкретную ссылку можно гнать двумя способами:

```bash
# 1. через mixed inbound + выбранную ссылку
curl -x http://127.0.0.1:<mixed_port> https://example.com

# 2. через per-link SOCKS5 с авторизацией
curl --socks5-hostname 127.0.0.1:<link_port> \
     --proxy-user milib:<generated-password> https://example.com
```

## Списки и владение

Списки выделяются библиотекой и освобождаются одной функцией:

```c
milib_proxy_list proxies = milib_load_proxies(m);
for (size_t i = 0; i < proxies.count; i++) {
    printf("%s (%s)\n", proxies.items[i].name, proxies.items[i].proxy_type);
}
printf("selected: %s\n", proxies.selected);
milib_unload_proxies(proxies);
```

То же для `milib_load_links` / `milib_unload_links` и
`milib_link_ping_many` / `milib_unload_ping_many`.

| Что | Кто освобождает |
|---|---|
| `milib_instance*` | `milib_close()` |
| `milib_link*` | `milib_link_destroy()` |
| `milib_task*` | `milib_task_destroy()` |
| `milib_load_*()` | `milib_unload_*()` |
| `milib_version()` | не нужно, статическая строка |

## Async

```c
static void on_ping(const milib_task_result *result, void *user) {
    if (result->state == MILIB_TASK_DONE && result->error == MILIB_ERROR_NONE) {
        printf("delay: %u\n", result->ping.delay_ms);
    }
}

milib_task *task = milib_link_ping_async(link, &delay, on_ping, NULL);
/* ... */
milib_task_destroy(task);
```

Callback вызывается из Go-горутины, то есть на произвольном OS-потоке. Внутри
callback нельзя вызывать `milib_close`, `milib_link_destroy` и
`milib_task_destroy`. Если нужен main-thread loop, используйте poll:

```c
milib_task *task = milib_link_ping_async(link, &delay, NULL, NULL);
milib_task_result result;

while ((result = milib_task_poll(task)).state == MILIB_TASK_RUNNING) {
    /* main loop */
}
milib_task_destroy(task);
```

`milib_link_destroy` и `milib_close` отменяют и ждут связанные задачи.

## API

Полное описание — в [`include/milib.h`](include/milib.h).

Основные группы:

- lifecycle: `milib_init`, `milib_close`, `milib_get_status`;
- proxy management: `milib_load_proxies`, `milib_add_proxy`,
  `milib_remove_proxy`, `milib_select_proxy`;
- latency: `milib_test_delay`, `milib_test_link`;
- `SharedVPNLink`: `milib_link_create`, `milib_link_destroy`,
  `milib_link_get_info`, `milib_load_links`, `milib_link_ping`,
  `milib_link_speed`, `milib_link_exit_ip`;
- async: `milib_*_async`, `milib_task_poll`, `milib_task_cancel`,
  `milib_task_destroy`;
- logging: `milib_set_log_level`, `milib_set_log_callback`.

## Тесты

```bash
make test-unit       # ABI/логика через ctypes
make test-links      # SharedVPNLink без сети (auth, ошибки, destroy)
make test-c          # C-тест с #include "milib.h"
make test-stress     # 100 циклов add/remove, утечки FD/goroutine/RSS
make xray            # скачать Xray для e2e (в /tmp, не коммитится)
make test-e2e        # локальный Xray + VLESS + mixed inbound
make test-e2e-links  # 4 прокси, hot-swap, конкурентные пинги
make test-c-ping-local                          # ping из C, локальный Xray
make test-c-ping MILIB_TEST_VLESS='vless://...' # ping из C, реальная ссылка
```

`test-e2e-links` — главный сценарий: Python создаёт 4 прокси через FFI,
гоняет трафик через mihomo, меняет активный прокси на горячую и одновременно
пингует три других. Результаты — в [`docs/TEST_REPORT.md`](docs/TEST_REPORT.md).

## Ограничения

- один экземпляр mihomo на процесс (глобальное состояние ядра);
- `dlclose` для `.so` запрещён — Go runtime не выгружается;
- TUN/системный VPN не входит в эту сборку (нужен тег `with_gvisor`);
- «пинг» — это HTTP-задержка (`URLTest`), не ICMP;
- per-link SOCKS5 поддерживает TCP (UDP не проксируется);
- `milib_close`/`milib_link_destroy` нельзя вызывать из async callback;
- panic в фоновой горутине mihomo роняет процесс: для недоверенных конфигов
  используйте отдельный процесс;
- сборка glibc-зависимая: для старых дистрибутивов собирайте в старом base image.

## Лицензия

GPL-3.0, как у mihomo. Распространение `.so` требует соблюдения GPL.
