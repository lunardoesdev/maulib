<img src="assets/icon.svg" width="64" height="64" alt="">

# milib

C-ABI (`c-shared`) обёртка над ядром [mihomo](https://github.com/MetaCubeX/mihomo)
**v1.19.33** для Linux x86_64. Позволяет управлять прокси из C/Python/любого
языка с FFI без запуска отдельного процесса.

Возможности:

- `init/shutdown` ядра mihomo в процессе;
- локальный mixed SOCKS/HTTP inbound на `127.0.0.1`;
- добавление прокси по VLESS/VMess/Trojan/... ссылке или base64-подписке;
- **`SharedVPNLink`** — объект-ссылка с методами `ping`, `speed`, `exit_ip`;
- авторизация per-link SOCKS5 генерируемыми ключами (пароль можно задать вручную);
- конкурентное тестирование нескольких прокси;
- hot-swap активного прокси без разрыва текущих соединений;
- C и Python тесты, e2e с локальным Xray.

## Сборка

Нужен Go 1.26+ и CGO (gcc/clang). Подробности — в
[`docs/BUILD.md`](docs/BUILD.md).

```bash
export PATH="$HOME/.local/go/bin:$PATH"
export GOPATH="$HOME/.cache/milib/gopath"
export GOCACHE="$HOME/.cache/milib/gocache"

make build          # dist/libmilib.so
```

Публичный заголовок: [`include/milib.h`](include/milib.h).

## SharedVPNLink

`milib_link_create` разбирает ссылку и создаёт:

1. прокси в группе `MILIB` (hot-swap через `milib_select`);
2. локальный SOCKS5 endpoint на `127.0.0.1:<random>` **с авторизацией**.

Логин по умолчанию `milib`, пароль генерируется (18 случайных байт).
Пароль можно задать вручную через `password`, а upstream-credential
(VLESS UUID / Trojan password) — через `proxy_password`.

```c
milib_link* link = milib_link_create(h, "vless://...", NULL, &err);
char* info = milib_link_info(link);   // socks.addr, socks.username, socks.password
char* ping = milib_link_ping(link, "{\"url\":\"https://www.gstatic.com/generate_204\"}");

int delay_ms = 0;                              /* пинг числом */
milib_link_ping_ms(link, NULL, &delay_ms, NULL);

char* speed = milib_link_speed(link, "{\"url\":\"https://speed.cloudflare.com/__down?bytes=5242880\"}");
char* ip = milib_link_exit_ip(link, NULL);
milib_link_destroy(link);
```

Методы:

| Метод | Что делает |
|---|---|
| `milib_link_ping` | HTTP-задержка **трафиком через прокси** (`URLTest`), JSON |
| `milib_link_ping_ms` | то же, но задержка числом в `int*` |
| `milib_link_speed` | скачивает N байт через прокси, возвращает Мбит/с |
| `milib_link_exit_ip` | GET к echo-сервису через прокси, возвращает exit IP |
| `milib_link_*_async` | то же в фоновой задаче + C-callback |
| `milib_links_ping_many` | пингует несколько ссылок одновременно |
| `milib_link_list` | все живые ссылки |
| `milib_link_destroy` | удаляет прокси, закрывает SOCKS и освобождает handle |

Трафик через конкретную ссылку можно гнать двумя способами:

```bash
# 1. через mixed inbound + выбранную ссылку
curl -x http://127.0.0.1:<mixed_port> https://example.com

# 2. через per-link SOCKS5 с авторизацией
curl --socks5-hostname 127.0.0.1:<link_port> \
     --proxy-user milib:<generated-password> https://example.com
```

## Быстрый старт (Python)

```python
import ctypes, json

lib = ctypes.CDLL("dist/libmilib.so")
lib.milib_init.argtypes = [
    ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_char_p),
]
lib.milib_init.restype = ctypes.c_int
lib.milib_link_create.argtypes = [
    ctypes.c_uint64, ctypes.c_char_p, ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_char_p),
]
lib.milib_link_create.restype = ctypes.c_void_p
lib.milib_link_info.argtypes = [ctypes.c_void_p]
for name in ("milib_link_ping", "milib_link_speed", "milib_link_exit_ip"):
    getattr(lib, name).argtypes = [ctypes.c_void_p, ctypes.c_char_p]
lib.milib_link_list.argtypes = [ctypes.c_uint64]
lib.milib_links_ping_many.argtypes = [ctypes.c_uint64, ctypes.c_char_p, ctypes.c_char_p]
for name in ("milib_link_info", "milib_link_ping", "milib_link_speed",
             "milib_link_exit_ip", "milib_link_list", "milib_links_ping_many"):
    getattr(lib, name).restype = ctypes.c_void_p
lib.milib_link_destroy.argtypes = [ctypes.c_void_p]
lib.milib_free_string.argtypes = [ctypes.c_void_p]

handle = ctypes.c_uint64()
err = ctypes.c_char_p()
assert lib.milib_init(None, None, b'{"mixed_port":0}',
                      ctypes.byref(handle), ctypes.byref(err)) == 0

def call(fn, *args):
    ptr = fn(*args)
    try:
        return json.loads(ctypes.cast(ptr, ctypes.c_char_p).value)
    finally:
        lib.milib_free_string(ptr)

link = lib.milib_link_create(handle, b"vless://...@host:443?...#node",
                             b'{"password":"my-local-pass"}', None)
print(call(lib.milib_link_info, link))
print(call(lib.milib_link_ping, link, None))
print(call(lib.milib_link_speed, link, None))
print(call(lib.milib_link_exit_ip, link, None))

lib.milib_link_destroy(link)
lib.milib_shutdown(handle)
```

## API

Полное описание — в [`include/milib.h`](include/milib.h). Низкоуровневые
функции (`milib_add_proxy`, `milib_remove_proxy`, `milib_test_delay` и т.д.)
остаются доступны, но для новых интеграций используйте `SharedVPNLink`.

Все функции возвращают JSON `{"ok":true,...}` или
`{"ok":false,"error":"..."}`. Строки освобождаются `milib_free_string`.

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
- panic в фоновой горутине mihomo роняет процесс: для недоверенных конфигов
  используйте отдельный процесс;
- сборка glibc-зависимая: для старых дистрибутивов собирайте в старом base image.

## Лицензия

GPL-3.0, как у mihomo. Распространение `.so` требует соблюдения GPL.
